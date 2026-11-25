#include "deskmate/windows/virtual_display.hpp"

#include "deskmate/windows/capture_encoder.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <unordered_set>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <setupapi.h>
#endif

namespace deskmate::windows {
namespace {

std::string to_lower_ascii(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

#ifdef _WIN32
// Parsec Virtual Display Adapter device interface GUID:
// {00b41627-04c4-429e-a26e-0265cf50c8fa}
constexpr GUID kParsecVddInterfaceGuid = {
    0x00b41627,
    0x04c4,
    0x429e,
    {0xa2, 0x6e, 0x02, 0x65, 0xcf, 0x50, 0xc8, 0xfa}};

constexpr DWORD kVddIoctlAdd = 0x0022e004;
constexpr DWORD kVddIoctlRemove = 0x0022a008;
constexpr DWORD kVddIoctlUpdate = 0x0022a00c;
constexpr DWORD kVddIoctlVersion = 0x0022e010;
constexpr wchar_t kMttVddPipeName[] = L"\\\\.\\pipe\\MTTVirtualDisplayPipe";

HANDLE open_parsec_vdd_handle() {
  HDEVINFO dev_info = SetupDiGetClassDevsW(
      &kParsecVddInterfaceGuid, nullptr, nullptr,
      DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
  if (dev_info == INVALID_HANDLE_VALUE) {
    return INVALID_HANDLE_VALUE;
  }

  HANDLE opened = INVALID_HANDLE_VALUE;
  SP_DEVICE_INTERFACE_DATA if_data{};
  if_data.cbSize = sizeof(if_data);
  for (DWORD idx = 0; SetupDiEnumDeviceInterfaces(
           dev_info, nullptr, &kParsecVddInterfaceGuid, idx, &if_data);
       ++idx) {
    DWORD required = 0;
    SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, nullptr, 0, &required,
                                     nullptr);
    if (required == 0) {
      continue;
    }
    std::vector<BYTE> detail_buf(required, 0);
    auto* detail =
        reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(detail_buf.data());
    detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
    if (!SetupDiGetDeviceInterfaceDetailW(dev_info, &if_data, detail, required,
                                          nullptr, nullptr)) {
      continue;
    }
    HANDLE h = CreateFileW(
        detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_NO_BUFFERING,
        nullptr);
    if (h != INVALID_HANDLE_VALUE) {
      opened = h;
      break;
    }
  }
  SetupDiDestroyDeviceInfoList(dev_info);
  return opened;
}

bool parsec_vdd_ioctl(HANDLE handle, DWORD code, const void* data,
                      DWORD data_size, int* out_value) {
  if (handle == nullptr || handle == INVALID_HANDLE_VALUE) {
    return false;
  }
  BYTE in_buf[32]{};
  if (data != nullptr && data_size > 0) {
    std::memcpy(in_buf, data,
                std::min<DWORD>(data_size, static_cast<DWORD>(sizeof(in_buf))));
  }
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (ov.hEvent == nullptr) {
    return false;
  }
  int out_buf = 0;
  DWORD bytes_ret = 0;
  BOOL ok = DeviceIoControl(handle, code, in_buf, sizeof(in_buf), &out_buf,
                            sizeof(out_buf), &bytes_ret, &ov);
  if (!ok) {
    const DWORD err = GetLastError();
    if (err == ERROR_IO_PENDING) {
      const DWORD wait = WaitForSingleObject(ov.hEvent, 1500);
      ok = (wait == WAIT_OBJECT_0) &&
           GetOverlappedResult(handle, &ov, &bytes_ret, FALSE);
    }
  }
  CloseHandle(ov.hEvent);
  if (ok && out_value != nullptr) {
    *out_value = out_buf;
  }
  return ok != FALSE;
}

void register_parsec_custom_resolution(int width, int height, int refresh_hz) {
  // Best-effort write into Parsec VDD registry resolution table (requires
  // admin rights when HKLM is protected; silently skipped otherwise).
  HKEY key = nullptr;
  const LONG rc = RegCreateKeyExW(
      HKEY_LOCAL_MACHINE, L"SOFTWARE\\Parsec\\vdd\\0", 0, nullptr,
      REG_OPTION_NON_VOLATILE, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (rc == ERROR_SUCCESS && key != nullptr) {
    const DWORD w = static_cast<DWORD>(width);
    const DWORD h = static_cast<DWORD>(height);
    const DWORD hz = static_cast<DWORD>(refresh_hz);
    RegSetValueExW(key, L"width", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&w), sizeof(w));
    RegSetValueExW(key, L"height", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&h), sizeof(h));
    RegSetValueExW(key, L"hz", 0, REG_DWORD,
                   reinterpret_cast<const BYTE*>(&hz), sizeof(hz));
    RegCloseKey(key);
  }
}

bool send_mtt_vdd_command(const std::string& command, std::string& response) {
  response.clear();
  HANDLE pipe = CreateFileW(kMttVddPipeName, GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_EXISTING, 0, nullptr);
  if (pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  std::string payload = command;
  if (payload.empty() || payload.back() != '\n') {
    payload.push_back('\n');
  }
  DWORD written = 0;
  const BOOL w_ok = WriteFile(pipe, payload.data(),
                              static_cast<DWORD>(payload.size()), &written,
                              nullptr);
  if (w_ok) {
    char buf[512]{};
    DWORD read = 0;
    if (ReadFile(pipe, buf, sizeof(buf) - 1, &read, nullptr) && read > 0) {
      response.assign(buf, buf + read);
    }
  }
  CloseHandle(pipe);
  return w_ok != FALSE;
}

bool mtt_vdd_pipe_available() {
  return WaitNamedPipeW(kMttVddPipeName, 50) != FALSE;
}

void apply_best_mode_for_monitor(std::uintptr_t monitor_handle, int target_w,
                                 int target_h, int target_hz,
                                 std::string& log_out) {
  if (monitor_handle == 0) {
    return;
  }
  MONITORINFOEXW mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(reinterpret_cast<HMONITOR>(monitor_handle), &mi)) {
    return;
  }
  std::vector<DisplayModeCandidate> candidates;
  DEVMODEW dm{};
  dm.dmSize = sizeof(dm);
  for (DWORD idx = 0; EnumDisplaySettingsW(mi.szDevice, idx, &dm); ++idx) {
    if (dm.dmPelsWidth > 0 && dm.dmPelsHeight > 0) {
      candidates.push_back(DisplayModeCandidate{
          static_cast<int>(dm.dmPelsWidth), static_cast<int>(dm.dmPelsHeight),
          dm.dmDisplayFrequency > 1 ? static_cast<int>(dm.dmDisplayFrequency)
                                    : 60});
    }
    ZeroMemory(&dm, sizeof(dm));
    dm.dmSize = sizeof(dm);
  }
  const auto picked =
      pick_best_display_mode(candidates, target_w, target_h, target_hz);
  if (!picked.has_value()) {
    return;
  }
  DEVMODEW cur{};
  cur.dmSize = sizeof(cur);
  if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur) &&
      static_cast<int>(cur.dmPelsWidth) == picked->width &&
      static_cast<int>(cur.dmPelsHeight) == picked->height &&
      static_cast<int>(cur.dmDisplayFrequency) == picked->refresh_hz) {
    return;
  }
  DEVMODEW apply{};
  apply.dmSize = sizeof(apply);
  apply.dmPelsWidth = static_cast<DWORD>(picked->width);
  apply.dmPelsHeight = static_cast<DWORD>(picked->height);
  apply.dmDisplayFrequency = static_cast<DWORD>(picked->refresh_hz);
  apply.dmFields = DM_PELSWIDTH | DM_PELSHEIGHT | DM_DISPLAYFREQUENCY;
  const LONG chg =
      ChangeDisplaySettingsExW(mi.szDevice, &apply, nullptr, CDS_UPDATEREGISTRY,
                               nullptr);
  log_out += " mode=" + std::to_string(picked->width) + "x" +
             std::to_string(picked->height) + "@" +
             std::to_string(picked->refresh_hz) + " (rc=" +
             std::to_string(chg) + ")";
}
#endif

}  // namespace

