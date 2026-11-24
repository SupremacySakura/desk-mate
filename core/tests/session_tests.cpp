#include "deskmate/byte_io.hpp"
#include "deskmate/clock.hpp"
#include "deskmate/protocol.hpp"
#include "deskmate/session.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                                  \
  do {                                                                                    \
    if (!(condition)) {                                                                   \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK failed: " #condition << '\n'; \
      ++failures;                                                                         \
      return;                                                                             \
    }                                                                                     \
  } while (false)

#define CHECK_EQ(actual, expected)                                                       \
  do {                                                                                   \
    const auto& actual_value = (actual);                                                 \
    const auto& expected_value = (expected);                                             \
    if (!(actual_value == expected_value)) {                                             \
      std::cerr << __FILE__ << ':' << __LINE__ << ": CHECK_EQ failed: " #actual          \
                << " != " #expected << '\n';                                             \
      ++failures;                                                                        \
      return;                                                                            \
    }                                                                                    \
  } while (false)

using deskmate::protocol::CapabilitySet;
using deskmate::protocol::ControlMessage;
using deskmate::protocol::ControlOpcode;
using deskmate::protocol::DeviceSession;
using deskmate::protocol::DeviceSessionConfig;
using deskmate::protocol::DisplayResolution;
using deskmate::protocol::BoundaryResult;
using deskmate::protocol::DeviceBoundary;
using deskmate::protocol::Header;
using deskmate::protocol::HostSession;
using deskmate::protocol::HostSessionConfig;
using deskmate::protocol::IoResult;
using deskmate::protocol::IoStatus;
using deskmate::protocol::ManualClock;
using deskmate::protocol::NegotiatedStream;
using deskmate::protocol::PixelFormat;
using deskmate::protocol::StreamProfile;
using deskmate::protocol::SessionFailure;
using deskmate::protocol::SessionState;
using deskmate::protocol::VideoEncoder;
using deskmate::protocol::VideoSendStatus;

class BytePipe {
 public:
  explicit BytePipe(std::size_t read_chunk_size = 1024, std::size_t write_chunk_size = 1024)
      : read_chunk_size_(read_chunk_size), write_chunk_size_(write_chunk_size) {}

  IoResult read(std::span<std::uint8_t> destination) {
    if (read_offset_ == bytes_.size()) {
      return {IoStatus::WouldBlock, 0};
    }
    const std::size_t available = bytes_.size() - read_offset_;
    const std::size_t amount = std::min({destination.size(), available, read_chunk_size_});
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(read_offset_), amount,
                destination.begin());
    read_offset_ += amount;
    return {IoStatus::Ok, amount};
  }

  IoResult write(std::span<const std::uint8_t> source) {
    const std::size_t amount = std::min(source.size(), write_chunk_size_);
    bytes_.insert(bytes_.end(), source.begin(),
                  source.begin() + static_cast<std::ptrdiff_t>(amount));
    return {IoStatus::Ok, amount};
  }

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return bytes_; }
  void append(std::span<const std::uint8_t> bytes) {
    bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
  }
  void clear() {
    bytes_.clear();
    read_offset_ = 0;
  }

 private:
  std::vector<std::uint8_t> bytes_;
  std::size_t read_offset_{};
  std::size_t read_chunk_size_;
  std::size_t write_chunk_size_;
};

class DirectedIo final : public deskmate::protocol::ByteIo {
 public:
  DirectedIo(BytePipe& inbound, BytePipe& outbound) : inbound_(inbound), outbound_(outbound) {}

  IoResult read(std::span<std::uint8_t> destination) override {
    return inbound_.read(destination);
  }
  IoResult write(std::span<const std::uint8_t> source) override {
    return outbound_.write(source);
  }

 private:
  BytePipe& inbound_;
  BytePipe& outbound_;
};

std::vector<ControlMessage> decode_controls(const BytePipe& pipe) {
  std::vector<ControlMessage> controls;
  std::size_t offset = 0;
  while (offset + deskmate::protocol::kWireHeaderSize <= pipe.bytes().size()) {
    const auto header = deskmate::protocol::decode_header(
        std::span<const std::uint8_t>(pipe.bytes()).subspan(offset));
    if (!header) {
      break;
    }
    offset += deskmate::protocol::kWireHeaderSize;
    if (offset + header.value().payload_len > pipe.bytes().size()) {
      break;
    }
    const auto payload = std::span<const std::uint8_t>(pipe.bytes())
                             .subspan(offset, header.value().payload_len);
    offset += header.value().payload_len;
    if (header.value().type != static_cast<std::uint8_t>(
                                   deskmate::protocol::MessageType::Control)) {
      continue;
    }
    const auto decoded = deskmate::protocol::decode_control_payload(payload);
    if (decoded) {
      controls.push_back(decoded.value());
    }
  }
  return controls;
}

std::vector<deskmate::protocol::MessageType> decode_message_types(const BytePipe& pipe) {
  std::vector<deskmate::protocol::MessageType> types;
  std::size_t offset = 0;
  while (offset + deskmate::protocol::kWireHeaderSize <= pipe.bytes().size()) {
    const auto header = deskmate::protocol::decode_header(
        std::span<const std::uint8_t>(pipe.bytes()).subspan(offset));
    if (!header) {
      break;
    }
    offset += deskmate::protocol::kWireHeaderSize;
    if (offset + header.value().payload_len > pipe.bytes().size()) {
      break;
    }
    offset += header.value().payload_len;
    types.push_back(static_cast<deskmate::protocol::MessageType>(header.value().type));
  }
  return types;
}

CapabilitySet host_capabilities() {
  return CapabilitySet{
      {VideoEncoder::Hevc, VideoEncoder::H264},
      {PixelFormat::P010, PixelFormat::Nv12},
      {{2560, 1600, 60}, {1920, 1200, 60}},
  };
}

