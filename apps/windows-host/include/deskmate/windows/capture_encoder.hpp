#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace deskmate::windows {

struct CaptureOptions {
  std::uint16_t width{1920};
  std::uint16_t height{1200};
  std::uint16_t refresh_hz{60};
  bool hevc{true};
  // Empty selects the primary monitor. A monitor handle from the Win32
  // enumeration is used when the host needs a dedicated virtual display.
  std::uintptr_t monitor_handle{};
  std::string monitor_device_name;
};

struct EncodedAccessUnit {
  std::vector<std::uint8_t> annex_b;
  std::uint64_t timestamp_us{};
  bool keyframe{};
};

// Physical monitor bounds. Input injection needs the desktop rectangle of the
// display a link is showing, not just its size.
struct MonitorInfo {
  std::uintptr_t handle{};
  std::uint32_t display_id{};
  std::string name;
  std::string friendly_name;
  int left{};
  int top{};
  int width{};
  int height{};
  int refresh_hz{};
  bool primary{};
  bool is_virtual{false};
};

// One capture+encoder instance per virtual display/device link. The callback is
// invoked on the encoder's worker thread; implementations must not block there.
class CaptureEncoder {
 public:
  using FrameCallback = std::function<void(EncodedAccessUnit)>;
  using FailureCallback = std::function<void(std::string)>;

  virtual ~CaptureEncoder() = default;

  virtual bool start(const CaptureOptions& options, FrameCallback on_frame,
                     FailureCallback on_failure, std::string& error) = 0;
  virtual void request_keyframe() = 0;
  virtual void stop() = 0;
  virtual void set_congestion_probe(std::function<bool()> is_congested) {
    (void)is_congested;
  }
  virtual void set_cursor_fallback_probe(std::function<bool()> probe) {
    (void)probe;
  }
  [[nodiscard]] virtual bool running() const = 0;
  [[nodiscard]] virtual std::uint64_t encoded_frame_count() const { return 0; }
  [[nodiscard]] virtual std::uint64_t received_frame_count() const { return 0; }
  [[nodiscard]] virtual std::uint64_t dropped_frame_count() const { return 0; }
  [[nodiscard]] virtual std::string status_summary() const { return {}; }
};

[[nodiscard]] std::unique_ptr<CaptureEncoder> make_capture_encoder();

[[nodiscard]] std::vector<MonitorInfo> enumerate_monitors();

// Enumerates monitors as "index|name|widthxheight@hz" strings for CLI/logging.
[[nodiscard]] std::vector<std::string> list_monitors();

// Probes whether a hardware (or software fallback) HEVC encoder is usable.
[[nodiscard]] bool probe_hevc_encoder_available();

// Arranges non-primary monitors relative to the primary display without
// overlap. `position_code` uses DisplayPosition order:
// 0 = Right, 1 = Left, 2 = Top, 3 = Bottom.
bool arrange_extended_monitors(
    const std::vector<std::pair<std::uintptr_t, int>>& monitor_positions);

struct MonitorPlacement {
  std::uintptr_t handle{};
  std::string device_name;
  int left{};
  int top{};
  bool primary{false};
};

// Atomically applies custom multi-monitor desktop positions normalized to the
// primary display at (0, 0).
bool apply_monitor_layout(const std::vector<MonitorPlacement>& placements);

// Normalizes a user-selected refresh rate preference (0 = auto/negotiated,
// or 30/60/90/120 Hz); unrecognized values collapse to 0 (auto).
[[nodiscard]] int normalize_preferred_refresh_hz(int hz) noexcept;

// Resolves the active capture refresh rate from user preference, device
// negotiated refresh rate, and host fallback, clamped to `negotiated_hz` when
// negotiated so the host never exceeds the client decoder's active profile.
[[nodiscard]] int resolve_effective_refresh_hz(int preferred_hz,
                                                 int negotiated_hz,
                                                 int fallback_hz = 60) noexcept;

