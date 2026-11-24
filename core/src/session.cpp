#include "deskmate/session.hpp"

#include "deskmate/message_stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace deskmate::protocol {
namespace {

constexpr std::uint8_t kCapabilityEncodingVersion = 1;
constexpr std::uint8_t kProfileEncodingVersion = 1;

std::vector<std::uint8_t> write_u32(std::uint32_t value) {
  std::vector<std::uint8_t> bytes(4);
  for (std::size_t index = 0; index < 4; ++index) {
    bytes[index] = static_cast<std::uint8_t>((value >> (index * 8U)) & 0xFFU);
  }
  return bytes;
}

std::uint32_t read_u16(std::span<const std::uint8_t> bytes, std::size_t offset) {
  return static_cast<std::uint16_t>(
      static_cast<std::uint16_t>(bytes[offset]) |
      (static_cast<std::uint16_t>(bytes[offset + 1]) << 8U));
}

std::uint32_t read_u32(std::span<const std::uint8_t> bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
  }
  return value;
}

void append_u16(std::vector<std::uint8_t>& bytes, std::uint16_t value) {
  bytes.push_back(static_cast<std::uint8_t>(value & 0xFFU));
  bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xFFU));
}

ProtocolError local_error(ErrorCode code, std::string_view detail) {
  return ProtocolError{code, ErrorAction::RejectLocalArgument, detail};
}

ProtocolError wire_error(ErrorCode code, std::string_view detail) {
  return ProtocolError{code, ErrorAction::DiscardMessage, detail};
}

template <typename T>
bool contains_value(const std::vector<T>& values, T target) {
  return std::find(values.begin(), values.end(), target) != values.end();
}

template <typename T>
bool has_valid_tags(const std::vector<T>& values, std::uint8_t maximum) {
  for (const T value : values) {
    if (static_cast<std::uint8_t>(value) == 0 ||
        static_cast<std::uint8_t>(value) > maximum) {
      return false;
    }
  }
  return true;
}

bool supports_profile(const CapabilitySet& capabilities, const StreamProfile& profile) {
  return contains_value(capabilities.encoders, profile.encoder) &&
         contains_value(capabilities.pixel_formats, profile.pixel_format) &&
         contains_value(capabilities.resolutions, profile.resolution);
}

std::optional<StreamProfile> intersect_capabilities(const CapabilitySet& host,
                                                    const CapabilitySet& device) {
  StreamProfile selected;
  bool have_encoder = false;
  bool have_pixel_format = false;
  bool have_resolution = false;

  for (const VideoEncoder encoder : host.encoders) {
    if (contains_value(device.encoders, encoder)) {
      selected.encoder = encoder;
      have_encoder = true;
      break;
    }
  }
  for (const PixelFormat format : host.pixel_formats) {
    if (contains_value(device.pixel_formats, format)) {
      selected.pixel_format = format;
      have_pixel_format = true;
      break;
    }
  }
  for (const DisplayResolution& resolution : host.resolutions) {
    if (contains_value(device.resolutions, resolution)) {
      selected.resolution = resolution;
      have_resolution = true;
      break;
    }
  }
  if (!have_encoder || !have_pixel_format || !have_resolution) {
    return std::nullopt;
  }
  return selected;
}

std::optional<SessionFailure> decode_failure_body(
    const std::vector<std::uint8_t>& body) {
  if (body.size() != 1) {
    return std::nullopt;
  }
  const auto raw = static_cast<SessionFailure>(body[0]);
  if (raw < SessionFailure::UnsupportedProtocolRevision ||
      raw > SessionFailure::InvalidWireData) {
    return std::nullopt;
  }
  return raw;
}

std::optional<std::uint32_t> decode_stream_id_body(
    const std::vector<std::uint8_t>& body) {
  if (body.size() != 4) {
    return std::nullopt;
  }
  const std::uint32_t value = read_u32(body, 0);
  if (value == 0) {
    return std::nullopt;
  }
  return value;
}

std::uint64_t saturating_add(std::uint64_t left, std::uint64_t right) {
  if (right > std::numeric_limits<std::uint64_t>::max() - left) {
    return std::numeric_limits<std::uint64_t>::max();
  }
  return left + right;
}

class EndpointCore {
 public:
  EndpointCore(ByteIo& io, MonotonicClock& clock, SessionLimits limits,
               std::uint32_t expected_session_nonce)
      : io_(io),
        clock_(clock),
        limits_(limits),
        decoder_(StreamDecoderConfig{expected_session_nonce, 0}) {}

  EndpointCore(const EndpointCore&) = delete;
  EndpointCore& operator=(const EndpointCore&) = delete;
  virtual ~EndpointCore() = default;