bool is_virtual_display_adapter_name(const std::string& friendly_name) {
  const std::string low = to_lower_ascii(friendly_name);
  if (low.empty()) {
    return false;
  }
  static constexpr const char* kTokens[] = {
      "parsec",       "pscn",         "virtual display", "iddsample",
      "mttvdd",       "sudomaker",    "indirect display", "spacedesk",
      "amyuni",       "luminoncore",  "vdd",
  };
  for (const char* token : kTokens) {
    if (low.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

std::optional<DisplayModeCandidate> pick_best_display_mode(
    const std::vector<DisplayModeCandidate>& modes, int target_w, int target_h,
    int target_hz) {
  if (modes.empty() || target_w <= 0 || target_h <= 0) {
    return std::nullopt;
  }
  const int want_hz = std::max(1, target_hz);
  const double target_aspect =
      static_cast<double>(target_w) / static_cast<double>(target_h);
  const double target_area =
      static_cast<double>(target_w) * static_cast<double>(target_h);

  const DisplayModeCandidate* best = nullptr;
  double best_score = std::numeric_limits<double>::max();

  for (const auto& m : modes) {
    if (m.width <= 0 || m.height <= 0) {
      continue;
    }
    const int hz = std::max(1, m.refresh_hz);
    double score = 0.0;
    if (m.width == target_w && m.height == target_h) {
      score = static_cast<double>(std::abs(hz - want_hz)) * 0.001;
    } else {
      const double aspect =
          static_cast<double>(m.width) / static_cast<double>(m.height);
      const double area =
          static_cast<double>(m.width) * static_cast<double>(m.height);
      const double aspect_diff = std::abs(aspect - target_aspect);
      const double area_diff =
          std::abs(area - target_area) / std::max(1.0, target_area);
      const double hz_diff = static_cast<double>(std::abs(hz - want_hz)) / 60.0;
      score = 100.0 + aspect_diff * 500.0 + area_diff * 50.0 + hz_diff * 2.0;
    }
    if (score < best_score) {
      best_score = score;
      best = &m;
    }
  }
  if (best == nullptr) {
    return std::nullopt;
  }
  return *best;
}

VirtualDisplayManager::~VirtualDisplayManager() { destroy_all(); }

VirtualDisplayStatus VirtualDisplayManager::probe_status() const {
  VirtualDisplayStatus status;
#ifdef _WIN32
  bool any_active_vdd = false;
  std::string active_name;
  for (const auto& m : enumerate_monitors()) {
    if (!m.primary && m.is_virtual) {
      any_active_vdd = true;
      active_name = m.friendly_name;
      break;
    }
  }

  HANDLE parsec = open_parsec_vdd_handle();
  if (parsec != INVALID_HANDLE_VALUE) {
    int ver = 0;
    (void)parsec_vdd_ioctl(parsec, kVddIoctlVersion, nullptr, 0, &ver);
    CloseHandle(parsec);
    status.backend = VirtualDisplayBackend::ParsecVdd;
    status.driver_installed = true;
    status.active_virtual_display = any_active_vdd;
    status.backend_name = "Parsec VDD";
    status.detail = "已就绪（Parsec Virtual Display Driver v" +
                    std::to_string(ver) + "，可自动创建独立虚拟扩展屏）";
    return status;
  }

  if (mtt_vdd_pipe_available()) {
    status.backend = VirtualDisplayBackend::MttVdd;
    status.driver_installed = true;
    status.active_virtual_display = any_active_vdd;
    status.backend_name = "MttVDD";
    status.detail = "已就绪（Virtual Display Driver 管道已运行）";
    return status;
  }

  if (any_active_vdd) {
    status.backend = VirtualDisplayBackend::ExistingExtended;
    status.driver_installed = true;
    status.active_virtual_display = true;
    status.backend_name = active_name;
    status.detail = "检测到系统已有虚拟扩展显示器，将自动独立分配副屏输出";
    return status;
  }

  status.backend = VirtualDisplayBackend::None;
  status.driver_installed = false;
  status.active_virtual_display = false;
  status.backend_name = "未安装";
  status.detail =
      "未检测到虚拟屏驱动（Parsec VDD / MttVDD）：Windows 需要安装 IddCx 虚拟屏驱动才能新增独立虚拟扩展屏，不会占用您现有的物理副屏";
#else
  status.backend = VirtualDisplayBackend::None;
  status.backend_name = "非 Windows 环境";
#endif
  return status;
}

bool VirtualDisplayManager::ensure_extended_topology(
    std::string& log_out, bool activate_inactive_targets) {
  log_out.clear();
#ifdef _WIN32
  UINT32 path_count = 0;
  UINT32 mode_count = 0;
  if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count,
                                  &mode_count) != ERROR_SUCCESS ||
      path_count == 0) {
    return false;
  }
  std::vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
  std::vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
  if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
                         &mode_count, modes.data(), nullptr) != ERROR_SUCCESS) {
    return false;
  }

  // Clone/Duplicate mode shows >= 2 active paths sharing the exact same source
  // (adapterId + sourceInfo.id). Switch to SDC_TOPOLOGY_EXTEND automatically.
  bool cloned = false;
  for (UINT32 i = 0; i < path_count && !cloned; ++i) {
    for (UINT32 j = i + 1; j < path_count; ++j) {
      if (paths[i].sourceInfo.adapterId.LowPart ==
              paths[j].sourceInfo.adapterId.LowPart &&
          paths[i].sourceInfo.adapterId.HighPart ==
              paths[j].sourceInfo.adapterId.HighPart &&
          paths[i].sourceInfo.id == paths[j].sourceInfo.id) {
        cloned = true;
        break;
      }
    }
  }

  // Also if Windows kept a freshly attached virtual monitor disabled while
  // QDC_ALL_PATHS knows about an inactive available target, extend it.
  bool inactive_target_exists = false;
  if (!cloned && (activate_inactive_targets || path_count == 1)) {
    UINT32 all_paths = 0;
    UINT32 all_modes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ALL_PATHS, &all_paths, &all_modes) ==
            ERROR_SUCCESS &&
        all_paths > 1) {
      std::vector<DISPLAYCONFIG_PATH_INFO> ap(all_paths);
      std::vector<DISPLAYCONFIG_MODE_INFO> am(all_modes);
      if (QueryDisplayConfig(QDC_ALL_PATHS, &all_paths, ap.data(), &all_modes,
                             am.data(), nullptr) == ERROR_SUCCESS) {
        for (UINT32 i = 0; i < all_paths; ++i) {
          if ((ap[i].flags & DISPLAYCONFIG_PATH_ACTIVE) == 0 &&
              ap[i].targetInfo.targetAvailable) {
            inactive_target_exists = true;
            break;
          }
        }
      }
    }
  }

  if (!cloned && !inactive_target_exists) {
    return false;
  }

  const LONG rc = SetDisplayConfig(0, nullptr, 0, nullptr,
                                   SDC_TOPOLOGY_EXTEND | SDC_APPLY);
  log_out = cloned ? "检测到复制模式，已切换为扩展屏幕 (rc=" +
                         std::to_string(rc) + ")"
                   : "检测到未激活显示输出，已尝试启用扩展模式 (rc=" +
                         std::to_string(rc) + ")";
  return rc == ERROR_SUCCESS;
