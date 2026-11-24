#include "deskmate/byte_io.hpp"
#include "deskmate/clock.hpp"
#include "deskmate/protocol.hpp"
#include "deskmate/session.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
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
using deskmate::protocol::ClockEstimate;
using deskmate::protocol::ClockMessage;
using deskmate::protocol::ClockMessageKind;
using deskmate::protocol::ClockSample;
using deskmate::protocol::ClockSyncEstimator;
using deskmate::protocol::ControlMessage;
using deskmate::protocol::ControlOpcode;
using deskmate::protocol::DeviceSession;
using deskmate::protocol::DeviceSessionConfig;
using deskmate::protocol::DisplayResolution;
using deskmate::protocol::BoundaryResult;
using deskmate::protocol::BorrowedAccessoryFd;
using deskmate::protocol::DeviceBoundary;
using deskmate::protocol::Header;
using deskmate::protocol::HostSession;
using deskmate::protocol::HostSessionConfig;
using deskmate::protocol::HostEvent;
using deskmate::protocol::HostInputState;
using deskmate::protocol::HostVideoState;
using deskmate::protocol::InputEvent;
using deskmate::protocol::InputKind;
using deskmate::protocol::SurfaceLease;
using deskmate::protocol::SurfaceOwner;
using deskmate::protocol::SurfaceReferenceOps;
using deskmate::protocol::IoResult;
using deskmate::protocol::IoStatus;
using deskmate::protocol::ManualClock;
using deskmate::protocol::NegotiatedStream;
using deskmate::protocol::PixelFormat;
using deskmate::protocol::ReconfigurationPhase;
using deskmate::protocol::StreamProfile;
using deskmate::protocol::SessionFailure;
using deskmate::protocol::SessionState;
using deskmate::protocol::SessionCommandStatus;
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
    if (fail_writes) {
      return {IoStatus::Error, 0};
    }
    if (writes_until_block == 0) {
      return {IoStatus::WouldBlock, 0};
    }
    if (writes_until_block != std::numeric_limits<std::size_t>::max()) {
      --writes_until_block;
    }
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
  void set_write_chunk_size(std::size_t size) { write_chunk_size_ = size; }

 private:
  std::vector<std::uint8_t> bytes_;
  std::size_t read_offset_{};
  std::size_t read_chunk_size_;
  std::size_t write_chunk_size_;

 public:
  bool fail_writes{};
  std::size_t writes_until_block{std::numeric_limits<std::size_t>::max()};
};

class DirectedIo final : public deskmate::protocol::ByteIo {
 public:
  DirectedIo(BytePipe& inbound, BytePipe& outbound,
             std::vector<std::string>* events = nullptr,
             std::string label = {})
      : inbound_(inbound),
        outbound_(outbound),
        events_(events),
        label_(std::move(label)) {}

  IoResult read(std::span<std::uint8_t> destination) override {
    return inbound_.read(destination);
  }
  IoResult write(std::span<const std::uint8_t> source) override {
    return outbound_.write(source);
  }
  void cancel() override {
    if (events_ != nullptr) {
      events_->push_back(label_ + ".io.cancel");
    }
  }

 private:
  BytePipe& inbound_;
  BytePipe& outbound_;
  std::vector<std::string>* events_;
  std::string label_;
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

std::size_t count_video_packets_for_stream(const BytePipe& pipe,
                                           std::uint32_t video_stream_id) {
  std::size_t count = 0;
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
    if (header.value().type ==
            static_cast<std::uint8_t>(
                deskmate::protocol::MessageType::Video) &&
        header.value().video_stream_id == video_stream_id) {
      ++count;
    }
  }
  return count;
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
              deskmate::protocol::DeviceBoundary* boundary = nullptr,
              deskmate::protocol::HostBoundary* host_boundary = nullptr,
              SurfaceOwner* surface = nullptr,
              std::vector<std::string>* events = nullptr)
      : host_io(device_to_host, host_to_device, events, "host"),
        device_io(host_to_device, device_to_host, events, "device"),
        host(host_io, clock, host_config, host_boundary),
        device(device_io, clock, device_config, boundary, surface) {}

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

  void discard_prepared_stream(std::uint32_t config_id) override {
    ++discard_calls;
    discarded_config_id = config_id;
    record("discard");
  }

  void rollback_stream(std::uint32_t config_id,
                       std::uint32_t video_stream_id) override {
    ++rollback_calls;
    rolled_back_config_id = config_id;
    rolled_back_stream_id = video_stream_id;
    record("rollback");
  }

  void release_stream(std::uint32_t config_id,
                      std::uint32_t video_stream_id) override {
    ++release_calls;
    released_config_id = config_id;
    released_stream_id = video_stream_id;
    record("release");
  }

  void drain_callbacks() override {
    ++drain_calls;
    record("drain");
  }

  void record(std::string event) {
    if (events != nullptr) {
      events->push_back("device." + std::move(event));
    }
  }

  BoundaryResult prepare_result{BoundaryResult::Ok};
  BoundaryResult commit_result{BoundaryResult::Ok};
  int prepare_calls{};
  int commit_calls{};
  int discard_calls{};
  int rollback_calls{};
  int release_calls{};
  int drain_calls{};
  bool stream_active{};
  std::optional<StreamProfile> prepared_profile;
  std::optional<std::uint32_t> prepared_config_id;
  std::optional<StreamProfile> committed_profile;
  std::optional<std::uint32_t> committed_config_id;
  std::optional<std::uint32_t> committed_stream_id;
  std::optional<std::uint32_t> discarded_config_id;
  std::optional<std::uint32_t> rolled_back_config_id;
  std::optional<std::uint32_t> rolled_back_stream_id;
  std::optional<std::uint32_t> released_config_id;
  std::optional<std::uint32_t> released_stream_id;
  std::vector<std::string>* events{};
};

class RecordingHostBoundary final : public deskmate::protocol::HostBoundary {
 public:
  void set_capture_encoding_intent(bool enabled) override {
    capture_encoding_intent = enabled;
    record(enabled ? "capture_on" : "capture_off");
  }

  void set_input_injection_intent(bool enabled) override {
    input_injection_intent = enabled;
    record(enabled ? "input_on" : "input_off");
  }

  void destroy_extended_display() override {
    ++display_destroy_calls;
    record("destroy");
  }

  void drain_callbacks() override {
    ++drain_calls;
    record("drain");
  }

  void record(std::string event) {
    if (events != nullptr) {
      events->push_back("host." + std::move(event));
    }
  }

  bool capture_encoding_intent{};
  bool input_injection_intent{};
  int display_destroy_calls{};
  int drain_calls{};
  std::vector<std::string>* events{};
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

std::vector<std::uint8_t> commit_ack_body(std::uint32_t video_stream_id,
                                          bool final_ack) {
  std::vector<std::uint8_t> body{
      static_cast<std::uint8_t>(video_stream_id & 0xFFU),
      static_cast<std::uint8_t>((video_stream_id >> 8U) & 0xFFU),
      static_cast<std::uint8_t>((video_stream_id >> 16U) & 0xFFU),
      static_cast<std::uint8_t>((video_stream_id >> 24U) & 0xFFU)};
  if (final_ack) {
    body.push_back(2);
  }
  return body;
}

void append_commit_ack(BytePipe& pipe, std::uint32_t session_nonce,
                       std::uint32_t config_id, std::uint32_t video_stream_id,
                       std::uint32_t seq, bool final_ack) {
  ControlMessage ack;
  ack.opcode = ControlOpcode::StreamCommitted;
  ack.config_id = config_id;
  ack.body = commit_ack_body(video_stream_id, final_ack);
  append_control(pipe, session_nonce, video_stream_id, seq, ack);
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
  pair.host.pump();
  CHECK(!pair.host.pending_idr_request().has_value());
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
  pair.host.pump();
  CHECK(!pair.host.pending_idr_request().has_value());
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
  pair.host.pump();
  CHECK(!pair.host.pending_idr_request().has_value());
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

void test_multifragment_congestion_idr_delivered_one_fragment_per_pump_does_not_loop() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_committed(pair);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x10}, 1, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(pair.device.take_video_frames().size(), 1U);

  const auto controls_before = decode_controls(pair.device_to_host).size();

  // Build a 4-fragment Host congestion recovery IDR with a seq gap (seq=5 after
  // seq=1) and a Host-side recovery_id (3) that differs from Device's next id (1).
  deskmate::protocol::VideoFrameToSend idr;
  idr.session_nonce = 0x88888888;
  idr.video_stream_id = 0x77777777;
  idr.seq = 5;
  idr.timestamp_us = 5000;
  idr.flags = deskmate::protocol::kFlagKeyFrame |
              deskmate::protocol::kFlagRecoveryIdr;
  idr.recovery_id = 3;
  idr.payload = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8};
  auto fragments = deskmate::protocol::encode_video_fragments(idr, 4);
  CHECK(fragments);
  CHECK(fragments.value().size() >= 3U);

  // Deliver exactly one fragment per device.pump() (matching FdByteIo's 1-permit
  // poll loop on the tablet). Fragment 0 must NOT emit a premature RequestIdr.
  for (std::size_t index = 0; index + 1 < fragments.value().size(); ++index) {
    pair.host_to_device.append(fragments.value()[index]);
    pair.device.pump();
    CHECK(pair.device.take_video_frames().empty());
    CHECK_EQ(decode_controls(pair.device_to_host).size(), controls_before);
  }

  // Final fragment completes the unsolicited recovery IDR, ungates cleanly, and
  // still emits zero RequestIdr controls.
  pair.host_to_device.append(fragments.value().back());
  pair.device.pump();
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 5U);
  CHECK_EQ(frames[0].payload, idr.payload);
  CHECK(!pair.device.recovery_gated());
  CHECK_EQ(decode_controls(pair.device_to_host).size(), controls_before);

  // Subsequent P-frame continues seamlessly without recovery.
  append_video(pair.host_to_device, 0x88888888, 0x77777777, 6, {0xB1, 0xB2}, 0);
  pair.device.pump();
  const auto next_frames = pair.device.take_video_frames();
  CHECK_EQ(next_frames.size(), 1U);
  CHECK_EQ(next_frames[0].header.seq, 6U);
  CHECK(!pair.device.recovery_gated());
  CHECK_EQ(decode_controls(pair.device_to_host).size(), controls_before);

  // If a multi-fragment unsolicited recovery IDR is interrupted mid-frame by a
  // newer frame, the device MUST emit RequestIdr (strictly above recovery_id 3).
  deskmate::protocol::VideoFrameToSend broken_idr = idr;
  broken_idr.seq = 10;
  broken_idr.recovery_id = 4;
  auto broken_fragments = deskmate::protocol::encode_video_fragments(broken_idr, 4);
  CHECK(broken_fragments);
  pair.host_to_device.append(broken_fragments.value()[0]);
  pair.device.pump();
  CHECK_EQ(decode_controls(pair.device_to_host).size(), controls_before);
  append_video(pair.host_to_device, 0x88888888, 0x77777777, 11, {0xC1}, 0);
  pair.device.pump();
  CHECK(pair.device.recovery_gated());
  const auto controls_after = decode_controls(pair.device_to_host);
  CHECK_EQ(controls_after.size(), controls_before + 1U);
  CHECK_EQ(static_cast<int>(controls_after.back().opcode),
           static_cast<int>(ControlOpcode::RequestIdr));
  CHECK(pair.device.recovery_id().has_value());
  CHECK(*pair.device.recovery_id() > 3U);
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

