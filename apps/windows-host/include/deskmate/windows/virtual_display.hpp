#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace deskmate::windows {

enum class VirtualDisplayBackend {
  None = 0,
  ParsecVdd,
  MttVdd,
  ExistingExtended,
};

struct VirtualDisplayStatus {
  VirtualDisplayBackend backend{VirtualDisplayBackend::None};
  bool driver_installed{false};
  bool active_virtual_display{false};
  std::string backend_name;
  std::string detail;
};

struct DisplayModeCandidate {
  int width{};
  int height{};
  int refresh_hz{};
};

// Case-insensitive match for common Windows virtual display adapter/driver names
// (Parsec VDD, MttVDD / Virtual Display Driver, IddSampleDriver, SudoMaker, etc.).
[[nodiscard]] bool is_virtual_display_adapter_name(
    const std::string& friendly_name);

// Selects the closest display mode supported by a monitor for a desired
// (width, height, refresh_hz), preferring exact match, then matching dimensions
// with closest refresh rate, then closest aspect ratio and pixel count.
[[nodiscard]] std::optional<DisplayModeCandidate> pick_best_display_mode(
    const std::vector<DisplayModeCandidate>& modes, int target_w, int target_h,
    int target_hz);

// Writes a custom Parsec VDD mode slot directly to HKLM\SOFTWARE\Parsec\vdd\0.
// Used both in-process (when elevated) and by `--register-parsec-mode`.
bool write_parsec_custom_mode_direct(int width, int height, int refresh_hz,
                                     std::string& error);

// Manages programmatic virtual extended monitors on Windows using installed IddCx
// user-mode bridges (Parsec Virtual Display Driver via IOCTLs and MttVDD /
// Virtual-Display-Driver via MTTVirtualDisplayPipe), plus DisplayConfig extend
// topology enforcement.
class VirtualDisplayManager {
 public:
  VirtualDisplayManager() = default;
  ~VirtualDisplayManager();

  VirtualDisplayManager(const VirtualDisplayManager&) = delete;
  VirtualDisplayManager& operator=(const VirtualDisplayManager&) = delete;

  [[nodiscard]] VirtualDisplayStatus probe_status() const;

  // Detects Windows Duplicate/Clone display topology (or inactive secondary
  // paths when `activate_inactive_targets` is true) and applies
  // SDC_TOPOLOGY_EXTEND so secondary monitors expose an independent desktop.
  bool ensure_extended_topology(std::string& log_out,
                                bool activate_inactive_targets = false);

  // Attempts to plug in a new virtual monitor for `link_id` and apply its best
  // matching mode. Returns the new HMONITOR handle (as uintptr_t) or 0 if no
  // virtual display driver is available or creation failed.
  [[nodiscard]] std::uintptr_t create_display(std::uint64_t link_id, int width,
                                              int height, int refresh_hz,
                                              std::string& log_out);
  bool apply_display_mode(std::uintptr_t monitor_handle, int width, int height,
                          int refresh_hz, std::string& log_out);

  void destroy_display(std::uint64_t link_id);
  void destroy_all();

  [[nodiscard]] std::uintptr_t monitor_for_link(std::uint64_t link_id) const;
  [[nodiscard]] bool has_created_display(std::uint64_t link_id) const;

 private:
  struct ManagedDisplay {
    VirtualDisplayBackend backend{VirtualDisplayBackend::None};
    int parsec_index{-1};
    std::uintptr_t monitor_handle{0};
  };

  mutable std::mutex mutex_;
  std::unordered_map<std::uint64_t, ManagedDisplay> managed_;
  int mtt_managed_count_{0};

#ifdef _WIN32
  void ensure_keepalive_started_locked();
  void stop_keepalive();
  void keepalive_loop();

  void* parsec_handle_{nullptr};
  std::atomic<bool> keepalive_stop_{false};
  std::thread keepalive_thread_;
#endif
};

}  // namespace deskmate::windows
