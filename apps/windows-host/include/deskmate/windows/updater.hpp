#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>

namespace deskmate::windows {

inline constexpr const char* kWindowsHostVersion = "1.3.0";

enum class UpdatePhase {
  Idle = 0,
  Checking = 1,
  UpToDate = 2,
  Available = 3,
  Downloading = 4,
  Verifying = 5,
  Installing = 6,
  Failed = 7,
};

struct UpdateUiState {
  std::string current_version{kWindowsHostVersion};
  UpdatePhase phase{UpdatePhase::Idle};
  std::string available_version;
  std::string release_notes;
  std::string release_url{"https://github.com/SupremacySakura/desk-mate/releases/latest"};
  double progress{0.0};
  std::uint64_t downloaded_bytes{0};
  std::uint64_t total_bytes{0};
  std::string status_text{"点击检查更新以获取最新版本"};
  std::string error_message;
  bool auto_check_updates{true};
  std::string mirror_prefix;
  bool restart_requested{false};
};

class UpdateManager {
 public:
  UpdateManager() = default;
  ~UpdateManager();

  UpdateManager(const UpdateManager&) = delete;
  UpdateManager& operator=(const UpdateManager&) = delete;

  void set_preferences(bool auto_check, std::string mirror_prefix);
  [[nodiscard]] UpdateUiState snapshot() const;

  void check_for_updates(bool manual);
  void download_and_install(std::function<void()> before_restart);

 private:
  mutable std::mutex mutex_;
  UpdateUiState state_;
  std::string pending_url_;
  std::string pending_sha256_;
  std::uint64_t pending_size_{0};
  std::string pending_signature_;
};

void cleanup_old_executable_if_present(const std::string& explicit_old_path = {});

}  // namespace deskmate::windows
