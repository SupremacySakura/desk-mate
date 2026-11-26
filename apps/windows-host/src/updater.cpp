#include "deskmate/windows/updater.hpp"

#include "deskmate/platform/update_crypto.hpp"
#include "deskmate/platform/update_manifest.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <winhttp.h>
#ifndef WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY
#define WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY 4
#endif
#ifndef WINHTTP_OPTION_REDIRECT_POLICY
#define WINHTTP_OPTION_REDIRECT_POLICY 88
#endif
#ifndef WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS
#define WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS 2
#endif
#endif

namespace deskmate::windows {
namespace {

std::string to_lower_ascii_str(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

bool starts_with_str(const std::string& s, const char* prefix) {
  const std::size_t len = std::strlen(prefix);
  return s.size() >= len && s.compare(0, len, prefix) == 0;
}

bool ends_with_str(const std::string& s, const char* suffix) {
  const std::size_t len = std::strlen(suffix);
  return s.size() >= len && s.compare(s.size() - len, len, suffix) == 0;
}

bool is_legacy_app_dir_filename(const std::string& lower_name) {
  if (starts_with_str(lower_name, "deskmate") &&
      (ends_with_str(lower_name, ".old") || ends_with_str(lower_name, ".bak"))) {
    return true;
  }
  if (starts_with_str(lower_name, "deskmatehost-v") &&
      ends_with_str(lower_name, ".exe")) {
    return true;
  }
  if ((starts_with_str(lower_name, "deskmate-v") &&
       lower_name.find("-windows-") != std::string::npos &&
       ends_with_str(lower_name, ".zip")) ||
      (starts_with_str(lower_name, "deskmate-windows-host-") &&
       ends_with_str(lower_name, ".zip"))) {
    return true;
  }
  return false;
}

std::uint64_t directory_size_bytes(const std::filesystem::path& dir) {
  std::error_code ec;
  std::uint64_t total = 0;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec)) {
      const auto sz = entry.file_size(ec);
      if (!ec) {
        total += sz;
      }
    }
  }
  return total;
}

bool is_pid_alive_os(std::uint32_t pid) {
  if (pid == 0) {
    return false;
  }
#ifdef _WIN32
  HANDLE proc = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (proc == nullptr) {
    return false;
  }
  const DWORD wait = WaitForSingleObject(proc, 0);
  CloseHandle(proc);
  return wait == WAIT_TIMEOUT;
#else
  return false;
#endif
}

std::string format_bytes_human(std::uint64_t bytes) {
  char buf[32]{};
  if (bytes >= 1024ULL * 1024ULL) {
    std::snprintf(buf, sizeof(buf), "%.1f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
  } else if (bytes >= 1024ULL) {
    std::snprintf(buf, sizeof(buf), "%.1f KB",
                  static_cast<double>(bytes) / 1024.0);
  } else {
    std::snprintf(buf, sizeof(buf), "%llu B",
                  static_cast<unsigned long long>(bytes));
  }
  return std::string(buf);
}

#ifdef _WIN32

inline constexpr const char* kWindowsPlatformKey = "windows-x64";

std::wstring utf8_to_wide(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int needed = MultiByteToWideChar(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  if (needed <= 0) {
    return {};
  }
  std::wstring out(static_cast<std::size_t>(needed), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), needed);
  return out;
}

std::string wide_to_utf8(const std::wstring& text) {
  if (text.empty()) {
    return {};
  }
  const int needed = WideCharToMultiByte(
      CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
      nullptr, nullptr);
  if (needed <= 0) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                      out.data(), needed, nullptr, nullptr);
  return out;
}

struct ScopedHInternet {
  HINTERNET handle{nullptr};
  ScopedHInternet() = default;
  explicit ScopedHInternet(HINTERNET h) : handle(h) {}
  ~ScopedHInternet() {
    if (handle != nullptr) {
      WinHttpCloseHandle(handle);
    }
  }
  ScopedHInternet(const ScopedHInternet&) = delete;
  ScopedHInternet& operator=(const ScopedHInternet&) = delete;
};