CapabilitySet device_capabilities() {
  return CapabilitySet{
      {VideoEncoder::H264},
      {PixelFormat::Nv12},
      {{1920, 1200, 60}},
  };
}

void test_handshake_selects_capability_intersection_and_accepts_first_idr() {
  ManualClock clock;
  BytePipe host_to_device(3, 2);
  BytePipe device_to_host(5, 4);
  DirectedIo host_io(device_to_host, host_to_device);
  DirectedIo device_io(host_to_device, device_to_host);

  HostSessionConfig host_config;
  host_config.capabilities = host_capabilities();
  host_config.video_stream_id = 0x11223344;
  DeviceSessionConfig device_config;
  device_config.session_nonce = 0xAABBCCDD;
  device_config.capabilities = device_capabilities();

  HostSession host(host_io, clock, host_config);
  DeviceSession device(device_io, clock, device_config);
  host.start();
  device.start();
  CHECK_EQ(static_cast<int>(host.state()), static_cast<int>(SessionState::AwaitingClientHello));
  CHECK_EQ(static_cast<int>(device.state()), static_cast<int>(SessionState::AwaitingPrepare));

  device.pump();
  host.pump();
  CHECK_EQ(static_cast<int>(host.state()), static_cast<int>(SessionState::AwaitingReady));
  device.pump();
  CHECK_EQ(static_cast<int>(device.state()), static_cast<int>(SessionState::AwaitingCommit));
  host.pump();
  CHECK_EQ(static_cast<int>(host.state()), static_cast<int>(SessionState::AwaitingCommitted));
  device.pump();
  CHECK_EQ(static_cast<int>(device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));
  host.pump();
  CHECK_EQ(static_cast<int>(host.state()), static_cast<int>(SessionState::Streaming));

  const auto host_controls = decode_controls(host_to_device);
  CHECK_EQ(host_controls.size(), 2U);
  CHECK_EQ(static_cast<int>(host_controls[0].opcode),
           static_cast<int>(ControlOpcode::StreamPrepare));
  CHECK_EQ(static_cast<int>(host_controls[1].opcode),
           static_cast<int>(ControlOpcode::StreamCommit));
  CHECK_EQ(host_controls[1].body,
           std::vector<std::uint8_t>({0x44, 0x33, 0x22, 0x11}));
  const auto prepare_profile =
      deskmate::protocol::decode_stream_profile(host_controls[0].body);
  CHECK(prepare_profile);
  CHECK_EQ(static_cast<int>(prepare_profile.value().encoder),
           static_cast<int>(VideoEncoder::H264));
  CHECK_EQ(static_cast<int>(prepare_profile.value().pixel_format),
           static_cast<int>(PixelFormat::Nv12));
  CHECK_EQ(prepare_profile.value().resolution, (DisplayResolution{1920, 1200, 60}));

  const auto device_controls = decode_controls(device_to_host);
  CHECK_EQ(device_controls.size(), 3U);
  CHECK_EQ(static_cast<int>(device_controls[0].opcode),
           static_cast<int>(ControlOpcode::ClientHello));
  CHECK_EQ(static_cast<int>(device_controls[1].opcode),
           static_cast<int>(ControlOpcode::StreamReady));
  CHECK_EQ(static_cast<int>(device_controls[2].opcode),
           static_cast<int>(ControlOpcode::StreamCommitted));
  CHECK_EQ(device_controls[2].body,
           std::vector<std::uint8_t>({0x44, 0x33, 0x22, 0x11}));

  CHECK_EQ(host.send_video_frame(std::vector<std::uint8_t>{0xDE, 0xAD}, 1234, false),
           VideoSendStatus::IdrRequired);
  CHECK(host_to_device.bytes().size() < 300U);

  CHECK_EQ(host.send_video_frame(std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF},
                                 1234, true),
           VideoSendStatus::Sent);
  host.pump();
  device.pump();
  CHECK_EQ(static_cast<int>(device.state()), static_cast<int>(SessionState::Streaming));
  const auto frames = device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].payload,
           std::vector<std::uint8_t>({0xDE, 0xAD, 0xBE, 0xEF}));
  CHECK_EQ(frames[0].header.seq, 1U);
  CHECK_EQ(frames[0].header.session_nonce, device_config.session_nonce);
  CHECK_EQ(frames[0].header.video_stream_id, host_config.video_stream_id);

  const auto host_stream = host.negotiated_stream();
  const auto device_stream = device.negotiated_stream();
  CHECK(host_stream.has_value());
  CHECK(device_stream.has_value());
  CHECK_EQ(host_stream->config_id, device_stream->config_id);
  CHECK_EQ(host_stream->video_stream_id, host_config.video_stream_id);
  CHECK_EQ(host_stream->profile.encoder, VideoEncoder::H264);
  CHECK_EQ(host_stream->profile.pixel_format, PixelFormat::Nv12);
  CHECK_EQ(host_stream->profile.resolution, (DisplayResolution{1920, 1200, 60}));
}

void test_empty_capability_intersection_fails_both_ends_with_protocol_error() {
  ManualClock clock;
  BytePipe host_to_device;
  BytePipe device_to_host;
  DirectedIo host_io(device_to_host, host_to_device);
  DirectedIo device_io(host_to_device, device_to_host);

  HostSessionConfig host_config;
  host_config.capabilities = CapabilitySet{
      {VideoEncoder::H264},
      {PixelFormat::Nv12},
      {{1920, 1200, 60}},
  };
  host_config.video_stream_id = 0x11111111;
  DeviceSessionConfig device_config;
  device_config.session_nonce = 0x22222222;
  device_config.capabilities = CapabilitySet{
      {VideoEncoder::Hevc},
      {PixelFormat::P010},
      {{2560, 1600, 60}},
  };

  HostSession host(host_io, clock, host_config);
  DeviceSession device(device_io, clock, device_config);
  host.start();
  device.start();
  device.pump();
  host.pump();

  CHECK_EQ(static_cast<int>(host.state()), static_cast<int>(SessionState::Failed));
  CHECK(host.failure().has_value());
  CHECK_EQ(static_cast<int>(*host.failure()),
           static_cast<int>(SessionFailure::NoCommonCapabilities));
  const auto error_controls = decode_controls(host_to_device);
  CHECK_EQ(error_controls.size(), 1U);
  CHECK_EQ(static_cast<int>(error_controls[0].opcode),
           static_cast<int>(ControlOpcode::ProtocolError));
  CHECK_EQ(error_controls[0].body,
           std::vector<std::uint8_t>{static_cast<std::uint8_t>(
               SessionFailure::NoCommonCapabilities)});

  device.pump();
  CHECK_EQ(static_cast<int>(device.state()), static_cast<int>(SessionState::Failed));
  CHECK(device.failure().has_value());
  CHECK_EQ(static_cast<int>(*device.failure()),
           static_cast<int>(SessionFailure::NoCommonCapabilities));
}