  void pump() {
    if (!started_ && state_ == SessionState::Idle) {
      return;
    }
    flush();
    if (state_ == SessionState::Failed) {
      flush();
      return;
    }
    if (deadline_ns_.has_value() && clock_.now_ns() >= *deadline_ns_) {
      fail_with(*timeout_failure_, true);
      flush();
      return;
    }

    std::array<std::uint8_t, 4096> buffer{};
    while (state_ != SessionState::Failed) {
      const IoResult result = io_.read(buffer);
      if (result.status == IoStatus::WouldBlock) {
        break;
      }
      if (result.status == IoStatus::Closed) {
        fail_with(SessionFailure::IoError, false);
        break;
      }
      if (result.status == IoStatus::Error || result.bytes == 0 ||
          result.bytes > buffer.size()) {
        fail_with(SessionFailure::IoError, false);
        break;
      }

      auto events = decoder_.feed(std::span<const std::uint8_t>(buffer.data(), result.bytes));
      for (auto& event : events) {
        if (state_ == SessionState::Failed) {
          break;
        }
        process_event(std::move(event));
      }
    }
    flush();
  }

  [[nodiscard]] SessionState state() const { return state_; }
  [[nodiscard]] bool is_terminal() const { return state_ == SessionState::Failed; }
  [[nodiscard]] std::optional<SessionFailure> failure() const { return failure_; }
  [[nodiscard]] std::string_view failure_detail() const {
    return failure_.has_value() ? session_failure_name(*failure_) : std::string_view{};
  }
  [[nodiscard]] std::uint32_t session_nonce() const { return session_nonce_; }
  [[nodiscard]] const std::optional<NegotiatedStream>& negotiated_stream() const {
    return negotiated_;
  }

 protected:
  void mark_started(SessionState state, SessionFailure timeout_failure) {
    if (started_ || state_ != SessionState::Idle) {
      return;
    }
    started_ = true;
    state_ = state;
    arm_timeout(timeout_failure);
  }

  [[nodiscard]] bool started() const { return started_; }

  void transition(SessionState state, std::optional<SessionFailure> timeout_failure) {
    state_ = state;
    if (timeout_failure.has_value()) {
      arm_timeout(*timeout_failure);
    } else {
      disarm_timeout();
    }
  }

  void set_session_nonce(std::uint32_t nonce, std::uint32_t video_stream_id = 0) {
    session_nonce_ = nonce;
    decoder_.reset(StreamDecoderConfig{nonce, video_stream_id});
  }

  void reset_decoder_stream(std::uint32_t video_stream_id) {
    decoder_.reset(StreamDecoderConfig{session_nonce_, video_stream_id});
  }

  void set_negotiated_stream(NegotiatedStream stream) {
    negotiated_ = std::move(stream);
  }

  void fail_with(SessionFailure failure, bool send_wire_error) {
    if (state_ == SessionState::Failed) {
      return;
    }
    state_ = SessionState::Failed;
    failure_ = failure;
    disarm_timeout();
    if (send_wire_error && session_nonce_ != 0) {
      queue_control(ControlOpcode::ProtocolError, 0, 0,
                    {static_cast<std::uint8_t>(failure)}, 0);
    }
  }

  void queue_control(ControlOpcode opcode, std::uint32_t proto_rev,
                     std::uint32_t config_id, std::vector<std::uint8_t> body,
                     std::uint32_t header_video_stream_id) {
    ControlMessage message;
    message.opcode = opcode;
    message.proto_rev = proto_rev;
    message.config_id = config_id;
    message.body = std::move(body);
    const auto payload = encode_control_payload(message);
    if (!payload) {
      fail_with(SessionFailure::InvalidConfiguration, false);
      return;
    }

    Packet packet;
    packet.header.type = static_cast<std::uint8_t>(MessageType::Control);
    packet.header.session_nonce = session_nonce_;
    packet.header.video_stream_id = header_video_stream_id;
    packet.header.seq = next_control_seq_++;
    packet.header.payload_len = static_cast<std::uint32_t>(payload.value().size());
    packet.payload = payload.value();
    const auto encoded = encode_packet(packet);
    if (!encoded) {
      fail_with(SessionFailure::InvalidConfiguration, false);
      return;
    }
    queue_bytes(std::move(encoded.value()));
  }

  void queue_bytes(std::vector<std::uint8_t> bytes) {
    if (!bytes.empty()) {
      outbound_.push_back(std::move(bytes));
    }
  }

  [[nodiscard]] std::uint32_t next_video_seq() { return next_video_seq_++; }

  virtual void process_control(const Header& header,
                               const ControlMessage& message) = 0;
  virtual void process_video(const Header& header,
                             std::vector<std::uint8_t> payload) = 0;