void test_pause_acks_immediately_stops_host_intent_and_resume_renegotiates() {
  RecordingBoundary boundary;
  RecordingHostBoundary host_boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary,
                   &host_boundary);
  drive_to_committed(pair);
  CHECK(host_boundary.capture_encoding_intent);
  CHECK(host_boundary.input_injection_intent);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x10}, 1, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.device.take_video_frames().size(), 1U);

  CHECK_EQ(pair.host.pause(), SessionCommandStatus::Ok);
  CHECK_EQ(static_cast<int>(pair.host.state()), static_cast<int>(SessionState::Paused));
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x11}, 2, false),
           VideoSendStatus::Paused);

  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()), static_cast<int>(SessionState::Paused));
  const auto device_controls = decode_controls(pair.device_to_host);
  CHECK(!device_controls.empty());
  CHECK_EQ(static_cast<int>(device_controls.back().opcode),
           static_cast<int>(ControlOpcode::PauseAck));
  pair.host.pump();
  CHECK(pair.host.pause_acknowledged());

  const StreamProfile resumed_profile{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  CHECK_EQ(pair.host.resume(resumed_profile, 2, 0x99999999),
           SessionCommandStatus::Ok);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();

  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
  CHECK(pair.host.negotiated_stream().has_value());
  CHECK_EQ(pair.host.negotiated_stream()->config_id, 1U);

  const auto host_controls = decode_controls(pair.host_to_device);
  CHECK(std::any_of(host_controls.begin(), host_controls.end(),
                    [](const ControlMessage& control) {
                      return control.opcode == ControlOpcode::Resume;
                    }));
  CHECK(std::any_of(host_controls.begin(), host_controls.end(),
                    [](const ControlMessage& control) {
                      return control.opcode == ControlOpcode::StreamPrepare &&
                             control.config_id == 2;
                    }));
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x12}, 3, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK(host_boundary.capture_encoding_intent);
  CHECK(host_boundary.input_injection_intent);
  CHECK_EQ(pair.host.negotiated_stream()->config_id, 2U);
  CHECK_EQ(pair.host.negotiated_stream()->video_stream_id, 0x99999999U);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.device.take_video_frames().size(), 1U);
}

void test_heartbeat_emits_each_second_and_three_second_silence_fails() {
  {
    SessionPair pair(default_host_config(), default_device_config());
    drive_to_committed(pair);
    pair.clock.advance_ns(deskmate::protocol::kHeartbeatIntervalNs);
    pair.host.pump();
    const auto controls = decode_controls(pair.host_to_device);
    CHECK(std::count_if(controls.begin(), controls.end(),
                        [](const ControlMessage& control) {
                          return control.opcode == ControlOpcode::Heartbeat;
                        }) == 1);
  }
  {
    SessionPair pair(default_host_config(), default_device_config());
    drive_to_committed(pair);
    pair.clock.advance_ns(deskmate::protocol::kPeerSilenceTimeoutNs);
    pair.host.pump();
    CHECK_EQ(static_cast<int>(pair.host.state()),
             static_cast<int>(SessionState::Failed));
    CHECK_EQ(static_cast<int>(*pair.host.failure()),
             static_cast<int>(SessionFailure::HeartbeatTimeout));
  }
}

void drive_to_streaming_with_first_idr(SessionPair& pair) {
  drive_to_committed(pair);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x01}, 1, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.device.take_video_frames().size(), 1U);
}

void drive_to_reconfiguration_pending_idr(SessionPair& pair,
                                          const StreamProfile& profile,
                                          std::uint32_t config_id,
                                          std::uint32_t video_stream_id) {
  CHECK_EQ(pair.host.reconfigure(profile, config_id, video_stream_id),
           SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
}

void test_streaming_reconfiguration_keeps_old_until_committed_and_is_idempotent() {
  RecordingBoundary boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary);
  drive_to_streaming_with_first_idr(pair);
  CHECK(pair.host.negotiated_stream().has_value());
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();

  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  CHECK_EQ(pair.host.reconfigure(replacement, 2, 0xABCDEF01),
           SessionCommandStatus::Ok);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK(pair.host.negotiated_stream().has_value());
  CHECK(*pair.host.negotiated_stream() == old_stream);
  CHECK_EQ(pair.host.reconfigure(replacement, 2, 0xABCDEF01),
           SessionCommandStatus::Duplicate);
  CHECK_EQ(boundary.prepare_calls, 1);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x02}, 2, false),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(boundary.prepare_calls, 2);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK(pair.device.negotiated_stream().has_value());
  CHECK(*pair.device.negotiated_stream() == old_stream);

  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK_EQ(pair.host.negotiated_stream()->video_stream_id, old_stream.video_stream_id);
  CHECK_EQ(pair.host.negotiated_stream()->config_id, old_stream.config_id);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));
  CHECK(pair.device.negotiated_stream().has_value());
  CHECK(*pair.device.negotiated_stream() == old_stream);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x03}, 3, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.host.negotiated_stream()->video_stream_id, 0xABCDEF01U);
  CHECK_EQ(pair.host.negotiated_stream()->config_id, 2U);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.device.negotiated_stream()->config_id, 2U);
  CHECK_EQ(pair.device.negotiated_stream()->video_stream_id, 0xABCDEF01U);

  CHECK_EQ(pair.host.reconfigure(replacement, 2, 0xABCDEF01),
           SessionCommandStatus::Duplicate);
  CHECK_EQ(boundary.prepare_calls, 2);
  CHECK_EQ(boundary.commit_calls, 2);
  CHECK_EQ(boundary.discard_calls, 0);
  CHECK_EQ(boundary.rollback_calls, 0);
}

void test_reconfiguration_failure_keeps_old_stream() {
  RecordingBoundary boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary);
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  boundary.prepare_result = BoundaryResult::Failed;

  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  CHECK_EQ(pair.host.reconfigure(replacement, 2, 0x12345678),
           SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();

  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK(pair.device.negotiated_stream().has_value());
  CHECK(*pair.device.negotiated_stream() == old_stream);
  CHECK_EQ(boundary.prepare_calls, 2);
  CHECK_EQ(boundary.discard_calls, 0);

  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK(pair.host.reconfiguration().has_value());
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::Failed));
  CHECK(pair.host.negotiated_stream().has_value());
  CHECK(*pair.host.negotiated_stream() == old_stream);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x04}, 4, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.video_stream_id, old_stream.video_stream_id);
}

void test_device_does_not_final_ack_a_rolled_back_candidate() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateConfigId = 2;
  constexpr std::uint32_t kCandidateStreamId = 0x60000025;

  drive_to_reconfiguration_pending_idr(pair, replacement, kCandidateConfigId,
                                       kCandidateStreamId);
  pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK(*pair.device.negotiated_stream() == old_stream);

  const auto final_ack_count = [&pair] {
    const auto controls = decode_controls(pair.device_to_host);
    return std::count_if(
        controls.begin(), controls.end(),
        [](const ControlMessage& control) {
          return control.opcode == ControlOpcode::StreamCommitted &&
                 control.config_id == 2U && control.body.size() == 5;
        });
  };
  const auto before = final_ack_count();

  // The Host retransmits the candidate commit after the Device already rolled
  // back. The Device must not answer with a final (5-byte) ACK for a stream it
  // is no longer switching to.
  ControlMessage stale_commit;
  stale_commit.opcode = ControlOpcode::StreamCommit;
  stale_commit.config_id = kCandidateConfigId;
  stale_commit.body = {0x25, 0x00, 0x00, 0x60};
  append_control(pair.host_to_device, 0x88888888, kCandidateStreamId, 520,
                 stale_commit);
  pair.device.pump();
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(final_ack_count(), before);
}

void test_device_initial_idr_timeout_rolls_back_without_terminating_host() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};

  drive_to_reconfiguration_pending_idr(pair, replacement, 2, 0x60000001);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));

  pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
  pair.device.pump();
  pair.host.pump();

  CHECK(!pair.device.is_terminal());
  CHECK(!pair.host.is_terminal());
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK(pair.device.negotiated_stream().has_value());
  CHECK(pair.host.negotiated_stream().has_value());
  CHECK(*pair.device.negotiated_stream() == old_stream);
  CHECK(*pair.host.negotiated_stream() == old_stream);
  CHECK(pair.host.reconfiguration().has_value());
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::Failed));
  CHECK_EQ(static_cast<int>(*pair.host.reconfiguration()->failure),
           static_cast<int>(SessionFailure::InitialIdrTimeout));

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x61}, 61, false),
           VideoSendStatus::IdrRequired);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x61}, 61, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK(!pair.device.failure().has_value());
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.video_stream_id, old_stream.video_stream_id);
}

void test_host_waits_for_device_idr_timeout_before_rollback() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};

  drive_to_reconfiguration_pending_idr(pair, replacement, 2, 0x60000002);
  pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
  pair.host.pump();

  CHECK(!pair.host.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK(*pair.host.negotiated_stream() == old_stream);

  pair.device.pump();
  pair.host.pump();

  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK(*pair.host.negotiated_stream() == old_stream);
  CHECK(*pair.device.negotiated_stream() == old_stream);
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::Failed));
  CHECK_EQ(static_cast<int>(*pair.host.reconfiguration()->failure),
           static_cast<int>(SessionFailure::InitialIdrTimeout));

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x62}, 62, false),
           VideoSendStatus::IdrRequired);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x62}, 62, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.video_stream_id, old_stream.video_stream_id);
}

void test_candidate_frames_are_discarded_when_reconfiguration_times_out() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};

  drive_to_reconfiguration_pending_idr(pair, replacement, 2, 0x60000005);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x63}, 63, true),
           VideoSendStatus::Sent);
  CHECK_EQ(count_video_packets_for_stream(pair.host_to_device, 0x60000005), 0U);

  pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
  pair.device.pump();
  pair.host.pump();

  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(count_video_packets_for_stream(pair.host_to_device, 0x60000005), 0U);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x64}, 64, false),
           VideoSendStatus::IdrRequired);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x64}, 64, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.video_stream_id, old_stream.video_stream_id);
}

void test_duplicate_commit_before_first_idr_does_not_confirm_switch() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateConfigId = 2;
  constexpr std::uint32_t kCandidateStreamId = 0x60000006;

  drive_to_reconfiguration_pending_idr(pair, replacement, kCandidateConfigId,
                                       kCandidateStreamId);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));

  ControlMessage duplicate_commit;
  duplicate_commit.opcode = ControlOpcode::StreamCommit;
  duplicate_commit.config_id = kCandidateConfigId;
  duplicate_commit.body = {0x06, 0x00, 0x00, 0x60};
  append_control(pair.host_to_device, 0x88888888, kCandidateStreamId, 401,
                 duplicate_commit);
  pair.device.pump();
  pair.host.pump();

  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));
  CHECK_EQ(pair.host.negotiated_stream()->config_id, 1U);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x65}, 65, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.host.negotiated_stream()->config_id, kCandidateConfigId);
  CHECK_EQ(pair.device.negotiated_stream()->video_stream_id, kCandidateStreamId);
}