void test_protocol_revision_mismatch_fails_both_ends_explicitly() {
  ManualClock clock;
  BytePipe host_to_device;
  BytePipe device_to_host;
  DirectedIo host_io(device_to_host, host_to_device);
  DirectedIo device_io(host_to_device, device_to_host);

  HostSessionConfig host_config;
  host_config.capabilities = host_capabilities();
  host_config.video_stream_id = 0x33333333;
  host_config.protocol_revision = 1;
  DeviceSessionConfig device_config;
  device_config.session_nonce = 0x44444444;
  device_config.capabilities = device_capabilities();
  device_config.protocol_revision = 2;

  HostSession host(host_io, clock, host_config);
  DeviceSession device(device_io, clock, device_config);
  host.start();
  device.start();
  device.pump();
  host.pump();

  CHECK(host.failure().has_value());
  CHECK_EQ(static_cast<int>(*host.failure()),
           static_cast<int>(SessionFailure::UnsupportedProtocolRevision));
  const auto error_controls = decode_controls(host_to_device);
  CHECK_EQ(error_controls.size(), 1U);
  CHECK_EQ(static_cast<int>(error_controls[0].opcode),
           static_cast<int>(ControlOpcode::ProtocolError));
  CHECK_EQ(error_controls[0].body,
           std::vector<std::uint8_t>{static_cast<std::uint8_t>(
               SessionFailure::UnsupportedProtocolRevision)});

  device.pump();
  CHECK(device.failure().has_value());
  CHECK_EQ(static_cast<int>(*device.failure()),
           static_cast<int>(SessionFailure::UnsupportedProtocolRevision));
}

void test_host_cannot_send_new_stream_video_before_committed() {
  ManualClock clock;
  BytePipe host_to_device;
  BytePipe device_to_host;
  DirectedIo host_io(device_to_host, host_to_device);
  DirectedIo device_io(host_to_device, device_to_host);

  HostSessionConfig host_config;
  host_config.capabilities = host_capabilities();
  host_config.video_stream_id = 0x55555555;
  DeviceSessionConfig device_config;
  device_config.session_nonce = 0x66666666;
  device_config.capabilities = device_capabilities();

  HostSession host(host_io, clock, host_config);
  DeviceSession device(device_io, clock, device_config);
  host.start();
  device.start();
  device.pump();
  host.pump();
  CHECK_EQ(static_cast<int>(host.state()), static_cast<int>(SessionState::AwaitingReady));
  CHECK_EQ(host.send_video_frame(std::vector<std::uint8_t>{0x01}, 1, true),
           VideoSendStatus::NotStreaming);

  device.pump();
  host.pump();
  CHECK_EQ(static_cast<int>(host.state()),
           static_cast<int>(SessionState::AwaitingCommitted));
  CHECK_EQ(host.send_video_frame(std::vector<std::uint8_t>{0x02}, 2, true),
           VideoSendStatus::NotStreaming);

  const auto types = decode_message_types(host_to_device);
  CHECK_EQ(std::count(types.begin(), types.end(),
                      deskmate::protocol::MessageType::Video),
           0);
}

struct SessionPair {
  SessionPair(HostSessionConfig host_config, DeviceSessionConfig device_config,
              deskmate::protocol::DeviceBoundary* boundary = nullptr)
      : host_io(device_to_host, host_to_device),
        device_io(host_to_device, device_to_host),
        host(host_io, clock, host_config),
        device(device_io, clock, device_config, boundary) {}

  void start() {
    host.start();
    device.start();
  }

  void pump() {
    device.pump();
    host.pump();
  }

  ManualClock clock;
  BytePipe host_to_device;
  BytePipe device_to_host;
  DirectedIo host_io;
  DirectedIo device_io;
  HostSession host;
  DeviceSession device;
};

HostSessionConfig default_host_config() {
  HostSessionConfig config;
  config.capabilities = host_capabilities();
  config.video_stream_id = 0x77777777;
  return config;
}

DeviceSessionConfig default_device_config() {
  DeviceSessionConfig config;
  config.session_nonce = 0x88888888;
  config.capabilities = device_capabilities();
  return config;
}

void drive_to_host_awaiting_ready(SessionPair& pair) {
  pair.start();
  pair.device.pump();
  pair.host.pump();
}

void drive_to_host_awaiting_committed(SessionPair& pair) {
  drive_to_host_awaiting_ready(pair);
  pair.device.pump();
  pair.host.pump();
}

void drive_to_device_awaiting_commit(SessionPair& pair) {
  drive_to_host_awaiting_ready(pair);
  pair.device.pump();
}

void drive_to_committed(SessionPair& pair) {
  drive_to_host_awaiting_committed(pair);
  pair.device.pump();
  pair.host.pump();
}

class RecordingBoundary final : public DeviceBoundary {
 public:
  BoundaryResult prepare_stream(const StreamProfile& profile,
                                std::uint32_t config_id) override {
    ++prepare_calls;
    prepared_profile = profile;
    prepared_config_id = config_id;
    return prepare_result;
  }