// Handshake advertisement always offers up to 120 Hz (120/90/60) unless an
// explicit CLI --fps fallback was supplied.
[[nodiscard]] int resolve_advertised_refresh_hz(int preferred_hz,
                                                bool explicit_fallback,
                                                int fallback_hz) noexcept;

// Steady-state idle keepalive matches the active target refresh rate so static
// desktops maintain the same cadence as active windows without 60 Hz dips.
[[nodiscard]] int resolve_idle_keepalive_hz(int effective_hz) noexcept;

// Two-phase idle AcquireNextFrame timeout: uses short (1..8 ms) DXGI waits
// until within ~4.5 ms of the deadline, then returns 0 so capture_loop()
// finishes the tail on the high-resolution waitable timer (~0.3 ms accuracy).
[[nodiscard]] std::uint32_t compute_idle_acquire_wait_ms(
    std::int64_t now_us, std::int64_t next_idle_deadline_us,
    bool urgent_retry) noexcept;

class FramePacer {
 public:
  explicit FramePacer(int fps);
  [[nodiscard]] std::int64_t next_deadline_us(
      std::int64_t submitted_at_us) noexcept;

 private:
  std::int64_t period_us_;
  std::int64_t next_deadline_us_{-1};
};

// A transient not-ready result leaves the newest sample queued. Only replacing
// an unsubmitted sample counts as a capture-side drop.
template <typename Sample, std::size_t Capacity = 1>
class LatestFrameQueue {
 public:
  static_assert(!std::is_reference_v<Sample>);
  static_assert(Capacity >= 1);

  bool push(Sample sample) {
    std::lock_guard lock(mutex_);
    bool replaced = false;
    if (queue_.size() >= Capacity) {
      queue_.pop_front();
      replaced = true;
    }
    queue_.push_back(std::move(sample));
    return replaced;
  }

  // Push and hand the evicted sample back to the caller so it can release any
  // resource (GPU texture slot, sample) the replaced frame was holding.
  std::optional<Sample> replace(Sample sample) {
    std::lock_guard lock(mutex_);
    std::optional<Sample> evicted;
    if (queue_.size() >= Capacity) {
      evicted = std::move(queue_.front());
      queue_.pop_front();
    }
    queue_.push_back(std::move(sample));
    return evicted;
  }

  [[nodiscard]] std::optional<Sample> peek() const
    requires std::copy_constructible<Sample>
  {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
      return std::nullopt;
    }
    return queue_.front();
  }

  [[nodiscard]] bool empty() const {
    std::lock_guard lock(mutex_);
    return queue_.empty();
  }

  [[nodiscard]] std::optional<Sample> take() {
    std::lock_guard lock(mutex_);
    if (queue_.empty()) {
      return std::nullopt;
    }
    Sample front = std::move(queue_.front());
    queue_.pop_front();
    return front;
  }

  bool restore_if_empty(Sample sample) {
    std::lock_guard lock(mutex_);
    if (!queue_.empty()) {
      return false;
    }
    queue_.push_front(std::move(sample));
    return true;
  }

  bool restore_front(Sample sample) {
    std::lock_guard lock(mutex_);
    if (queue_.size() >= Capacity) {
      return false;
    }
    queue_.push_front(std::move(sample));
    return true;
  }

  void discard() {
    std::lock_guard lock(mutex_);
    queue_.clear();
  }

 private:
  mutable std::mutex mutex_;
  std::deque<Sample> queue_;
};

// Computes a target H.264/HEVC bitrate sized to avoid overflowing the 12 KiB
// x 64 fragment USB keyframe budget while preserving clarity at high FPS.
[[nodiscard]] std::uint32_t compute_target_bitrate_bps(int width, int height,
                                                       int fps,
                                                       bool hevc) noexcept;

// Returns true when an Annex-B access unit contains at least one coded slice
// (VCL NAL unit), distinguishing real frames from parameter-set-only packets.
[[nodiscard]] bool annex_b_has_vcl_nal(std::span<const std::uint8_t> annex_b,
                                       bool hevc) noexcept;

}  // namespace deskmate::windows