void test_late_ack_from_failed_configuration_is_ignored() {
  RecordingBoundary boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary);
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kFailedConfigId = 2;
  constexpr std::uint32_t kFailedStreamId = 0x60000020;
  constexpr std::uint32_t kNewConfigId = 3;
  constexpr std::uint32_t kNewStreamId = 0x60000021;

  boundary.prepare_result = BoundaryResult::Failed;
  CHECK_EQ(pair.host.reconfigure(replacement, kFailedConfigId, kFailedStreamId),
           SessionCommandStatus::Ok);
  for (int cycle = 0; cycle < 3; ++cycle) {
    pair.host.pump();
    pair.device.pump();
    pair.host.pump();
  }
  CHECK(!pair.host.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK(*pair.host.negotiated_stream() == old_stream);
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::Failed));
  CHECK_EQ(pair.host.reconfiguration()->config_id, kFailedConfigId);

  boundary.prepare_result = BoundaryResult::Ok;
  CHECK_EQ(pair.host.reconfigure(replacement, kNewConfigId, kNewStreamId),
           SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->config_id),
           static_cast<int>(kNewConfigId));

  // A stale ACK for the failed candidate must not be mistaken for the ACK of
  // the running configuration.
  append_commit_ack(pair.device_to_host, 0x88888888, kFailedConfigId,
                    kFailedStreamId, 500, false);
  append_commit_ack(pair.device_to_host, 0x88888888, kFailedConfigId,
                    kFailedStreamId, 501, true);
  pair.host.pump();
  CHECK(!pair.host.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::AwaitingFirstIdr));
  CHECK_EQ(pair.host.reconfiguration()->config_id, kNewConfigId);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x69}, 69, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.host.negotiated_stream()->config_id, kNewConfigId);
  CHECK_EQ(pair.host.negotiated_stream()->video_stream_id, kNewStreamId);
}

void test_duplicate_committed_after_switch_is_idempotent() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateConfigId = 2;
  constexpr std::uint32_t kCandidateStreamId = 0x60000007;

  drive_to_reconfiguration_pending_idr(pair, replacement, kCandidateConfigId,
                                       kCandidateStreamId);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x66}, 66, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));

  ControlMessage duplicate_commit;
  duplicate_commit.opcode = ControlOpcode::StreamCommit;
  duplicate_commit.config_id = kCandidateConfigId;
  duplicate_commit.body = {0x07, 0x00, 0x00, 0x60};
  append_control(pair.host_to_device, 0x88888888, kCandidateStreamId, 402,
                 duplicate_commit);
  pair.device.pump();
  pair.host.pump();

  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.host.negotiated_stream()->video_stream_id, kCandidateStreamId);
}

void test_duplicate_stream_ready_while_awaiting_first_idr_is_idempotent() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateConfigId = 2;
  constexpr std::uint32_t kCandidateStreamId = 0x60000022;

  drive_to_reconfiguration_pending_idr(pair, replacement, kCandidateConfigId,
                                       kCandidateStreamId);
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::AwaitingFirstIdr));

  // A duplicate STREAM_READY from the Device (e.g. a retransmission) must not
  // push the Host back to AwaitingCommitted while the Device already accepted
  // the commit.
  ControlMessage duplicate_ready;
  duplicate_ready.opcode = ControlOpcode::StreamReady;
  duplicate_ready.proto_rev = deskmate::protocol::kProtocolRevision;
  duplicate_ready.config_id = kCandidateConfigId;
  const auto profile_body =
      deskmate::protocol::encode_stream_profile(replacement);
  CHECK(profile_body);
  duplicate_ready.body = profile_body.value();
  append_control(pair.device_to_host, 0x88888888, 0, 510, duplicate_ready);
  pair.host.pump();

  CHECK(!pair.host.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::AwaitingFirstIdr));

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x6A}, 70, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.host.negotiated_stream()->video_stream_id, kCandidateStreamId);
}

void test_duplicate_first_commit_ack_does_not_complete_reconfiguration() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateConfigId = 2;
  constexpr std::uint32_t kCandidateStreamId = 0x60000009;

  drive_to_reconfiguration_pending_idr(pair, replacement, kCandidateConfigId,
                                       kCandidateStreamId);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::AwaitingFirstIdr));

  append_commit_ack(pair.device_to_host, 0x88888888, kCandidateConfigId,
                    kCandidateStreamId, 403, false);
  pair.host.pump();

  CHECK(!pair.host.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK(*pair.host.negotiated_stream() == old_stream);

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x68}, 68, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.host.negotiated_stream()->video_stream_id, kCandidateStreamId);
  CHECK_EQ(pair.device.negotiated_stream()->video_stream_id, kCandidateStreamId);
}

void test_lost_first_commit_ack_is_retried_without_terminating() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateConfigId = 2;
  constexpr std::uint32_t kCandidateStreamId = 0x6000000A;

  CHECK_EQ(pair.host.reconfigure(replacement, kCandidateConfigId,
                                 kCandidateStreamId),
           SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::AwaitingCommitted));

  pair.device_to_host.clear();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::AwaitingCommitted));

  pair.clock.advance_ns(default_host_config().scheduler.recovery_retry_ns);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();

  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::AwaitingFirstIdr));
  CHECK_EQ(static_cast<int>(pair.host.negotiated_stream()->config_id), 1);
}

void test_lost_final_commit_ack_is_retried_until_switch_is_confirmed() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateConfigId = 2;
  constexpr std::uint32_t kCandidateStreamId = 0x6000000B;

  drive_to_reconfiguration_pending_idr(pair, replacement, kCandidateConfigId,
                                       kCandidateStreamId);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x69}, 69, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));

  pair.device_to_host.clear();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));

  pair.clock.advance_ns(default_host_config().scheduler.recovery_retry_ns);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();

  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.host.negotiated_stream()->config_id, kCandidateConfigId);
  CHECK_EQ(pair.host.negotiated_stream()->video_stream_id, kCandidateStreamId);
}

void test_each_running_reconfiguration_stage_timeout_or_failure_preserves_old_stream() {
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  {
    SessionPair pair(default_host_config(), default_device_config());
    drive_to_streaming_with_first_idr(pair);
    CHECK_EQ(pair.host.reconfigure(replacement, 2, 0x20000001),
             SessionCommandStatus::Ok);
    pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
    pair.host.pump();
    CHECK_EQ(static_cast<int>(pair.host.state()),
             static_cast<int>(SessionState::Streaming));
    CHECK_EQ(pair.host.negotiated_stream()->config_id, 1U);
    CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
             static_cast<int>(ReconfigurationPhase::Failed));
    CHECK_EQ(static_cast<int>(*pair.host.reconfiguration()->failure),
             static_cast<int>(SessionFailure::ReadyTimeout));
  }
  {
    SessionPair pair(default_host_config(), default_device_config());
    drive_to_streaming_with_first_idr(pair);
    CHECK_EQ(pair.host.reconfigure(replacement, 2, 0x20000002),
             SessionCommandStatus::Ok);
    pair.host.pump();
    pair.device.pump();
    pair.host.pump();
    pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
    pair.host.pump();
    CHECK_EQ(static_cast<int>(pair.host.state()),
             static_cast<int>(SessionState::Streaming));
    CHECK_EQ(pair.host.negotiated_stream()->config_id, 1U);
    CHECK_EQ(static_cast<int>(*pair.host.reconfiguration()->failure),
             static_cast<int>(SessionFailure::CommittedTimeout));
  }
  {
    RecordingBoundary boundary;
    SessionPair pair(default_host_config(), default_device_config(), &boundary);
    drive_to_streaming_with_first_idr(pair);
    boundary.commit_result = BoundaryResult::Failed;
    CHECK_EQ(pair.host.reconfigure(replacement, 2, 0x20000003),
             SessionCommandStatus::Ok);
    pair.host.pump();
    pair.device.pump();
    pair.host.pump();
    pair.device.pump();
    pair.host.pump();
    CHECK_EQ(static_cast<int>(pair.device.state()),
             static_cast<int>(SessionState::Streaming));
    CHECK_EQ(pair.device.negotiated_stream()->config_id, 1U);
    CHECK_EQ(boundary.discard_calls, 1);
    CHECK_EQ(static_cast<int>(pair.host.state()),
             static_cast<int>(SessionState::Streaming));
    CHECK_EQ(pair.host.negotiated_stream()->config_id, 1U);
  }
  {
    SessionPair pair(default_host_config(), default_device_config());
    drive_to_streaming_with_first_idr(pair);
    CHECK_EQ(pair.host.reconfigure(replacement, 2, 0x20000004),
             SessionCommandStatus::Ok);
    pair.host.pump();
    pair.device.pump();
    pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
    pair.device.pump();
    CHECK_EQ(static_cast<int>(pair.device.state()),
             static_cast<int>(SessionState::Streaming));
    CHECK_EQ(pair.device.negotiated_stream()->config_id, 1U);
  }
}

