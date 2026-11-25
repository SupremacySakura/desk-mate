#pragma once

#include "deskmate/windows/aoa_links.hpp"
#include "deskmate/windows/capture_encoder.hpp"
#include "deskmate/windows/host_state.hpp"
#include "deskmate/windows/input_injector.hpp"
#include "deskmate/windows/libusb_aoa.hpp"
#include "deskmate/windows/updater.hpp"
#include "deskmate/windows/virtual_display.hpp"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace deskmate::windows {

struct HostOptions {
  std::uint16_t width{1920};
  std::uint16_t height{1200};
  std::uint16_t refresh_hz{60};
  bool refresh_hz_explicit{false};
  bool hevc{true};
  bool codec_explicit{false};
  bool size_explicit{false};
  bool auto_match{true};
  bool list_monitors{false};
  bool dry_run{false};
  bool headless{false};
};

class HostController {
 public:
  explicit HostController(HostOptions options = {},
                          std::string settings_path = {});
  ~HostController();

  HostController(const HostController&) = delete;
  HostController& operator=(const HostController&) = delete;

  void start();
  void stop();

  // Actions invoked by the UI, System Tray Popover, or system notifications.
  void set_service_running(bool enabled);
  void set_video_enabled(bool enabled);
  void set_input_enabled(bool enabled);
  void set_touch_input_mode(TouchInputMode mode);
  void set_auto_virtual_display(bool enabled);
  void set_preferred_refresh_hz(int hz);
  void choose_monitor(std::uintptr_t handle);
  void set_default_position(DisplayPosition pos);
  void select_link(std::uint64_t link_id);
  void set_link_position(std::uint64_t link_id, DisplayPosition pos);
  void apply_display_layout(const std::vector<MonitorPlacement>& placements);
  void refresh_displays();
  void disconnect_link(std::uint64_t link_id);
  void set_launch_at_login(bool enabled);
  void open_startup_settings();
  void set_auto_start_service(bool enabled);
  void refresh_devices();
  void set_device_allowed(const std::string& device_id, bool allowed);
  void set_device_preferred_position(const std::string& device_id,
                                     std::optional<DisplayPosition> pos);
  void rename_device(const std::string& device_id, const std::string& name);
  void remove_device(const std::string& device_id);
  void retry();
  void handle_host_event(deskmate::protocol::HostEvent event);

  void check_for_updates(bool manual = true);
  void install_available_update();
  void set_auto_check_updates(bool enabled);
  void set_update_mirror_prefix(const std::string& prefix);
  void cleanup_legacy_update_files();

  [[nodiscard]] HostUiState snapshot_ui_state();
  [[nodiscard]] std::string diagnostic_text();

 private:
  struct LinkChannel {
    std::uint64_t link_id{};
    std::string key;
    deskmate::platform::AoaIdentity identity;
    std::string device_id;
    std::string device_name;
    DisplayPosition position{DisplayPosition::Right};

    std::shared_ptr<LinkSession> session;
    mutable std::mutex capture_mutex;
    std::unique_ptr<CaptureEncoder> capture;
    std::atomic<bool> capture_active{false};
    std::atomic<bool> capture_dirty{true};
    std::unique_ptr<InputInjector> injector;
    std::uintptr_t monitor_handle{};
    std::uint32_t monitor_display_id{};
    bool owns_virtual_display{false};
    std::optional<deskmate::protocol::StreamProfile> capture_profile;
    int capture_refresh_hz{0};
    std::string capture_error;
    std::string last_error;
    std::chrono::steady_clock::time_point retry_after{};

    // Telemetry state
    std::chrono::steady_clock::time_point last_metric_time{};
    std::uint64_t last_metric_frames{0};
    std::uint64_t last_metric_tx_bytes{0};
    int live_fps{0};
    double live_bitrate_mbps{0.0};
    std::chrono::steady_clock::time_point last_device_touch{};

    // Per-link dedicated full-duplex worker
    std::thread worker;
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    bool wakeup{false};
    std::atomic<bool> stop_requested{false};
    std::atomic<bool> disconnect_requested{false};
    std::atomic<bool> finished{false};
  };

  void start_service_internal();
  void stop_service_internal();
  void discovery_loop();
  void link_worker_loop(const std::shared_ptr<LinkChannel>& ch);
  void reap_finished_channels();
  void ensure_capture_for_channel(const std::shared_ptr<LinkChannel>& ch);
  void stop_capture_for_channel(const std::shared_ptr<LinkChannel>& ch);
  void restart_all_captures();
  void rearrange_monitors();
  void refresh_monitors_locked();
  void refresh_environment_if_stale(bool force = false);
  const MonitorInfo* pick_monitor_locked(
      const std::shared_ptr<LinkChannel>& target) const;
  DisplayPosition default_position_for_new_channel_locked(
      const std::string& device_id) const;
  void update_channel_metrics(const std::shared_ptr<LinkChannel>& ch);
  void update_power_assertion();
  void append_log(const std::string& line);
  std::string build_diagnostic_locked() const;

  HostOptions options_;
  SettingsStore store_;
  StartupManager startup_manager_;
  StartupStatus startup_status_;
  bool hevc_available_{true};
  bool native_touch_available_{false};

  std::unique_ptr<LibusbAoaDiscovery> discovery_;
  VirtualDisplayManager vdd_;
  UpdateManager updater_;
  std::thread discovery_thread_;
  std::mutex discovery_mutex_;
  std::condition_variable discovery_cv_;
  bool discovery_wakeup_{false};
  std::atomic<bool> discovery_stop_{false};

  mutable std::mutex mutex_;
  bool started_{false};
  bool service_running_{false};
  std::atomic<bool> video_enabled_{true};
  std::atomic<bool> allow_input_{true};
  TouchInputMode touch_mode_{TouchInputMode::CompatibleGestures};
  bool auto_virtual_display_{true};
  DisplayPosition default_position_{DisplayPosition::Right};
  std::atomic<int> preferred_refresh_hz_{0};
  bool auto_start_service_{true};
  std::uintptr_t selected_monitor_{0};
  std::uint64_t selected_link_id_{0};
  std::uint64_t next_link_id_{1};

  VirtualDisplayStatus cached_vdd_status_;
  std::chrono::steady_clock::time_point last_env_refresh_{};
  std::vector<MonitorInfo> monitors_;
  std::unordered_map<std::uint64_t, std::shared_ptr<LinkChannel>> channels_;
  std::vector<std::shared_ptr<LinkChannel>> finished_channels_;

  // Dry-run state
  std::unique_ptr<CaptureEncoder> dry_run_capture_;
  std::string dry_run_error_;

  // Discovery diagnostics
  std::set<std::string> online_device_ids_;
  std::vector<std::string> blocked_device_names_;
  std::string discovery_error_;
  std::string last_usb_scan_summary_;
  std::string last_pnp_aoa_summary_;
  bool driver_missing_{false};
  std::string driver_hint_;
  [[maybe_unused]] bool power_boost_active_{false};

  mutable std::mutex log_mutex_;
  std::deque<std::string> logs_;
};

}  // namespace deskmate::windows
