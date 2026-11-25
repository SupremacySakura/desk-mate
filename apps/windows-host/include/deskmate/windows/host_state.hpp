#pragma once

#include "deskmate/windows/display_topology.hpp"
#include "deskmate/windows/input_injector.hpp"
#include "deskmate/windows/updater.hpp"

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace deskmate::windows {

enum class DisplayPosition {
  Right = 0,
  Left = 1,
  Top = 2,
  Bottom = 3,
};

[[nodiscard]] const char* display_position_id(DisplayPosition pos) noexcept;
[[nodiscard]] std::optional<DisplayPosition> display_position_from_id(
    const std::string& id) noexcept;
[[nodiscard]] std::string display_position_label(DisplayPosition pos);
[[nodiscard]] std::string display_position_short_label(DisplayPosition pos);

struct PairedDevice {
  std::string id;
  std::string default_name;
  std::string custom_name;
  std::string serial;
  std::string manufacturer;
  bool allowed{true};
  std::int64_t last_connected_epoch_s{};
  std::int64_t first_paired_epoch_s{};
  std::string last_resolution;
  std::optional<DisplayPosition> preferred_position;

  [[nodiscard]] std::string display_name() const;
  [[nodiscard]] static bool is_generic_name(const std::string& name);
};

struct HostPreferences {
  bool auto_virtual_display{true};
  DisplayPosition default_position{DisplayPosition::Right};
  bool allow_input{true};
  TouchInputMode touch_input_mode{TouchInputMode::CompatibleGestures};
  int preferred_refresh_hz{0};
  bool auto_start_service{true};
  bool auto_check_updates{true};
  std::string update_mirror_prefix;
  std::string preferred_monitor_name;
  std::vector<PairedDevice> paired_devices;
};

class SettingsStore {
 public:
  explicit SettingsStore(std::string storage_path = {});

  [[nodiscard]] const std::string& path() const { return storage_path_; }
  [[nodiscard]] HostPreferences& prefs() { return prefs_; }
  [[nodiscard]] const HostPreferences& prefs() const { return prefs_; }

  bool load();
  bool save() const;

  bool record_seen_device(const std::string& id, const std::string& name,
                          const std::string& manufacturer,
                          const std::string& serial,
                          const std::string& resolution = {},
                          bool mark_connected = true);
  void set_allowed(const std::string& id, bool allowed);
  void rename(const std::string& id, const std::string& custom_name);
  void remove(const std::string& id);
  [[nodiscard]] bool is_allowed(const std::string& id) const;
  [[nodiscard]] std::vector<std::string> blocked_device_ids() const;
  [[nodiscard]] std::string display_name_for(const std::string& id) const;
  [[nodiscard]] std::optional<DisplayPosition> preferred_position_for(
      const std::string& id) const;
  void set_preferred_position(const std::string& id,
                              std::optional<DisplayPosition> pos);

  [[nodiscard]] static std::string relative_time_text(
      std::int64_t epoch_s, std::int64_t now_epoch_s = 0);
  [[nodiscard]] static std::int64_t now_epoch_seconds();

 private:
  void sort_devices();

  std::string storage_path_;
  HostPreferences prefs_;
};

struct StartupStatus {
  bool launch_at_login_enabled{false};
  std::string status_description{
      "未开启。开启后登录 Windows 时将自动运行 DeskMate。"};
  std::string error_message;
};

class StartupManager {
 public:
  [[nodiscard]] StartupStatus current_status() const;
  StartupStatus set_launch_at_login(bool enabled);
  void open_startup_settings() const;

 private:
  std::string last_error_;
};

struct DisplayChoice {
  std::uintptr_t handle{};
  std::uint32_t display_id{};
  std::string name;
  std::string title;
  int width{};
  int height{};
  int refresh_hz{};
  bool primary{};
};

struct ActiveSessionView {
  std::uint64_t link_id{};
  std::string device_id;
  std::string device_name;
  std::string state;
  std::string badge;
  bool linked{};
  bool streaming{};
  std::string stream_summary;
  DisplayPosition position{DisplayPosition::Right};
  int live_fps{};
  double live_bitrate_mbps{};
  std::uint64_t input_event_count{};
  std::uint32_t display_id{};
  std::string error;
};

struct HostUiState {
  std::string title{"等待设备连接"};
  std::string guidance{"打开设备上的 DeskMate，再接上 USB 数据线。"};
  std::string badge{"未连接"};
  bool linked{false};
  bool service_running{true};
  bool auto_start_service{true};
  bool launch_at_login{false};
  std::string startup_summary;
  std::string startup_error;

  std::vector<PairedDevice> paired_devices;
  std::vector<ActiveSessionView> active_sessions;
  std::vector<TopologyDisplayItem> topology_displays;
  std::uint64_t selected_link_id{0};
  std::set<std::string> connected_device_ids;
  std::string connected_device_id;
  std::set<std::string> online_device_ids;
  std::string connected_device_name;
  std::vector<std::string> blocked_device_names;
  std::string blocked_device_name;

  std::string source{"自动匹配扩展显示器"};
  std::string stream_summary;
  std::string video{"等待连接"};
  std::string input{"未开启"};

  bool video_enabled{true};
  bool input_enabled{true};
  TouchInputMode touch_input_mode{TouchInputMode::CompatibleGestures};
  TouchCapabilityStatus touch_capability;

  bool auto_virtual_display{true};
  DisplayPosition position{DisplayPosition::Right};
  int preferred_refresh_hz{0};
  int negotiated_refresh_hz{0};
  int live_fps{0};
  double live_bitrate_mbps{0.0};
  std::uint64_t input_event_count{0};

  std::vector<DisplayChoice> choices;
  std::uintptr_t selected_monitor{0};
  bool hevc_available{true};
  bool native_touch_available{false};
  bool driver_missing{false};
  std::string driver_hint;
  bool vdd_installed{false};
  bool vdd_active{false};
  bool mirroring_primary{false};
  std::string vdd_backend_name;
  std::string vdd_status_detail;
  bool can_retry{false};
  UpdateUiState update;
  std::string diagnostic;
  std::vector<std::string> recent_logs;
};

}  // namespace deskmate::windows