 private:
  void arm_timeout(SessionFailure failure) {
    timeout_failure_ = failure;
    deadline_ns_ = saturating_add(clock_.now_ns(), limits_.stage_timeout_ns);
  }

  void disarm_timeout() {
    deadline_ns_.reset();
    timeout_failure_.reset();
  }

  void process_event(StreamEvent event) {
    if (event.kind == StreamEventKind::StreamError) {
      fail_with(SessionFailure::InvalidWireData, false);
      return;
    }
    if (event.kind == StreamEventKind::RejectedMessage) {
      return;
    }
    if (event.kind == StreamEventKind::VideoFrame) {
      if (session_nonce_ != 0 && event.header.session_nonce != session_nonce_) {
        return;
      }
      process_video(event.header, std::move(event.payload));
      return;
    }
    if (event.kind != StreamEventKind::Message) {
      return;
    }
    if (session_nonce_ != 0 && event.header.session_nonce != session_nonce_) {
      return;
    }
    if (event.header.type != static_cast<std::uint8_t>(MessageType::Control)) {
      return;
    }

    const auto decoded = decode_control_payload(event.payload);
    if (!decoded) {
      fail_with(SessionFailure::MalformedControlMessage, true);
      return;
    }
    process_control(event.header, decoded.value());
  }

  void flush() {
    while (!outbound_.empty()) {
      auto& packet = outbound_.front();
      const std::span<const std::uint8_t> remaining(packet.data() + write_offset_,
                                                   packet.size() - write_offset_);
      const IoResult result = io_.write(remaining);
      if (result.status == IoStatus::WouldBlock) {
        return;
      }
      if (result.status != IoStatus::Ok || result.bytes == 0 ||
          result.bytes > remaining.size()) {
        state_ = SessionState::Failed;
        failure_ = SessionFailure::IoError;
        disarm_timeout();
        outbound_.clear();
        write_offset_ = 0;
        return;
      }
      write_offset_ += result.bytes;
      if (write_offset_ == packet.size()) {
        outbound_.pop_front();
        write_offset_ = 0;
      }
    }
  }

  ByteIo& io_;
  MonotonicClock& clock_;
  SessionLimits limits_;
  MessageStreamDecoder decoder_;
  SessionState state_{SessionState::Idle};
  std::optional<SessionFailure> failure_;
  std::optional<SessionFailure> timeout_failure_;
  std::optional<std::uint64_t> deadline_ns_;
  std::optional<NegotiatedStream> negotiated_;
  std::deque<std::vector<std::uint8_t>> outbound_;
  std::size_t write_offset_{};
  std::uint32_t session_nonce_{};
  std::uint32_t next_control_seq_{1};
  std::uint32_t next_video_seq_{1};
  bool started_{};
};

bool validate_limits(const SessionLimits& limits) {
  return limits.stage_timeout_ns != 0 && limits.video_fragment_payload != 0 &&
         limits.video_fragment_payload <= kMaxPacketPayload;
}

bool validate_capabilities_for_encode(const CapabilitySet& capabilities) {
  return capabilities.encoders.size() <= kMaxCapabilityItems &&
         capabilities.pixel_formats.size() <= kMaxCapabilityItems &&
         capabilities.resolutions.size() <= kMaxCapabilityItems &&
         has_valid_tags(capabilities.encoders,
                        static_cast<std::uint8_t>(VideoEncoder::Hevc)) &&
         has_valid_tags(capabilities.pixel_formats,
                        static_cast<std::uint8_t>(PixelFormat::Bgra8)) &&
         std::all_of(capabilities.resolutions.begin(),
                     capabilities.resolutions.end(),
                     [](const DisplayResolution& resolution) {
                       return resolution.width != 0 && resolution.height != 0 &&
                              resolution.refresh_hz != 0;
                     });
}

}  // namespace

std::string_view session_failure_name(SessionFailure failure) {
  switch (failure) {
    case SessionFailure::None:
      return "none";
    case SessionFailure::UnsupportedProtocolRevision:
      return "unsupported_protocol_revision";
    case SessionFailure::NoCommonCapabilities:
      return "no_common_capabilities";
    case SessionFailure::InvalidConfiguration:
      return "invalid_configuration";
    case SessionFailure::MalformedControlMessage:
      return "malformed_control_message";
    case SessionFailure::UnexpectedMessage:
      return "unexpected_message";
    case SessionFailure::HelloTimeout:
      return "hello_timeout";
    case SessionFailure::ReadyTimeout:
      return "ready_timeout";
    case SessionFailure::CommittedTimeout:
      return "committed_timeout";
    case SessionFailure::PrepareTimeout:
      return "prepare_timeout";
    case SessionFailure::CommitTimeout:
      return "commit_timeout";
    case SessionFailure::InitialIdrTimeout:
      return "initial_idr_timeout";
    case SessionFailure::PrepareFailed:
      return "prepare_failed";
    case SessionFailure::CommitFailed:
      return "commit_failed";
    case SessionFailure::DisplayError:
      return "display_error";
    case SessionFailure::IoError:
      return "io_error";
    case SessionFailure::InvalidWireData:
      return "invalid_wire_data";
  }
  return "unknown";
}