  BoundaryResult commit_stream(const StreamProfile& profile,
                               std::uint32_t config_id,
                               std::uint32_t video_stream_id) override {
    ++commit_calls;
    committed_profile = profile;
    committed_config_id = config_id;
    committed_stream_id = video_stream_id;
    if (commit_result == BoundaryResult::Ok) {
      stream_active = true;
    }
    return commit_result;
  }

  BoundaryResult prepare_result{BoundaryResult::Ok};
  BoundaryResult commit_result{BoundaryResult::Ok};
  int prepare_calls{};
  int commit_calls{};
  bool stream_active{};
  std::optional<StreamProfile> prepared_profile;
  std::optional<std::uint32_t> prepared_config_id;
  std::optional<StreamProfile> committed_profile;
  std::optional<std::uint32_t> committed_config_id;
  std::optional<std::uint32_t> committed_stream_id;
};

void append_packet(BytePipe& pipe, std::uint32_t session_nonce,
                   std::uint32_t video_stream_id, std::uint32_t seq,
                   deskmate::protocol::MessageType type,
                   std::vector<std::uint8_t> payload, std::uint16_t flags = 0,
                   std::uint8_t fragment_index = 0,
                   std::uint8_t fragment_total = 1) {
  deskmate::protocol::Packet packet;
  packet.header.type = static_cast<std::uint8_t>(type);
  packet.header.flags = flags;
  packet.header.fragment_index = fragment_index;
  packet.header.fragment_total = fragment_total;
  packet.header.session_nonce = session_nonce;
  packet.header.video_stream_id = video_stream_id;
  packet.header.seq = seq;
  packet.header.payload_len = static_cast<std::uint32_t>(payload.size());
  packet.payload = std::move(payload);
  const auto encoded = deskmate::protocol::encode_packet(packet);
  if (encoded) {
    pipe.append(encoded.value());
  }
}

void append_control(BytePipe& pipe, std::uint32_t session_nonce,
                    std::uint32_t video_stream_id, std::uint32_t seq,
                    const ControlMessage& message) {
  const auto payload = deskmate::protocol::encode_control_payload(message);
  if (payload) {
    append_packet(pipe, session_nonce, video_stream_id, seq,
                  deskmate::protocol::MessageType::Control, payload.value());
  }
}

void append_video(BytePipe& pipe, std::uint32_t session_nonce,
                  std::uint32_t video_stream_id, std::uint32_t seq,
                  std::vector<std::uint8_t> payload, std::uint16_t flags) {
  deskmate::protocol::VideoFrameToSend frame;
  frame.session_nonce = session_nonce;
  frame.video_stream_id = video_stream_id;
  frame.seq = seq;
  frame.timestamp_us = seq * 100;
  frame.flags = flags;
  frame.payload = std::move(payload);
  const auto fragments = deskmate::protocol::encode_video_fragments(frame, 32);
  if (!fragments) {
    return;
  }
  for (const auto& fragment : fragments.value()) {
    pipe.append(fragment);
  }
}

void append_recovery_video(BytePipe& pipe, std::uint32_t session_nonce,
                           std::uint32_t video_stream_id, std::uint32_t seq,
                           std::vector<std::uint8_t> payload,
                           std::uint32_t recovery_id) {
  deskmate::protocol::VideoFrameToSend frame;
  frame.session_nonce = session_nonce;
  frame.video_stream_id = video_stream_id;
  frame.seq = seq;
  frame.timestamp_us = seq * 100;
  frame.flags =
      deskmate::protocol::kFlagKeyFrame | deskmate::protocol::kFlagRecoveryIdr;
  frame.recovery_id = recovery_id;
  frame.payload = std::move(payload);
  const auto fragments = deskmate::protocol::encode_video_fragments(frame, 32);
  if (!fragments) {
    return;
  }
  for (const auto& fragment : fragments.value()) {
    pipe.append(fragment);
  }
}

void test_duplicate_prepare_is_idempotent_and_non_destructive() {
  RecordingBoundary boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary);
  drive_to_host_awaiting_ready(pair);
  pair.device.pump();
  CHECK_EQ(boundary.prepare_calls, 1);
  CHECK_EQ(boundary.commit_calls, 0);
  CHECK(!boundary.stream_active);
  CHECK(!pair.device.negotiated_stream().has_value());
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingCommit));

  ControlMessage duplicate;
  duplicate.opcode = ControlOpcode::StreamPrepare;
  duplicate.proto_rev = deskmate::protocol::kProtocolRevision;
  duplicate.config_id = 1;
  const auto body = deskmate::protocol::encode_stream_profile(StreamProfile{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}});
  CHECK(body);
  duplicate.body = body.value();
  append_control(pair.host_to_device, 0x88888888, 0, 90, duplicate);

  pair.device.pump();
  CHECK_EQ(boundary.prepare_calls, 1);
  CHECK_EQ(boundary.commit_calls, 0);
  CHECK(!boundary.stream_active);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingCommit));
  const auto controls = decode_controls(pair.device_to_host);
  CHECK_EQ(controls.size(), 3U);
  CHECK_EQ(static_cast<int>(controls[1].opcode),
           static_cast<int>(ControlOpcode::StreamReady));
  CHECK_EQ(static_cast<int>(controls[2].opcode),
           static_cast<int>(ControlOpcode::StreamReady));

  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::AwaitingCommitted));
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));
  CHECK_EQ(boundary.commit_calls, 1);
  CHECK(boundary.stream_active);
  CHECK_EQ(boundary.committed_config_id, 1U);
  CHECK_EQ(boundary.committed_stream_id, 0x77777777U);
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
}