void test_four_timestamp_clock_estimator_reports_offset_rtt_drift_and_bound() {
  ClockSyncEstimator estimator;
  CHECK(estimator.observe(
      ClockSample{1'000, 2'100, 2'600, 1'700}));
  CHECK(estimator.estimate().has_value());
  ClockEstimate estimate = *estimator.estimate();
  CHECK_EQ(estimate.offset_ns, 1'000);
  CHECK_EQ(estimate.rtt_ns, 200U);
  CHECK_EQ(estimate.error_bound_ns, 100U);
  CHECK_EQ(estimate.drift_ppb, 0);
  CHECK_EQ(estimate.sample_count, 1U);

  CHECK(estimator.observe(ClockSample{
      1'000'001'000, 1'000'003'100, 1'000'003'600, 1'000'001'700}));
  estimate = *estimator.estimate();
  CHECK_EQ(estimate.offset_ns, 2'000);
  CHECK_EQ(estimate.rtt_ns, 200U);
  CHECK_EQ(estimate.error_bound_ns, 100U);
  CHECK_EQ(estimate.drift_ppb, 1'000);
  CHECK_EQ(estimate.sample_count, 2U);

  CHECK(!estimator.observe(ClockSample{100, 200, 150, 300}));
  const auto encoded = deskmate::protocol::encode_clock_message(
      ClockMessage{ClockMessageKind::Response, 1, 2, 3});
  CHECK(encoded);
  CHECK_EQ(encoded.value().size(), 28U);
  const auto decoded =
      deskmate::protocol::decode_clock_message(encoded.value());
  CHECK(decoded);
  CHECK(decoded.value().kind == ClockMessageKind::Response);
  CHECK_EQ(decoded.value().t0_ns, 1U);
  CHECK_EQ(decoded.value().t1_ns, 2U);
  CHECK_EQ(decoded.value().t2_ns, 3U);
}

void test_clock_exchange_over_session_messages_produces_estimate() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const std::size_t before = pair.host_to_device.bytes().size();

  CHECK_EQ(pair.host.request_clock_sync(), SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();

  CHECK(pair.host.clock_estimate().has_value());
  CHECK_EQ(pair.host.clock_estimate()->offset_ns, 0);
  CHECK_EQ(pair.host.clock_estimate()->rtt_ns, 0U);
  CHECK_EQ(pair.host.clock_estimate()->sample_count, 1U);

  std::size_t offset = before;
  std::size_t clock_packets = 0;
  while (offset + deskmate::protocol::kWireHeaderSize <=
         pair.host_to_device.bytes().size()) {
    const auto header = deskmate::protocol::decode_header(
        std::span<const std::uint8_t>(pair.host_to_device.bytes()).subspan(offset));
    CHECK(header);
    offset += deskmate::protocol::kWireHeaderSize + header.value().payload_len;
    if (header.value().type ==
        static_cast<std::uint8_t>(deskmate::protocol::MessageType::Clock)) {
      ++clock_packets;
    }
  }
  CHECK_EQ(clock_packets, 1U);

  offset = 0;
  std::size_t device_clock_packets = 0;
  while (offset + deskmate::protocol::kWireHeaderSize <=
         pair.device_to_host.bytes().size()) {
    const auto header = deskmate::protocol::decode_header(
        std::span<const std::uint8_t>(pair.device_to_host.bytes()).subspan(offset));
    CHECK(header);
    offset += deskmate::protocol::kWireHeaderSize + header.value().payload_len;
    if (header.value().type ==
        static_cast<std::uint8_t>(deskmate::protocol::MessageType::Clock)) {
      ++device_clock_packets;
    }
  }
  CHECK_EQ(device_clock_packets, 1U);
}

void test_lock_and_unlock_publish_distinct_states_and_require_resume() {
  RecordingHostBoundary host_boundary;
  SessionPair pair(default_host_config(), default_device_config(), nullptr,
                   &host_boundary);
  drive_to_streaming_with_first_idr(pair);

  CHECK_EQ(pair.host.handle_host_event(HostEvent::ScreenLocked),
           SessionCommandStatus::Ok);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::ScreenLocked));
  CHECK_EQ(static_cast<int>(pair.host.input_state()),
           static_cast<int>(HostInputState::ScreenLocked));
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
  CHECK(pair.host.extended_display_present());

  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK(pair.host.pause_acknowledged());

  CHECK_EQ(pair.host.handle_host_event(HostEvent::ScreenUnlocked),
           SessionCommandStatus::Ok);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::AwaitingResume));
  CHECK_EQ(static_cast<int>(pair.host.input_state()),
           static_cast<int>(HostInputState::AwaitingResume));
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x20}, 20, false),
           VideoSendStatus::Paused);

  const StreamProfile profile{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  CHECK_EQ(pair.host.resume(profile, 2, 0x11112222),
           SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x24}, 24, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::Active));
  CHECK_EQ(static_cast<int>(pair.host.input_state()),
           static_cast<int>(HostInputState::Active));
}

void test_lock_during_handshake_stays_paused_and_blocks_resume() {
  RecordingHostBoundary host_boundary;
  SessionPair pair(default_host_config(), default_device_config(), nullptr,
                   &host_boundary);
  drive_to_host_awaiting_committed(pair);

  CHECK_EQ(pair.host.handle_host_event(HostEvent::ScreenLocked),
           SessionCommandStatus::Ok);
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);

  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::ScreenLocked));
  CHECK_EQ(static_cast<int>(pair.host.input_state()),
           static_cast<int>(HostInputState::ScreenLocked));
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);

  // The Device must observe the Pause instead of failing the fresh session.
  pair.device.pump();
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Paused));

  const StreamProfile profile{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  CHECK_EQ(pair.host.resume(profile, 2, 0x10101010),
           SessionCommandStatus::DisplayUnavailable);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);

  CHECK_EQ(pair.host.handle_host_event(HostEvent::ScreenUnlocked),
           SessionCommandStatus::Ok);
  CHECK_EQ(pair.host.resume(profile, 2, 0x10101010),
           SessionCommandStatus::Ok);
}

void test_lock_during_reconfiguration_keeps_old_stream_and_pauses() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};

  CHECK_EQ(pair.host.reconfigure(replacement, 2, 0x60000003),
           SessionCommandStatus::Ok);
  CHECK_EQ(pair.host.handle_host_event(HostEvent::ScreenLocked),
           SessionCommandStatus::Ok);
  CHECK(!pair.host.capture_encoding_intent());
  CHECK(!pair.host.input_injection_intent());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));

  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Paused));
  CHECK(pair.host.negotiated_stream().has_value());
  CHECK(pair.device.negotiated_stream().has_value());
  CHECK(*pair.host.negotiated_stream() == old_stream);
  CHECK(*pair.device.negotiated_stream() == old_stream);
  CHECK(!pair.host.capture_encoding_intent());
  CHECK(!pair.host.input_injection_intent());
}

void test_lock_after_candidate_idr_waits_for_reconfiguration_verdict() {
  RecordingHostBoundary host_boundary;
  SessionPair pair(default_host_config(), default_device_config(), nullptr,
                   &host_boundary);
  drive_to_streaming_with_first_idr(pair);
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};

  drive_to_reconfiguration_pending_idr(pair, replacement, 2, 0x60000008);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x67}, 67, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));

  CHECK_EQ(pair.host.handle_host_event(HostEvent::ScreenLocked),
           SessionCommandStatus::Ok);
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::ScreenLocked));

  pair.device.pump();
  pair.host.pump();
  pair.device.pump();

  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Paused));
  CHECK(pair.host.negotiated_stream().has_value());
  CHECK(pair.device.negotiated_stream().has_value());
  CHECK_EQ(pair.host.negotiated_stream()->video_stream_id, 0x60000008U);
  CHECK_EQ(pair.device.negotiated_stream()->video_stream_id, 0x60000008U);
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
}

void test_lock_after_candidate_idr_started_preserves_it_until_final_ack() {
  HostSessionConfig host_config = default_host_config();
  host_config.limits.video_fragment_payload = 32;
  host_config.scheduler.max_video_urbs = 2;
  RecordingHostBoundary host_boundary;
  SessionPair pair(host_config, default_device_config(), nullptr,
                   &host_boundary);
  drive_to_streaming_with_first_idr(pair);
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateStreamId = 0x6000000C;

  drive_to_reconfiguration_pending_idr(pair, replacement, 2,
                                       kCandidateStreamId);
  pair.host_to_device.set_write_chunk_size(16);
  pair.host_to_device.writes_until_block = 1;
  const std::vector<std::uint8_t> candidate_payload(80, 0x42);
  CHECK_EQ(pair.host.send_video_frame(candidate_payload, 70, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));

  CHECK_EQ(pair.host.handle_host_event(HostEvent::ScreenLocked),
           SessionCommandStatus::Ok);
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::ScreenLocked));
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));

  pair.host_to_device.writes_until_block =
      std::numeric_limits<std::size_t>::max();
  pair.host_to_device.set_write_chunk_size(1024);
  for (int cycle = 0; cycle < 6; ++cycle) {
    pair.host.pump();
    pair.device.pump();
    pair.host.pump();
  }

  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(pair.host.negotiated_stream()->video_stream_id,
           kCandidateStreamId);
  CHECK_EQ(pair.device.negotiated_stream()->video_stream_id,
           kCandidateStreamId);
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.video_stream_id, kCandidateStreamId);
  CHECK_EQ(frames[0].payload, candidate_payload);
}

void test_lock_before_candidate_idr_starts_aborts_reconfiguration() {
  HostSessionConfig host_config = default_host_config();
  host_config.limits.video_fragment_payload = 32;
  RecordingHostBoundary host_boundary;
  SessionPair pair(host_config, default_device_config(), nullptr,
                   &host_boundary);
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateStreamId = 0x6000000D;

  drive_to_reconfiguration_pending_idr(pair, replacement, 2,
                                       kCandidateStreamId);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>(80, 0x43), 71,
                                      true),
           VideoSendStatus::Sent);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));

  CHECK_EQ(pair.host.handle_host_event(HostEvent::ScreenLocked),
           SessionCommandStatus::Ok);
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::Failed));

  for (int cycle = 0; cycle < 3; ++cycle) {
    pair.host.pump();
    pair.device.pump();
  }
  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Paused));
  CHECK(*pair.host.negotiated_stream() == old_stream);
  CHECK(*pair.device.negotiated_stream() == old_stream);
  CHECK_EQ(count_video_packets_for_stream(pair.host_to_device,
                                          kCandidateStreamId),
           0U);
}

void test_display_error_before_candidate_idr_keeps_both_ends_paused() {
  RecordingHostBoundary host_boundary;
  SessionPair pair(default_host_config(), default_device_config(), nullptr,
                   &host_boundary);
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateStreamId = 0x60000011;

  drive_to_reconfiguration_pending_idr(pair, replacement, 2,
                                       kCandidateStreamId);
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::AwaitingFirstIdr));

  CHECK_EQ(pair.host.handle_host_event(HostEvent::DisplayError),
           SessionCommandStatus::Ok);
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::DisplayError));

  for (int cycle = 0; cycle < 4; ++cycle) {
    pair.host.pump();
    pair.device.pump();
  }

  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK(!pair.host.failure().has_value());
  CHECK(!pair.device.failure().has_value());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Paused));
  CHECK(*pair.host.negotiated_stream() == old_stream);
  CHECK(*pair.device.negotiated_stream() == old_stream);
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
}

void test_display_error_after_candidate_idr_started_preserves_it() {
  HostSessionConfig host_config = default_host_config();
  host_config.limits.video_fragment_payload = 32;
  RecordingHostBoundary host_boundary;
  SessionPair pair(host_config, default_device_config(), nullptr,
                   &host_boundary);
  drive_to_streaming_with_first_idr(pair);
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateStreamId = 0x60000010;

  drive_to_reconfiguration_pending_idr(pair, replacement, 2,
                                       kCandidateStreamId);
  pair.host_to_device.set_write_chunk_size(16);
  pair.host_to_device.writes_until_block = 1;
  const std::vector<std::uint8_t> candidate_payload(80, 0x46);
  CHECK_EQ(pair.host.send_video_frame(candidate_payload, 74, true),
           VideoSendStatus::Sent);
  pair.host.pump();

  CHECK_EQ(pair.host.handle_host_event(HostEvent::DisplayError),
           SessionCommandStatus::Ok);
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::DisplayError));
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));

  pair.host_to_device.writes_until_block =
      std::numeric_limits<std::size_t>::max();
  pair.host_to_device.set_write_chunk_size(1024);
  for (int cycle = 0; cycle < 6; ++cycle) {
    pair.host.pump();
    pair.device.pump();
    pair.host.pump();
  }

  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(pair.host.negotiated_stream()->video_stream_id,
           kCandidateStreamId);
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].payload, candidate_payload);
}

void test_rollback_discards_active_candidate_packet_before_old_stream() {
  HostSessionConfig host_config = default_host_config();
  host_config.limits.video_fragment_payload = 32;
  SessionPair pair(host_config, default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateStreamId = 0x6000000E;

  drive_to_reconfiguration_pending_idr(pair, replacement, 2,
                                       kCandidateStreamId);
  pair.host_to_device.set_write_chunk_size(16);
  pair.host_to_device.writes_until_block = 1;
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>(64, 0x44), 72,
                                      true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));

  pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
  pair.device.pump();
  pair.host.pump();
  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK(*pair.host.negotiated_stream() == old_stream);

  pair.host_to_device.writes_until_block =
      std::numeric_limits<std::size_t>::max();
  pair.host_to_device.set_write_chunk_size(1024);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x45}, 73, true),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();

  CHECK(!pair.device.is_terminal());
  CHECK(!pair.device.failure().has_value());
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.video_stream_id, old_stream.video_stream_id);
  CHECK_EQ(frames[0].payload, std::vector<std::uint8_t>({0x45}));
}

