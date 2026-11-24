#pragma once

#include "deskmate/byte_io.hpp"
#include "deskmate/clock.hpp"
#include "deskmate/protocol.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace deskmate::protocol {

inline constexpr std::size_t kDefaultMaxVideoUrbs = 2;
inline constexpr std::size_t kDefaultMaxPendingVideoBytes = 512U * 1024U;
inline constexpr std::uint64_t kDefaultMaxUnsentFrameAgeNs = 18'000'000ULL;
inline constexpr std::uint64_t kDefaultRecoveryRetryNs = 150'000'000ULL;

struct SendSchedulerConfig {
  std::size_t max_video_urbs{kDefaultMaxVideoUrbs};
  std::size_t max_pending_video_bytes{kDefaultMaxPendingVideoBytes};
  std::uint64_t max_oldest_unsent_frame_age_ns{kDefaultMaxUnsentFrameAgeNs};
  std::uint64_t recovery_retry_ns{kDefaultRecoveryRetryNs};
};

enum class SchedulerQueueStatus {
  Queued,
  IdrRequired,
  InvalidArgument,
  Stopped,
};

enum class IdrRequestReason {
  Congestion,
  PeerRequest,
};

struct SchedulerIdrRequest {
  std::uint32_t recovery_id{};
  IdrRequestReason reason{IdrRequestReason::Congestion};
  std::uint64_t requested_at_ns{};
};

struct ScheduledVideoFrame {
  std::vector<std::vector<std::uint8_t>> fragments;
  bool keyframe{};
  std::optional<std::uint32_t> recovery_id;
};

class HostSendScheduler {
 public:
  HostSendScheduler(ByteIo& io, MonotonicClock& clock,
                    SendSchedulerConfig config = {});

  HostSendScheduler(const HostSendScheduler&) = delete;
  HostSendScheduler& operator=(const HostSendScheduler&) = delete;
  ~HostSendScheduler() = default;

  void enqueue_priority(std::vector<std::uint8_t> packet);
  [[nodiscard]] SchedulerQueueStatus enqueue_video_frame(
      std::vector<std::vector<std::uint8_t>> fragments);
  [[nodiscard]] SchedulerQueueStatus enqueue_video_frame(
      ScheduledVideoFrame frame);
  [[nodiscard]] SchedulerQueueStatus enqueue_video_frame(
      VideoFrameToSend frame, std::size_t fragment_payload);
  void request_idr_from_peer(std::uint32_t recovery_id);

  void pump();
  void pump_priority();
  void begin_stopping() noexcept;

  [[nodiscard]] bool stopping() const noexcept { return stopping_; }
  [[nodiscard]] bool idle() const noexcept;
  [[nodiscard]] std::size_t pending_video_bytes() const noexcept;
  [[nodiscard]] const std::optional<SchedulerIdrRequest>& idr_request() const {
    return idr_request_;
  }

 private:
  struct VideoFrame {
    std::vector<std::vector<std::uint8_t>> fragments;
    std::size_t next_fragment{};
    std::uint64_t enqueue_ns{};
  };

  [[nodiscard]] bool has_pending_output() const noexcept;
  [[nodiscard]] SchedulerQueueStatus admit_video_frame(
      bool keyframe, std::size_t incoming_bytes);
  void queue_video_frame(ScheduledVideoFrame frame);
  [[nodiscard]] std::optional<std::vector<std::uint8_t>> next_video_fragment();
  void drop_unstarted_video_frames();
  void check_oldest_unsent_frame_age();
  void request_recovery(IdrRequestReason reason);
  void write_active();

  ByteIo& io_;
  MonotonicClock& clock_;
  SendSchedulerConfig config_;
  std::deque<std::vector<std::uint8_t>> priority_;
  std::deque<VideoFrame> video_;
  std::optional<std::vector<std::uint8_t>> active_;
  std::size_t active_offset_{};
  std::optional<SchedulerIdrRequest> idr_request_;
  std::uint32_t last_recovery_id_{};
  bool stopping_{};
};

}  // namespace deskmate::protocol