std::string_view session_state_name(SessionState state) {
  switch (state) {
    case SessionState::Idle:
      return "idle";
    case SessionState::AwaitingClientHello:
      return "awaiting_client_hello";
    case SessionState::AwaitingPrepare:
      return "awaiting_prepare";
    case SessionState::AwaitingReady:
      return "awaiting_ready";
    case SessionState::AwaitingCommit:
      return "awaiting_commit";
    case SessionState::AwaitingCommitted:
      return "awaiting_committed";
    case SessionState::AwaitingFirstIdr:
      return "awaiting_first_idr";
    case SessionState::Streaming:
      return "streaming";
    case SessionState::Failed:
      return "failed";
  }
  return "unknown";
}

Result<std::vector<std::uint8_t>> encode_capabilities(
    const CapabilitySet& capabilities) {
  if (!validate_capabilities_for_encode(capabilities)) {
    return Result<std::vector<std::uint8_t>>::failure(local_error(
        ErrorCode::InvalidLocalArgument,
        "capabilities contain invalid tags or exceed eight items per category"));
  }

  std::vector<std::uint8_t> bytes;
  bytes.reserve(4 + capabilities.encoders.size() +
                capabilities.pixel_formats.size() +
                capabilities.resolutions.size() * 6);
  bytes.push_back(kCapabilityEncodingVersion);
  bytes.push_back(static_cast<std::uint8_t>(capabilities.encoders.size()));
  for (const VideoEncoder encoder : capabilities.encoders) {
    bytes.push_back(static_cast<std::uint8_t>(encoder));
  }
  bytes.push_back(static_cast<std::uint8_t>(capabilities.pixel_formats.size()));
  for (const PixelFormat format : capabilities.pixel_formats) {
    bytes.push_back(static_cast<std::uint8_t>(format));
  }
  bytes.push_back(static_cast<std::uint8_t>(capabilities.resolutions.size()));
  for (const DisplayResolution& resolution : capabilities.resolutions) {
    append_u16(bytes, resolution.width);
    append_u16(bytes, resolution.height);
    append_u16(bytes, resolution.refresh_hz);
  }
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

Result<CapabilitySet> decode_capabilities(std::span<const std::uint8_t> bytes) {
  std::size_t offset = 0;
  if (bytes.size() < 4 || bytes[offset++] != kCapabilityEncodingVersion) {
    return Result<CapabilitySet>::failure(
        wire_error(ErrorCode::InvalidControlPayload,
                   "capability body has an unsupported encoding"));
  }

  const std::uint8_t encoder_count = bytes[offset++];
  if (encoder_count > kMaxCapabilityItems || offset + encoder_count > bytes.size()) {
    return Result<CapabilitySet>::failure(
        wire_error(ErrorCode::InvalidControlPayload,
                   "capability encoder list is out of bounds"));
  }
  CapabilitySet capabilities;
  for (std::size_t index = 0; index < encoder_count; ++index) {
    const auto encoder = static_cast<VideoEncoder>(bytes[offset++]);
    if (!has_valid_tags(std::vector<VideoEncoder>{encoder},
                        static_cast<std::uint8_t>(VideoEncoder::Hevc))) {
      return Result<CapabilitySet>::failure(
          wire_error(ErrorCode::InvalidControlPayload,
                     "capability body contains an unknown encoder"));
    }
    capabilities.encoders.push_back(encoder);
  }

  if (offset >= bytes.size()) {
    return Result<CapabilitySet>::failure(
        wire_error(ErrorCode::InvalidControlPayload,
                   "capability body is missing its pixel format list"));
  }
  const std::uint8_t format_count = bytes[offset++];
  if (format_count > kMaxCapabilityItems || offset + format_count > bytes.size()) {
    return Result<CapabilitySet>::failure(
        wire_error(ErrorCode::InvalidControlPayload,
                   "capability pixel format list is out of bounds"));
  }
  for (std::size_t index = 0; index < format_count; ++index) {
    const auto format = static_cast<PixelFormat>(bytes[offset++]);
    if (!has_valid_tags(std::vector<PixelFormat>{format},
                        static_cast<std::uint8_t>(PixelFormat::Bgra8))) {
      return Result<CapabilitySet>::failure(
          wire_error(ErrorCode::InvalidControlPayload,
                     "capability body contains an unknown pixel format"));
    }
    capabilities.pixel_formats.push_back(format);
  }

  if (offset >= bytes.size()) {
    return Result<CapabilitySet>::failure(
        wire_error(ErrorCode::InvalidControlPayload,
                   "capability body is missing its resolution list"));
  }
  const std::uint8_t resolution_count = bytes[offset++];
  if (resolution_count > kMaxCapabilityItems ||
      offset + resolution_count * 6U != bytes.size()) {
    return Result<CapabilitySet>::failure(
        wire_error(ErrorCode::InvalidControlPayload,
                   "capability resolution list is out of bounds"));
  }
  for (std::size_t index = 0; index < resolution_count; ++index) {
    DisplayResolution resolution;
    resolution.width = static_cast<std::uint16_t>(read_u16(bytes, offset));
    resolution.height = static_cast<std::uint16_t>(read_u16(bytes, offset + 2));
    resolution.refresh_hz = static_cast<std::uint16_t>(read_u16(bytes, offset + 4));
    offset += 6;
    if (resolution.width == 0 || resolution.height == 0 ||
        resolution.refresh_hz == 0) {
      return Result<CapabilitySet>::failure(
          wire_error(ErrorCode::InvalidControlPayload,
                     "capability body contains a zero display dimension"));
    }
    capabilities.resolutions.push_back(resolution);
  }
  return Result<CapabilitySet>::success(std::move(capabilities));
}

Result<std::vector<std::uint8_t>> encode_stream_profile(const StreamProfile& profile) {
  if (profile.encoder != VideoEncoder::H264 &&
      profile.encoder != VideoEncoder::Hevc) {
    return Result<std::vector<std::uint8_t>>::failure(
        local_error(ErrorCode::InvalidLocalArgument,
                    "stream profile has an unknown encoder"));
  }
  if (profile.pixel_format != PixelFormat::Nv12 &&
      profile.pixel_format != PixelFormat::P010 &&
      profile.pixel_format != PixelFormat::Bgra8) {
    return Result<std::vector<std::uint8_t>>::failure(
        local_error(ErrorCode::InvalidLocalArgument,
                    "stream profile has an unknown pixel format"));
  }
  if (profile.resolution.width == 0 || profile.resolution.height == 0 ||
      profile.resolution.refresh_hz == 0) {
    return Result<std::vector<std::uint8_t>>::failure(
        local_error(ErrorCode::InvalidLocalArgument,
                    "stream profile has a zero display dimension"));
  }

  std::vector<std::uint8_t> bytes;
  bytes.reserve(9);
  bytes.push_back(kProfileEncodingVersion);
  bytes.push_back(static_cast<std::uint8_t>(profile.encoder));
  bytes.push_back(static_cast<std::uint8_t>(profile.pixel_format));
  append_u16(bytes, profile.resolution.width);
  append_u16(bytes, profile.resolution.height);
  append_u16(bytes, profile.resolution.refresh_hz);
  return Result<std::vector<std::uint8_t>>::success(std::move(bytes));
}

Result<StreamProfile> decode_stream_profile(std::span<const std::uint8_t> bytes) {
  if (bytes.size() != 9 || bytes[0] != kProfileEncodingVersion) {
    return Result<StreamProfile>::failure(
        wire_error(ErrorCode::InvalidControlPayload,
                   "stream profile body has an unsupported encoding"));
  }
  StreamProfile profile;
  profile.encoder = static_cast<VideoEncoder>(bytes[1]);
  profile.pixel_format = static_cast<PixelFormat>(bytes[2]);
  profile.resolution.width = static_cast<std::uint16_t>(read_u16(bytes, 3));
  profile.resolution.height = static_cast<std::uint16_t>(read_u16(bytes, 5));
  profile.resolution.refresh_hz = static_cast<std::uint16_t>(read_u16(bytes, 7));
  if ((profile.encoder != VideoEncoder::H264 &&
       profile.encoder != VideoEncoder::Hevc) ||
      (profile.pixel_format != PixelFormat::Nv12 &&
       profile.pixel_format != PixelFormat::P010 &&
       profile.pixel_format != PixelFormat::Bgra8) ||
      profile.resolution.width == 0 || profile.resolution.height == 0 ||
      profile.resolution.refresh_hz == 0) {
    return Result<StreamProfile>::failure(
        wire_error(ErrorCode::InvalidControlPayload,
                   "stream profile contains an invalid value"));
  }
  return Result<StreamProfile>::success(profile);
}

struct HostSession::Impl final : EndpointCore {
  Impl(ByteIo& io, MonotonicClock& clock, HostSessionConfig config)
      : EndpointCore(io, clock, config.limits, 0),
        config_(std::move(config)) {}

  void start() {
    if (started()) {
      return;
    }
    if (!validate_limits(config_.limits) || config_.video_stream_id == 0 ||
        config_.initial_config_id == 0 || config_.protocol_revision == 0 ||
        !encode_capabilities(config_.capabilities)) {
      fail_with(SessionFailure::InvalidConfiguration, false);
      return;
    }
    mark_started(SessionState::AwaitingClientHello, SessionFailure::HelloTimeout);
  }

  void process_control(const Header& header, const ControlMessage& message) override {
    if (message.opcode == ControlOpcode::ProtocolError) {
      const auto failure = decode_failure_body(message.body);
      if (!failure.has_value()) {
        fail_with(SessionFailure::MalformedControlMessage, false);
      } else {
        fail_with(*failure, false);
      }
      return;
    }

    if (message.opcode == ControlOpcode::StreamReady &&
        state() == SessionState::AwaitingCommitted) {
      const auto ready_profile = decode_stream_profile(message.body);
      if (header.video_stream_id != 0 ||
          message.config_id != config_.initial_config_id ||
          !prepared_profile_.has_value() || !ready_profile ||
          !(ready_profile.value() == *prepared_profile_)) {
        fail_with(SessionFailure::UnexpectedMessage, true);
      }
      return;
    }

    switch (state()) {
      case SessionState::AwaitingClientHello:
        handle_client_hello(header, message);
        return;
      case SessionState::AwaitingReady:
        handle_stream_ready(header, message);
        return;
      case SessionState::AwaitingCommitted:
        handle_stream_committed(header, message);
        return;
      default:
        fail_with(SessionFailure::UnexpectedMessage, true);
        return;
    }
  }

  void process_video(const Header&, std::vector<std::uint8_t>) override {
    fail_with(SessionFailure::UnexpectedMessage, true);
  }

  VideoSendStatus send_video_frame(std::span<const std::uint8_t> payload,
                                   std::uint64_t timestamp_us, bool keyframe) {
    if (is_terminal()) {
      return VideoSendStatus::SessionFailed;
    }
    if (state() != SessionState::Streaming || !negotiated_stream().has_value()) {
      return VideoSendStatus::NotStreaming;
    }
    if (payload.empty()) {
      return VideoSendStatus::EmptyPayload;
    }
    if (!sent_initial_idr_ && !keyframe) {
      return VideoSendStatus::IdrRequired;
    }

    VideoFrameToSend frame;
    frame.session_nonce = session_nonce();
    frame.video_stream_id = config_.video_stream_id;
    frame.seq = next_video_seq();
    frame.timestamp_us = timestamp_us;
    frame.flags = keyframe ? kFlagKeyFrame : 0;
    frame.payload.assign(payload.begin(), payload.end());
    const auto fragments =
        encode_video_fragments(frame, config_.limits.video_fragment_payload);
    if (!fragments) {
      return VideoSendStatus::InvalidArgument;
    }
    for (const auto& fragment : fragments.value()) {
      queue_bytes(fragment);
    }
    if (keyframe) {
      sent_initial_idr_ = true;
    }
    return VideoSendStatus::Sent;
  }

  void handle_client_hello(const Header& header, const ControlMessage& message) {
    if (message.opcode != ControlOpcode::ClientHello ||
        header.video_stream_id != 0) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    if (message.proto_rev != config_.protocol_revision) {
      set_session_nonce(header.session_nonce);
      fail_with(SessionFailure::UnsupportedProtocolRevision, true);
      return;
    }
    const auto device_capabilities = decode_capabilities(message.body);
    if (!device_capabilities) {
      set_session_nonce(header.session_nonce);
      fail_with(SessionFailure::MalformedControlMessage, true);
      return;
    }
    const auto selected =
        intersect_capabilities(config_.capabilities, device_capabilities.value());
    set_session_nonce(header.session_nonce);
    if (!selected.has_value()) {
      fail_with(SessionFailure::NoCommonCapabilities, true);
      return;
    }

    prepared_profile_ = *selected;
    const auto body = encode_stream_profile(*selected);
    if (!body) {
      fail_with(SessionFailure::InvalidConfiguration, true);
      return;
    }
    transition(SessionState::AwaitingReady, SessionFailure::ReadyTimeout);
    queue_control(ControlOpcode::StreamPrepare, config_.protocol_revision,
                  config_.initial_config_id, body.value(), 0);
  }

  void handle_stream_ready(const Header& header, const ControlMessage& message) {
    if (message.opcode != ControlOpcode::StreamReady || header.video_stream_id != 0 ||
        message.config_id != config_.initial_config_id) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    const auto ready_profile = decode_stream_profile(message.body);
    if (!ready_profile || !prepared_profile_.has_value() ||
        !(ready_profile.value() == *prepared_profile_)) {
      fail_with(SessionFailure::MalformedControlMessage, true);
      return;
    }

    transition(SessionState::AwaitingCommitted, SessionFailure::CommittedTimeout);
    queue_control(ControlOpcode::StreamCommit, 0, config_.initial_config_id,
                  write_u32(config_.video_stream_id), config_.video_stream_id);
  }

  void handle_stream_committed(const Header& header,
                               const ControlMessage& message) {
    const auto stream_id = decode_stream_id_body(message.body);
    if (message.opcode != ControlOpcode::StreamCommitted ||
        message.config_id != config_.initial_config_id ||
        header.video_stream_id != config_.video_stream_id ||
        !stream_id.has_value() || *stream_id != config_.video_stream_id ||
        !prepared_profile_.has_value()) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }

    set_negotiated_stream(NegotiatedStream{*prepared_profile_,
                                           config_.initial_config_id,
                                           config_.video_stream_id});
    reset_decoder_stream(config_.video_stream_id);
    transition(SessionState::Streaming, std::nullopt);
  }

  HostSessionConfig config_;
  std::optional<StreamProfile> prepared_profile_;
  bool sent_initial_idr_{};
};

