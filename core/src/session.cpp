#include "deskmate/session.hpp"

#include "deskmate/message_stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <vector>

namespace deskmate::protocol {
namespace {

constexpr std::uint8_t kCapabilityEncodingVersion = 1;
constexpr std::uint8_t kProfileEncodingVersion = 1;
constexpr std::uint8_t kCommitAckFirstIdrStage = 2;

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
      raw > SessionFailure::SystemSleep) {
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

std::optional<std::uint32_t> decode_stream_id_prefix(
    const std::vector<std::uint8_t>& body) {
  if (body.size() != 4 && body.size() != 5) {
    return std::nullopt;
  }
  const std::uint32_t value = read_u32(body, 0);
  if (value == 0) {
    return std::nullopt;
  }
  return value;
}

std::vector<std::uint8_t> commit_ack_body(std::uint32_t video_stream_id,
                                          bool first_idr_confirmed) {
  auto body = write_u32(video_stream_id);
  if (first_idr_confirmed) {
    body.push_back(kCommitAckFirstIdrStage);
  }
  return body;
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

  virtual void pump() {
    if (!started_ && state_ == SessionState::Idle) {
      return;
    }
    if (state_ == SessionState::Failed || state_ == SessionState::Stopping ||
        state_ == SessionState::Stopped) {
      clear_outbound();
      return;
    }
    flush_outbound();
    if (state_ == SessionState::Failed || state_ == SessionState::Stopping ||
        state_ == SessionState::Stopped) {
      clear_outbound();
      return;
    }
    if (deadline_ns_.has_value() && clock_.now_ns() >= *deadline_ns_) {
      handle_stage_timeout();
      flush_outbound();
      return;
    }
    if (liveness_active_ &&
        clock_.now_ns() >=
            saturating_add(last_valid_message_ns_, kPeerSilenceTimeoutNs)) {
      fail_with(SessionFailure::HeartbeatTimeout, false);
      return;
    }
    maybe_send_heartbeat();

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
    finish_receive_cycle();
    if (state_ == SessionState::Failed || state_ == SessionState::Stopping ||
        state_ == SessionState::Stopped) {
      clear_outbound();
      return;
    }
    flush_outbound();
  }

  [[nodiscard]] SessionState state() const { return state_; }
  [[nodiscard]] bool is_terminal() const {
    return state_ == SessionState::Failed || state_ == SessionState::Stopped;
  }
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

  void begin_liveness() {
    liveness_active_ = true;
    last_valid_message_ns_ = clock_.now_ns();
    next_heartbeat_ns_ =
        saturating_add(last_valid_message_ns_, kHeartbeatIntervalNs);
  }

  void end_liveness() { liveness_active_ = false; }

  void mark_valid_message() {
    if (liveness_active_) {
      last_valid_message_ns_ = clock_.now_ns();
    }
  }

  void cancel_pending_io() { io_.cancel(); }

  void clear_outbound() noexcept {
    outbound_.clear();
    write_offset_ = 0;
  }

  void record_failure(SessionFailure failure) { failure_ = failure; }

  void set_session_nonce(std::uint32_t nonce, std::uint32_t video_stream_id = 0) {
    session_nonce_ = nonce;
    decoder_.reset(StreamDecoderConfig{nonce, video_stream_id});
  }

  void reset_decoder_stream(std::uint32_t video_stream_id) {
    decoder_.reset(StreamDecoderConfig{session_nonce_, video_stream_id});
  }

  void retarget_decoder_stream(std::uint32_t video_stream_id) {
    decoder_.retarget_video_stream(video_stream_id);
  }

  void set_negotiated_stream(NegotiatedStream stream) {
    negotiated_ = std::move(stream);
  }

  void fail_with(SessionFailure failure, bool send_wire_error) {
    if (is_terminal() || state_ == SessionState::Stopping) {
      clear_outbound();
      return;
    }
    state_ = SessionState::Failed;
    failure_ = failure;
    disarm_timeout();
    if (send_wire_error && session_nonce_ != 0) {
      queue_control(ControlOpcode::ProtocolError, 0, 0,
                    {static_cast<std::uint8_t>(failure)}, 0);
      flush_outbound();
    }
    on_terminal_begin();
    cancel_pending_io();
    on_terminal_failure();
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
      queue_outbound(std::move(bytes));
    }
  }

  [[nodiscard]] std::uint32_t next_video_seq() { return next_video_seq_++; }

  void queue_clock(const ClockMessage& message, std::uint32_t seq) {
    const auto payload = encode_clock_message(message);
    if (!payload) {
      fail_with(SessionFailure::InvalidConfiguration, false);
      return;
    }
    Packet packet;
    packet.header.type = static_cast<std::uint8_t>(MessageType::Clock);
    packet.header.session_nonce = session_nonce_;
    packet.header.seq = seq;
    packet.header.payload_len =
        static_cast<std::uint32_t>(payload.value().size());
    packet.header.timestamp_us = message.t0_ns / 1'000U;
    packet.payload = payload.value();
    const auto encoded = encode_packet(packet);
    if (!encoded) {
      fail_with(SessionFailure::InvalidConfiguration, false);
      return;
    }
    queue_bytes(encoded.value());
  }

  [[nodiscard]] std::uint64_t now_ns() const { return clock_.now_ns(); }
  [[nodiscard]] std::optional<SessionFailure> timeout_reason() const {
    return timeout_failure_;
  }
  [[nodiscard]] const SessionLimits& limits() const { return limits_; }

  virtual void queue_outbound(std::vector<std::uint8_t> bytes) {
    outbound_.push_back(std::move(bytes));
  }
  virtual void flush_outbound() { flush(); }

  virtual void process_control(const Header& header,
                               const ControlMessage& message) = 0;
  virtual void process_clock(const Header& header,
                             std::span<const std::uint8_t> payload) = 0;
  virtual void process_recovery_event(const StreamEvent&) {}
  virtual void finish_receive_cycle() {}
  virtual void process_video(const Header& header,
                             std::vector<std::uint8_t> payload,
                             std::optional<std::uint32_t> recovery_id) = 0;
  virtual void process_input(const Header&, std::span<const std::uint8_t>) {}
  virtual void on_terminal_begin() {}
  virtual void on_terminal_failure() {}

  virtual void handle_stage_timeout() {
    if (timeout_failure_.has_value()) {
      fail_with(*timeout_failure_, true);
    }
  }

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
      if (event.error.action == ErrorAction::RequestRecovery) {
        process_recovery_event(event);
      }
      return;
    }
    if (event.kind == StreamEventKind::VideoFrame) {
      if (session_nonce_ != 0 && event.header.session_nonce != session_nonce_) {
        return;
      }
      mark_valid_message();
      process_video(event.header, std::move(event.payload),
                    std::move(event.recovery_id));
      return;
    }
    if (event.kind != StreamEventKind::Message) {
      return;
    }
    if (session_nonce_ != 0 && event.header.session_nonce != session_nonce_) {
      return;
    }
    if (event.header.type == static_cast<std::uint8_t>(MessageType::Clock)) {
      mark_valid_message();
      process_clock(event.header, event.payload);
      return;
    }
    if (event.header.type == static_cast<std::uint8_t>(MessageType::Input)) {
      mark_valid_message();
      process_input(event.header, event.payload);
      return;
    }
    if (event.header.type != static_cast<std::uint8_t>(MessageType::Control)) {
      return;
    }

    const auto decoded = decode_control_payload(event.payload);
    if (!decoded) {
      if (decoded.error().code == ErrorCode::UnsupportedControlOpcode &&
          decoded.error().action == ErrorAction::DiscardMessage) {
        mark_valid_message();
        return;
      }
      fail_with(SessionFailure::MalformedControlMessage, true);
      return;
    }
    mark_valid_message();
    process_control(event.header, decoded.value());
  }

  void maybe_send_heartbeat() {
    if (!liveness_active_ || state_ == SessionState::Stopping ||
        state_ == SessionState::Stopped) {
      return;
    }
    const std::uint64_t now = clock_.now_ns();
    if (now < next_heartbeat_ns_) {
      return;
    }
    queue_control(ControlOpcode::Heartbeat, 0, 0, {}, 0);
    next_heartbeat_ns_ = saturating_add(now, kHeartbeatIntervalNs);
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
        clear_outbound();
        fail_with(SessionFailure::IoError, false);
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
  bool liveness_active_{};
  std::uint64_t last_valid_message_ns_{};
  std::uint64_t next_heartbeat_ns_{};
};