void test_commit_failure_is_explicit_and_never_sends_committed() {
  RecordingBoundary boundary;
  boundary.commit_result = BoundaryResult::Failed;
  SessionPair pair(default_host_config(), default_device_config(), &boundary);
  drive_to_committed(pair);

  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.device.failure()),
           static_cast<int>(SessionFailure::CommitFailed));
  CHECK_EQ(boundary.commit_calls, 1);
  CHECK(!pair.device.negotiated_stream().has_value());
  const auto device_controls = decode_controls(pair.device_to_host);
  CHECK(std::none_of(device_controls.begin(), device_controls.end(),
                     [](const ControlMessage& control) {
                       return control.opcode == ControlOpcode::StreamCommitted;
                     }));
  CHECK_EQ(static_cast<int>(device_controls.back().opcode),
           static_cast<int>(ControlOpcode::ProtocolError));
  CHECK_EQ(device_controls.back().body,
           std::vector<std::uint8_t>{
               static_cast<std::uint8_t>(SessionFailure::CommitFailed)});

  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.host.failure()),
           static_cast<int>(SessionFailure::CommitFailed));
}

void test_display_error_during_prepare_is_explicit_and_terminates() {
  RecordingBoundary boundary;
  boundary.prepare_result = BoundaryResult::DisplayError;
  SessionPair pair(default_host_config(), default_device_config(), &boundary);
  drive_to_host_awaiting_ready(pair);
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.device.failure()),
           static_cast<int>(SessionFailure::DisplayError));
  CHECK_EQ(boundary.prepare_calls, 1);
  CHECK_EQ(boundary.commit_calls, 0);
  CHECK(!boundary.stream_active);
  CHECK(!pair.device.negotiated_stream().has_value());

  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.host.failure()),
           static_cast<int>(SessionFailure::DisplayError));
}

void test_device_ignores_p_frames_until_matching_stream_keyframe() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_committed(pair);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));

  append_video(pair.host_to_device, 0x88888888, 0x77777777, 1, {0x10, 0x11}, 0);
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));
  CHECK(pair.device.take_video_frames().empty());

  pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs - 1);
  append_video(pair.host_to_device, 0x88888888, 0x77777777, 2,
               {0x20, 0x21, 0x22}, deskmate::protocol::kFlagKeyFrame);
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 2U);
  CHECK_EQ(frames[0].payload,
           std::vector<std::uint8_t>({0x20, 0x21, 0x22}));
}

void test_device_discards_stale_nonce_and_stream_video() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_committed(pair);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x30}, 1, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(pair.device.take_video_frames().size(), 1U);

  append_video(pair.host_to_device, 0x99999999, 0x77777777, 2, {0x40},
               deskmate::protocol::kFlagKeyFrame);
  append_video(pair.host_to_device, 0x88888888, 0xAAAAAAAA, 3, {0x50},
               deskmate::protocol::kFlagKeyFrame);
  append_video(pair.host_to_device, 0x88888888, 0x77777777, 2, {0x60}, 0);
  pair.device.pump();

  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 2U);
  CHECK_EQ(frames[0].payload, std::vector<std::uint8_t>({0x60}));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
}

void test_device_gates_old_recovery_epoch_and_retries_same_id() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_committed(pair);

  CHECK_EQ(pair.host.send_video_frame(
               std::vector<std::uint8_t>{0x10, 0x11}, 1, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 1U);
  CHECK(pair.device.last_good_frame().has_value());
  CHECK_EQ(pair.device.last_good_frame()->header.seq, 1U);
  CHECK(!pair.device.recovery_gated());

  const auto controls_before =
      decode_controls(pair.device_to_host).size();
  append_video(pair.host_to_device, 0x88888888, 0x77777777, 3, {0x30}, 0);
  pair.device.pump();
  CHECK(pair.device.take_video_frames().empty());
  CHECK(pair.device.recovery_gated());
  CHECK(pair.device.recovery_id().has_value());
  CHECK_EQ(*pair.device.recovery_id(), 1U);
  CHECK_EQ(pair.device.last_good_frame()->header.seq, 1U);

  auto controls = decode_controls(pair.device_to_host);
  CHECK_EQ(controls.size(), controls_before + 1U);
  CHECK_EQ(static_cast<int>(controls.back().opcode),
           static_cast<int>(ControlOpcode::RequestIdr));
  CHECK_EQ(controls.back().body,
           std::vector<std::uint8_t>({0x01, 0x00, 0x00, 0x00}));

  pair.clock.advance_ns(deskmate::protocol::kDefaultRecoveryRetryNs - 1);
  pair.device.pump();
  CHECK_EQ(decode_controls(pair.device_to_host).size(),
           controls_before + 1U);
  pair.clock.advance_ns(1);
  pair.device.pump();
  controls = decode_controls(pair.device_to_host);
  CHECK_EQ(controls.size(), controls_before + 2U);
  CHECK_EQ(static_cast<int>(controls.back().opcode),
           static_cast<int>(ControlOpcode::RequestIdr));
  CHECK_EQ(controls.back().body,
           std::vector<std::uint8_t>({0x01, 0x00, 0x00, 0x00}));

  pair.host.pump();
  CHECK(pair.host.pending_idr_request().has_value());
  CHECK_EQ(pair.host.pending_idr_request()->recovery_id, 1U);

  append_recovery_video(pair.host_to_device, 0x88888888, 0x77777777, 4,
                        {0x40}, 2);
  pair.device.pump();
  CHECK(pair.device.take_video_frames().empty());
  CHECK(pair.device.recovery_gated());
  CHECK_EQ(pair.device.last_good_frame()->header.seq, 1U);

  append_recovery_video(pair.host_to_device, 0x88888888, 0x77777777, 5,
                        {0x50, 0x51}, 1);
  pair.device.pump();
  frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 5U);
  CHECK_EQ(frames[0].payload,
           std::vector<std::uint8_t>({0x50, 0x51}));
  CHECK(!pair.device.recovery_gated());
  CHECK(pair.device.last_good_frame().has_value());
  CHECK_EQ(pair.device.last_good_frame()->header.seq, 5U);

  append_video(pair.host_to_device, 0x88888888, 0x77777777, 7, {0x70}, 0);
  pair.device.pump();
  CHECK(pair.device.recovery_gated());
  CHECK(pair.device.recovery_id().has_value());
  CHECK_EQ(*pair.device.recovery_id(), 2U);
  CHECK_EQ(pair.device.last_good_frame()->header.seq, 5U);
  controls = decode_controls(pair.device_to_host);
  CHECK_EQ(static_cast<int>(controls.back().opcode),
           static_cast<int>(ControlOpcode::RequestIdr));
  CHECK_EQ(controls.back().body,
           std::vector<std::uint8_t>({0x02, 0x00, 0x00, 0x00}));

  append_recovery_video(pair.host_to_device, 0x88888888, 0x77777777, 8,
                        {0x80}, 2);
  pair.device.pump();
  frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 8U);
  CHECK(!pair.device.recovery_gated());
}