struct DeviceSession::Impl final : EndpointCore {
  Impl(ByteIo& io, MonotonicClock& clock, DeviceSessionConfig config,
       DeviceBoundary* boundary)
      : EndpointCore(io, clock, config.limits, config.session_nonce),
        config_(std::move(config)),
        boundary_(boundary) {}

  void start() {
    if (started()) {
      return;
    }
    if (!validate_limits(config_.limits) || config_.session_nonce == 0 ||
        config_.protocol_revision == 0 ||
        !encode_capabilities(config_.capabilities)) {
      fail_with(SessionFailure::InvalidConfiguration, false);
      return;
    }
    const auto capabilities = encode_capabilities(config_.capabilities);
    if (!capabilities) {
      fail_with(SessionFailure::InvalidConfiguration, false);
      return;
    }
    set_session_nonce(config_.session_nonce);
    mark_started(SessionState::AwaitingPrepare, SessionFailure::PrepareTimeout);
    queue_control(ControlOpcode::ClientHello, config_.protocol_revision, 0,
                  capabilities.value(), 0);
  }

  void process_control(const Header& header, const ControlMessage& message) override {
    if (message.opcode == ControlOpcode::ProtocolError) {
      const auto failure = decode_failure_body(message.body);
      fail_with(failure.value_or(SessionFailure::MalformedControlMessage), false);
      return;
    }

    if (message.opcode == ControlOpcode::StreamPrepare) {
      handle_stream_prepare(header, message);
      return;
    }
    if (message.opcode == ControlOpcode::StreamCommit) {
      if (state() != SessionState::AwaitingCommit) {
        fail_with(SessionFailure::UnexpectedMessage, true);
        return;
      }
      handle_stream_commit(header, message);
      return;
    }
    fail_with(SessionFailure::UnexpectedMessage, true);
  }