void test_rollback_waits_for_old_stream_keyframe_before_p_frames() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateStreamId = 0x6000000F;

  drive_to_reconfiguration_pending_idr(pair, replacement, 2,
                                       kCandidateStreamId);
  pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK(*pair.device.negotiated_stream() == old_stream);

  append_video(pair.host_to_device, 0x88888888,
               old_stream.video_stream_id, 500, {0x50}, 0);
  pair.device.pump();
  CHECK(pair.device.take_video_frames().empty());
  CHECK(!pair.device.is_terminal());

  append_video(pair.host_to_device, 0x88888888,
               old_stream.video_stream_id, 501, {0x51},
               deskmate::protocol::kFlagKeyFrame);
  pair.device.pump();
  auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].payload, std::vector<std::uint8_t>({0x51}));

  append_video(pair.host_to_device, 0x88888888,
               old_stream.video_stream_id, 502, {0x52}, 0);
  pair.device.pump();
  frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].payload, std::vector<std::uint8_t>({0x52}));
}

void test_screen_recording_and_accessibility_revocations_degrade_independently() {
  RecordingHostBoundary host_boundary;
  SessionPair pair(default_host_config(), default_device_config(), nullptr,
                   &host_boundary);
  drive_to_streaming_with_first_idr(pair);

  CHECK_EQ(pair.host.handle_host_event(
               HostEvent::AccessibilityPermissionRevoked),
           SessionCommandStatus::Ok);
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::Active));
  CHECK_EQ(static_cast<int>(pair.host.input_state()),
           static_cast<int>(HostInputState::AccessibilityPermissionRevoked));
  CHECK(host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x21}, 21, false),
           VideoSendStatus::Sent);

  CHECK_EQ(pair.host.handle_host_event(
               HostEvent::ScreenRecordingPermissionRevoked),
           SessionCommandStatus::Ok);
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::RecordingPermissionRevoked));
  CHECK_EQ(static_cast<int>(pair.host.input_state()),
           static_cast<int>(HostInputState::AccessibilityPermissionRevoked));
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x22}, 22, false),
           VideoSendStatus::VideoUnavailable);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
}

void test_display_error_is_a_control_preserving_degraded_state() {
  RecordingHostBoundary host_boundary;
  SessionPair pair(default_host_config(), default_device_config(), nullptr,
                   &host_boundary);
  drive_to_streaming_with_first_idr(pair);

  CHECK_EQ(pair.host.handle_host_event(HostEvent::DisplayError),
           SessionCommandStatus::Ok);
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::DisplayError));
  CHECK_EQ(static_cast<int>(pair.host.input_state()),
           static_cast<int>(HostInputState::DisplayError));
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
  CHECK(!pair.host.extended_display_present());
  CHECK_EQ(host_boundary.display_destroy_calls, 1);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK(!pair.host.is_terminal());
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x23}, 23, false),
           VideoSendStatus::VideoUnavailable);

  CHECK_EQ(pair.host.handle_host_event(HostEvent::DisplayError),
           SessionCommandStatus::Duplicate);
  CHECK_EQ(host_boundary.display_destroy_calls, 1);
}

void test_pause_grace_timeout_destroys_display_once() {
  RecordingHostBoundary host_boundary;
  SessionPair pair(default_host_config(), default_device_config(), nullptr,
                   &host_boundary);
  drive_to_streaming_with_first_idr(pair);
  CHECK_EQ(pair.host.pause(), SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(host_boundary.display_destroy_calls, 0);

  for (std::uint64_t second = 0; second < 30; ++second) {
    pair.clock.advance_ns(deskmate::protocol::kHeartbeatIntervalNs);
    pair.device.pump();
    pair.host.pump();
    if (pair.host.is_terminal()) {
      break;
    }
  }
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.host.failure()),
           static_cast<int>(SessionFailure::PauseGraceExpired));
  CHECK_EQ(host_boundary.display_destroy_calls, 1);
  CHECK(!pair.host.extended_display_present());
  pair.host.pump();
  CHECK_EQ(host_boundary.display_destroy_calls, 1);
}

void test_pause_grace_timeout_must_be_between_thirty_and_sixty_seconds() {
  {
    SessionPair pair(default_host_config(), default_device_config());
    pair.host.start();
    CHECK(!pair.host.is_terminal());
  }
  {
    HostSessionConfig config = default_host_config();
    config.limits.pause_grace_timeout_ns =
        deskmate::protocol::kMinPauseGraceTimeoutNs - 1;
    SessionPair pair(config, default_device_config());
    pair.host.start();
    CHECK_EQ(static_cast<int>(pair.host.state()),
             static_cast<int>(SessionState::Failed));
    CHECK_EQ(static_cast<int>(*pair.host.failure()),
             static_cast<int>(SessionFailure::InvalidConfiguration));
  }
  {
    HostSessionConfig config = default_host_config();
    config.limits.pause_grace_timeout_ns =
        deskmate::protocol::kMaxPauseGraceTimeoutNs;
    SessionPair pair(config, default_device_config());
    pair.host.start();
    CHECK(!pair.host.is_terminal());
  }
}

class RecordingSurfaceOps final : public SurfaceReferenceOps {
 public:
  void add_reference() override { ++add_calls; }
  void remove_reference() override { ++remove_calls; }
  int add_calls{};
  int remove_calls{};
};

void test_surface_lease_tracks_generation_references_and_destruction() {
  RecordingSurfaceOps ops;
  SurfaceOwner owner(&ops);
  const std::uint64_t generation = owner.attach();
  CHECK(generation != 0);
  CHECK(owner.available());
  CHECK_EQ(owner.generation(), generation);

  auto lease = owner.acquire();
  CHECK(lease.has_value());
  CHECK_EQ(lease->generation(), generation);
  CHECK(lease->can_render());
  CHECK_EQ(owner.reference_count(), 1U);
  CHECK_EQ(ops.add_calls, 1);

  owner.invalidate();
  CHECK(!owner.available());
  CHECK(!lease->can_render());
  CHECK(!owner.acquire().has_value());
  CHECK_EQ(owner.reference_count(), 1U);

  lease->release();
  lease->release();
  CHECK_EQ(owner.reference_count(), 0U);
  CHECK_EQ(ops.remove_calls, 1);

  const std::uint64_t next_generation = owner.attach();
  CHECK(next_generation > generation);
  auto next_lease = owner.acquire();
  CHECK(next_lease.has_value());
  CHECK(next_lease->can_render());
  CHECK_EQ(owner.reference_count(), 1U);
  next_lease->release();
  CHECK_EQ(ops.add_calls, 2);
  CHECK_EQ(ops.remove_calls, 2);
}

void test_surface_lease_outlives_owner_with_shared_reference_ops() {
  auto ops = std::make_shared<RecordingSurfaceOps>();
  std::optional<SurfaceLease> lease;
  {
    SurfaceOwner owner(ops);
    const std::uint64_t generation = owner.attach();
    CHECK(generation != 0);
    lease = owner.acquire();
    CHECK(lease.has_value());
    CHECK(lease->can_render());
    CHECK_EQ(ops->add_calls, 1);
  }

  CHECK(!lease->can_render());
  CHECK(lease->holds_reference());
  lease->release();
  CHECK_EQ(ops->remove_calls, 1);
  CHECK(!lease->holds_reference());
}

void test_surface_lease_invalidates_raw_ops_when_owner_is_destroyed() {
  RecordingSurfaceOps ops;
  std::optional<SurfaceLease> lease;
  {
    SurfaceOwner owner(&ops);
    const std::uint64_t generation = owner.attach();
    CHECK(generation != 0);
    lease = owner.acquire();
    CHECK(lease.has_value());
    CHECK(lease->can_render());
  }

  CHECK(!lease->can_render());
  CHECK(!lease->holds_reference());
  lease->release();
  CHECK(!lease->holds_reference());
  CHECK_EQ(ops.add_calls, 1);
  CHECK_EQ(ops.remove_calls, 1);
  lease->release();
  CHECK_EQ(ops.remove_calls, 1);
}

void test_borrowed_accessory_fd_never_closes_on_destruction() {
  {
    BorrowedAccessoryFd owner(9);
    CHECK(owner.valid());
    CHECK_EQ(owner.borrow_fd(), 9);
    BorrowedAccessoryFd moved(std::move(owner));
    CHECK(!owner.valid());
    CHECK(moved.valid());
    CHECK_EQ(moved.borrow_fd(), 9);
    moved.invalidate();
    CHECK(!moved.valid());
    CHECK_EQ(moved.borrow_fd(), -1);
  }
}

void test_input_payload_round_trips_and_rejects_malformed_bodies() {
  InputEvent event;
  event.kind = InputKind::TouchMove;
  event.pointer_id = 2;
  event.flags = deskmate::protocol::kInputFlagPrimaryButton;
  event.x = 0.25F;
  event.y = 0.75F;
  event.dx = -0.5F;
  event.dy = 1.5F;

  const auto encoded = deskmate::protocol::encode_input_payload(event);
  CHECK(encoded);
  CHECK_EQ(encoded.value().size(), 28U);
  const auto decoded = deskmate::protocol::decode_input_payload(encoded.value());
  CHECK(decoded);
  CHECK_EQ(static_cast<int>(decoded.value().kind),
           static_cast<int>(InputKind::TouchMove));
  CHECK_EQ(decoded.value().pointer_id, 2U);
  CHECK_EQ(decoded.value().flags,
           deskmate::protocol::kInputFlagPrimaryButton);
  CHECK(decoded.value().x == 0.25F);
  CHECK(decoded.value().y == 0.75F);
  CHECK(decoded.value().dx == -0.5F);
  CHECK(decoded.value().dy == 1.5F);

  CHECK(!deskmate::protocol::decode_input_payload(
      std::span<const std::uint8_t>(encoded.value()).first(27)));

  auto bad_reserved = encoded.value();
  bad_reserved[24] = 1;
  CHECK(!deskmate::protocol::decode_input_payload(bad_reserved));

  auto bad_kind = encoded.value();
  bad_kind[0] = 0x7F;
  CHECK(!deskmate::protocol::decode_input_payload(bad_kind));

  InputEvent nan_event;
  nan_event.kind = InputKind::TouchDown;
  nan_event.x = std::numeric_limits<float>::quiet_NaN();
  CHECK(!deskmate::protocol::encode_input_payload(nan_event));

  InputEvent key_without_code;
  key_without_code.kind = InputKind::Key;
  CHECK(!deskmate::protocol::encode_input_payload(key_without_code));
}

