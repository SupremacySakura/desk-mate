#pragma once

#include "deskmate/byte_io.hpp"
#include "deskmate/clock.hpp"
#include "deskmate/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace deskmate::protocol {

inline constexpr std::uint64_t kHandshakeStageTimeoutNs = 2'000'000'000ULL;
inline constexpr std::size_t kDefaultVideoFragmentPayload = 32U * 1024U;
inline constexpr std::size_t kMaxCapabilityItems = 8;

enum class VideoEncoder : std::uint8_t {
  H264 = 1,
  Hevc = 2,
};

enum class PixelFormat : std::uint8_t {
  Nv12 = 1,
  P010 = 2,
  Bgra8 = 3,
};

struct DisplayResolution {
  std::uint16_t width{};
  std::uint16_t height{};
  std::uint16_t refresh_hz{60};

  friend bool operator==(const DisplayResolution&, const DisplayResolution&) = default;
};

struct CapabilitySet {
  std::vector<VideoEncoder> encoders;
  std::vector<PixelFormat> pixel_formats;
  std::vector<DisplayResolution> resolutions;
};

struct StreamProfile {
  VideoEncoder encoder{VideoEncoder::H264};
  PixelFormat pixel_format{PixelFormat::Nv12};
  DisplayResolution resolution{};

  friend bool operator==(const StreamProfile&, const StreamProfile&) = default;
};

struct NegotiatedStream {
  StreamProfile profile{};
  std::uint32_t config_id{};
  std::uint32_t video_stream_id{};

  friend bool operator==(const NegotiatedStream&, const NegotiatedStream&) = default;
};

struct ReceivedVideoFrame {
  Header header{};
  std::vector<std::uint8_t> payload;
};

enum class SessionState {
  Idle,
  AwaitingClientHello,
  AwaitingPrepare,
  AwaitingReady,
  AwaitingCommit,
  AwaitingCommitted,
  AwaitingFirstIdr,
  Streaming,
  Failed,
};

enum class SessionFailure : std::uint8_t {
  None = 0,
  UnsupportedProtocolRevision = 1,
  NoCommonCapabilities = 2,
  InvalidConfiguration = 3,
  MalformedControlMessage = 4,
  UnexpectedMessage = 5,
  HelloTimeout = 6,
  ReadyTimeout = 7,
  CommittedTimeout = 8,
  PrepareTimeout = 9,
  CommitTimeout = 10,
  InitialIdrTimeout = 11,
  PrepareFailed = 12,
  CommitFailed = 13,
  DisplayError = 14,
  IoError = 15,
  InvalidWireData = 16,
};

enum class VideoSendStatus {
  Sent,
  NotStreaming,
  IdrRequired,
  EmptyPayload,
  InvalidArgument,
  SessionFailed,
};

enum class BoundaryResult {
  Ok,
  DisplayError,
  Failed,
};

struct SessionLimits {
  std::uint64_t stage_timeout_ns{kHandshakeStageTimeoutNs};
  std::size_t video_fragment_payload{kDefaultVideoFragmentPayload};
};

struct HostSessionConfig {
  CapabilitySet capabilities;
  std::uint32_t video_stream_id{};
  std::uint32_t initial_config_id{1};
  std::uint32_t protocol_revision{kProtocolRevision};
  SessionLimits limits{};
};

struct DeviceSessionConfig {
  std::uint32_t session_nonce{};
  CapabilitySet capabilities;
  std::uint32_t protocol_revision{kProtocolRevision};
  SessionLimits limits{};
};

class DeviceBoundary {
 public:
  DeviceBoundary() = default;
  DeviceBoundary(const DeviceBoundary&) = delete;
  DeviceBoundary& operator=(const DeviceBoundary&) = delete;
  virtual ~DeviceBoundary() = default;

  [[nodiscard]] virtual BoundaryResult prepare_stream(
      const StreamProfile& profile, std::uint32_t config_id) = 0;
  [[nodiscard]] virtual BoundaryResult commit_stream(
      const StreamProfile& profile, std::uint32_t config_id,
      std::uint32_t video_stream_id) = 0;
};

[[nodiscard]] std::string_view session_failure_name(SessionFailure failure);
[[nodiscard]] std::string_view session_state_name(SessionState state);

[[nodiscard]] Result<std::vector<std::uint8_t>> encode_capabilities(
    const CapabilitySet& capabilities);
[[nodiscard]] Result<CapabilitySet> decode_capabilities(
    std::span<const std::uint8_t> bytes);
[[nodiscard]] Result<std::vector<std::uint8_t>> encode_stream_profile(
    const StreamProfile& profile);
[[nodiscard]] Result<StreamProfile> decode_stream_profile(
    std::span<const std::uint8_t> bytes);

class HostSession {
 public:
  HostSession(ByteIo& io, MonotonicClock& clock, HostSessionConfig config);
  ~HostSession();

  HostSession(const HostSession&) = delete;
  HostSession& operator=(const HostSession&) = delete;

  void start();
  void pump();

  [[nodiscard]] SessionState state() const;
  [[nodiscard]] bool is_terminal() const;
  [[nodiscard]] std::optional<SessionFailure> failure() const;
  [[nodiscard]] std::string_view failure_detail() const;
  [[nodiscard]] std::uint32_t session_nonce() const;
  [[nodiscard]] const std::optional<NegotiatedStream>& negotiated_stream() const;
  [[nodiscard]] VideoSendStatus send_video_frame(
      std::span<const std::uint8_t> payload, std::uint64_t timestamp_us,
      bool keyframe);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

class DeviceSession {
 public:
  DeviceSession(ByteIo& io, MonotonicClock& clock, DeviceSessionConfig config,
                DeviceBoundary* boundary = nullptr);
  ~DeviceSession();

  DeviceSession(const DeviceSession&) = delete;
  DeviceSession& operator=(const DeviceSession&) = delete;

  void start();
  void pump();

  [[nodiscard]] SessionState state() const;
  [[nodiscard]] bool is_terminal() const;
  [[nodiscard]] std::optional<SessionFailure> failure() const;
  [[nodiscard]] std::string_view failure_detail() const;
  [[nodiscard]] std::uint32_t session_nonce() const;
  [[nodiscard]] std::uint32_t video_stream_id() const;
  [[nodiscard]] const std::optional<NegotiatedStream>& negotiated_stream() const;
  [[nodiscard]] std::vector<ReceivedVideoFrame> take_video_frames();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deskmate::protocol