#else
  (void)activate_inactive_targets;
  return false;
#endif
}

std::uintptr_t VirtualDisplayManager::create_display(std::uint64_t link_id,
                                                     int width, int height,
                                                     int refresh_hz,
                                                     std::string& log_out) {
  log_out.clear();
#ifdef _WIN32
  if (keepalive_stop_.load(std::memory_order_acquire)) {
    stop_keepalive();
  }
  std::unique_lock lock(mutex_);
  auto existing_it = managed_.find(link_id);
  if (existing_it != managed_.end() &&
      existing_it->second.monitor_handle != 0) {
    return existing_it->second.monitor_handle;
  }

  std::unordered_set<std::uintptr_t> before;
  for (const auto& m : enumerate_monitors()) {
    before.insert(m.handle);
  }

  HANDLE parsec =
      parsec_handle_ != nullptr ? static_cast<HANDLE>(parsec_handle_)
                                : open_parsec_vdd_handle();
  if (parsec != INVALID_HANDLE_VALUE) {
    parsec_handle_ = parsec;
    register_parsec_custom_resolution(width, height, refresh_hz);
    int vdd_index = -1;
    if (!parsec_vdd_ioctl(parsec, kVddIoctlAdd, nullptr, 0, &vdd_index) ||
        vdd_index < 0) {
      log_out = "Parsec VDD VDD_IOCTL_ADD 失败";
      if (managed_.empty()) {
        CloseHandle(parsec);
        parsec_handle_ = nullptr;
      }
      return 0;
    }
    (void)parsec_vdd_ioctl(parsec, kVddIoctlUpdate, nullptr, 0, nullptr);
    ManagedDisplay md;
    md.backend = VirtualDisplayBackend::ParsecVdd;
    md.parsec_index = vdd_index;
    managed_[link_id] = md;
    ensure_keepalive_started_locked();

    // Release mutex during PnP wait so keepalive_loop() pings VDD_IOCTL_UPDATE
    // every 100ms uninterrupted (otherwise Parsec VDD unplugs after ~1s).
    lock.unlock();

    std::uintptr_t new_handle = 0;
    std::string new_name;
    bool tried_extend = false;
    for (int step = 0; step < 20; ++step) {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      for (const auto& m : enumerate_monitors()) {
        if (before.count(m.handle) == 0) {
          new_handle = m.handle;
          new_name = m.name;
          break;
        }
      }
      if (new_handle != 0) {
        break;
      }
      if (!tried_extend && step >= 3) {
        tried_extend = true;
        std::string topo;
        (void)ensure_extended_topology(topo, true);
      }
    }

    log_out = "已通过 Parsec VDD 创建虚拟显示器 (idx=" +
              std::to_string(vdd_index) + ")";
    if (new_handle != 0) {
      apply_best_mode_for_monitor(new_handle, width, height, refresh_hz,
                                  log_out);
      if (!new_name.empty()) {
        for (const auto& m : enumerate_monitors()) {
          if (m.name == new_name) {
            new_handle = m.handle;
            break;
          }
        }
      }
    } else {
      log_out += "，但系统暂未暴露新的扩展 HMONITOR";
    }

    lock.lock();
    auto it = managed_.find(link_id);
    if (it != managed_.end()) {
      it->second.monitor_handle = new_handle;
    }
    return new_handle;
  }

  if (mtt_vdd_pipe_available()) {
    const int target_count = mtt_managed_count_ + 1;
    std::string resp;
    if (!send_mtt_vdd_command(
            "SETDISPLAYCOUNT " + std::to_string(target_count), resp)) {
      log_out = "MttVDD 管道指令发送失败";
      return 0;
    }
    mtt_managed_count_ = target_count;
    ManagedDisplay md;
    md.backend = VirtualDisplayBackend::MttVdd;
    managed_[link_id] = md;
    lock.unlock();

    std::uintptr_t new_handle = 0;
    std::string new_name;
    bool tried_extend = false;
    for (int step = 0; step < 20; ++step) {
      std::this_thread::sleep_for(std::chrono::milliseconds(150));
      for (const auto& m : enumerate_monitors()) {
        if (before.count(m.handle) == 0) {
          new_handle = m.handle;
          new_name = m.name;
          break;
        }
      }
      if (new_handle != 0) {
        break;
      }
      if (!tried_extend && step >= 3) {
        tried_extend = true;
        std::string topo;
        (void)ensure_extended_topology(topo, true);
      }
    }

    log_out = "已通过 MttVDD 创建虚拟显示器 (count=" +
              std::to_string(target_count) + ")";
    if (new_handle != 0) {
      apply_best_mode_for_monitor(new_handle, width, height, refresh_hz,
                                  log_out);
      if (!new_name.empty()) {
        for (const auto& m : enumerate_monitors()) {
          if (m.name == new_name) {
            new_handle = m.handle;
            break;
          }
        }
      }
    }

    lock.lock();
    auto it = managed_.find(link_id);
    if (it != managed_.end()) {
      it->second.monitor_handle = new_handle;
    }
    return new_handle;
  }

  (void)width;
  (void)height;
  (void)refresh_hz;
  log_out = "未检测到 Parsec VDD 或 MttVDD 虚拟屏驱动";
  return 0;
#else
  (void)link_id;
  (void)width;
  (void)height;
  (void)refresh_hz;
  log_out = "non-Windows stub";
  return 0;
#endif
}