void test_host_recovery_idr_resumes_gated_device() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_committed(pair);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x10}, 1, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(pair.device.take_video_frames().size(), 1U);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x20}, 2, false),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.host_to_device.clear();
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x30}, 3, false),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK(pair.device.recovery_gated());
  CHECK(pair.device.take_video_frames().empty());

  pair.host.pump();
  CHECK(pair.host.pending_idr_request().has_value());
  CHECK_EQ(pair.host.pending_idr_request()->recovery_id, 1U);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x40, 0x41}, 4,
                                      true),
           VideoSendStatus::Sent);
  CHECK(!pair.host.pending_idr_request().has_value());
  pair.host.pump();
  pair.device.pump();
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 4U);
  CHECK(frames[0].recovery_id.has_value());
  CHECK_EQ(*frames[0].recovery_id, 1U);
  CHECK_EQ(frames[0].payload,
           std::vector<std::uint8_t>({0x40, 0x41}));
  CHECK(!pair.device.recovery_gated());
  CHECK_EQ(pair.device.last_good_frame()->header.seq, 4U);
}

void test_recovery_idr_in_same_batch_suppresses_idle_request() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_committed(pair);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x10}, 1, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(pair.device.take_video_frames().size(), 1U);
  const auto controls_before = decode_controls(pair.device_to_host).size();

  append_video(pair.host_to_device, 0x88888888, 0x77777777, 3, {0x30}, 0);
  append_recovery_video(pair.host_to_device, 0x88888888, 0x77777777, 4,
                        {0x40}, 1);
  pair.device.pump();

  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 4U);
  CHECK(!pair.device.recovery_gated());
  CHECK_EQ(decode_controls(pair.device_to_host).size(), controls_before);
}

void test_host_congestion_drops_unstarted_gop_and_recovers_with_new_idr() {
  HostSessionConfig host_config = default_host_config();
  host_config.limits.video_fragment_payload = 2;
  host_config.scheduler.max_video_urbs = 1;
  host_config.scheduler.max_pending_video_bytes = 80;
  SessionPair pair(host_config, default_device_config());
  drive_to_committed(pair);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x10}, 1, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(pair.device.take_video_frames().size(), 1U);

  CHECK_EQ(pair.host.send_video_frame(
               std::vector<std::uint8_t>{0x20, 0x21, 0x22, 0x23}, 2, false),
           VideoSendStatus::Sent);
  pair.host.pump();
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x30, 0x31}, 3,
                                      false),
           VideoSendStatus::Sent);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x40, 0x41}, 4,
                                      false),
           VideoSendStatus::IdrRequired);
  CHECK(pair.host.pending_idr_request().has_value());
  CHECK_EQ(pair.host.pending_idr_request()->recovery_id, 1U);

  pair.host.pump();
  pair.device.pump();
  auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 2U);
  CHECK_EQ(pair.device.last_good_frame()->header.seq, 2U);
  CHECK(!pair.device.recovery_gated());

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x50, 0x51}, 5,
                                      true),
           VideoSendStatus::Sent);
  CHECK(!pair.host.pending_idr_request().has_value());
  pair.host.pump();
  pair.host.pump();
  pair.host.pump();
  pair.device.pump();
  frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 5U);
  CHECK(frames[0].recovery_id.has_value());
  CHECK_EQ(*frames[0].recovery_id, 1U);
  CHECK(!pair.device.recovery_gated());
  CHECK_EQ(pair.device.last_good_frame()->header.seq, 5U);
}

void test_host_oldest_unsent_frame_age_triggers_recovery() {
  HostSessionConfig host_config = default_host_config();
  host_config.limits.video_fragment_payload = 2;
  host_config.scheduler.max_video_urbs = 1;
  host_config.scheduler.max_pending_video_bytes = 1'000'000;
  host_config.scheduler.max_oldest_unsent_frame_age_ns = 10'000'000;
  SessionPair pair(host_config, default_device_config());
  drive_to_committed(pair);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x10}, 1, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(pair.device.take_video_frames().size(), 1U);

  CHECK_EQ(pair.host.send_video_frame(
               std::vector<std::uint8_t>{0x20, 0x21, 0x22, 0x23}, 2, false),
           VideoSendStatus::Sent);
  pair.host.pump();
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x30, 0x31}, 3,
                                      false),
           VideoSendStatus::Sent);
  CHECK(!pair.host.pending_idr_request().has_value());

  pair.clock.advance_ns(10'000'000);
  pair.host.pump();
  CHECK(pair.host.pending_idr_request().has_value());
  CHECK_EQ(pair.host.pending_idr_request()->recovery_id, 1U);
  pair.device.pump();
  auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 2U);
  CHECK(!pair.device.recovery_gated());

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x40, 0x41}, 4,
                                      true),
           VideoSendStatus::Sent);
  CHECK(!pair.host.pending_idr_request().has_value());
  pair.host.pump();
  pair.host.pump();
  pair.host.pump();
  pair.device.pump();
  frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 4U);
  CHECK(frames[0].recovery_id.has_value());
  CHECK_EQ(*frames[0].recovery_id, 1U);
  CHECK(!pair.device.recovery_gated());
  CHECK_EQ(pair.device.last_good_frame()->header.seq, 4U);
}

