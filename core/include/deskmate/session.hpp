#pragma once

#include "deskmate/byte_io.hpp"
#include "deskmate/clock.hpp"
#include "deskmate/protocol.hpp"
#include "deskmate/recovery.hpp"
#include "deskmate/scheduler.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace deskmate::protocol {

inline constexpr std::uint64_t kHandshakeStageTimeoutNs = 2'000'000'000ULL;
inline constexpr std::uint64_t kHeartbeatIntervalNs = 1'000'000'000ULL;
inline constexpr std::uint64_t kPeerSilenceTimeoutNs = 3'000'000'000ULL;
inline constexpr std::uint64_t kMinPauseGraceTimeoutNs = 30'000'000'000ULL;
inline constexpr std::uint64_t kMaxPauseGraceTimeoutNs = 60'000'000'000ULL;
inline constexpr std::uint64_t kDefaultPauseGraceTimeoutNs =
    kMinPauseGraceTimeoutNs;
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
  std::uint64_t surface_generation{};
  std::optional<std::uint32_t> recovery_id;
};

class SurfaceReferenceOps {
 public:
  SurfaceReferenceOps() = default;
  SurfaceReferenceOps(const SurfaceReferenceOps&) = delete;
  SurfaceReferenceOps& operator=(const SurfaceReferenceOps&) = delete;
  virtual ~SurfaceReferenceOps() = default;

  virtual void add_reference() = 0;
  virtual void remove_reference() = 0;
};

class SurfaceOwner;

class SurfaceLease {
 public:
  SurfaceLease() = default;
  ~SurfaceLease();
  SurfaceLease(const SurfaceLease&) = delete;
  SurfaceLease& operator=(const SurfaceLease&) = delete;
  SurfaceLease(SurfaceLease&& other) noexcept;
  SurfaceLease& operator=(SurfaceLease&& other) noexcept;

  void release() noexcept;
  [[nodiscard]] bool holds_reference() const noexcept;
  [[nodiscard]] bool can_render() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept {
    return generation_;
  }

 private:
  friend class SurfaceOwner;
  struct State;
  SurfaceLease(std::shared_ptr<State> state, std::uint64_t generation);

  std::shared_ptr<State> state_;
  std::uint64_t generation_{};
  bool released_{true};
};

class SurfaceOwner {
 public:
  explicit SurfaceOwner(SurfaceReferenceOps* reference_ops = nullptr);
  explicit SurfaceOwner(std::shared_ptr<SurfaceReferenceOps> reference_ops);
  ~SurfaceOwner();
  SurfaceOwner(const SurfaceOwner&) = delete;
  SurfaceOwner& operator=(const SurfaceOwner&) = delete;

  [[nodiscard]] std::uint64_t attach();
  void invalidate() noexcept;
  [[nodiscard]] bool available() const noexcept;
  [[nodiscard]] std::uint64_t generation() const noexcept;
  [[nodiscard]] std::size_t reference_count() const noexcept;
  [[nodiscard]] std::optional<SurfaceLease> acquire();
  [[nodiscard]] bool can_render(std::uint64_t generation) const noexcept;

 private:
  struct State;
  std::shared_ptr<State> state_;
};

// ArkTS owns the USBAccessoryHandle; C++ only borrows the descriptor for the
// lifetime of a session and must never close it (ADR 0004). This type therefore
// has no close path: it can be invalidated but never releases the descriptor.
class BorrowedAccessoryFd {
 public:
  explicit BorrowedAccessoryFd(int descriptor) noexcept;
  ~BorrowedAccessoryFd() = default;
  BorrowedAccessoryFd(const BorrowedAccessoryFd&) = delete;
  BorrowedAccessoryFd& operator=(const BorrowedAccessoryFd&) = delete;
  BorrowedAccessoryFd(BorrowedAccessoryFd&& other) noexcept;
  BorrowedAccessoryFd& operator=(BorrowedAccessoryFd&& other) noexcept;

  // Stops lending the descriptor without closing it; the owner is untouched.
  void invalidate() noexcept;
  [[nodiscard]] int borrow_fd() const noexcept { return descriptor_; }
  [[nodiscard]] bool valid() const noexcept { return descriptor_ >= 0; }

 private:
  int descriptor_{-1};
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
  Paused,
  Reconfiguring,
  Stopping,
  Stopped,
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
  HeartbeatTimeout = 17,
  PauseGraceExpired = 18,
  SystemSleep = 19,
};

enum class VideoSendStatus {
  Sent,
  NotStreaming,
  Paused,
  Stopping,
  VideoUnavailable,
  IdrRequired,
  EmptyPayload,
  InvalidArgument,
  SessionFailed,
};

enum class SessionCommandStatus {
  Ok,
  InvalidState,
  InvalidArgument,
  Duplicate,
  DisplayUnavailable,
};

enum class HostVideoState {
  Inactive,
  Active,
  Paused,
  ScreenLocked,
  RecordingPermissionRevoked,
  DisplayError,
  AwaitingResume,
  Sleeping,
  Stopping,
  Stopped,
};

enum class HostInputState {
  Inactive,
  Active,
  Paused,
  ScreenLocked,
  AccessibilityPermissionRevoked,
  DisplayError,
  AwaitingResume,
  Sleeping,
  Stopping,
  Stopped,
};

enum class HostEvent {
  ScreenLocked,
  ScreenUnlocked,
  WillSleep,
  ScreenRecordingPermissionRevoked,
  AccessibilityPermissionRevoked,
  DisplayError,
};