void VirtualDisplayManager::destroy_display(std::uint64_t link_id) {
#ifdef _WIN32
  std::lock_guard lock(mutex_);
  auto it = managed_.find(link_id);
  if (it == managed_.end()) {
    return;
  }
  const ManagedDisplay md = it->second;
  managed_.erase(it);

  if (md.backend == VirtualDisplayBackend::ParsecVdd &&
      parsec_handle_ != nullptr && md.parsec_index >= 0) {
    const std::uint16_t idx_val = static_cast<std::uint16_t>(md.parsec_index);
    const std::uint16_t be = static_cast<std::uint16_t>(
        ((idx_val & 0x00FFU) << 8U) | ((idx_val >> 8U) & 0x00FFU));
    (void)parsec_vdd_ioctl(static_cast<HANDLE>(parsec_handle_),
                           kVddIoctlRemove, &be, sizeof(be), nullptr);
    (void)parsec_vdd_ioctl(static_cast<HANDLE>(parsec_handle_),
                           kVddIoctlUpdate, nullptr, 0, nullptr);
  } else if (md.backend == VirtualDisplayBackend::MttVdd) {
    mtt_managed_count_ = std::max(0, mtt_managed_count_ - 1);
    std::string resp;
    (void)send_mtt_vdd_command(
        "SETDISPLAYCOUNT " + std::to_string(mtt_managed_count_), resp);
  }

  bool any_parsec = false;
  for (const auto& [id, item] : managed_) {
    (void)id;
    if (item.backend == VirtualDisplayBackend::ParsecVdd) {
      any_parsec = true;
      break;
    }
  }
  if (!any_parsec) {
    keepalive_stop_.store(true, std::memory_order_release);
  }
#else
  (void)link_id;
#endif
}