void test_device_rejects_prepare_revision_mismatch() {
  ManualClock clock;
  BytePipe host_to_device;
  BytePipe device_to_host;
  DirectedIo device_io(host_to_device, device_to_host);
  DeviceSessionConfig device_config = default_device_config();
  DeviceSession device(device_io, clock, device_config);
  device.start();

  ControlMessage prepare;
  prepare.opcode = ControlOpcode::StreamPrepare;
  prepare.proto_rev = deskmate::protocol::kProtocolRevision + 1;
  prepare.config_id = 1;
  const auto body = deskmate::protocol::encode_stream_profile(StreamProfile{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}});
  CHECK(body);
  prepare.body = body.value();
  append_control(host_to_device, device_config.session_nonce, 0, 1, prepare);
  device.pump();

  CHECK_EQ(static_cast<int>(device.state()), static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*device.failure()),
           static_cast<int>(SessionFailure::UnsupportedProtocolRevision));
  const auto controls = decode_controls(device_to_host);
  CHECK(!controls.empty());
  CHECK_EQ(static_cast<int>(controls.back().opcode),
           static_cast<int>(ControlOpcode::ProtocolError));
  CHECK_EQ(controls.back().body,
           std::vector<std::uint8_t>{static_cast<std::uint8_t>(
               SessionFailure::UnsupportedProtocolRevision)});
}

void test_different_config_id_does_not_repeat_prepare_or_switch_stream() {
  RecordingBoundary boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary);
  drive_to_host_awaiting_ready(pair);
  pair.device.pump();
  CHECK_EQ(boundary.prepare_calls, 1);

  ControlMessage replacement = ControlMessage{
      ControlOpcode::StreamPrepare,
      deskmate::protocol::kProtocolRevision,
      2,
      {},
  };
  const auto body = deskmate::protocol::encode_stream_profile(StreamProfile{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}});
  CHECK(body);
  replacement.body = body.value();
  append_control(pair.host_to_device, 0x88888888, 0, 91, replacement);
  pair.device.pump();

  CHECK_EQ(boundary.prepare_calls, 1);
  CHECK_EQ(boundary.commit_calls, 0);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.device.failure()),
           static_cast<int>(SessionFailure::UnexpectedMessage));
  CHECK(!pair.device.negotiated_stream().has_value());
}

void test_display_error_during_commit_is_explicit() {
  RecordingBoundary boundary;
  boundary.commit_result = BoundaryResult::DisplayError;
  SessionPair pair(default_host_config(), default_device_config(), &boundary);
  drive_to_committed(pair);

  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.device.failure()),
           static_cast<int>(SessionFailure::DisplayError));
  CHECK_EQ(boundary.commit_calls, 1);
  CHECK(!boundary.stream_active);
  const auto controls = decode_controls(pair.device_to_host);
  CHECK(std::none_of(controls.begin(), controls.end(),
                     [](const ControlMessage& control) {
                       return control.opcode == ControlOpcode::StreamCommitted;
                     }));
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.host.failure()),
           static_cast<int>(SessionFailure::DisplayError));
}

void test_session_payload_helpers_round_trip_and_reject_bad_encodings() {
  const CapabilitySet capabilities = host_capabilities();
  const auto encoded = deskmate::protocol::encode_capabilities(capabilities);
  CHECK(encoded);
  const auto device_encoded =
      deskmate::protocol::encode_capabilities(device_capabilities());
  CHECK(device_encoded);
  CHECK_EQ(device_encoded.value(),
           std::vector<std::uint8_t>({1, 1, 1, 1, 1, 1, 0x80, 0x07, 0xB0, 0x04,
                                      0x3C, 0x00}));
  const auto decoded = deskmate::protocol::decode_capabilities(encoded.value());
  CHECK(decoded);
  CHECK_EQ(decoded.value().encoders, capabilities.encoders);
  CHECK_EQ(decoded.value().pixel_formats, capabilities.pixel_formats);
  CHECK_EQ(decoded.value().resolutions, capabilities.resolutions);

  CHECK(!deskmate::protocol::decode_capabilities(
      std::span<const std::uint8_t>(encoded.value()).first(3)));
  std::vector<std::uint8_t> too_many = encoded.value();
  too_many[1] = 9;
  CHECK(!deskmate::protocol::decode_capabilities(too_many));
  std::vector<std::uint8_t> wrong_version = encoded.value();
  wrong_version[0] = 2;
  CHECK(!deskmate::protocol::decode_capabilities(wrong_version));

  const StreamProfile profile{
      VideoEncoder::Hevc, PixelFormat::P010, DisplayResolution{2560, 1600, 60}};
  const auto profile_body = deskmate::protocol::encode_stream_profile(profile);
  CHECK(profile_body);
  CHECK_EQ(profile_body.value(),
           std::vector<std::uint8_t>({1, 2, 2, 0x00, 0x0A, 0x40, 0x06, 0x3C, 0x00}));
  const auto profile_decoded =
      deskmate::protocol::decode_stream_profile(profile_body.value());
  CHECK(profile_decoded);
  CHECK(profile_decoded.value() == profile);
  CHECK(!deskmate::protocol::decode_stream_profile(
      std::span<const std::uint8_t>(profile_body.value()).first(8)));
  std::vector<std::uint8_t> zero_resolution = profile_body.value();
  zero_resolution[3] = 0;
  zero_resolution[4] = 0;
  CHECK(!deskmate::protocol::decode_stream_profile(zero_resolution));
}