enum class ReconfigurationPhase {
  Preparing,
  AwaitingReady,
  AwaitingCommitted,
  AwaitingFirstIdr,
  Completed,
  Failed,
};

struct ReconfigurationStatus {
  std::uint32_t config_id{};
  ReconfigurationPhase phase{ReconfigurationPhase::Preparing};
  std::optional<SessionFailure> failure;
  bool resuming{};
};

enum class ClockMessageKind : std::uint8_t {
  Request = 1,
  Response = 2,
};

struct ClockMessage {
  ClockMessageKind kind{ClockMessageKind::Request};
  std::uint64_t t0_ns{};
  std::uint64_t t1_ns{};
  std::uint64_t t2_ns{};
};

struct ClockSample {
  std::uint64_t t0_host_ns{};
  std::uint64_t t1_device_ns{};
  std::uint64_t t2_device_ns{};
  std::uint64_t t3_host_ns{};
};

struct ClockEstimate {
  std::int64_t offset_ns{};
  std::uint64_t rtt_ns{};
  std::int64_t drift_ppb{};
  std::uint64_t error_bound_ns{};
  std::size_t sample_count{};
};

class ClockSyncEstimator {
 public:
  [[nodiscard]] bool observe(const ClockSample& sample);
  [[nodiscard]] const std::optional<ClockEstimate>& estimate() const {
    return estimate_;
  }

 private:
  std::optional<ClockEstimate> estimate_;
  std::optional<std::uint64_t> first_host_midpoint_ns_;
  std::optional<std::int64_t> first_offset_ns_;
  std::size_t sample_count_{};
};

[[nodiscard]] Result<std::vector<std::uint8_t>> encode_clock_message(
    const ClockMessage& message);
[[nodiscard]] Result<ClockMessage> decode_clock_message(
    std::span<const std::uint8_t> bytes);

enum class BoundaryResult {
  Ok,
  DisplayError,
  Failed,
};

struct SessionLimits {
  std::uint64_t stage_timeout_ns{kHandshakeStageTimeoutNs};
  std::uint64_t pause_grace_timeout_ns{kDefaultPauseGraceTimeoutNs};
  std::size_t video_fragment_payload{kDefaultVideoFragmentPayload};
};

struct HostSessionConfig {
  CapabilitySet capabilities;
  std::uint32_t video_stream_id{};
  std::uint32_t initial_config_id{1};
  std::uint32_t protocol_revision{kProtocolRevision};
  SessionLimits limits{};
  SendSchedulerConfig scheduler{};
};

struct DeviceSessionConfig {
  std::uint32_t session_nonce{};
  CapabilitySet capabilities;
  std::uint32_t protocol_revision{kProtocolRevision};
  SessionLimits limits{};
  DeviceRecoveryConfig recovery{};
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
  virtual void discard_prepared_stream(std::uint32_t) {}
  virtual void rollback_stream(std::uint32_t, std::uint32_t) {}
  virtual void release_stream(std::uint32_t, std::uint32_t) {}
  virtual void drain_callbacks() {}
};

class HostBoundary {
 public:
  HostBoundary() = default;
  HostBoundary(const HostBoundary&) = delete;
  HostBoundary& operator=(const HostBoundary&) = delete;
  virtual ~HostBoundary() = default;

  virtual void set_capture_encoding_intent(bool) {}
  virtual void set_input_injection_intent(bool) {}
  virtual void destroy_extended_display() {}
  virtual void drain_callbacks() {}
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
  HostSession(ByteIo& io, MonotonicClock& clock, HostSessionConfig config,
              HostBoundary* boundary = nullptr);
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
  [[nodiscard]] const std::optional<SchedulerIdrRequest>& pending_idr_request()
      const;
  [[nodiscard]] bool capture_encoding_intent() const;
  [[nodiscard]] bool input_injection_intent() const;
  [[nodiscard]] bool pause_acknowledged() const;
  [[nodiscard]] bool extended_display_present() const;
  [[nodiscard]] HostVideoState video_state() const;
  [[nodiscard]] HostInputState input_state() const;
  [[nodiscard]] SessionCommandStatus handle_host_event(HostEvent event);
  void request_stop();
  void complete_stop();
  void stop();
  [[nodiscard]] SessionCommandStatus pause();
  [[nodiscard]] SessionCommandStatus resume(const StreamProfile& profile,
                                           std::uint32_t config_id,
                                           std::uint32_t video_stream_id);
  [[nodiscard]] SessionCommandStatus reconfigure(
      const StreamProfile& profile, std::uint32_t config_id,
      std::uint32_t video_stream_id);
  [[nodiscard]] SessionCommandStatus request_clock_sync();
  [[nodiscard]] const std::optional<ReconfigurationStatus>& reconfiguration() const;
  [[nodiscard]] const std::optional<ClockEstimate>& clock_estimate() const;
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
                DeviceBoundary* boundary = nullptr,
                SurfaceOwner* surface = nullptr);
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
  [[nodiscard]] const std::optional<ReconfigurationStatus>& reconfiguration() const;
  [[nodiscard]] std::uint64_t attach_surface();
  void invalidate_surface();
  [[nodiscard]] bool surface_available() const;
  void request_stop();
  void complete_stop();
  void stop();
  [[nodiscard]] std::vector<ReceivedVideoFrame> take_video_frames();
  [[nodiscard]] bool recovery_gated() const;
  [[nodiscard]] const std::optional<std::uint32_t>& recovery_id() const;
  [[nodiscard]] const std::optional<ReceivedVideoFrame>& last_good_frame() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace deskmate::protocol