  void process_video(const Header& header, std::vector<std::uint8_t> payload) override {
    if (!committed_stream_id_.has_value() ||
        header.video_stream_id != *committed_stream_id_ ||
        header.session_nonce != config_.session_nonce) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    if (state() == SessionState::AwaitingFirstIdr) {
      if ((header.flags & kFlagKeyFrame) == 0) {
        return;
      }
      transition(SessionState::Streaming, std::nullopt);
    } else if (state() != SessionState::Streaming) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    received_frames_.push_back(ReceivedVideoFrame{header, std::move(payload)});
  }

  void handle_stream_prepare(const Header& header,
                             const ControlMessage& message) {
    if (message.opcode != ControlOpcode::StreamPrepare ||
        header.video_stream_id != 0 || message.config_id == 0) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    if (message.proto_rev != config_.protocol_revision) {
      fail_with(SessionFailure::UnsupportedProtocolRevision, true);
      return;
    }
    const auto profile = decode_stream_profile(message.body);
    if (!profile || !supports_profile(config_.capabilities, profile.value())) {
      fail_with(SessionFailure::NoCommonCapabilities, true);
      return;
    }

    if (prepared_config_id_.has_value()) {
      if (state() != SessionState::AwaitingCommit ||
          *prepared_config_id_ != message.config_id ||
          !prepared_profile_.has_value() ||
          !(*prepared_profile_ == profile.value())) {
        fail_with(SessionFailure::UnexpectedMessage, true);
        return;
      }
      queue_profile_control(ControlOpcode::StreamReady, *prepared_config_id_,
                            *prepared_profile_);
      return;
    }

    if (boundary_ != nullptr) {
      const BoundaryResult result =
          boundary_->prepare_stream(profile.value(), message.config_id);
      if (result != BoundaryResult::Ok) {
        fail_with(result == BoundaryResult::DisplayError
                      ? SessionFailure::DisplayError
                      : SessionFailure::PrepareFailed,
                  true);
        return;
      }
    }

    prepared_config_id_ = message.config_id;
    prepared_profile_ = profile.value();
    transition(SessionState::AwaitingCommit, SessionFailure::CommitTimeout);
    queue_profile_control(ControlOpcode::StreamReady, message.config_id,
                          *prepared_profile_);
  }