void test_each_handshake_stage_times_out_after_two_seconds() {
  {
    SessionPair pair(default_host_config(), default_device_config());
    pair.host.start();
    pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
    pair.host.pump();
    CHECK_EQ(static_cast<int>(pair.host.state()),
             static_cast<int>(SessionState::Failed));
    CHECK_EQ(static_cast<int>(*pair.host.failure()),
             static_cast<int>(SessionFailure::HelloTimeout));
    CHECK_EQ(decode_controls(pair.host_to_device).size(), 0U);
  }
  {
    SessionPair pair(default_host_config(), default_device_config());
    drive_to_host_awaiting_ready(pair);
    CHECK_EQ(static_cast<int>(pair.host.state()),
             static_cast<int>(SessionState::AwaitingReady));
    pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
    pair.host.pump();
    CHECK_EQ(static_cast<int>(*pair.host.failure()),
             static_cast<int>(SessionFailure::ReadyTimeout));
    const auto errors = decode_controls(pair.host_to_device);
    CHECK(!errors.empty());
    CHECK_EQ(static_cast<int>(errors.back().opcode),
             static_cast<int>(ControlOpcode::ProtocolError));
  }
  {
    SessionPair pair(default_host_config(), default_device_config());
    drive_to_host_awaiting_committed(pair);
    CHECK_EQ(static_cast<int>(pair.host.state()),
             static_cast<int>(SessionState::AwaitingCommitted));
    pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
    pair.host.pump();
    CHECK_EQ(static_cast<int>(*pair.host.failure()),
             static_cast<int>(SessionFailure::CommittedTimeout));
  }
  {
    SessionPair pair(default_host_config(), default_device_config());
    pair.start();
    pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
    pair.device.pump();
    CHECK_EQ(static_cast<int>(pair.device.state()),
             static_cast<int>(SessionState::Failed));
    CHECK_EQ(static_cast<int>(*pair.device.failure()),
             static_cast<int>(SessionFailure::PrepareTimeout));
    const auto errors = decode_controls(pair.device_to_host);
    CHECK(!errors.empty());
    CHECK_EQ(static_cast<int>(errors.back().opcode),
             static_cast<int>(ControlOpcode::ProtocolError));
  }
  {
    SessionPair pair(default_host_config(), default_device_config());
    drive_to_device_awaiting_commit(pair);
    CHECK_EQ(static_cast<int>(pair.device.state()),
             static_cast<int>(SessionState::AwaitingCommit));
    pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
    pair.device.pump();
    CHECK_EQ(static_cast<int>(*pair.device.failure()),
             static_cast<int>(SessionFailure::CommitTimeout));
  }
  {
    SessionPair pair(default_host_config(), default_device_config());
    drive_to_committed(pair);
    CHECK_EQ(static_cast<int>(pair.device.state()),
             static_cast<int>(SessionState::AwaitingFirstIdr));
    CHECK_EQ(static_cast<int>(pair.host.state()),
             static_cast<int>(SessionState::Streaming));
    pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
    pair.device.pump();
    CHECK_EQ(static_cast<int>(pair.device.state()),
             static_cast<int>(SessionState::Failed));
    CHECK_EQ(static_cast<int>(*pair.device.failure()),
             static_cast<int>(SessionFailure::InitialIdrTimeout));
    pair.host.pump();
    CHECK_EQ(static_cast<int>(pair.host.state()),
             static_cast<int>(SessionState::Failed));
    CHECK_EQ(static_cast<int>(*pair.host.failure()),
             static_cast<int>(SessionFailure::InitialIdrTimeout));
  }
}

struct TestCase {
  const char* name;
  void (*run)();
};

}  // namespace

int main() {
  const std::vector<TestCase> tests{
      {"handshake selects capability intersection and accepts first IDR",
       test_handshake_selects_capability_intersection_and_accepts_first_idr},
      {"empty capability intersection fails both ends with protocol error",
       test_empty_capability_intersection_fails_both_ends_with_protocol_error},
      {"protocol revision mismatch fails both ends explicitly",
       test_protocol_revision_mismatch_fails_both_ends_explicitly},
      {"host cannot send new stream video before committed",
       test_host_cannot_send_new_stream_video_before_committed},
      {"each handshake stage times out after two seconds",
       test_each_handshake_stage_times_out_after_two_seconds},
      {"duplicate prepare is idempotent and non-destructive",
       test_duplicate_prepare_is_idempotent_and_non_destructive},
      {"commit failure is explicit and never sends committed",
       test_commit_failure_is_explicit_and_never_sends_committed},
      {"display error during prepare is explicit and terminates",
       test_display_error_during_prepare_is_explicit_and_terminates},
      {"device ignores P frames until matching stream keyframe",
       test_device_ignores_p_frames_until_matching_stream_keyframe},
      {"device discards stale nonce and stream video",
       test_device_discards_stale_nonce_and_stream_video},
      {"device gates old recovery epoch and retries same id",
       test_device_gates_old_recovery_epoch_and_retries_same_id},
      {"host recovery IDR resumes gated device",
       test_host_recovery_idr_resumes_gated_device},
      {"recovery IDR in same batch suppresses idle request",
       test_recovery_idr_in_same_batch_suppresses_idle_request},
      {"host congestion drops unstarted GOP and recovers with new IDR",
       test_host_congestion_drops_unstarted_gop_and_recovers_with_new_idr},
      {"host oldest unsent frame age triggers recovery",
       test_host_oldest_unsent_frame_age_triggers_recovery},
      {"device rejects prepare revision mismatch",
       test_device_rejects_prepare_revision_mismatch},
      {"different config id does not repeat prepare or switch stream",
       test_different_config_id_does_not_repeat_prepare_or_switch_stream},
      {"display error during commit is explicit",
       test_display_error_during_commit_is_explicit},
      {"session payload helpers round trip and reject bad encodings",
       test_session_payload_helpers_round_trip_and_reject_bad_encodings},
  };
  for (const auto& test : tests) {
    const int before = failures;
    test.run();
    std::cout << (failures == before ? "PASS " : "FAIL ") << test.name << '\n';
  }
  if (failures != 0) {
    std::cerr << failures << " test assertion(s) failed\n";
    return 1;
  }
  std::cout << "All session tests passed\n";
  return 0;
}