void test_device_input_reaches_host_only_while_streaming_and_permitted() {
  RecordingBoundary boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary);
  pair.start();

  InputEvent early;
  early.kind = InputKind::TouchDown;
  CHECK_EQ(static_cast<int>(pair.device.send_input(early)),
           static_cast<int>(SessionCommandStatus::InvalidState));

  drive_to_streaming_with_first_idr(pair);
  CHECK(pair.host.input_injection_intent());

  InputEvent down;
  down.kind = InputKind::TouchDown;
  down.pointer_id = 1;
  down.flags = deskmate::protocol::kInputFlagPrimaryButton;
  down.x = 0.4F;
  down.y = 0.6F;
  CHECK_EQ(static_cast<int>(pair.device.send_input(down)),
           static_cast<int>(SessionCommandStatus::Ok));
  pair.device.pump();
  pair.host.pump();

  const auto events = pair.host.take_input_events();
  CHECK_EQ(events.size(), 1U);
  CHECK_EQ(static_cast<int>(events[0].kind),
           static_cast<int>(InputKind::TouchDown));
  CHECK_EQ(events[0].pointer_id, 1U);
  CHECK(events[0].x == 0.4F);
  CHECK(events[0].y == 0.6F);
  CHECK(pair.host.take_input_events().empty());

  CHECK_EQ(static_cast<int>(pair.host.handle_host_event(HostEvent::ScreenLocked)),
           static_cast<int>(SessionCommandStatus::Ok));
  CHECK(!pair.host.input_injection_intent());
  pair.host.pump();
  pair.device.pump();
  // Locking pauses the stream, so the Device refuses new input outright.
  CHECK_EQ(static_cast<int>(pair.device.send_input(down)),
           static_cast<int>(SessionCommandStatus::InvalidState));
  pair.device.pump();
  pair.host.pump();
  CHECK(pair.host.take_input_events().empty());
}

void test_input_from_a_stale_stream_generation_is_dropped() {
  RecordingBoundary boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary);
  pair.start();
  drive_to_streaming_with_first_idr(pair);

  const auto negotiated = pair.device.negotiated_stream();
  CHECK(negotiated.has_value());
  deskmate::protocol::InputEvent event;
  event.kind = InputKind::Scroll;
  event.dx = 0.0F;
  event.dy = -0.1F;
  const auto payload = deskmate::protocol::encode_input_payload(event);
  CHECK(payload);

  deskmate::protocol::Packet packet;
  packet.header.type = static_cast<std::uint8_t>(
      deskmate::protocol::MessageType::Input);
  packet.header.session_nonce = negotiated->video_stream_id == 0
                                    ? 1U
                                    : pair.device.session_nonce();
  packet.header.video_stream_id = negotiated->video_stream_id + 1U;
  packet.header.seq = 1;
  packet.header.payload_len = static_cast<std::uint32_t>(payload.value().size());
  packet.payload = payload.value();
  const auto encoded = deskmate::protocol::encode_packet(packet);
  CHECK(encoded);
  pair.device_to_host.append(encoded.value());
  pair.host.pump();
  CHECK(pair.host.take_input_events().empty());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
}

void test_device_stops_output_after_surface_destroy_and_uses_new_generation() {
  RecordingSurfaceOps ops;
  SurfaceOwner surface(&ops);
  RecordingBoundary boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary,
                   nullptr, &surface);
  const std::uint64_t first_generation = pair.device.attach_surface();
  CHECK(first_generation != 0);
  drive_to_streaming_with_first_idr(pair);
  CHECK(pair.device.surface_available());

  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x31}, 31, false),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].surface_generation, first_generation);

  pair.device.invalidate_surface();
  CHECK(!surface.available());
  CHECK(!pair.device.surface_available());
  CHECK(pair.device.take_video_frames().empty());
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x32}, 32, false),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  CHECK(pair.device.take_video_frames().empty());
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));

  const std::uint64_t second_generation = pair.device.attach_surface();
  CHECK(second_generation > first_generation);
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x33}, 33, false),
           VideoSendStatus::Sent);
  pair.host.pump();
  pair.device.pump();
  frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].surface_generation, second_generation);
}

void test_stopping_rejects_work_then_cancels_drains_and_releases_in_order() {
  std::vector<std::string> events;
  RecordingBoundary boundary;
  RecordingHostBoundary host_boundary;
  boundary.events = &events;
  host_boundary.events = &events;
  SessionPair pair(default_host_config(), default_device_config(), &boundary,
                   &host_boundary, nullptr, &events);
  drive_to_streaming_with_first_idr(pair);
  events.clear();

  pair.host.request_stop();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Stopping));
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x40}, 40, false),
           VideoSendStatus::Stopping);
  CHECK_EQ(pair.host.pause(), SessionCommandStatus::InvalidState);
  CHECK_EQ(events,
           std::vector<std::string>({"host.capture_off", "host.input_off",
                                     "host.io.cancel"}));

  pair.host.complete_stop();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Stopped));
  CHECK(pair.host.is_terminal());
  CHECK_EQ(events,
           std::vector<std::string>({"host.capture_off", "host.input_off",
                                     "host.io.cancel", "host.drain",
                                     "host.destroy"}));
  pair.host.complete_stop();
  CHECK_EQ(host_boundary.drain_calls, 1);
  CHECK_EQ(host_boundary.display_destroy_calls, 1);

  pair.device.request_stop();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Stopping));
  pair.device.complete_stop();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Stopped));
  CHECK_EQ(boundary.drain_calls, 1);
  CHECK_EQ(boundary.release_calls, 1);
  CHECK_EQ(boundary.released_config_id, 1U);
  CHECK_EQ(boundary.released_stream_id, 0x77777777U);
  pair.device.complete_stop();
  CHECK_EQ(boundary.release_calls, 1);
}

void test_stopping_discards_inflight_video_work_without_waiting() {
  std::vector<std::string> events;
  RecordingHostBoundary host_boundary;
  host_boundary.events = &events;
  SessionPair pair(default_host_config(), default_device_config(), nullptr,
                   &host_boundary, nullptr, &events);
  drive_to_streaming_with_first_idr(pair);
  events.clear();

  CHECK_EQ(pair.host.send_video_frame(
               std::vector<std::uint8_t>(64U * 1024U, 0x41), 70, false),
           VideoSendStatus::Sent);
  pair.host.pump();
  const std::size_t bytes_before_stop = pair.host_to_device.bytes().size();
  CHECK(bytes_before_stop > 0);

  pair.host.request_stop();
  pair.host.pump();
  CHECK_EQ(pair.host_to_device.bytes().size(), bytes_before_stop);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Stopping));
  CHECK_EQ(events,
           std::vector<std::string>({"host.capture_off", "host.input_off",
                                     "host.io.cancel"}));
}

void test_stopping_does_not_flush_queued_outbound_control() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);

  const std::size_t bytes_before =
      pair.device_to_host.bytes().size();
  pair.device_to_host.writes_until_block = 0;
  ControlMessage pause;
  pause.opcode = ControlOpcode::Pause;
  pause.config_id = 1;
  append_control(pair.host_to_device, 0x88888888, 0, 330, pause);
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(pair.device_to_host.bytes().size(), bytes_before);

  pair.device_to_host.writes_until_block =
      std::numeric_limits<std::size_t>::max();
  pair.device.request_stop();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Stopping));
  pair.device.pump();

  CHECK_EQ(pair.device_to_host.bytes().size(), bytes_before);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Stopping));
  pair.device.complete_stop();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Stopped));
}

void test_sleep_uses_stop_order_and_never_closes_borrowed_fd() {
  std::vector<std::string> events;
  RecordingBoundary boundary;
  RecordingHostBoundary host_boundary;
  boundary.events = &events;
  host_boundary.events = &events;
  SessionPair pair(default_host_config(), default_device_config(), &boundary,
                   &host_boundary, nullptr, &events);
  drive_to_streaming_with_first_idr(pair);
  BorrowedAccessoryFd fd_owner(11);
  events.clear();

  CHECK_EQ(pair.host.handle_host_event(HostEvent::WillSleep),
           SessionCommandStatus::Ok);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Stopped));
  CHECK_EQ(static_cast<int>(*pair.host.failure()),
           static_cast<int>(SessionFailure::SystemSleep));
  CHECK_EQ(static_cast<int>(pair.host.video_state()),
           static_cast<int>(HostVideoState::Sleeping));
  CHECK_EQ(static_cast<int>(pair.host.input_state()),
           static_cast<int>(HostInputState::Sleeping));
  CHECK_EQ(host_boundary.display_destroy_calls, 1);
  CHECK_EQ(host_boundary.drain_calls, 1);
  CHECK(fd_owner.valid());
  CHECK_EQ(fd_owner.borrow_fd(), 11);
}

void test_heartbeat_timeout_releases_established_session_resources() {
  RecordingSurfaceOps ops;
  SurfaceOwner surface(&ops);
  RecordingBoundary boundary;
  RecordingHostBoundary host_boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary,
                   &host_boundary, &surface);
  CHECK(pair.device.attach_surface() != 0);
  drive_to_streaming_with_first_idr(pair);

  pair.clock.advance_ns(deskmate::protocol::kPeerSilenceTimeoutNs);
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.host.failure()),
           static_cast<int>(SessionFailure::HeartbeatTimeout));
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
  CHECK_EQ(host_boundary.display_destroy_calls, 1);
  CHECK_EQ(host_boundary.drain_calls, 1);

  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.device.failure()),
           static_cast<int>(SessionFailure::HeartbeatTimeout));
  CHECK_EQ(boundary.drain_calls, 1);
  CHECK_EQ(boundary.release_calls, 1);
  CHECK(!pair.device.surface_available());
  CHECK_EQ(surface.reference_count(), 0U);
}

void test_fatal_timeout_cancels_io_before_drain_and_release() {
  std::vector<std::string> events;
  RecordingBoundary boundary;
  RecordingHostBoundary host_boundary;
  boundary.events = &events;
  host_boundary.events = &events;
  SessionPair pair(default_host_config(), default_device_config(), &boundary,
                   &host_boundary, nullptr, &events);
  drive_to_streaming_with_first_idr(pair);
  events.clear();

  pair.clock.advance_ns(deskmate::protocol::kPeerSilenceTimeoutNs);
  pair.host.pump();
  CHECK_EQ(events,
           std::vector<std::string>({"host.capture_off", "host.input_off",
                                     "host.io.cancel", "host.drain",
                                     "host.destroy"}));

  events.clear();
  pair.device.pump();
  CHECK_EQ(events,
           std::vector<std::string>({"device.io.cancel", "device.drain",
                                     "device.release"}));
}

void test_host_write_failure_uses_terminal_cleanup_and_cancels_io() {
  std::vector<std::string> events;
  RecordingHostBoundary host_boundary;
  host_boundary.events = &events;
  SessionPair pair(default_host_config(), default_device_config(), nullptr,
                   &host_boundary, nullptr, &events);
  drive_to_streaming_with_first_idr(pair);
  events.clear();

  pair.host_to_device.fail_writes = true;
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x31}, 31, false),
           VideoSendStatus::Sent);
  pair.host.pump();

  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.host.failure()),
           static_cast<int>(SessionFailure::IoError));
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK(!host_boundary.input_injection_intent);
  CHECK_EQ(host_boundary.drain_calls, 1);
  CHECK_EQ(host_boundary.display_destroy_calls, 1);
  CHECK_EQ(events,
           std::vector<std::string>({"host.capture_off", "host.input_off",
                                     "host.io.cancel", "host.drain",
                                     "host.destroy"}));
  CHECK_EQ(pair.host.send_video_frame(std::vector<std::uint8_t>{0x32}, 32, false),
           VideoSendStatus::SessionFailed);
}