  void handle_stream_commit(const Header& header, const ControlMessage& message) {
    const auto stream_id = decode_stream_id_body(message.body);
    if (message.opcode != ControlOpcode::StreamCommit ||
        !prepared_config_id_.has_value() ||
        message.config_id != *prepared_config_id_ ||
        !stream_id.has_value() || header.video_stream_id != *stream_id) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }

    if (boundary_ != nullptr) {
      const BoundaryResult result = boundary_->commit_stream(
          *prepared_profile_, *prepared_config_id_, *stream_id);
      if (result != BoundaryResult::Ok) {
        fail_with(result == BoundaryResult::DisplayError
                      ? SessionFailure::DisplayError
                      : SessionFailure::CommitFailed,
                  true);
        return;
      }
    }

    committed_stream_id_ = *stream_id;
    set_negotiated_stream(NegotiatedStream{*prepared_profile_,
                                           *prepared_config_id_, *stream_id});
    reset_decoder_stream(*stream_id);
    transition(SessionState::AwaitingFirstIdr, SessionFailure::InitialIdrTimeout);
    queue_control(ControlOpcode::StreamCommitted, 0, *prepared_config_id_,
                  write_u32(*stream_id), *stream_id);
  }

  void queue_profile_control(ControlOpcode opcode, std::uint32_t config_id,
                             const StreamProfile& profile) {
    const auto body = encode_stream_profile(profile);
    if (!body) {
      fail_with(SessionFailure::InvalidConfiguration, false);
      return;
    }
    queue_control(opcode, 0, config_id, body.value(), 0);
  }

  std::vector<ReceivedVideoFrame> take_video_frames() {
    return std::exchange(received_frames_, {});
  }

  DeviceSessionConfig config_;
  DeviceBoundary* boundary_;
  std::optional<std::uint32_t> prepared_config_id_;
  std::optional<StreamProfile> prepared_profile_;
  std::optional<std::uint32_t> committed_stream_id_;
  std::vector<ReceivedVideoFrame> received_frames_;
};