bool validate_limits(const SessionLimits& limits) {
  return limits.stage_timeout_ns != 0 && limits.video_fragment_payload != 0 &&
         limits.video_fragment_payload <= kMaxPacketPayload &&
         limits.pause_grace_timeout_ns >= kMinPauseGraceTimeoutNs &&
         limits.pause_grace_timeout_ns <= kMaxPauseGraceTimeoutNs;
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
    case SessionFailure::HeartbeatTimeout:
      return "heartbeat_timeout";
    case SessionFailure::PauseGraceExpired:
      return "pause_grace_expired";
    case SessionFailure::SystemSleep:
      return "system_sleep";
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
    case SessionState::Paused:
      return "paused";
    case SessionState::Reconfiguring:
      return "reconfiguring";
    case SessionState::Stopping:
      return "stopping";
    case SessionState::Stopped:
      return "stopped";
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
  Impl(ByteIo& io, MonotonicClock& clock, HostSessionConfig config,
       HostBoundary* boundary)
      : EndpointCore(io, clock, config.limits, 0),
        config_(std::move(config)),
        boundary_(boundary),
        scheduler_(io, clock, config_.scheduler) {
    highest_config_id_ = config_.initial_config_id;
  }

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

  void pump() override {
    if (pause_deadline_ns_.has_value() &&
        now_ns() >= *pause_deadline_ns_) {
      expire_pause_grace();
      return;
    }
    retry_reconfiguration_commit_if_due();
    EndpointCore::pump();
    if (is_terminal() || state() == SessionState::Stopping ||
        state() == SessionState::Stopped) {
      scheduler_.begin_stopping();
    } else {
      scheduler_.pump();
      if (scheduler_.stopping()) {
        fail_with(SessionFailure::IoError, false);
      }
    }
  }

  void queue_outbound(std::vector<std::uint8_t> bytes) override {
    scheduler_.enqueue_priority(std::move(bytes));
  }

  void flush_outbound() override { scheduler_.pump_priority(); }

  void process_control(const Header& header, const ControlMessage& message) override {
    if (message.opcode == ControlOpcode::ProtocolError) {
      const auto failure = decode_failure_body(message.body);
      if (!failure.has_value()) {
        fail_with(SessionFailure::MalformedControlMessage, false);
      } else if (state() == SessionState::Reconfiguring) {
        abort_reconfiguration(*failure, false);
      } else if (reconfiguration_.has_value() &&
                 reconfiguration_->phase == ReconfigurationPhase::Failed &&
                 message.config_id == reconfiguration_->config_id) {
        return;
      } else {
        fail_with(*failure, false);
      }
      return;
    }

    if (message.opcode == ControlOpcode::Heartbeat) {
      return;
    }
    if (message.opcode == ControlOpcode::PauseAck) {
      const auto active = negotiated_stream();
      if (state() == SessionState::Paused && active.has_value() &&
          message.config_id == active->config_id) {
        pause_acknowledged_ = true;
        return;
      }
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    if (message.opcode == ControlOpcode::RequestIdr) {
      const auto recovery_id = decode_recovery_id(message.body);
      const auto stream = negotiated_stream();
      if (state() != SessionState::Streaming || !stream.has_value() ||
          header.video_stream_id != stream->video_stream_id ||
          !recovery_id) {
        fail_with(SessionFailure::UnexpectedMessage, true);
        return;
      }
      scheduler_.request_idr_from_peer(recovery_id.value());
      return;
    }
    if (message.opcode == ControlOpcode::StreamCommitted &&
        negotiated_stream().has_value() &&
        message.config_id == negotiated_stream()->config_id &&
        header.video_stream_id == negotiated_stream()->video_stream_id) {
      const auto duplicate_stream_id = decode_stream_id_prefix(message.body);
      if (duplicate_stream_id.has_value() &&
          *duplicate_stream_id == negotiated_stream()->video_stream_id) {
        return;
      }
    }
    if (reconfiguration_.has_value() &&
        reconfiguration_->phase == ReconfigurationPhase::Failed &&
        message.config_id == reconfiguration_->config_id &&
        (message.opcode == ControlOpcode::StreamReady ||
         message.opcode == ControlOpcode::StreamCommitted)) {
      return;
    }
    if (reconfiguration_.has_value() &&
        reconfiguration_->phase == ReconfigurationPhase::Completed &&
        message.config_id == reconfiguration_->config_id &&
        message.opcode == ControlOpcode::StreamReady) {
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
    if (state() == SessionState::Reconfiguring) {
      if (message.opcode == ControlOpcode::StreamReady) {
        handle_reconfiguration_ready(header, message);
      } else if (message.opcode == ControlOpcode::StreamCommitted) {
        handle_reconfiguration_committed(header, message);
      } else {
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

  void process_video(const Header&, std::vector<std::uint8_t>,
                     std::optional<std::uint32_t>) override {
    fail_with(SessionFailure::UnexpectedMessage, true);
  }

  void process_input(const Header& header,
                     std::span<const std::uint8_t> payload) override {
    if (state() != SessionState::Streaming) {
      return;
    }
    if (!negotiated_stream().has_value() ||
        header.video_stream_id != negotiated_stream()->video_stream_id) {
      return;
    }
    if (!input_injection_intent_) {
      return;
    }
    const auto decoded = decode_input_payload(payload);
    if (!decoded) {
      return;
    }
    input_events_.push_back(decoded.value());
    if (input_events_.size() > kMaxQueuedInputEvents) {
      input_events_.erase(input_events_.begin());
      ++input_events_dropped_;
    }
  }

  std::vector<InputEvent> take_input_events() {
    std::vector<InputEvent> events;
    events.swap(input_events_);
    return events;
  }

  void process_clock(const Header& header,
                     std::span<const std::uint8_t> payload) override {
    const auto message = decode_clock_message(payload);
    if (!message || message.value().kind != ClockMessageKind::Response) {
      return;
    }
    const auto pending = std::find_if(
        pending_clock_requests_.begin(), pending_clock_requests_.end(),
        [&header](const PendingClockRequest& request) {
          return request.seq == header.seq;
        });
    if (pending == pending_clock_requests_.end()) {
      return;
    }
    const bool observed = clock_estimator_.observe(ClockSample{
        pending->t0_ns, message.value().t1_ns, message.value().t2_ns,
        now_ns()});
    (void)observed;
    pending_clock_requests_.erase(pending);
  }

  VideoSendStatus send_video_frame(std::span<const std::uint8_t> payload,
                                   std::uint64_t timestamp_us, bool keyframe) {
    if (is_terminal()) {
      return VideoSendStatus::SessionFailed;
    }
    if (state() == SessionState::Stopping) {
      return VideoSendStatus::Stopping;
    }
    if (video_state_ == HostVideoState::RecordingPermissionRevoked ||
        video_state_ == HostVideoState::DisplayError ||
        video_state_ == HostVideoState::Sleeping) {
      return VideoSendStatus::VideoUnavailable;
    }
    if (state() == SessionState::Paused) {
      return VideoSendStatus::Paused;
    }
    if (pause_after_reconfiguration_) {
      return VideoSendStatus::Paused;
    }
    if (state() == SessionState::Reconfiguring && resuming_ &&
        !pending_committed_stream_.has_value()) {
      return VideoSendStatus::Paused;
    }
    if ((state() != SessionState::Streaming &&
         state() != SessionState::Reconfiguring) ||
        !negotiated_stream().has_value()) {
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
    frame.video_stream_id =
        pending_committed_stream_.has_value()
            ? pending_committed_stream_->video_stream_id
            : negotiated_stream()->video_stream_id;
    frame.seq = next_video_seq();
    frame.timestamp_us = timestamp_us;
    frame.flags = keyframe ? kFlagKeyFrame : 0;
    frame.payload.assign(payload.begin(), payload.end());
    const SchedulerQueueStatus queued = scheduler_.enqueue_video_frame(
        std::move(frame), config_.limits.video_fragment_payload);
    switch (queued) {
      case SchedulerQueueStatus::Queued:
        if (keyframe) {
          sent_initial_idr_ = true;
        }
        return VideoSendStatus::Sent;
      case SchedulerQueueStatus::IdrRequired:
        return VideoSendStatus::IdrRequired;
      case SchedulerQueueStatus::InvalidArgument:
        return VideoSendStatus::InvalidArgument;
      case SchedulerQueueStatus::Stopped:
        return state() == SessionState::Stopping ? VideoSendStatus::Stopping
                                                 : VideoSendStatus::SessionFailed;
    }
    return VideoSendStatus::InvalidArgument;
  }

  [[nodiscard]] const std::optional<SchedulerIdrRequest>& pending_idr_request()
      const {
    return scheduler_.idr_request();
  }

  SessionCommandStatus pause() {
    if (state() != SessionState::Streaming || !negotiated_stream().has_value()) {
      if (state() == SessionState::Paused) {
        return SessionCommandStatus::Duplicate;
      }
      return SessionCommandStatus::InvalidState;
    }
    begin_pause(true);
    return SessionCommandStatus::Ok;
  }

  SessionCommandStatus resume(const StreamProfile& profile,
                              std::uint32_t config_id,
                              std::uint32_t video_stream_id) {
    if (reconfiguration_.has_value() &&
        reconfiguration_->config_id == config_id) {
      if (last_attempt_profile_.has_value() &&
          *last_attempt_profile_ == profile &&
          last_attempt_video_stream_id_ == video_stream_id) {
        return SessionCommandStatus::Duplicate;
      }
      return SessionCommandStatus::InvalidArgument;
    }
    if (state() != SessionState::Paused || !negotiated_stream().has_value()) {
      return SessionCommandStatus::InvalidState;
    }
    if (display_error_ ||
        screen_locked_ ||
        video_state_ == HostVideoState::RecordingPermissionRevoked ||
        video_state_ == HostVideoState::Sleeping) {
      return SessionCommandStatus::DisplayUnavailable;
    }
    if (config_id == 0 || video_stream_id == 0 ||
        config_id <= highest_config_id_ ||
        video_stream_id == negotiated_stream()->video_stream_id ||
        !supports_profile(config_.capabilities, profile)) {
      return SessionCommandStatus::InvalidArgument;
    }

    pending_profile_ = profile;
    pending_config_id_ = config_id;
    pending_video_stream_id_ = video_stream_id;
    scheduler_.clear_started_video_streams();
    reconfiguration_ack_retry_ns_.reset();
    last_attempt_profile_ = profile;
    last_attempt_video_stream_id_ = video_stream_id;
    resuming_ = true;
    pause_after_reconfiguration_ = false;
    reconfiguration_return_state_ = SessionState::Paused;
    pause_acknowledged_ = false;
    highest_config_id_ = config_id;
    reconfiguration_ = ReconfigurationStatus{
        config_id, ReconfigurationPhase::AwaitingReady, std::nullopt, true};
    transition(SessionState::Reconfiguring, SessionFailure::ReadyTimeout);
    queue_control(ControlOpcode::Resume, 0, config_id, {}, 0);
    const auto body = encode_stream_profile(profile);
    if (!body) {
      fail_with(SessionFailure::InvalidConfiguration, true);
      return SessionCommandStatus::InvalidArgument;
    }
    queue_control(ControlOpcode::StreamPrepare, config_.protocol_revision,
                  config_id, body.value(), 0);
    return SessionCommandStatus::Ok;
  }

  SessionCommandStatus handle_host_event(HostEvent event) {
    switch (event) {
      case HostEvent::ScreenLocked:
        if (screen_locked_) {
          return SessionCommandStatus::Duplicate;
        }
        screen_locked_ = true;
        pause_deadline_ns_.reset();
        set_capture_intent(false);
        set_input_intent(false);
        begin_pause(false);
        refresh_states();
        return SessionCommandStatus::Ok;
      case HostEvent::ScreenUnlocked:
        if (!screen_locked_) {
          return SessionCommandStatus::Duplicate;
        }
        screen_locked_ = false;
        resume_required_ = state() == SessionState::Paused;
        refresh_states();
        return SessionCommandStatus::Ok;
      case HostEvent::AccessibilityPermissionRevoked:
        if (accessibility_revoked_) {
          return SessionCommandStatus::Duplicate;
        }
        accessibility_revoked_ = true;
        set_input_intent(false);
        refresh_states();
        return SessionCommandStatus::Ok;
      case HostEvent::ScreenRecordingPermissionRevoked:
        if (screen_recording_revoked_) {
          return SessionCommandStatus::Duplicate;
        }
        screen_recording_revoked_ = true;
        set_capture_intent(false);
        refresh_states();
        return SessionCommandStatus::Ok;
      case HostEvent::DisplayError:
        if (display_error_) {
          return SessionCommandStatus::Duplicate;
        }
        display_error_ = true;
        pause_deadline_ns_.reset();
        set_capture_intent(false);
        set_input_intent(false);
        destroy_extended_display();
        if (state() == SessionState::Streaming ||
            state() == SessionState::Reconfiguring) {
          if (state() == SessionState::Reconfiguring) {
            if (can_defer_reconfiguration_pause()) {
              pause_after_reconfiguration_ = true;
              scheduler_.discard_unstarted_video_frames();
              refresh_states();
            } else {
              abort_reconfiguration(SessionFailure::DisplayError, false);
            }
          }
          if (state() == SessionState::Streaming && negotiated_stream().has_value()) {
            begin_pause(false);
          }
        }
        refresh_states();
        return SessionCommandStatus::Ok;
      case HostEvent::WillSleep:
        if (sleeping_) {
          return SessionCommandStatus::Duplicate;
        }
        sleeping_ = true;
        pause_deadline_ns_.reset();
        set_capture_intent(false);
        set_input_intent(false);
        refresh_states();
        request_stop();
        complete_stop();
        record_failure(SessionFailure::SystemSleep);
        return SessionCommandStatus::Ok;
    }
    return SessionCommandStatus::InvalidArgument;
  }

  [[nodiscard]] HostVideoState video_state() const { return video_state_; }
  [[nodiscard]] HostInputState input_state() const { return input_state_; }
  [[nodiscard]] bool extended_display_present() const {
    return extended_display_present_;
  }

  void request_stop() {
    if (is_terminal() || state() == SessionState::Stopping) {
      return;
    }
    transition(SessionState::Stopping, std::nullopt);
    end_liveness();
    pause_deadline_ns_.reset();
    reconfiguration_ack_retry_ns_.reset();
    set_capture_intent(false);
    set_input_intent(false);
    scheduler_.begin_stopping();
    cancel_pending_io();
    refresh_states();
  }

  void complete_stop() {
    if (state() != SessionState::Stopping) {
      return;
    }
    if (boundary_ != nullptr) {
      boundary_->drain_callbacks();
    }
    if (pending_config_id_.has_value()) {
      reconfiguration_ = ReconfigurationStatus{
          *pending_config_id_, ReconfigurationPhase::Failed,
          SessionFailure::UnexpectedMessage, resuming_};
      pending_config_id_.reset();
      pending_video_stream_id_.reset();
      pending_profile_.reset();
      resuming_ = false;
    }
    reconfiguration_ack_retry_ns_.reset();
    destroy_extended_display();
    transition(SessionState::Stopped, std::nullopt);
    refresh_states();
  }

  SessionCommandStatus reconfigure(const StreamProfile& profile,
                                   std::uint32_t config_id,
                                   std::uint32_t video_stream_id) {
    if (reconfiguration_.has_value() &&
        reconfiguration_->config_id == config_id) {
      if (last_attempt_profile_.has_value() &&
          *last_attempt_profile_ == profile &&
          last_attempt_video_stream_id_ == video_stream_id) {
        return SessionCommandStatus::Duplicate;
      }
      return SessionCommandStatus::InvalidArgument;
    }
    if (state() == SessionState::Reconfiguring) {
      if (pending_config_id_ == config_id &&
          pending_video_stream_id_ == video_stream_id &&
          pending_profile_.has_value() && *pending_profile_ == profile) {
        return SessionCommandStatus::Duplicate;
      }
      return SessionCommandStatus::InvalidState;
    }
    if (state() != SessionState::Streaming || !negotiated_stream().has_value()) {
      return SessionCommandStatus::InvalidState;
    }
    if (config_id == negotiated_stream()->config_id &&
        video_stream_id == negotiated_stream()->video_stream_id &&
        profile == negotiated_stream()->profile) {
      return SessionCommandStatus::Duplicate;
    }
    if (config_id == 0 || video_stream_id == 0 ||
        config_id <= highest_config_id_ ||
        video_stream_id == negotiated_stream()->video_stream_id ||
        !supports_profile(config_.capabilities, profile)) {
      return SessionCommandStatus::InvalidArgument;
    }

    pending_profile_ = profile;
    pending_config_id_ = config_id;
    pending_video_stream_id_ = video_stream_id;
    scheduler_.clear_started_video_streams();
    reconfiguration_ack_retry_ns_.reset();
    last_attempt_profile_ = profile;
    last_attempt_video_stream_id_ = video_stream_id;
    resuming_ = false;
    pause_after_reconfiguration_ = false;
    reconfiguration_return_state_ = SessionState::Streaming;
    highest_config_id_ = config_id;
    reconfiguration_ = ReconfigurationStatus{
        config_id, ReconfigurationPhase::AwaitingReady, std::nullopt, false};
    transition(SessionState::Reconfiguring, SessionFailure::ReadyTimeout);
    const auto body = encode_stream_profile(profile);
    if (!body) {
      abort_reconfiguration(SessionFailure::InvalidConfiguration, false);
      return SessionCommandStatus::InvalidArgument;
    }
    queue_control(ControlOpcode::StreamPrepare, config_.protocol_revision,
                  config_id, body.value(), 0);
    return SessionCommandStatus::Ok;
  }

  [[nodiscard]] const std::optional<ReconfigurationStatus>& reconfiguration() const {
    return reconfiguration_;
  }

  [[nodiscard]] bool capture_encoding_intent() const {
    return capture_encoding_intent_;
  }
  [[nodiscard]] bool input_injection_intent() const {
    return input_injection_intent_;
  }
  [[nodiscard]] bool pause_acknowledged() const { return pause_acknowledged_; }

  SessionCommandStatus request_clock_sync() {
    if (session_nonce() == 0 || is_terminal() ||
        state() == SessionState::Stopping ||
        state() == SessionState::Idle ||
        state() == SessionState::AwaitingClientHello) {
      return SessionCommandStatus::InvalidState;
    }
    if (pending_clock_requests_.size() >= 8) {
      pending_clock_requests_.erase(pending_clock_requests_.begin());
    }
    const std::uint64_t t0_ns = now_ns();
    const std::uint32_t seq = next_clock_seq_++;
    pending_clock_requests_.push_back(PendingClockRequest{seq, t0_ns});
    queue_clock(ClockMessage{ClockMessageKind::Request, t0_ns, 0, 0}, seq);
    return SessionCommandStatus::Ok;
  }

  [[nodiscard]] const std::optional<ClockEstimate>& clock_estimate() const {
    return clock_estimator_.estimate();
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
    begin_liveness();
    pause_deadline_ns_.reset();
    resume_required_ = false;
    if (screen_locked_) {
      begin_pause(false);
    } else {
      set_capture_intent(!screen_recording_revoked_ && !display_error_);
      set_input_intent(!accessibility_revoked_ && !display_error_);
      refresh_states();
    }
  }

  void handle_reconfiguration_ready(const Header& header,
                                    const ControlMessage& message) {
    if (message.opcode == ControlOpcode::StreamReady &&
        reconfiguration_.has_value() &&
        reconfiguration_->config_id == message.config_id &&
        (reconfiguration_->phase == ReconfigurationPhase::AwaitingFirstIdr ||
         reconfiguration_->phase == ReconfigurationPhase::Completed)) {
      // A retransmitted STREAM_READY for a configuration that already advanced
      // past AwaitingCommitted is stale; ignore it instead of regressing the
      // Host state machine back to AwaitingCommitted.
      return;
    }
    if (message.opcode != ControlOpcode::StreamReady ||
        header.video_stream_id != 0 || !pending_config_id_.has_value() ||
        message.config_id != *pending_config_id_ || !pending_profile_.has_value()) {
      if (retired_config_ids_.count(message.config_id) != 0) {
        return;
      }
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    const auto ready_profile = decode_stream_profile(message.body);
    if (!ready_profile || !(ready_profile.value() == *pending_profile_)) {
      fail_with(SessionFailure::MalformedControlMessage, true);
      return;
    }
    transition(SessionState::Reconfiguring, SessionFailure::CommittedTimeout);
    reconfiguration_->phase = ReconfigurationPhase::AwaitingCommitted;
    queue_control(ControlOpcode::StreamCommit, 0, *pending_config_id_,
                  write_u32(*pending_video_stream_id_),
                  *pending_video_stream_id_);
    schedule_reconfiguration_commit_retry();
  }

  void handle_reconfiguration_committed(const Header& header,
                                        const ControlMessage& message) {
    const auto stream_id = decode_stream_id_prefix(message.body);
    if (retired_config_ids_.count(message.config_id) != 0) {
      return;
    }
    if (message.opcode != ControlOpcode::StreamCommitted ||
        !pending_config_id_.has_value() ||
        !pending_video_stream_id_.has_value() ||
        !pending_profile_.has_value() ||
        !reconfiguration_.has_value() ||
        message.config_id != *pending_config_id_ ||
        header.video_stream_id != *pending_video_stream_id_ ||
        !stream_id.has_value() || *stream_id != *pending_video_stream_id_) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    if (reconfiguration_->phase == ReconfigurationPhase::AwaitingFirstIdr) {
      if (message.body.size() == 4) {
        return;
      }
      if (message.body.size() != 5 ||
          message.body[4] != kCommitAckFirstIdrStage) {
        fail_with(SessionFailure::UnexpectedMessage, true);
        return;
      }
      const bool pause_after =
          std::exchange(pause_after_reconfiguration_, false);
      set_negotiated_stream(*pending_committed_stream_);
      reset_decoder_stream(*pending_video_stream_id_);
      transition(SessionState::Streaming, std::nullopt);
      begin_liveness();
      pause_deadline_ns_.reset();
      resume_required_ = false;
      if (screen_locked_ || pause_after) {
        begin_pause(false);
      } else {
        set_capture_intent(!screen_recording_revoked_ && !display_error_);
        set_input_intent(!accessibility_revoked_ && !display_error_);
        refresh_states();
      }
      const bool was_resuming = resuming_;
      reconfiguration_ = ReconfigurationStatus{
          *pending_config_id_, ReconfigurationPhase::Completed, std::nullopt,
          was_resuming};
      resuming_ = false;
      highest_config_id_ = *pending_config_id_;
      pending_config_id_.reset();
      pending_video_stream_id_.reset();
      pending_profile_.reset();
      pending_committed_stream_.reset();
      reconfiguration_ack_retry_ns_.reset();
      return;
    }
    if (reconfiguration_->phase != ReconfigurationPhase::AwaitingCommitted ||
        message.body.size() != 4) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }

    pending_committed_stream_ =
        NegotiatedStream{*pending_profile_, *pending_config_id_,
                         *pending_video_stream_id_};
    reconfiguration_->phase = ReconfigurationPhase::AwaitingFirstIdr;
    sent_initial_idr_ = false;
    transition(SessionState::Reconfiguring, std::nullopt);
    schedule_reconfiguration_commit_retry();
    refresh_states();
  }

  void schedule_reconfiguration_commit_retry() {
    const std::uint64_t retry_ns =
        std::max<std::uint64_t>(1, config_.scheduler.recovery_retry_ns);
    reconfiguration_ack_retry_ns_ = saturating_add(now_ns(), retry_ns);
  }

  void retry_reconfiguration_commit_if_due() {
    if (!reconfiguration_ack_retry_ns_.has_value() ||
        state() != SessionState::Reconfiguring ||
        !pending_config_id_.has_value() ||
        !pending_video_stream_id_.has_value() ||
        !reconfiguration_.has_value()) {
      return;
    }
    if (reconfiguration_->phase != ReconfigurationPhase::AwaitingCommitted &&
        reconfiguration_->phase != ReconfigurationPhase::AwaitingFirstIdr) {
      return;
    }
    if (now_ns() < *reconfiguration_ack_retry_ns_) {
      return;
    }
    queue_control(ControlOpcode::StreamCommit, 0, *pending_config_id_,
                  write_u32(*pending_video_stream_id_),
                  *pending_video_stream_id_);
    schedule_reconfiguration_commit_retry();
  }

  void abort_reconfiguration(SessionFailure failure, bool notify_peer) {
    reconfiguration_ack_retry_ns_.reset();
    if (!pending_config_id_.has_value()) {
      if (state() != SessionState::Failed) {
        fail_with(failure, false);
      }
      return;
    }
    const std::uint32_t failed_config_id = *pending_config_id_;
    const auto failed_stream_id = pending_video_stream_id_;
    retired_config_ids_.insert(failed_config_id);
    const bool was_resuming = resuming_;
    const bool pause_after = std::exchange(pause_after_reconfiguration_, false);
    reconfiguration_ = ReconfigurationStatus{
        failed_config_id, ReconfigurationPhase::Failed, failure, was_resuming};
    resuming_ = false;
    pending_config_id_.reset();
    pending_video_stream_id_.reset();
    pending_profile_.reset();
    pending_committed_stream_.reset();
    scheduler_.discard_pending_video_frames();
    if (negotiated_stream().has_value()) {
      reset_decoder_stream(negotiated_stream()->video_stream_id);
      sent_initial_idr_ = false;
    }
    transition(reconfiguration_return_state_, std::nullopt);
    if (reconfiguration_return_state_ == SessionState::Paused) {
      set_capture_intent(false);
      set_input_intent(false);
    }
    refresh_states();
    if (notify_peer) {
      queue_control(ControlOpcode::ProtocolError, 0, failed_config_id,
                    {static_cast<std::uint8_t>(failure)}, 0);
    }
    if (state() == SessionState::Streaming &&
        (screen_locked_ || pause_after)) {
      begin_pause(false);
    }
    (void)failed_stream_id;
  }

  void handle_stage_timeout() override {
    const auto failure = timeout_reason().value_or(SessionFailure::UnexpectedMessage);
    if (state() == SessionState::Reconfiguring &&
        pending_config_id_.has_value()) {
      abort_reconfiguration(failure, true);
      return;
    }
    EndpointCore::handle_stage_timeout();
  }

  void on_terminal_begin() override {
    end_liveness();
    pause_deadline_ns_.reset();
    reconfiguration_ack_retry_ns_.reset();
    set_capture_intent(false);
    set_input_intent(false);
    scheduler_.begin_stopping();
  }

  void on_terminal_failure() override {
    if (boundary_ != nullptr) {
      boundary_->drain_callbacks();
    }
    destroy_extended_display();
    refresh_states();
  }

  void begin_pause(bool with_grace) {
    if (state() == SessionState::Reconfiguring) {
      if (can_defer_reconfiguration_pause()) {
        pause_after_reconfiguration_ = true;
        set_capture_intent(false);
        set_input_intent(false);
        scheduler_.discard_unstarted_video_frames();
        refresh_states();
        return;
      }
      abort_reconfiguration(SessionFailure::UnexpectedMessage, true);
    }
    if (state() != SessionState::Streaming || !negotiated_stream().has_value()) {
      return;
    }
    set_capture_intent(false);
    set_input_intent(false);
    pause_acknowledged_ = false;
    resume_required_ = false;
    transition(SessionState::Paused, std::nullopt);
    queue_control(ControlOpcode::Pause, 0, negotiated_stream()->config_id, {}, 0);
    if (with_grace) {
      pause_deadline_ns_ = saturating_add(
          now_ns(), config_.limits.pause_grace_timeout_ns);
    } else {
      pause_deadline_ns_.reset();
    }
    refresh_states();
  }

  [[nodiscard]] bool can_defer_reconfiguration_pause() const {
    return reconfiguration_.has_value() &&
           reconfiguration_->phase == ReconfigurationPhase::AwaitingFirstIdr &&
           pending_video_stream_id_.has_value() &&
           scheduler_.has_started_video_stream(*pending_video_stream_id_);
  }

  void refresh_states() {
    if (sleeping_) {
      video_state_ = HostVideoState::Sleeping;
      input_state_ = HostInputState::Sleeping;
      return;
    }
    if (state() == SessionState::Stopping) {
      video_state_ = HostVideoState::Stopping;
      input_state_ = HostInputState::Stopping;
      return;
    }
    if (state() == SessionState::Stopped) {
      video_state_ = HostVideoState::Stopped;
      input_state_ = HostInputState::Stopped;
      return;
    }
    if (screen_locked_) {
      video_state_ = HostVideoState::ScreenLocked;
      input_state_ = HostInputState::ScreenLocked;
      return;
    }
    if (display_error_) {
      video_state_ = HostVideoState::DisplayError;
      input_state_ = HostInputState::DisplayError;
      return;
    }
    if (state() == SessionState::Paused) {
      video_state_ = screen_recording_revoked_
                         ? HostVideoState::RecordingPermissionRevoked
                         : (resume_required_ ? HostVideoState::AwaitingResume
                                             : HostVideoState::Paused);
      input_state_ =
          accessibility_revoked_
              ? HostInputState::AccessibilityPermissionRevoked
              : (resume_required_ ? HostInputState::AwaitingResume
                                  : HostInputState::Paused);
      return;
    }
    if (screen_recording_revoked_) {
      video_state_ = HostVideoState::RecordingPermissionRevoked;
    } else if (state() == SessionState::Streaming ||
               state() == SessionState::Reconfiguring) {
      video_state_ = HostVideoState::Active;
    } else {
      video_state_ = HostVideoState::Inactive;
    }

    if (accessibility_revoked_) {
      input_state_ = HostInputState::AccessibilityPermissionRevoked;
    } else if (state() == SessionState::Streaming ||
               state() == SessionState::Reconfiguring) {
      input_state_ = HostInputState::Active;
    } else {
      input_state_ = HostInputState::Inactive;
    }
  }

  void destroy_extended_display() {
    if (!extended_display_present_) {
      return;
    }
    extended_display_present_ = false;
    if (boundary_ != nullptr) {
      boundary_->destroy_extended_display();
    }
  }

  void expire_pause_grace() {
    if (is_terminal()) {
      return;
    }
    pause_deadline_ns_.reset();
    set_capture_intent(false);
    set_input_intent(false);
    destroy_extended_display();
    fail_with(SessionFailure::PauseGraceExpired, true);
    refresh_states();
  }

  void set_capture_intent(bool enabled) {
    enabled = enabled && !screen_locked_;
    if (capture_encoding_intent_ == enabled) {
      return;
    }
    capture_encoding_intent_ = enabled;
    if (boundary_ != nullptr) {
      boundary_->set_capture_encoding_intent(enabled);
    }
  }

  void set_input_intent(bool enabled) {
    enabled = enabled && !screen_locked_;
    if (input_injection_intent_ == enabled) {
      return;
    }
    input_injection_intent_ = enabled;
    // Input that arrived before the intent was withdrawn must never be
    // injected after the screen locks, the session pauses, or the stream is
    // replaced.
    input_events_.clear();
    if (boundary_ != nullptr) {
      boundary_->set_input_injection_intent(enabled);
    }
  }

  HostSessionConfig config_;
  HostBoundary* boundary_;
  std::optional<StreamProfile> prepared_profile_;
  std::optional<StreamProfile> pending_profile_;
  std::optional<std::uint32_t> pending_config_id_;
  std::optional<std::uint32_t> pending_video_stream_id_;
  std::optional<StreamProfile> last_attempt_profile_;
  std::uint32_t last_attempt_video_stream_id_{};
  std::optional<ReconfigurationStatus> reconfiguration_;
  std::optional<NegotiatedStream> pending_committed_stream_;
  struct PendingClockRequest {
    std::uint32_t seq{};
    std::uint64_t t0_ns{};
  };
  std::vector<PendingClockRequest> pending_clock_requests_;
  ClockSyncEstimator clock_estimator_;
  std::uint32_t next_clock_seq_{1};
  std::uint32_t highest_config_id_{};
  bool sent_initial_idr_{};
  bool capture_encoding_intent_{};
  bool input_injection_intent_{};
  bool pause_acknowledged_{};
  bool resuming_{};
  SessionState reconfiguration_return_state_{SessionState::Streaming};
  HostVideoState video_state_{HostVideoState::Inactive};
  HostInputState input_state_{HostInputState::Inactive};
  std::optional<std::uint64_t> pause_deadline_ns_;
  bool extended_display_present_{true};
  bool screen_locked_{};
  bool screen_recording_revoked_{};
  bool accessibility_revoked_{};
  bool display_error_{};
  bool sleeping_{};
  bool resume_required_{};
  bool pause_after_reconfiguration_{};
  std::optional<std::uint64_t> reconfiguration_ack_retry_ns_;
  std::set<std::uint32_t> retired_config_ids_;
  std::vector<InputEvent> input_events_;
  std::uint64_t input_events_dropped_{};
  HostSendScheduler scheduler_;
};

struct DeviceSession::Impl final : EndpointCore {
  Impl(ByteIo& io, MonotonicClock& clock, DeviceSessionConfig config,
       DeviceBoundary* boundary, SurfaceOwner* surface)
      : EndpointCore(io, clock, config.limits, config.session_nonce),
        config_(std::move(config)),
        boundary_(boundary),
        surface_(surface),
        recovery_gate_(clock, config_.recovery) {}

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

  void pump() override {
    queue_recovery_retry_if_due();
    EndpointCore::pump();
  }

  void process_control(const Header& header, const ControlMessage& message) override {
    if (message.opcode == ControlOpcode::ProtocolError) {
      const auto failure = decode_failure_body(message.body);
      if (!failure.has_value()) {
        fail_with(SessionFailure::MalformedControlMessage, false);
      } else if (state() == SessionState::Reconfiguring ||
                 (state() == SessionState::AwaitingFirstIdr &&
                  previous_stream_.has_value())) {
        abort_reconfiguration(*failure, false);
      } else if (reconfiguration_.has_value() &&
                 reconfiguration_->phase == ReconfigurationPhase::Failed &&
                 message.config_id == reconfiguration_->config_id) {
        return;
      } else {
        fail_with(*failure, false);
      }
      return;
    }

    if (message.opcode == ControlOpcode::Heartbeat) {
      return;
    }
    if (message.opcode == ControlOpcode::Pause) {
      handle_pause(header, message);
      return;
    }
    if (message.opcode == ControlOpcode::Resume) {
      handle_resume(header, message);
      return;
    }
    if (message.opcode == ControlOpcode::StreamPrepare) {
      handle_stream_prepare(header, message);
      return;
    }
    if (message.opcode == ControlOpcode::StreamCommit) {
      const auto stream_id = decode_stream_id_body(message.body);
      if (rolled_back_config_ids_.count(message.config_id) != 0) {
        return;
      }
      if (last_commit_succeeded_ &&
          last_committed_config_id_ == message.config_id &&
          stream_id.has_value() &&
          last_committed_stream_id_ == *stream_id) {
        if (header.video_stream_id != *stream_id ||
            message.config_id < highest_config_id_) {
          fail_with(SessionFailure::UnexpectedMessage, true);
          return;
        }
        if (state() == SessionState::AwaitingFirstIdr) {
          queue_commit_ack(*last_committed_config_id_, *stream_id, false);
          return;
        }
        if (state() == SessionState::Streaming &&
            last_commit_was_reconfiguration_) {
          queue_commit_ack(*last_committed_config_id_, *stream_id, true);
          return;
        }
        queue_commit_ack(*last_committed_config_id_, *stream_id, false);
        return;
      }
      if (state() != SessionState::AwaitingCommit &&
          state() != SessionState::Reconfiguring) {
        fail_with(SessionFailure::UnexpectedMessage, true);
        return;
      }
      handle_stream_commit(header, message);
      return;
    }
    fail_with(SessionFailure::UnexpectedMessage, true);
  }

  void process_recovery_event(const StreamEvent& event) override {
    if (state() != SessionState::Streaming ||
        !committed_stream_id_.has_value() ||
        event.header.video_stream_id != *committed_stream_id_) {
      return;
    }
    (void)recovery_gate_.begin_recovery();
  }

  void finish_receive_cycle() override {
    if (state() != SessionState::Streaming ||
        !recovery_gate_.gated() ||
        !recovery_gate_.recovery_id().has_value()) {
      return;
    }
    const std::uint32_t recovery_id = *recovery_gate_.recovery_id();
    if (requested_recovery_id_.has_value() &&
        *requested_recovery_id_ == recovery_id) {
      return;
    }
    queue_recovery_request(recovery_id);
    requested_recovery_id_ = recovery_id;
  }

  void process_video(const Header& header, std::vector<std::uint8_t> payload,
                     std::optional<std::uint32_t> recovery_id) override {
    if (state() == SessionState::Paused) {
      return;
    }
    if (header.session_nonce != config_.session_nonce) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    if (surface_ != nullptr && !surface_available()) {
      return;
    }
    const bool is_new_stream =
        committed_stream_id_.has_value() &&
        header.video_stream_id == *committed_stream_id_;
    const bool is_retained_old_stream =
        previous_stream_.has_value() &&
        header.video_stream_id == previous_stream_->video_stream_id;
    if (!is_new_stream && !is_retained_old_stream) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    if (is_retained_old_stream && state() != SessionState::Reconfiguring &&
        state() != SessionState::AwaitingFirstIdr) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    if (state() == SessionState::AwaitingFirstIdr) {
      if (is_retained_old_stream) {
        received_frames_.push_back(ReceivedVideoFrame{
            header, std::move(payload), surface_generation_, std::nullopt});
        last_good_frame_ = received_frames_.back();
        return;
      }
      if ((header.flags & kFlagKeyFrame) == 0) {
        return;
      }
      if (previous_stream_.has_value()) {
        if (!released_previous_stream_) {
          if (boundary_ != nullptr) {
            boundary_->release_stream(previous_stream_->config_id,
                                      previous_stream_->video_stream_id);
          }
          released_previous_stream_ = true;
        }
        previous_stream_.reset();
      }
      if (pending_committed_stream_.has_value()) {
        set_negotiated_stream(*pending_committed_stream_);
        pending_committed_stream_.reset();
        reconfiguration_ = ReconfigurationStatus{
            negotiated_stream()->config_id, ReconfigurationPhase::Completed,
            std::nullopt, false};
      }
      transition(SessionState::Streaming, std::nullopt);
      begin_liveness();
      awaiting_rollback_idr_ = false;
      if (awaiting_reconfiguration_idr_) {
        awaiting_reconfiguration_idr_ = false;
        queue_commit_ack(negotiated_stream()->config_id,
                         negotiated_stream()->video_stream_id, true);
      }
    } else if (state() == SessionState::Reconfiguring) {
      received_frames_.push_back(ReceivedVideoFrame{
          header, std::move(payload), surface_generation_, std::nullopt});
      last_good_frame_ = received_frames_.back();
      return;
    } else if (state() != SessionState::Streaming) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    if (awaiting_rollback_idr_) {
      if ((header.flags & kFlagKeyFrame) == 0) {
        return;
      }
      awaiting_rollback_idr_ = false;
    }
    if (recovery_gate_.observe_frame(header.flags, recovery_id) ==
        RecoveryFrameAction::Drop) {
      return;
    }
    ReceivedVideoFrame frame{
        header, std::move(payload), surface_generation_, std::move(recovery_id)};
    last_good_frame_ = frame;
    received_frames_.push_back(std::move(frame));
  }

  void queue_recovery_retry_if_due() {
    if (state() != SessionState::Streaming ||
        !committed_stream_id_.has_value()) {
      return;
    }
    const auto recovery_id = recovery_gate_.retry_if_due();
    if (!recovery_id.has_value()) {
      return;
    }
    queue_recovery_request(*recovery_id);
    requested_recovery_id_ = *recovery_id;
  }

  void queue_recovery_request(std::uint32_t recovery_id) {
    queue_control(ControlOpcode::RequestIdr, 0, 0,
                  encode_recovery_id(recovery_id), *committed_stream_id_);
  }

  void process_clock(const Header& header,
                     std::span<const std::uint8_t> payload) override {
    const auto message = decode_clock_message(payload);
    if (!message || message.value().kind != ClockMessageKind::Request) {
      return;
    }
    const std::uint64_t t1_ns = now_ns();
    const std::uint64_t t2_ns = now_ns();
    queue_clock(
        ClockMessage{ClockMessageKind::Response, message.value().t0_ns, t1_ns,
                     t2_ns},
        header.seq);
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

    if (last_failed_config_id_ == message.config_id) {
      queue_control(ControlOpcode::ProtocolError, 0, message.config_id,
                    {static_cast<std::uint8_t>(
                        last_failed_reason_.value_or(SessionFailure::PrepareFailed))},
                    0);
      return;
    }
    if (last_commit_succeeded_ &&
        message.config_id == last_committed_config_id_ &&
        last_committed_profile_.has_value() &&
        *last_committed_profile_ == profile.value()) {
      queue_profile_control(ControlOpcode::StreamReady, message.config_id,
                            *last_committed_profile_);
      return;
    }

    if (prepared_config_id_.has_value()) {
      if ((state() != SessionState::AwaitingCommit &&
           state() != SessionState::Reconfiguring) ||
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

    const bool reconfiguring_existing_stream =
        negotiated_stream().has_value() &&
        state() != SessionState::AwaitingPrepare;
    if (reconfiguring_existing_stream) {
      if (state() != SessionState::Reconfiguring) {
        reconfiguration_previous_state_ = state();
      }
    }

    if (surface_ != nullptr && !surface_available()) {
      const SessionFailure failure = SessionFailure::PrepareFailed;
      if (reconfiguring_existing_stream) {
        last_failed_config_id_ = message.config_id;
        last_failed_reason_ = failure;
        reconfiguration_ = ReconfigurationStatus{
            message.config_id, ReconfigurationPhase::Failed, failure, false};
        const SessionState return_state =
            reconfiguration_previous_state_.value_or(SessionState::Streaming);
        transition(return_state, std::nullopt);
        queue_control(ControlOpcode::ProtocolError, 0, message.config_id,
                      {static_cast<std::uint8_t>(failure)}, 0);
      } else {
        fail_with(failure, true);
      }
      return;
    }

    if (boundary_ != nullptr) {
      const BoundaryResult result =
          boundary_->prepare_stream(profile.value(), message.config_id);
      if (result != BoundaryResult::Ok) {
        const SessionFailure failure =
            result == BoundaryResult::DisplayError
                ? SessionFailure::DisplayError
                : SessionFailure::PrepareFailed;
        if (reconfiguring_existing_stream) {
          last_failed_config_id_ = message.config_id;
          last_failed_reason_ = failure;
          reconfiguration_ = ReconfigurationStatus{
              message.config_id, ReconfigurationPhase::Failed, failure, false};
          const SessionState return_state =
              reconfiguration_previous_state_.value_or(SessionState::Streaming);
          transition(return_state == SessionState::Paused
                         ? SessionState::Paused
                         : SessionState::Streaming,
                     std::nullopt);
          if (return_state != SessionState::Paused &&
              negotiated_stream().has_value()) {
            reset_decoder_stream(negotiated_stream()->video_stream_id);
          }
          queue_control(ControlOpcode::ProtocolError, 0, message.config_id,
                        {static_cast<std::uint8_t>(failure)}, 0);
        } else {
          fail_with(failure, true);
        }
        return;
      }
    }

    prepared_config_id_ = message.config_id;
    prepared_profile_ = profile.value();
    prepared_stream_id_.reset();
    discard_called_ = false;
    rollback_called_ = false;
    highest_config_id_ = std::max(highest_config_id_, message.config_id);
    last_failed_config_id_.reset();
    last_failed_reason_.reset();
    reconfiguration_ = ReconfigurationStatus{
        message.config_id, ReconfigurationPhase::AwaitingCommitted, std::nullopt,
        false};
    transition(reconfiguring_existing_stream ? SessionState::Reconfiguring
                                             : SessionState::AwaitingCommit,
               SessionFailure::CommitTimeout);
    queue_profile_control(ControlOpcode::StreamReady, message.config_id,
                          *prepared_profile_);
  }

  void handle_pause(const Header& header, const ControlMessage& message) {
    if (header.video_stream_id != 0 || message.config_id == 0) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    if (state() == SessionState::Paused) {
      queue_control(ControlOpcode::PauseAck, 0, message.config_id, {}, 0);
      return;
    }
    if (state() == SessionState::Reconfiguring ||
        (state() == SessionState::AwaitingFirstIdr &&
         previous_stream_.has_value())) {
      // The Host may pause before the candidate stream's first IDR (lock screen
      // or display error). Roll the unconfirmed candidate back first, then
      // honor the pause on the retained stream instead of failing the session.
      abort_reconfiguration(SessionFailure::UnexpectedMessage, false);
      if (is_terminal()) {
        return;
      }
    }
    if (state() == SessionState::AwaitingFirstIdr &&
        !previous_stream_.has_value()) {
      // The initial handshake committed but no IDR has arrived yet. Honor the
      // pause instead of terminating the freshly established session.
      if (!negotiated_stream().has_value() ||
          message.config_id != negotiated_stream()->config_id) {
        fail_with(SessionFailure::UnexpectedMessage, true);
        return;
      }
      awaiting_reconfiguration_idr_ = false;
      transition(SessionState::Paused, std::nullopt);
      queue_control(ControlOpcode::PauseAck, 0, message.config_id, {}, 0);
      return;
    }
    if (state() != SessionState::Streaming || !negotiated_stream().has_value() ||
        message.config_id != negotiated_stream()->config_id) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    transition(SessionState::Paused, std::nullopt);
    queue_control(ControlOpcode::PauseAck, 0, message.config_id, {}, 0);
  }

  void handle_resume(const Header& header, const ControlMessage& message) {
    if (header.video_stream_id != 0 || message.config_id == 0) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    if (state() == SessionState::Reconfiguring) {
      return;
    }
    if (state() != SessionState::Paused) {
      fail_with(SessionFailure::UnexpectedMessage, true);
      return;
    }
    reconfiguration_previous_state_ = SessionState::Paused;
    transition(SessionState::Reconfiguring, SessionFailure::PrepareTimeout);
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
    prepared_stream_id_ = *stream_id;
    rollback_called_ = false;

    if (boundary_ != nullptr) {
      const BoundaryResult result = boundary_->commit_stream(
          *prepared_profile_, *prepared_config_id_, *stream_id);
      if (result != BoundaryResult::Ok) {
        const SessionFailure failure =
            result == BoundaryResult::DisplayError
                ? SessionFailure::DisplayError
                : SessionFailure::CommitFailed;
        if (negotiated_stream().has_value()) {
          abort_prepared_configuration(*prepared_config_id_, failure, true, false);
        } else {
          fail_with(failure, true);
        }
        return;
      }
    }

    const std::uint32_t committed_config_id = *prepared_config_id_;
    const StreamProfile committed_profile = *prepared_profile_;
    const bool had_active_stream = negotiated_stream().has_value();
    last_commit_was_reconfiguration_ = had_active_stream;
    set_commit_history(committed_config_id, committed_profile, *stream_id);
    if (had_active_stream) {
      previous_stream_ = negotiated_stream();
      pending_committed_stream_ =
          NegotiatedStream{committed_profile, committed_config_id, *stream_id};
      released_previous_stream_ = false;
      awaiting_reconfiguration_idr_ = true;
      reset_decoder_stream(*stream_id);
      reconfiguration_ = ReconfigurationStatus{
          committed_config_id, ReconfigurationPhase::AwaitingFirstIdr, std::nullopt,
          false};
    } else {
      set_negotiated_stream(
          NegotiatedStream{committed_profile, committed_config_id, *stream_id});
      reset_decoder_stream(*stream_id);
      reconfiguration_ = ReconfigurationStatus{
          committed_config_id, ReconfigurationPhase::AwaitingFirstIdr, std::nullopt,
          false};
    }
    committed_stream_id_ = *stream_id;
    prepared_config_id_.reset();
    prepared_profile_.reset();
    prepared_stream_id_.reset();
    recovery_gate_.reset();
    requested_recovery_id_.reset();
    transition(SessionState::AwaitingFirstIdr, SessionFailure::InitialIdrTimeout);
    queue_commit_ack(committed_config_id, *stream_id, false);
  }

  void queue_commit_ack(std::uint32_t config_id, std::uint32_t video_stream_id,
                        bool first_idr_confirmed) {
    queue_control(ControlOpcode::StreamCommitted, 0, config_id,
                  commit_ack_body(video_stream_id, first_idr_confirmed),
                  video_stream_id);
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
    if (surface_ != nullptr && !surface_available()) {
      received_frames_.clear();
    }
    return std::exchange(received_frames_, {});
  }

  SessionCommandStatus send_input(const InputEvent& event) {
    if (is_terminal()) {
      return SessionCommandStatus::InvalidState;
    }
    if (state() != SessionState::Streaming ||
        !committed_stream_id_.has_value()) {
      return SessionCommandStatus::InvalidState;
    }
    const auto payload = encode_input_payload(event);
    if (!payload) {
      return SessionCommandStatus::InvalidArgument;
    }
    Packet packet;
    packet.header.type = static_cast<std::uint8_t>(MessageType::Input);
    packet.header.session_nonce = config_.session_nonce;
    packet.header.video_stream_id = *committed_stream_id_;
    packet.header.seq = next_input_seq_++;
    packet.header.payload_len =
        static_cast<std::uint32_t>(payload.value().size());
    packet.payload = payload.value();
    const auto encoded = encode_packet(packet);
    if (!encoded) {
      return SessionCommandStatus::InvalidArgument;
    }
    queue_outbound(std::move(encoded.value()));
    ++input_events_sent_;
    return SessionCommandStatus::Ok;
  }

  std::uint64_t attach_surface() {
    if (surface_ == nullptr) {
      return 0;
    }
    surface_lease_.release();
    surface_generation_ = surface_->attach();
    auto lease = surface_->acquire();
    if (!lease.has_value()) {
      surface_generation_ = 0;
      return 0;
    }
    surface_lease_ = std::move(lease.value());
    return surface_generation_;
  }

  void invalidate_surface() {
    if (surface_ == nullptr) {
      return;
    }
    surface_->invalidate();
    surface_lease_.release();
    surface_generation_ = 0;
    received_frames_.clear();
  }

  [[nodiscard]] bool surface_available() const {
    if (surface_ == nullptr) {
      return true;
    }
    return surface_lease_.holds_reference() &&
           surface_lease_.can_render() &&
           surface_->can_render(surface_generation_);
  }

  [[nodiscard]] bool recovery_gated() const {
    return recovery_gate_.gated();
  }
  [[nodiscard]] const std::optional<std::uint32_t>& recovery_id() const {
    return recovery_gate_.recovery_id();
  }
  [[nodiscard]] const std::optional<ReceivedVideoFrame>& last_good_frame()
      const {
    return last_good_frame_;
  }

  void request_stop() {
    if (is_terminal() || state() == SessionState::Stopping) {
      return;
    }
    transition(SessionState::Stopping, std::nullopt);
    end_liveness();
    clear_outbound();
    cancel_pending_io();
  }

  void complete_stop() {
    if (state() != SessionState::Stopping) {
      return;
    }
    if (boundary_ != nullptr) {
      boundary_->drain_callbacks();
    }
    if (prepared_config_id_.has_value() && boundary_ != nullptr &&
        !discard_called_) {
      boundary_->discard_prepared_stream(*prepared_config_id_);
      discard_called_ = true;
    }
    if (pending_committed_stream_.has_value() && boundary_ != nullptr &&
        !rollback_called_) {
      boundary_->rollback_stream(pending_committed_stream_->config_id,
                                 pending_committed_stream_->video_stream_id);
      rollback_called_ = true;
    }
    release_active_stream_once();
    invalidate_surface();
    prepared_config_id_.reset();
    prepared_profile_.reset();
    prepared_stream_id_.reset();
    pending_committed_stream_.reset();
    previous_stream_.reset();
    received_frames_.clear();
    transition(SessionState::Stopped, std::nullopt);
  }

  void release_active_stream_once() {
    if (!negotiated_stream().has_value() || active_stream_released_ ||
        boundary_ == nullptr) {
      return;
    }
    boundary_->release_stream(negotiated_stream()->config_id,
                              negotiated_stream()->video_stream_id);
    active_stream_released_ = true;
  }

  [[nodiscard]] const std::optional<ReconfigurationStatus>& reconfiguration() const {
    return reconfiguration_;
  }

  void set_commit_history(std::uint32_t config_id, const StreamProfile& profile,
                          std::uint32_t stream_id) {
    last_committed_config_id_ = config_id;
    last_committed_profile_ = profile;
    last_committed_stream_id_ = stream_id;
    last_commit_succeeded_ = true;
    highest_config_id_ = std::max(highest_config_id_, config_id);
  }

  void abort_prepared_configuration(std::uint32_t config_id,
                                    SessionFailure failure, bool notify_peer,
                                    bool commit_was_attempted) {
    if (!prepared_config_id_.has_value() ||
        *prepared_config_id_ != config_id) {
      return;
    }
    const auto stream_id = prepared_stream_id_;
    if (boundary_ != nullptr) {
      if (commit_was_attempted && stream_id.has_value()) {
        if (!rollback_called_) {
          boundary_->rollback_stream(config_id, *stream_id);
          rollback_called_ = true;
        }
      } else if (!discard_called_) {
        boundary_->discard_prepared_stream(config_id);
        discard_called_ = true;
      }
    }
    last_failed_config_id_ = config_id;
    last_failed_reason_ = failure;
    reconfiguration_ = ReconfigurationStatus{
        config_id, ReconfigurationPhase::Failed, failure, false};
    prepared_config_id_.reset();
    prepared_profile_.reset();
    prepared_stream_id_.reset();
    awaiting_reconfiguration_idr_ = false;
    const SessionState rollback_state =
        reconfiguration_previous_state_.value_or(SessionState::Streaming);
    if (previous_stream_.has_value()) {
      const std::uint32_t previous_stream_id =
          previous_stream_->video_stream_id;
      committed_stream_id_ = previous_stream_id;
      if (commit_was_attempted) {
        retarget_decoder_stream(previous_stream_id);
        awaiting_rollback_idr_ = true;
      }
      transition(rollback_state, std::nullopt);
    } else {
      transition(rollback_state, std::nullopt);
    }
    if (notify_peer) {
      queue_control(ControlOpcode::ProtocolError, 0, config_id,
                    {static_cast<std::uint8_t>(failure)}, 0);
    }
  }

  void abort_reconfiguration(SessionFailure failure, bool notify_peer) {
    if (prepared_config_id_.has_value()) {
      abort_prepared_configuration(
          *prepared_config_id_, failure, notify_peer,
          pending_committed_stream_.has_value());
      return;
    }
    if (pending_committed_stream_.has_value() && previous_stream_.has_value()) {
      const std::uint32_t failed_config_id = pending_committed_stream_->config_id;
      const std::uint32_t failed_stream_id =
          pending_committed_stream_->video_stream_id;
      if (boundary_ != nullptr && !rollback_called_) {
        boundary_->rollback_stream(failed_config_id, failed_stream_id);
        rollback_called_ = true;
      }
      reconfiguration_ = ReconfigurationStatus{
          failed_config_id, ReconfigurationPhase::Failed, failure, false};
      pending_committed_stream_.reset();
      awaiting_reconfiguration_idr_ = false;
      committed_stream_id_ = previous_stream_->video_stream_id;
      retarget_decoder_stream(*committed_stream_id_);
      awaiting_rollback_idr_ = true;
      rolled_back_config_ids_.insert(failed_config_id);
      previous_stream_.reset();
      transition(reconfiguration_previous_state_.value_or(SessionState::Streaming),
                 std::nullopt);
      if (notify_peer) {
        queue_control(ControlOpcode::ProtocolError, 0, failed_config_id,
                      {static_cast<std::uint8_t>(failure)}, 0);
      }
      return;
    }
    fail_with(failure, notify_peer);
  }

  void handle_stage_timeout() override {
    const auto failure = timeout_reason().value_or(SessionFailure::UnexpectedMessage);
    if (state() == SessionState::Reconfiguring) {
      abort_reconfiguration(failure, true);
      return;
    }
    if (state() == SessionState::AwaitingFirstIdr &&
        previous_stream_.has_value()) {
      abort_reconfiguration(failure, true);
      return;
    }
    EndpointCore::handle_stage_timeout();
  }

  void on_terminal_failure() override {
    end_liveness();
    if (boundary_ != nullptr) {
      boundary_->drain_callbacks();
    }
    if (prepared_config_id_.has_value() && boundary_ != nullptr &&
        !discard_called_) {
      boundary_->discard_prepared_stream(*prepared_config_id_);
      discard_called_ = true;
    }
    if (pending_committed_stream_.has_value() && boundary_ != nullptr &&
        !rollback_called_) {
      boundary_->rollback_stream(pending_committed_stream_->config_id,
                                 pending_committed_stream_->video_stream_id);
      rollback_called_ = true;
    }
    release_active_stream_once();
    invalidate_surface();
    received_frames_.clear();
  }

  DeviceSessionConfig config_;
  DeviceBoundary* boundary_;
  std::optional<std::uint32_t> prepared_config_id_;
  std::optional<StreamProfile> prepared_profile_;
  std::optional<std::uint32_t> committed_stream_id_;
  std::optional<SessionState> reconfiguration_previous_state_;
  std::optional<std::uint32_t> prepared_stream_id_;
  std::optional<NegotiatedStream> previous_stream_;
  std::optional<NegotiatedStream> pending_committed_stream_;
  std::optional<ReconfigurationStatus> reconfiguration_;
  std::optional<std::uint32_t> last_failed_config_id_;
  std::optional<SessionFailure> last_failed_reason_;
  std::optional<std::uint32_t> last_committed_config_id_;
  std::optional<StreamProfile> last_committed_profile_;
  std::optional<std::uint32_t> last_committed_stream_id_;
  std::uint32_t highest_config_id_{};
  SurfaceOwner* surface_;
  SurfaceLease surface_lease_;
  std::uint64_t surface_generation_{};
  bool last_commit_succeeded_{};
  bool last_commit_was_reconfiguration_{};
  std::set<std::uint32_t> rolled_back_config_ids_;
  bool discard_called_{};
  bool rollback_called_{};
  bool released_previous_stream_{};
  bool active_stream_released_{};
  bool awaiting_reconfiguration_idr_{};
  bool awaiting_rollback_idr_{};
  std::vector<ReceivedVideoFrame> received_frames_;
  DeviceRecoveryGate recovery_gate_;
  std::optional<ReceivedVideoFrame> last_good_frame_;
  std::optional<std::uint32_t> requested_recovery_id_;
  std::uint32_t next_input_seq_{1};
  std::uint64_t input_events_sent_{};
};

HostSession::HostSession(ByteIo& io, MonotonicClock& clock, HostSessionConfig config,
                         HostBoundary* boundary)
    : impl_(std::make_unique<Impl>(io, clock, std::move(config), boundary)) {}

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
const std::optional<SchedulerIdrRequest>& HostSession::pending_idr_request()
    const {
  return impl_->pending_idr_request();
}
bool HostSession::capture_encoding_intent() const {
  return impl_->capture_encoding_intent();
}
bool HostSession::input_injection_intent() const {
  return impl_->input_injection_intent();
}
bool HostSession::pause_acknowledged() const {
  return impl_->pause_acknowledged();
}
bool HostSession::extended_display_present() const {
  return impl_->extended_display_present();
}
HostVideoState HostSession::video_state() const { return impl_->video_state(); }
HostInputState HostSession::input_state() const { return impl_->input_state(); }
SessionCommandStatus HostSession::handle_host_event(HostEvent event) {
  return impl_->handle_host_event(event);
}
void HostSession::request_stop() { impl_->request_stop(); }
void HostSession::complete_stop() { impl_->complete_stop(); }
void HostSession::stop() {
  request_stop();
  complete_stop();
}
SessionCommandStatus HostSession::pause() { return impl_->pause(); }
SessionCommandStatus HostSession::resume(const StreamProfile& profile,
                                        std::uint32_t config_id,
                                        std::uint32_t video_stream_id) {
  return impl_->resume(profile, config_id, video_stream_id);
}
SessionCommandStatus HostSession::reconfigure(const StreamProfile& profile,
                                              std::uint32_t config_id,
                                              std::uint32_t video_stream_id) {
  return impl_->reconfigure(profile, config_id, video_stream_id);
}
SessionCommandStatus HostSession::request_clock_sync() {
  return impl_->request_clock_sync();
}
const std::optional<ReconfigurationStatus>& HostSession::reconfiguration() const {
  return impl_->reconfiguration();
}
const std::optional<ClockEstimate>& HostSession::clock_estimate() const {
  return impl_->clock_estimate();
}
VideoSendStatus HostSession::send_video_frame(std::span<const std::uint8_t> payload,
                                              std::uint64_t timestamp_us,
                                              bool keyframe) {
  return impl_->send_video_frame(payload, timestamp_us, keyframe);
}
std::vector<InputEvent> HostSession::take_input_events() {
  return impl_->take_input_events();
}

DeviceSession::DeviceSession(ByteIo& io, MonotonicClock& clock,
                             DeviceSessionConfig config, DeviceBoundary* boundary,
                             SurfaceOwner* surface)
    : impl_(std::make_unique<Impl>(io, clock, std::move(config), boundary,
                                   surface)) {}

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
const std::optional<ReconfigurationStatus>& DeviceSession::reconfiguration() const {
  return impl_->reconfiguration();
}
std::uint64_t DeviceSession::attach_surface() { return impl_->attach_surface(); }
void DeviceSession::invalidate_surface() { impl_->invalidate_surface(); }
bool DeviceSession::surface_available() const {
  return impl_->surface_available();
}
void DeviceSession::request_stop() { impl_->request_stop(); }
void DeviceSession::complete_stop() { impl_->complete_stop(); }
void DeviceSession::stop() {
  request_stop();
  complete_stop();
}
std::vector<ReceivedVideoFrame> DeviceSession::take_video_frames() {
  return impl_->take_video_frames();
}
SessionCommandStatus DeviceSession::send_input(const InputEvent& event) {
  return impl_->send_input(event);
}
bool DeviceSession::recovery_gated() const { return impl_->recovery_gated(); }
const std::optional<std::uint32_t>& DeviceSession::recovery_id() const {
  return impl_->recovery_id();
}
const std::optional<ReceivedVideoFrame>& DeviceSession::last_good_frame() const {
  return impl_->last_good_frame();
}

}  // namespace deskmate::protocol
