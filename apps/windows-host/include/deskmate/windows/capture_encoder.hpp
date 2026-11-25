#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
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
  [[nodiscard]] virtual bool running() const = 0;
  [[nodiscard]] virtual std::uint64_t encoded_frame_count() const { return 0; }
  [[nodiscard]] virtual std::uint64_t received_frame_count() const { return 0; }
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
// negotiated refresh rate, and host fallback.
[[nodiscard]] int resolve_effective_refresh_hz(int preferred_hz,
                                                 int negotiated_hz,
                                                 int fallback_hz = 60) noexcept;

// Auto mode keeps the proven 60 Hz USB baseline. Users can still opt into a
// higher rate explicitly through the UI or --fps.
[[nodiscard]] int resolve_advertised_refresh_hz(int preferred_hz,
                                                bool explicit_fallback,
                                                int fallback_hz) noexcept;
[[nodiscard]] int async_input_wait_budget_ms(int fps) noexcept;

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
