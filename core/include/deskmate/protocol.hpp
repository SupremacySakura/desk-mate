#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace deskmate::protocol {

inline constexpr std::size_t kWireHeaderSize = 32;
inline constexpr std::uint8_t kWireVersion = 1;
inline constexpr std::uint32_t kProtocolRevision = 1;
inline constexpr std::size_t kMaxPacketPayload = 65536;
inline constexpr std::size_t kMaxNonVideoPayload = 4096;
inline constexpr std::uint8_t kMaxVideoFragments = 64;
inline constexpr std::size_t kMaxVideoFramePayload =
    kMaxPacketPayload * static_cast<std::size_t>(kMaxVideoFragments);

enum class MessageType : std::uint8_t {
  Video = 0x01,
  Audio = 0x02,
  Input = 0x03,
  Control = 0x04,
  Clock = 0x05,
};

enum class ControlOpcode : std::uint8_t {
  ClientHello = 0x01,
  StreamPrepare = 0x02,
  StreamReady = 0x03,
  StreamCommit = 0x04,
  StreamCommitted = 0x05,
  Pause = 0x06,
  PauseAck = 0x07,
  Resume = 0x08,
  Heartbeat = 0x09,
  RequestIdr = 0x0A,
  ProtocolError = 0x0B,
};

inline constexpr std::uint16_t kFlagKeyFrame = 0x0001;
inline constexpr std::uint16_t kFlagRecoveryIdr = 0x0002;

enum class ErrorCode {
  None,
  TruncatedHeader,
  InvalidMagic,
  UnsupportedVersion,
  UnsupportedMessageType,
  InvalidFlags,
  InvalidFragmentation,
  PayloadTooLarge,
  EmptyVideoFragment,
  InvalidSessionNonce,
  InvalidVideoStreamId,
  SessionMismatch,
  VideoStreamMismatch,
  DuplicateFragment,
  FragmentMetadataMismatch,
  InterleavedFrame,
  OutOfOrderFrame,
  SequenceDiscontinuity,
  RecoveryIdMissing,
  InvalidControlPayload,
  UnsupportedControlOpcode,
  InvalidLocalArgument,
};

enum class ErrorAction {
  None,
  DiscardMessage,
  RequestRecovery,
  ResetStream,
  RejectLocalArgument,
};

struct ProtocolError {
  ErrorCode code{};
  ErrorAction action{};
  std::string_view detail{};
};

template <typename T>
class Result {
 public:
  static Result success(T value) { return Result(std::move(value)); }
  static Result failure(ProtocolError error) { return Result(error); }

  [[nodiscard]] bool has_value() const { return std::holds_alternative<T>(storage_); }
  explicit operator bool() const { return has_value(); }

  [[nodiscard]] T& value() { return std::get<T>(storage_); }
  [[nodiscard]] const T& value() const { return std::get<T>(storage_); }
  [[nodiscard]] const ProtocolError& error() const { return std::get<ProtocolError>(storage_); }

 private:
  explicit Result(T value) : storage_(std::move(value)) {}
  explicit Result(ProtocolError error) : storage_(error) {}

  std::variant<T, ProtocolError> storage_;
};

struct Header {
  std::array<std::uint8_t, 2> magic{'D', 'M'};
  std::uint8_t version{kWireVersion};
  std::uint8_t type{};
  std::uint16_t flags{};
  std::uint8_t fragment_index{};
  std::uint8_t fragment_total{1};
  std::uint32_t session_nonce{};
  std::uint32_t video_stream_id{};
  std::uint32_t seq{};
  std::uint32_t payload_len{};
  std::uint64_t timestamp_us{};
};

struct Packet {
  Header header{};
  std::vector<std::uint8_t> payload;
};

struct ControlMessage {
  ControlOpcode opcode{};
  std::uint32_t proto_rev{};
  std::uint32_t config_id{};
  std::vector<std::uint8_t> body;
};

enum class InputKind : std::uint8_t {
  TouchDown = 1,
  TouchMove = 2,
  TouchUp = 3,
  TouchCancel = 4,
  Scroll = 5,
  Key = 6,
};

// Bit 0 of `flags` is the primary-button state for pointer events. `x` and `y`
// are normalized to the Device display (0..1); `dx`/`dy` carry scroll deltas in
// display widths/heights. A `keycode` is only meaningful for `InputKind::Key`.
inline constexpr std::uint16_t kInputFlagPrimaryButton = 0x0001;

struct InputEvent {
  InputKind kind{InputKind::TouchMove};
  std::uint8_t pointer_id{};
  std::uint16_t flags{};
  float x{};
  float y{};
  float dx{};
  float dy{};
  std::uint32_t keycode{};
};

struct VideoFrameToSend {
  std::uint32_t session_nonce{};
  std::uint32_t video_stream_id{};
  std::uint32_t seq{};
  std::uint64_t timestamp_us{};
  std::uint16_t flags{};
  std::optional<std::uint32_t> recovery_id;
  std::vector<std::uint8_t> payload;
};

[[nodiscard]] std::array<std::uint8_t, kWireHeaderSize> encode_header(const Header& header);
[[nodiscard]] Result<Header> decode_header(std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_packet(const Packet& packet);
[[nodiscard]] Result<std::vector<std::vector<std::uint8_t>>> encode_video_fragments(
    const VideoFrameToSend& frame, std::size_t max_fragment_payload);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_control_payload(const ControlMessage& message);
[[nodiscard]] Result<ControlMessage> decode_control_payload(
    std::span<const std::uint8_t> payload);

[[nodiscard]] Result<std::vector<std::uint8_t>> encode_input_payload(
    const InputEvent& event);
[[nodiscard]] Result<InputEvent> decode_input_payload(
    std::span<const std::uint8_t> payload);

[[nodiscard]] bool is_supported_message_type(std::uint8_t type);
[[nodiscard]] bool is_session_level_message(std::uint8_t type);
[[nodiscard]] std::optional<ProtocolError> validate_header_structure(const Header& header);

}  // namespace deskmate::protocol