void VirtualDisplayManager::destroy_all() {
#ifdef _WIN32
  std::vector<std::uint64_t> ids;
  {
    std::lock_guard lock(mutex_);
    ids.reserve(managed_.size());
    for (const auto& [id, md] : managed_) {
      (void)md;
      ids.push_back(id);
    }
  }
  for (std::uint64_t id : ids) {
    destroy_display(id);
  }
  stop_keepalive();
  std::lock_guard lock(mutex_);
  if (parsec_handle_ != nullptr) {
    CloseHandle(static_cast<HANDLE>(parsec_handle_));
    parsec_handle_ = nullptr;
  }
#else
  std::lock_guard lock(mutex_);
  managed_.clear();
  mtt_managed_count_ = 0;
#endif
}

std::uintptr_t VirtualDisplayManager::monitor_for_link(
    std::uint64_t link_id) const {
  std::lock_guard lock(mutex_);
  const auto it = managed_.find(link_id);
  return it != managed_.end() ? it->second.monitor_handle : 0;
}

bool VirtualDisplayManager::has_created_display(std::uint64_t link_id) const {
  std::lock_guard lock(mutex_);
  const auto it = managed_.find(link_id);
  return it != managed_.end() && it->second.monitor_handle != 0;
}

#ifdef _WIN32
void VirtualDisplayManager::ensure_keepalive_started_locked() {
  if (keepalive_thread_.joinable()) {
    return;
  }
  keepalive_stop_.store(false, std::memory_order_release);
  keepalive_thread_ = std::thread([this] { keepalive_loop(); });
}

void VirtualDisplayManager::stop_keepalive() {
  std::thread to_join;
  {
    std::lock_guard lock(mutex_);
    keepalive_stop_.store(true, std::memory_order_release);
    if (keepalive_thread_.joinable()) {
      to_join = std::move(keepalive_thread_);
    }
  }
  if (to_join.joinable()) {
    to_join.join();
  }
}

void VirtualDisplayManager::keepalive_loop() {
  while (!keepalive_stop_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (keepalive_stop_.load(std::memory_order_acquire)) {
      break;
    }
    std::lock_guard lock(mutex_);
    if (keepalive_stop_.load(std::memory_order_acquire) ||
        parsec_handle_ == nullptr) {
      break;
    }
    (void)parsec_vdd_ioctl(static_cast<HANDLE>(parsec_handle_),
                           kVddIoctlUpdate, nullptr, 0, nullptr);
  }
}
#endif

}  // namespace deskmate::windows