bool winhttp_get(
    const std::string& url_utf8,
    const std::function<bool(const std::uint8_t*, std::size_t, std::uint64_t)>&
        on_chunk,
    std::string& error) {
  const std::wstring url_w = utf8_to_wide(url_utf8);
  if (url_w.empty()) {
    error = "更新地址无效";
    return false;
  }

  URL_COMPONENTS uc{};
  uc.dwStructSize = sizeof(uc);
  wchar_t host[256]{};
  wchar_t path[2048]{};
  uc.lpszHostName = host;
  uc.dwHostNameLength = static_cast<DWORD>(std::size(host));
  uc.lpszUrlPath = path;
  uc.dwUrlPathLength = static_cast<DWORD>(std::size(path));
  if (!WinHttpCrackUrl(url_w.c_str(), static_cast<DWORD>(url_w.size()), 0,
                       &uc)) {
    error = "无法解析更新 URL (err=" + std::to_string(GetLastError()) + ")";
    return false;
  }

  const std::wstring ua =
      utf8_to_wide(std::string("DeskMate-Windows-Host/") + kWindowsHostVersion);
  ScopedHInternet session(WinHttpOpen(
      ua.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
      WINHTTP_NO_PROXY_BYPASS, 0));
  if (session.handle == nullptr) {
    session.handle = WinHttpOpen(ua.c_str(), WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME,
                                 WINHTTP_NO_PROXY_BYPASS, 0);
  }
  if (session.handle == nullptr) {
    error = "初始化 WinHTTP 会话失败 (err=" + std::to_string(GetLastError()) +
            ")";
    return false;
  }

  WinHttpSetTimeouts(session.handle, 15000, 15000, 15000, 30000);

  ScopedHInternet connect(
      WinHttpConnect(session.handle, host, uc.nPort, 0));
  if (connect.handle == nullptr) {
    error = "连接服务器失败 (err=" + std::to_string(GetLastError()) + ")";
    return false;
  }

  const DWORD flags =
      (uc.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
  ScopedHInternet request(WinHttpOpenRequest(
      connect.handle, L"GET", path[0] != L'\0' ? path : L"/", nullptr,
      WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
  if (request.handle == nullptr) {
    error = "创建 HTTP 请求失败 (err=" + std::to_string(GetLastError()) + ")";
    return false;
  }

  DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
  WinHttpSetOption(request.handle, WINHTTP_OPTION_REDIRECT_POLICY, &redir,
                   sizeof(redir));

  if (!WinHttpSendRequest(request.handle, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                          WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
      !WinHttpReceiveResponse(request.handle, nullptr)) {
    error = "请求 GitHub 失败 (err=" + std::to_string(GetLastError()) + ")";
    return false;
  }

  DWORD status_code = 0;
  DWORD status_size = sizeof(status_code);
  WinHttpQueryHeaders(
      request.handle, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
      WINHTTP_HEADER_NAME_BY_INDEX, &status_code, &status_size,
      WINHTTP_NO_HEADER_INDEX);
  if (status_code < 200 || status_code >= 300) {
    error = "服务器返回 HTTP " + std::to_string(status_code);
    return false;
  }

  std::uint64_t content_len = 0;
  wchar_t cl_buf[64]{};
  DWORD cl_size = sizeof(cl_buf);
  if (WinHttpQueryHeaders(request.handle, WINHTTP_QUERY_CONTENT_LENGTH,
                          WINHTTP_HEADER_NAME_BY_INDEX, cl_buf, &cl_size,
                          WINHTTP_NO_HEADER_INDEX)) {
    content_len = std::wcstoull(cl_buf, nullptr, 10);
  }

  std::array<std::uint8_t, 65536> buf{};
  for (;;) {
    DWORD avail = 0;
    if (!WinHttpQueryDataAvailable(request.handle, &avail)) {
      error = "读取网络响应失败 (err=" + std::to_string(GetLastError()) + ")";
      return false;
    }
    if (avail == 0) {
      break;
    }
    DWORD want =
        static_cast<DWORD>(std::min<std::size_t>(avail, buf.size()));
    DWORD read = 0;
    if (!WinHttpReadData(request.handle, buf.data(), want, &read)) {
      error = "下载数据中断 (err=" + std::to_string(GetLastError()) + ")";
      return false;
    }
    if (read == 0) {
      break;
    }
    if (!on_chunk(buf.data(), static_cast<std::size_t>(read), content_len)) {
      if (error.empty()) {
        error = "下载处理被中止";
      }
      return false;
    }
  }
  return true;
}

bool run_hidden_command(const std::wstring& cmdline, DWORD& exit_code) {
  std::wstring mutable_cmd = cmdline;
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  si.dwFlags = STARTF_USESHOWWINDOW;
  si.wShowWindow = SW_HIDE;
  PROCESS_INFORMATION pi{};
  if (!CreateProcessW(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
                      CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
    return false;
  }
  WaitForSingleObject(pi.hProcess, 120000);
  GetExitCodeProcess(pi.hProcess, &exit_code);
  CloseHandle(pi.hThread);
  CloseHandle(pi.hProcess);
  return true;
}

std::wstring quote_arg(const std::wstring& arg) {
  return L"\"" + arg + L"\"";
}

bool extract_zip_archive(const std::filesystem::path& zip_path,
                         const std::filesystem::path& dest_dir,
                         std::string& error) {
  std::error_code ec;
  std::filesystem::create_directories(dest_dir, ec);

  wchar_t sys_dir[MAX_PATH]{};
  GetSystemDirectoryW(sys_dir, MAX_PATH);
  const std::filesystem::path tar_exe =
      std::filesystem::path(sys_dir) / L"tar.exe";

  DWORD exit_code = 1;
  if (std::filesystem::exists(tar_exe, ec)) {
    const std::wstring cmd = quote_arg(tar_exe.wstring()) + L" -xf " +
                             quote_arg(zip_path.wstring()) + L" -C " +
                             quote_arg(dest_dir.wstring());
    if (run_hidden_command(cmd, exit_code) && exit_code == 0) {
      return true;
    }
  }

  // Fallback to PowerShell Expand-Archive on older Windows 10 builds.
  const std::wstring ps_cmd =
      L"powershell.exe -NoProfile -NonInteractive -WindowStyle Hidden "
      L"-Command \"Expand-Archive -LiteralPath '" +
      zip_path.wstring() + L"' -DestinationPath '" + dest_dir.wstring() +
      L"' -Force\"";
  if (run_hidden_command(ps_cmd, exit_code) && exit_code == 0) {
    return true;
  }

  error = "解压更新压缩包失败 (exit=" + std::to_string(exit_code) + ")";
  return false;
}

std::filesystem::path find_extracted_exe(const std::filesystem::path& dir) {
  std::error_code ec;
  const auto direct = dir / L"DeskMateHost.exe";
  if (std::filesystem::is_regular_file(direct, ec)) {
    return direct;
  }
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(dir, ec)) {
    if (entry.is_regular_file(ec) &&
        entry.path().filename() == L"DeskMateHost.exe") {
      return entry.path();
    }
  }
  return {};
}

bool validate_pe_executable(const std::filesystem::path& exe_path) {
  std::error_code ec;
  const auto sz = std::filesystem::file_size(exe_path, ec);
  if (ec || sz < 100ULL * 1024ULL) {
    return false;
  }
  std::ifstream in(exe_path, std::ios::binary);
  if (!in.is_open()) {
    return false;
  }
  char hdr[2]{};
  in.read(hdr, 2);
  return in.gcount() == 2 && hdr[0] == 'M' && hdr[1] == 'Z';
}

#endif  // _WIN32

}  // namespace

UpdateManager::~UpdateManager() = default;

void UpdateManager::set_preferences(bool auto_check,
                                    std::string mirror_prefix) {
  std::lock_guard lock(mutex_);
  state_.auto_check_updates = auto_check;
  state_.mirror_prefix = std::move(mirror_prefix);
}

UpdateUiState UpdateManager::snapshot() const {
  std::lock_guard lock(mutex_);
  return state_;
}

void UpdateManager::check_for_updates(bool manual) {
  std::string mirror;
  {
    std::lock_guard lock(mutex_);
    if (state_.phase == UpdatePhase::Checking ||
        state_.phase == UpdatePhase::Downloading ||
        state_.phase == UpdatePhase::Verifying ||
        state_.phase == UpdatePhase::Installing) {
      return;
    }
    state_.phase = UpdatePhase::Checking;
    state_.error_message.clear();
    state_.status_text = "正在从 GitHub 检查最新版本...";
    mirror = state_.mirror_prefix;
  }

  std::thread([this, manual, mirror]() {
#ifndef _WIN32
    (void)mirror;
    std::lock_guard lock(mutex_);
    if (!manual) {
      state_.phase = UpdatePhase::Idle;
      state_.status_text = "点击检查更新以获取最新版本";
    } else {
      state_.phase = UpdatePhase::UpToDate;
      state_.status_text =
          std::string("已是最新版本 (v") + kWindowsHostVersion + ")";
    }
#else
    const std::string manifest_url = deskmate::platform::apply_mirror_prefix(
        deskmate::platform::kDefaultUpdateManifestUrl, mirror);
    std::string body;
    std::string err;
    constexpr std::size_t kMaxManifestBytes = 1024U * 1024U;
    const bool ok = winhttp_get(
        manifest_url,
        [&](const std::uint8_t* data, std::size_t len, std::uint64_t) {
          if (body.size() + len > kMaxManifestBytes) {
            err = "更新清单体积过大";
            return false;
          }
          body.append(reinterpret_cast<const char*>(data), len);
          return true;
        },
        err);

    if (!ok) {
      std::lock_guard lock(mutex_);
      if (!manual && state_.phase == UpdatePhase::Checking) {
        state_.phase = UpdatePhase::Idle;
        state_.status_text = "自动检查失败，可稍后手动重试";
        state_.error_message.clear();
      } else {
        state_.phase = UpdatePhase::Failed;
        state_.status_text = "检查更新失败";
        state_.error_message = err;
      }
      return;
    }

    deskmate::platform::UpdateManifest manifest;
    std::string parse_err;
    if (!deskmate::platform::parse_update_manifest(body, kWindowsPlatformKey,
                                                   manifest, parse_err)) {
      std::lock_guard lock(mutex_);
      state_.phase = manual ? UpdatePhase::Failed : UpdatePhase::Idle;
      state_.status_text = manual ? "解析更新清单失败" : "点击检查更新以获取最新版本";
      state_.error_message = manual ? parse_err : std::string{};
      return;
    }

    if (!deskmate::platform::is_newer_version(manifest.version,
                                              kWindowsHostVersion)) {
      std::lock_guard lock(mutex_);
      pending_url_.clear();
      pending_sha256_.clear();
      pending_size_ = 0;
      pending_signature_.clear();
      state_.phase = UpdatePhase::UpToDate;
      state_.available_version.clear();
      state_.release_notes.clear();
      state_.release_url = manifest.release_url;
      state_.status_text =
          std::string("已是最新版本 (v") + kWindowsHostVersion + ")";
      state_.error_message.clear();
      return;
    }

    if (!manifest.artifact.has_value()) {
      std::lock_guard lock(mutex_);
      state_.phase = UpdatePhase::UpToDate;
      state_.available_version = manifest.version;
      state_.release_url = manifest.release_url;
      state_.status_text =
          "发现 v" + manifest.version + "，但暂未包含 windows-x64 安装包";
      state_.error_message.clear();
      return;
    }

    if (!deskmate::platform::verify_artifact_signature(
            manifest.version, kWindowsPlatformKey, *manifest.artifact)) {
      std::lock_guard lock(mutex_);
      state_.phase = UpdatePhase::Failed;
      state_.status_text = "更新清单签名校验失败";
      state_.error_message = "Ed25519 数字签名校验不通过，已拒绝此更新";
      return;
    }

    std::lock_guard lock(mutex_);
    pending_url_ = manifest.artifact->url;
    pending_sha256_ = manifest.artifact->sha256;
    pending_size_ = manifest.artifact->size;
    pending_signature_ = manifest.artifact->signature;
    state_.phase = UpdatePhase::Available;
    state_.available_version = manifest.version;
    state_.release_notes = manifest.notes;
    state_.release_url = manifest.release_url;
    state_.downloaded_bytes = 0;
    state_.total_bytes = manifest.artifact->size;
    state_.progress = 0.0;
    state_.status_text = "发现新版本 v" + manifest.version;
    state_.error_message.clear();
#endif
  }).detach();
}

void UpdateManager::download_and_install(
    std::function<void()> before_restart) {
  std::string version;
  std::string raw_url;
  std::string expected_sha;
  std::uint64_t expected_size = 0;
  std::string signature;
  std::string mirror;
  {
    std::lock_guard lock(mutex_);
    if (pending_url_.empty() || state_.available_version.empty() ||
        state_.phase == UpdatePhase::Downloading ||
        state_.phase == UpdatePhase::Verifying ||
        state_.phase == UpdatePhase::Installing) {
      return;
    }
    version = state_.available_version;
    raw_url = pending_url_;
    expected_sha = pending_sha256_;
    expected_size = pending_size_;
    signature = pending_signature_;
    mirror = state_.mirror_prefix;

    state_.phase = UpdatePhase::Downloading;
    state_.downloaded_bytes = 0;
    state_.total_bytes = expected_size;
    state_.progress = 0.0;
    state_.error_message.clear();
    state_.status_text = "正在下载 v" + version + "...";
  }

  std::thread([this, version, raw_url, expected_sha, expected_size, signature,
               mirror, before_restart = std::move(before_restart)]() {
#ifndef _WIN32
    (void)version;
    (void)raw_url;
    (void)expected_sha;
    (void)expected_size;
    (void)signature;
    (void)mirror;
    (void)before_restart;
    std::lock_guard lock(mutex_);
    state_.phase = UpdatePhase::Failed;
    state_.status_text = "仅 Windows 端支持原地更新";
#else
    std::error_code ec;
    const std::filesystem::path work_dir =
        std::filesystem::temp_directory_path(ec) /
        (L"DeskMateUpdate-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(work_dir, ec);
    std::filesystem::create_directories(work_dir, ec);

    const auto fail_with = [&](const std::string& msg) {
      std::error_code rm_ec;
      std::filesystem::remove_all(work_dir, rm_ec);
      std::lock_guard lock(mutex_);
      state_.phase = UpdatePhase::Failed;
      state_.status_text = "安装更新失败";
      state_.error_message = msg;
    };

    const std::filesystem::path zip_path = work_dir / L"update.zip";
    std::ofstream out_file(zip_path, std::ios::binary | std::ios::trunc);
    if (!out_file.is_open()) {
      fail_with("无法在临时目录创建更新文件");
      return;
    }

    const std::string dl_url =
        deskmate::platform::apply_mirror_prefix(raw_url, mirror);
    std::string err;
    std::uint64_t written = 0;
    auto last_ui = std::chrono::steady_clock::now();
    const bool dl_ok = winhttp_get(
        dl_url,
        [&](const std::uint8_t* data, std::size_t len,
            std::uint64_t content_len) {
          if (written + len > deskmate::platform::kMaxUpdateArtifactBytes) {
            err = "安装包大小超过安全限制";
            return false;
          }
          out_file.write(reinterpret_cast<const char*>(data),
                         static_cast<std::streamsize>(len));
          if (!out_file.good()) {
            err = "写入更新临时文件失败";
            return false;
          }
          written += static_cast<std::uint64_t>(len);
          const auto now = std::chrono::steady_clock::now();
          if (now - last_ui >= std::chrono::milliseconds(100)) {
            last_ui = now;
            const std::uint64_t total =
                expected_size > 0 ? expected_size : content_len;
            const double prog =
                total > 0
                    ? std::min(1.0, static_cast<double>(written) /
                                        static_cast<double>(total))
                    : 0.0;
            const int pct = static_cast<int>(std::lround(prog * 100.0));
            std::lock_guard lock(mutex_);
            state_.downloaded_bytes = written;
            state_.total_bytes = total;
            state_.progress = prog;
            state_.status_text = "正在下载更新 (" + std::to_string(pct) + "%)...";
          }
          return true;
        },
        err);
    out_file.close();
    if (!dl_ok) {
      fail_with(err);
      return;
    }

    {
      std::lock_guard lock(mutex_);
      state_.phase = UpdatePhase::Verifying;
      state_.progress = 1.0;
      state_.downloaded_bytes = written;
      state_.status_text = "正在校验 SHA-256 与 Ed25519 签名...";
    }

    std::array<std::uint8_t, 32> actual_hash{};
    std::uint64_t actual_size = 0;
    if (!deskmate::platform::sha256_file(wide_to_utf8(zip_path.wstring()),
                                         actual_hash, actual_size)) {
      fail_with("读取已下载安装包计算哈希失败");
      return;
    }
    const std::string actual_sha_hex =
        deskmate::platform::hex_encode(actual_hash);
    deskmate::platform::UpdateArtifact downloaded_art{
        raw_url, expected_sha, expected_size, signature};
    if (actual_size != expected_size || actual_sha_hex != expected_sha ||
        !deskmate::platform::verify_artifact_signature(
            version, kWindowsPlatformKey, downloaded_art)) {
      fail_with("下载文件 SHA-256 或 Ed25519 数字签名校验失败，已中止替换");
      return;
    }

    {
      std::lock_guard lock(mutex_);
      state_.phase = UpdatePhase::Installing;
      state_.status_text = "正在解压并原地替换程序...";
    }

    const std::filesystem::path extract_dir = work_dir / L"extracted";
    if (!extract_zip_archive(zip_path, extract_dir, err)) {
      fail_with(err);
      return;
    }
    const std::filesystem::path new_exe = find_extracted_exe(extract_dir);
    if (new_exe.empty() || !validate_pe_executable(new_exe)) {
      fail_with("更新包中未找到有效的 DeskMateHost.exe");
      return;
    }

    wchar_t exe_buf[MAX_PATH]{};
    const DWORD exe_len = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
    if (exe_len == 0 || exe_len >= MAX_PATH) {
      fail_with("获取当前可执行文件路径失败");
      return;
    }
    const std::filesystem::path current_exe(exe_buf);
    const std::filesystem::path backup_exe(std::wstring(exe_buf) + L".old");

    // Gracefully release USB links & Parsec VDD virtual displays before swap.
    if (before_restart) {
      before_restart();
    }

    DeleteFileW(backup_exe.c_str());
    if (!MoveFileExW(current_exe.c_str(), backup_exe.c_str(),
                     MOVEFILE_REPLACE_EXISTING)) {
      fail_with("重命名当前程序失败 (err=" + std::to_string(GetLastError()) +
                ")；若位于系统受保护目录，请手动替换。");
      return;
    }
    if (!MoveFileExW(new_exe.c_str(), current_exe.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
      const DWORD move_err = GetLastError();
      MoveFileExW(backup_exe.c_str(), current_exe.c_str(),
                  MOVEFILE_REPLACE_EXISTING);
      fail_with("写入新版 DeskMateHost.exe 失败 (err=" +
                std::to_string(move_err) + ")，已自动恢复原版本。");
      return;
    }

    const std::wstring relaunch_cmd =
        quote_arg(current_exe.wstring()) + L" --cleanup-old " +
        quote_arg(backup_exe.wstring()) + L" --cleanup-pid " +
        std::to_wstring(GetCurrentProcessId());
    std::wstring mutable_relaunch = relaunch_cmd;
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(current_exe.c_str(), mutable_relaunch.data(), nullptr,
                        nullptr, FALSE, 0, nullptr,
                        current_exe.parent_path().c_str(), &si, &pi)) {
      const DWORD launch_err = GetLastError();
      fail_with("已替换至 v" + version + "，但自动重启失败 (err=" +
                std::to_string(launch_err) + ")，请手动双击打开 DeskMateHost.exe。");
      return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    std::filesystem::remove_all(work_dir, ec);
    {
      std::lock_guard lock(mutex_);
      state_.status_text = "正在重启进入新版本...";
      state_.restart_requested = true;
    }
#endif
  }).detach();
}

void cleanup_old_executable_if_present(const std::string& explicit_old_path,
                                       std::uint32_t wait_pid,
                                       std::function<void()> on_done) {
#ifdef _WIN32
  std::wstring target = utf8_to_wide(explicit_old_path);
  wchar_t exe_buf[MAX_PATH]{};
  const DWORD len = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
  const std::filesystem::path current_exe =
      (len > 0 && len < MAX_PATH) ? std::filesystem::path(exe_buf)
                                  : std::filesystem::path{};
  if (target.empty() && !current_exe.empty()) {
    target = current_exe.wstring() + L".old";
  }

  std::thread([target, current_exe, wait_pid,
               on_done = std::move(on_done)]() {
    if (wait_pid != 0) {
      HANDLE old_proc = OpenProcess(SYNCHRONIZE, FALSE, wait_pid);
      if (old_proc != nullptr) {
        WaitForSingleObject(old_proc, 15000);
        CloseHandle(old_proc);
      }
    }
    if (!target.empty()) {
      std::error_code ec;
      if (std::filesystem::exists(target, ec)) {
        for (int i = 0; i < 60; ++i) {
          if (DeleteFileW(target.c_str()) != FALSE) {
            break;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
      }
    }
    if (!current_exe.empty()) {
      std::error_code ec;
      const auto tmp = std::filesystem::temp_directory_path(ec);
      (void)cleanup_legacy_update_files_in_dirs(
          current_exe.parent_path(), current_exe, tmp, GetCurrentProcessId());
    }
    if (on_done) {
      on_done();
    }
  }).detach();
#else
  (void)explicit_old_path;
  (void)wait_pid;
  if (on_done) {
    on_done();
  }
#endif
}

std::vector<LegacyUpdateArtifact> scan_legacy_update_files_in_dirs(
    const std::filesystem::path& app_dir,
    const std::filesystem::path& current_exe_path,
    const std::filesystem::path& temp_dir, std::uint32_t current_pid) {
  std::vector<LegacyUpdateArtifact> found;
  std::error_code ec;

  std::filesystem::path canonical_cur;
  if (!current_exe_path.empty()) {
    canonical_cur = std::filesystem::weakly_canonical(current_exe_path, ec);
    if (ec) {
      canonical_cur = current_exe_path;
      ec.clear();
    }
  }

  if (!app_dir.empty() && std::filesystem::is_directory(app_dir, ec)) {
    for (const auto& entry :
         std::filesystem::directory_iterator(app_dir, ec)) {
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      if (!canonical_cur.empty()) {
        std::error_code cmp_ec;
        auto cand = std::filesystem::weakly_canonical(entry.path(), cmp_ec);
        if (cmp_ec) {
          cand = entry.path();
        }
        if (cand == canonical_cur) {
          continue;
        }
      }
      const std::string lower_name =
          to_lower_ascii_str(entry.path().filename().string());
      if (is_legacy_app_dir_filename(lower_name)) {
        const auto sz = entry.file_size(ec);
        found.push_back(
            {entry.path().string(), ec ? 0ULL : sz, false});
        ec.clear();
      }
    }
  }

  ec.clear();
  if (!temp_dir.empty() && std::filesystem::is_directory(temp_dir, ec)) {
    for (const auto& entry :
         std::filesystem::directory_iterator(temp_dir, ec)) {
      if (!entry.is_directory(ec)) {
        continue;
      }
      const std::string fname = entry.path().filename().string();
      if (!starts_with_str(fname, "DeskMateUpdate-")) {
        continue;
      }
      const std::string suffix = fname.substr(std::strlen("DeskMateUpdate-"));
      std::uint32_t dir_pid = 0;
      if (!suffix.empty() &&
          std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
          })) {
        dir_pid = static_cast<std::uint32_t>(std::strtoul(suffix.c_str(), nullptr, 10));
      }
      if (dir_pid != 0 &&
          ((current_pid != 0 && dir_pid == current_pid) ||
           is_pid_alive_os(dir_pid))) {
        continue;
      }
      found.push_back({entry.path().string(),
                       directory_size_bytes(entry.path()), true});
    }
  }

  return found;
}

LegacyCleanupSummary cleanup_legacy_update_files_in_dirs(
    const std::filesystem::path& app_dir,
    const std::filesystem::path& current_exe_path,
    const std::filesystem::path& temp_dir, std::uint32_t current_pid) {
  const auto artifacts = scan_legacy_update_files_in_dirs(
      app_dir, current_exe_path, temp_dir, current_pid);
  LegacyCleanupSummary summary;
  summary.scanned_count = artifacts.size();
  for (const auto& item : artifacts) {
    summary.scanned_bytes += item.size_bytes;
    std::error_code ec;
    if (item.is_directory) {
      std::filesystem::remove_all(item.path, ec);
    } else {
      std::filesystem::remove(item.path, ec);
    }
    if (!ec) {
      ++summary.deleted_count;
      summary.freed_bytes += item.size_bytes;
    } else {
      ++summary.failed_count;
    }
  }
  if (summary.scanned_count == 0) {
    summary.message = "未发现旧版本或临时更新残留";
  } else if (summary.failed_count == 0) {
    summary.message = "已清理 " + std::to_string(summary.deleted_count) +
                      " 个旧文件，释放 " +
                      format_bytes_human(summary.freed_bytes);
  } else {
    summary.message = "已清理 " + std::to_string(summary.deleted_count) +
                      " 个，仍有 " + std::to_string(summary.failed_count) +
                      " 个被系统占用（可稍后重试）";
  }
  return summary;
}

void UpdateManager::refresh_legacy_artifacts() {
#ifdef _WIN32
  wchar_t exe_buf[MAX_PATH]{};
  const DWORD len = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) {
    return;
  }
  const std::filesystem::path current_exe(exe_buf);
  std::error_code ec;
  const auto items = scan_legacy_update_files_in_dirs(
      current_exe.parent_path(), current_exe,
      std::filesystem::temp_directory_path(ec), GetCurrentProcessId());
  std::uint64_t total = 0;
  for (const auto& it : items) {
    total += it.size_bytes;
  }
  std::lock_guard lock(mutex_);
  state_.legacy_file_count = items.size();
  state_.legacy_total_bytes = total;
#endif
}

LegacyCleanupSummary UpdateManager::cleanup_legacy_artifacts() {
  LegacyCleanupSummary summary;
#ifdef _WIN32
  wchar_t exe_buf[MAX_PATH]{};
  const DWORD len = GetModuleFileNameW(nullptr, exe_buf, MAX_PATH);
  if (len > 0 && len < MAX_PATH) {
    const std::filesystem::path current_exe(exe_buf);
    std::error_code ec;
    summary = cleanup_legacy_update_files_in_dirs(
        current_exe.parent_path(), current_exe,
        std::filesystem::temp_directory_path(ec), GetCurrentProcessId());
  }
#else
  summary.message = "仅 Windows 端支持本地旧版本文件清理";
#endif
  refresh_legacy_artifacts();
  {
    std::lock_guard lock(mutex_);
    state_.legacy_cleanup_message = summary.message;
  }
  return summary;
}

}  // namespace deskmate::windows