void test_device_write_failure_uses_terminal_cleanup_and_cancels_io() {
  std::vector<std::string> events;
  RecordingBoundary boundary;
  boundary.events = &events;
  SessionPair pair(default_host_config(), default_device_config(), &boundary,
                   nullptr, nullptr, &events);
  drive_to_streaming_with_first_idr(pair);
  events.clear();

  pair.device_to_host.fail_writes = true;
  ControlMessage pause;
  pause.opcode = ControlOpcode::Pause;
  pause.config_id = 1;
  append_control(pair.host_to_device, 0x88888888, 0, 300, pause);
  pair.device.pump();

  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.device.failure()),
           static_cast<int>(SessionFailure::IoError));
  CHECK_EQ(boundary.release_calls, 1);
  CHECK_EQ(boundary.drain_calls, 1);
  CHECK_EQ(events,
           std::vector<std::string>({"device.io.cancel", "device.drain",
                                     "device.release"}));
}

void test_device_replays_config_status_without_repeating_callbacks() {
  RecordingBoundary boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary);
  drive_to_streaming_with_first_idr(pair);
  CHECK_EQ(boundary.prepare_calls, 1);
  CHECK_EQ(boundary.commit_calls, 1);
  const StreamProfile profile{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};

  ControlMessage duplicate_prepare;
  duplicate_prepare.opcode = ControlOpcode::StreamPrepare;
  duplicate_prepare.proto_rev = deskmate::protocol::kProtocolRevision;
  duplicate_prepare.config_id = 1;
  const auto profile_body =
      deskmate::protocol::encode_stream_profile(profile);
  CHECK(profile_body);
  duplicate_prepare.body = profile_body.value();
  append_control(pair.host_to_device, 0x88888888, 0, 200, duplicate_prepare);
  pair.device.pump();
  CHECK_EQ(boundary.prepare_calls, 1);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));

  ControlMessage duplicate_commit;
  duplicate_commit.opcode = ControlOpcode::StreamCommit;
  duplicate_commit.config_id = 1;
  duplicate_commit.body = {0x77, 0x77, 0x77, 0x77};
  append_control(pair.host_to_device, 0x88888888, 0x77777777, 201,
                 duplicate_commit);
  pair.device.pump();
  CHECK_EQ(boundary.commit_calls, 1);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));

  boundary.prepare_result = BoundaryResult::Failed;
  ControlMessage failed_prepare = duplicate_prepare;
  failed_prepare.config_id = 2;
  append_control(pair.host_to_device, 0x88888888, 0, 202, failed_prepare);
  pair.device.pump();
  CHECK_EQ(boundary.prepare_calls, 2);
  CHECK_EQ(boundary.discard_calls, 0);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));

  append_control(pair.host_to_device, 0x88888888, 0, 203, failed_prepare);
  pair.device.pump();
  CHECK_EQ(boundary.prepare_calls, 2);
  CHECK_EQ(boundary.discard_calls, 0);
  CHECK_EQ(boundary.rollback_calls, 0);
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
}

void test_duplicate_commit_requires_matching_header_stream() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const auto count_committed = [&pair] {
    const auto controls = decode_controls(pair.device_to_host);
    return std::count_if(controls.begin(), controls.end(),
                         [](const ControlMessage& control) {
                           return control.opcode ==
                                  ControlOpcode::StreamCommitted;
                         });
  };
  const auto committed_before = count_committed();

  ControlMessage duplicate_commit;
  duplicate_commit.opcode = ControlOpcode::StreamCommit;
  duplicate_commit.config_id = 1;
  duplicate_commit.body = {0x77, 0x77, 0x77, 0x77};
  append_control(pair.host_to_device, 0x88888888, 0, 310, duplicate_commit);
  pair.device.pump();

  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.device.failure()),
           static_cast<int>(SessionFailure::UnexpectedMessage));
  const auto controls = decode_controls(pair.device_to_host);
  CHECK_EQ(count_committed(), committed_before);
}

void test_duplicate_commit_rejects_config_generation_rollback() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);

  ControlMessage prepare;
  prepare.opcode = ControlOpcode::StreamPrepare;
  prepare.proto_rev = deskmate::protocol::kProtocolRevision;
  prepare.config_id = 2;
  const auto body = deskmate::protocol::encode_stream_profile(StreamProfile{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}});
  CHECK(body);
  prepare.body = body.value();
  append_control(pair.host_to_device, 0x88888888, 0, 311, prepare);
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Reconfiguring));
  const auto count_committed = [&pair] {
    const auto controls = decode_controls(pair.device_to_host);
    return std::count_if(controls.begin(), controls.end(),
                         [](const ControlMessage& control) {
                           return control.opcode ==
                                  ControlOpcode::StreamCommitted;
                         });
  };
  const auto committed_before = count_committed();

  ControlMessage old_commit;
  old_commit.opcode = ControlOpcode::StreamCommit;
  old_commit.config_id = 1;
  old_commit.body = {0x77, 0x77, 0x77, 0x77};
  append_control(pair.host_to_device, 0x88888888, 0x77777777, 312, old_commit);
  pair.device.pump();

  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.device.failure()),
           static_cast<int>(SessionFailure::UnexpectedMessage));
  CHECK_EQ(count_committed(), committed_before);
}

void test_reconfiguration_decoder_rejects_old_stream_p_frames() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);

  const StreamProfile replacement{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  ControlMessage prepare;
  prepare.opcode = ControlOpcode::StreamPrepare;
  prepare.proto_rev = deskmate::protocol::kProtocolRevision;
  prepare.config_id = 2;
  const auto profile_body =
      deskmate::protocol::encode_stream_profile(replacement);
  CHECK(profile_body);
  prepare.body = profile_body.value();
  append_control(pair.host_to_device, 0x88888888, 0, 320, prepare);
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Reconfiguring));

  ControlMessage commit;
  commit.opcode = ControlOpcode::StreamCommit;
  commit.config_id = 2;
  commit.body = {0x01, 0xEF, 0xCD, 0xAB};
  append_control(pair.host_to_device, 0x88888888, 0xABCDEF01, 321, commit);
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));

  append_video(pair.host_to_device, 0x88888888, 0xABCDEF01, 2,
               {0x20, 0x21}, deskmate::protocol::kFlagKeyFrame);
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK_EQ(pair.device.take_video_frames().size(), 1U);

  append_video(pair.host_to_device, 0x88888888, 0x77777777, 3, {0x30}, 0);
  pair.device.pump();
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  CHECK(!pair.device.is_terminal());
  CHECK(pair.device.take_video_frames().empty());
  CHECK(pair.device.last_good_frame().has_value());
  CHECK_EQ(pair.device.last_good_frame()->header.seq, 2U);

  append_video(pair.host_to_device, 0x88888888, 0xABCDEF01, 3, {0x40}, 0);
  pair.device.pump();
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].header.seq, 3U);
  CHECK_EQ(frames[0].payload, std::vector<std::uint8_t>({0x40}));
}

void test_unknown_control_opcode_is_discarded_without_terminating() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);

  std::vector<std::uint8_t> unknown_control(10, 0);
  unknown_control[0] = 0x7F;
  append_packet(pair.host_to_device, 0x88888888, 0, 2,
                deskmate::protocol::MessageType::Control,
                std::move(unknown_control));
  append_video(pair.host_to_device, 0x88888888, 0x77777777, 2, {0x51}, 0);
  pair.device.pump();

  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Streaming));
  const auto frames = pair.device.take_video_frames();
  CHECK_EQ(frames.size(), 1U);
  CHECK_EQ(frames[0].payload, std::vector<std::uint8_t>({0x51}));
}

void test_malformed_control_payload_terminates_session() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);

  append_packet(pair.host_to_device, 0x88888888, 0, 2,
                deskmate::protocol::MessageType::Control,
                std::vector<std::uint8_t>(9, 0));
  pair.device.pump();

  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.device.failure()),
           static_cast<int>(SessionFailure::MalformedControlMessage));
}

void test_paused_resume_rollback_keeps_both_ends_paused_and_resumable() {
  SessionPair pair(default_host_config(), default_device_config());
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  CHECK_EQ(pair.host.pause(), SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Paused));

  const StreamProfile profile{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  constexpr std::uint32_t kCandidateConfigId = 2;
  constexpr std::uint32_t kCandidateStreamId = 0x60000023;
  CHECK_EQ(pair.host.resume(profile, kCandidateConfigId, kCandidateStreamId),
           SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::AwaitingFirstIdr));

  // No candidate IDR: the Device times out and rolls back. The Host must roll
  // back to Paused as well, because the resume started from Paused.
  pair.clock.advance_ns(deskmate::protocol::kHandshakeStageTimeoutNs);
  for (int cycle = 0; cycle < 4; ++cycle) {
    pair.host.pump();
    pair.device.pump();
  }

  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Paused));
  CHECK(*pair.host.negotiated_stream() == old_stream);
  CHECK(*pair.device.negotiated_stream() == old_stream);

  // A second resume from the rolled-back Paused state must be accepted by both
  // ends.
  CHECK_EQ(pair.host.resume(profile, 3, 0x60000024),
           SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK(!pair.host.is_terminal());
  CHECK(!pair.device.is_terminal());
  CHECK(!pair.device.failure().has_value());
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Reconfiguring));
  CHECK(pair.device.state() == SessionState::Reconfiguring ||
        pair.device.state() == SessionState::AwaitingFirstIdr);
}

void test_resume_failure_returns_to_paused_and_preserves_grace_and_old_stream() {
  RecordingBoundary boundary;
  RecordingHostBoundary host_boundary;
  SessionPair pair(default_host_config(), default_device_config(), &boundary,
                   &host_boundary);
  drive_to_streaming_with_first_idr(pair);
  const NegotiatedStream old_stream = *pair.host.negotiated_stream();
  CHECK_EQ(pair.host.pause(), SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();
  CHECK(pair.host.pause_acknowledged());

  boundary.prepare_result = BoundaryResult::Failed;
  const StreamProfile profile{
      VideoEncoder::H264, PixelFormat::Nv12, DisplayResolution{1920, 1200, 60}};
  CHECK_EQ(pair.host.resume(profile, 2, 0x33334444),
           SessionCommandStatus::Ok);
  pair.host.pump();
  pair.device.pump();
  pair.host.pump();

  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Paused));
  CHECK_EQ(static_cast<int>(pair.device.state()),
           static_cast<int>(SessionState::Paused));
  CHECK(pair.host.negotiated_stream().has_value());
  CHECK(*pair.host.negotiated_stream() == old_stream);
  CHECK(pair.device.negotiated_stream().has_value());
  CHECK(*pair.device.negotiated_stream() == old_stream);
  CHECK(!host_boundary.capture_encoding_intent);
  CHECK_EQ(static_cast<int>(pair.host.reconfiguration()->phase),
           static_cast<int>(ReconfigurationPhase::Failed));

  pair.clock.advance_ns(deskmate::protocol::kMinPauseGraceTimeoutNs);
  pair.host.pump();
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Failed));
  CHECK_EQ(static_cast<int>(*pair.host.failure()),
           static_cast<int>(SessionFailure::PauseGraceExpired));
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