HostSession::HostSession(ByteIo& io, MonotonicClock& clock, HostSessionConfig config)
    : impl_(std::make_unique<Impl>(io, clock, std::move(config))) {}

HostSession::~HostSession() = default;

void HostSession::start() { impl_->start(); }
void HostSession::pump() { impl_->pump(); }

SessionState HostSession::state() const { return impl_->state(); }
bool HostSession::is_terminal() const { return impl_->is_terminal(); }
std::optional<SessionFailure> HostSession::failure() const { return impl_->failure(); }
std::string_view HostSession::failure_detail() const { return impl_->failure_detail(); }
std::uint32_t HostSession::session_nonce() const { return impl_->session_nonce(); }
const std::optional<NegotiatedStream>& HostSession::negotiated_stream() const {
  return impl_->negotiated_stream();
}
VideoSendStatus HostSession::send_video_frame(std::span<const std::uint8_t> payload,
                                              std::uint64_t timestamp_us,
                                              bool keyframe) {
  return impl_->send_video_frame(payload, timestamp_us, keyframe);
}

DeviceSession::DeviceSession(ByteIo& io, MonotonicClock& clock,
                             DeviceSessionConfig config, DeviceBoundary* boundary)
    : impl_(std::make_unique<Impl>(io, clock, std::move(config), boundary)) {}

DeviceSession::~DeviceSession() = default;

void DeviceSession::start() { impl_->start(); }
void DeviceSession::pump() { impl_->pump(); }

SessionState DeviceSession::state() const { return impl_->state(); }
bool DeviceSession::is_terminal() const { return impl_->is_terminal(); }
std::optional<SessionFailure> DeviceSession::failure() const { return impl_->failure(); }
std::string_view DeviceSession::failure_detail() const { return impl_->failure_detail(); }
std::uint32_t DeviceSession::session_nonce() const { return impl_->session_nonce(); }
std::uint32_t DeviceSession::video_stream_id() const {
  return impl_->negotiated_stream().has_value()
             ? impl_->negotiated_stream()->video_stream_id
             : 0;
}
const std::optional<NegotiatedStream>& DeviceSession::negotiated_stream() const {
  return impl_->negotiated_stream();
}
std::vector<ReceivedVideoFrame> DeviceSession::take_video_frames() {
  return impl_->take_video_frames();
}

}  // namespace deskmate::protocol