void test_host_accepts_device_native_resolution_and_codec_preference() {
  HostSessionConfig host_config = default_host_config();
  host_config.accept_device_resolution = true;
  host_config.capabilities = CapabilitySet{
      {VideoEncoder::Hevc, VideoEncoder::H264},
      {PixelFormat::Nv12},
      {{1920, 1200, 60}},
  };

  DeviceSessionConfig device_config = default_device_config();
  device_config.capabilities = CapabilitySet{
      {VideoEncoder::H264},
      {PixelFormat::Nv12},
      {{3120, 2080, 120}, {2800, 1840, 60}},
  };

  SessionPair pair(host_config, device_config);
  drive_to_committed(pair);
  CHECK_EQ(static_cast<int>(pair.host.state()),
           static_cast<int>(SessionState::Streaming));
  const auto negotiated = pair.host.negotiated_stream();
  CHECK(negotiated.has_value());
  CHECK_EQ(static_cast<int>(negotiated->profile.encoder),
           static_cast<int>(VideoEncoder::H264));
  CHECK_EQ(negotiated->profile.resolution, (DisplayResolution{3120, 2080, 120}));
}

void test_custom_initial_idr_timeout_outlives_stage_timeout() {
  {
    DeviceSessionConfig device_config = default_device_config();
    device_config.limits.stage_timeout_ns = 2'000'000'000ULL;
    device_config.limits.initial_idr_timeout_ns = 10'000'000'000ULL;

    SessionPair pair(default_host_config(), device_config);
    drive_to_committed(pair);
    CHECK_EQ(static_cast<int>(pair.device.state()),
             static_cast<int>(SessionState::AwaitingFirstIdr));

    for (int second = 0; second < 5; ++second) {
      pair.clock.advance_ns(1'000'000'000ULL);
      pair.pump();
    }
    CHECK_EQ(static_cast<int>(pair.host.state()),
             static_cast<int>(SessionState::Streaming));
    CHECK_EQ(static_cast<int>(pair.device.state()),
             static_cast<int>(SessionState::AwaitingFirstIdr));

    CHECK_EQ(pair.host.send_video_frame(
                 std::vector<std::uint8_t>{0xDE, 0xAD, 0xBE, 0xEF}, 5000, true),
             VideoSendStatus::Sent);
    pair.host.pump();
    pair.device.pump();
    CHECK_EQ(static_cast<int>(pair.device.state()),
             static_cast<int>(SessionState::Streaming));
    CHECK_EQ(pair.device.take_video_frames().size(), 1U);
  }
  {
    DeviceSessionConfig device_config = default_device_config();
    device_config.limits.stage_timeout_ns = 2'000'000'000ULL;
    device_config.limits.initial_idr_timeout_ns = 10'000'000'000ULL;

    SessionPair pair(default_host_config(), device_config);
    drive_to_committed(pair);
    for (int second = 0; second < 9; ++second) {
      pair.clock.advance_ns(1'000'000'000ULL);
      pair.pump();
    }
    CHECK_EQ(static_cast<int>(pair.device.state()),
             static_cast<int>(SessionState::AwaitingFirstIdr));

    pair.clock.advance_ns(1'000'000'000ULL);
    pair.device.pump();
    CHECK_EQ(static_cast<int>(pair.device.state()),
             static_cast<int>(SessionState::Failed));
    CHECK(pair.device.failure().has_value());
    CHECK_EQ(static_cast<int>(*pair.device.failure()),
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
      {"multifragment congestion IDR delivered one fragment per pump does not loop",
       test_multifragment_congestion_idr_delivered_one_fragment_per_pump_does_not_loop},
      {"device rejects prepare revision mismatch",
       test_device_rejects_prepare_revision_mismatch},
      {"different config id does not repeat prepare or switch stream",
       test_different_config_id_does_not_repeat_prepare_or_switch_stream},
      {"display error during commit is explicit",
       test_display_error_during_commit_is_explicit},
      {"session payload helpers round trip and reject bad encodings",
       test_session_payload_helpers_round_trip_and_reject_bad_encodings},
      {"pause acks immediately stops host intent and resume renegotiates",
       test_pause_acks_immediately_stops_host_intent_and_resume_renegotiates},
      {"heartbeat emits each second and three second silence fails",
       test_heartbeat_emits_each_second_and_three_second_silence_fails},
      {"streaming reconfiguration keeps old until committed and is idempotent",
       test_streaming_reconfiguration_keeps_old_until_committed_and_is_idempotent},
      {"reconfiguration failure keeps old stream",
       test_reconfiguration_failure_keeps_old_stream},
      {"device does not final ack a rolled back candidate",
       test_device_does_not_final_ack_a_rolled_back_candidate},
      {"device initial idr timeout rolls back without terminating host",
       test_device_initial_idr_timeout_rolls_back_without_terminating_host},
      {"host waits for device idr timeout before rollback",
       test_host_waits_for_device_idr_timeout_before_rollback},
      {"candidate frames are discarded when reconfiguration times out",
       test_candidate_frames_are_discarded_when_reconfiguration_times_out},
      {"duplicate commit before first idr does not confirm switch",
       test_duplicate_commit_before_first_idr_does_not_confirm_switch},
      {"late ack from failed configuration is ignored",
       test_late_ack_from_failed_configuration_is_ignored},
      {"duplicate committed after switch is idempotent",
       test_duplicate_committed_after_switch_is_idempotent},
      {"duplicate stream ready while awaiting first idr is idempotent",
       test_duplicate_stream_ready_while_awaiting_first_idr_is_idempotent},
      {"duplicate first commit ack does not complete reconfiguration",
       test_duplicate_first_commit_ack_does_not_complete_reconfiguration},
      {"lost first commit ack is retried without terminating",
       test_lost_first_commit_ack_is_retried_without_terminating},
      {"lost final commit ack is retried until switch is confirmed",
       test_lost_final_commit_ack_is_retried_until_switch_is_confirmed},
      {"each running reconfiguration stage timeout or failure preserves old stream",
       test_each_running_reconfiguration_stage_timeout_or_failure_preserves_old_stream},
      {"four timestamp clock estimator reports offset rtt drift and bound",
       test_four_timestamp_clock_estimator_reports_offset_rtt_drift_and_bound},
      {"clock exchange over session messages produces estimate",
       test_clock_exchange_over_session_messages_produces_estimate},
      {"lock and unlock publish distinct states and require resume",
       test_lock_and_unlock_publish_distinct_states_and_require_resume},
      {"lock during handshake stays paused and blocks resume",
       test_lock_during_handshake_stays_paused_and_blocks_resume},
      {"lock during reconfiguration keeps old stream and pauses",
       test_lock_during_reconfiguration_keeps_old_stream_and_pauses},
      {"lock after candidate idr waits for reconfiguration verdict",
       test_lock_after_candidate_idr_waits_for_reconfiguration_verdict},
      {"lock after candidate idr started preserves it until final ack",
       test_lock_after_candidate_idr_started_preserves_it_until_final_ack},
      {"lock before candidate idr starts aborts reconfiguration",
       test_lock_before_candidate_idr_starts_aborts_reconfiguration},
      {"display error before candidate idr keeps both ends paused",
       test_display_error_before_candidate_idr_keeps_both_ends_paused},
      {"display error after candidate idr started preserves it",
       test_display_error_after_candidate_idr_started_preserves_it},
      {"rollback discards active candidate packet before old stream",
       test_rollback_discards_active_candidate_packet_before_old_stream},
      {"rollback waits for old stream keyframe before p frames",
       test_rollback_waits_for_old_stream_keyframe_before_p_frames},
      {"screen recording and accessibility revocations degrade independently",
       test_screen_recording_and_accessibility_revocations_degrade_independently},
      {"display error is a control preserving degraded state",
       test_display_error_is_a_control_preserving_degraded_state},
      {"pause grace timeout destroys display once",
       test_pause_grace_timeout_destroys_display_once},
      {"pause grace timeout must be between thirty and sixty seconds",
       test_pause_grace_timeout_must_be_between_thirty_and_sixty_seconds},
      {"surface lease tracks generation references and destruction",
       test_surface_lease_tracks_generation_references_and_destruction},
      {"surface lease outlives owner with shared reference ops",
       test_surface_lease_outlives_owner_with_shared_reference_ops},
      {"surface lease invalidates raw ops when owner is destroyed",
       test_surface_lease_invalidates_raw_ops_when_owner_is_destroyed},
      {"borrowed accessory fd never closes on destruction",
       test_borrowed_accessory_fd_never_closes_on_destruction},
      {"input payload round trips and rejects malformed bodies",
       test_input_payload_round_trips_and_rejects_malformed_bodies},
      {"device input reaches host only while streaming and permitted",
       test_device_input_reaches_host_only_while_streaming_and_permitted},
      {"input from a stale stream generation is dropped",
       test_input_from_a_stale_stream_generation_is_dropped},
      {"device stops output after surface destroy and uses new generation",
       test_device_stops_output_after_surface_destroy_and_uses_new_generation},
      {"stopping rejects work then cancels drains and releases in order",
       test_stopping_rejects_work_then_cancels_drains_and_releases_in_order},
      {"stopping discards inflight video work without waiting",
       test_stopping_discards_inflight_video_work_without_waiting},
      {"stopping does not flush queued outbound control",
       test_stopping_does_not_flush_queued_outbound_control},
      {"sleep uses stop order and never closes borrowed fd",
       test_sleep_uses_stop_order_and_never_closes_borrowed_fd},
      {"heartbeat timeout releases established session resources",
       test_heartbeat_timeout_releases_established_session_resources},
      {"fatal timeout cancels io before drain and release",
       test_fatal_timeout_cancels_io_before_drain_and_release},
      {"host write failure uses terminal cleanup and cancels io",
       test_host_write_failure_uses_terminal_cleanup_and_cancels_io},
      {"device write failure uses terminal cleanup and cancels io",
       test_device_write_failure_uses_terminal_cleanup_and_cancels_io},
      {"device replays config status without repeating callbacks",
       test_device_replays_config_status_without_repeating_callbacks},
      {"duplicate commit requires matching header stream",
       test_duplicate_commit_requires_matching_header_stream},
      {"duplicate commit rejects config generation rollback",
       test_duplicate_commit_rejects_config_generation_rollback},
      {"reconfiguration decoder rejects old stream p frames",
       test_reconfiguration_decoder_rejects_old_stream_p_frames},
      {"unknown control opcode is discarded without terminating",
       test_unknown_control_opcode_is_discarded_without_terminating},
      {"malformed control payload terminates session",
       test_malformed_control_payload_terminates_session},
      {"paused resume rollback keeps both ends paused and resumable",
       test_paused_resume_rollback_keeps_both_ends_paused_and_resumable},
      {"resume failure returns to paused and preserves grace and old stream",
       test_resume_failure_returns_to_paused_and_preserves_grace_and_old_stream},
      {"host accepts device native resolution and codec preference",
       test_host_accepts_device_native_resolution_and_codec_preference},
      {"custom initial idr timeout outlives stage timeout",
       test_custom_initial_idr_timeout_outlives_stage_timeout},
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
