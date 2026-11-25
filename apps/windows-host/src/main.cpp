#include "deskmate/windows/capture_encoder.hpp"
#include "deskmate/windows/host_controller.hpp"
#include "deskmate/windows/studio_ui.hpp"
#include "deskmate/windows/updater.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>  // must follow windows.h
#include <objbase.h>
#endif

namespace {

deskmate::windows::HostOptions parse_options(int argc, char** argv,
                                             bool& show_help) {
  deskmate::windows::HostOptions options;
  std::string cleanup_old_path;
  std::uint32_t cleanup_pid = 0;
  show_help = false;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--width" && index + 1 < argc) {
      options.width = static_cast<std::uint16_t>(std::stoi(argv[++index]));
      options.size_explicit = true;
    } else if (arg == "--height" && index + 1 < argc) {
      options.height = static_cast<std::uint16_t>(std::stoi(argv[++index]));
      options.size_explicit = true;
    } else if (arg == "--fps" && index + 1 < argc) {
      options.refresh_hz = static_cast<std::uint16_t>(std::stoi(argv[++index]));
      options.refresh_hz_explicit = true;
    } else if (arg == "--codec" && index + 1 < argc) {
      options.hevc = std::string(argv[++index]) != "h264";
      options.codec_explicit = true;
    } else if (arg == "--no-auto-match") {
      options.auto_match = false;
    } else if (arg == "--list-monitors" || arg == "--list-displays") {
      options.list_monitors = true;
    } else if (arg == "--dry-run") {
      options.dry_run = true;
    } else if (arg == "--headless") {
      options.headless = true;
    } else if (arg == "--cleanup-old" && index + 1 < argc) {
      cleanup_old_path = argv[++index];
    } else if (arg == "--cleanup-pid" && index + 1 < argc) {
      cleanup_pid = static_cast<std::uint32_t>(std::stoul(argv[++index]));
    } else if (arg == "--help" || arg == "-h") {
      show_help = true;
    }
  }
  deskmate::windows::cleanup_old_executable_if_present(cleanup_old_path,
                                                       cleanup_pid);
  return options;
}

void print_usage() {
  std::printf(
      "DeskMate Windows Host\n"
      "  --width N --height N --fps N\n"
      "  --codec hevc|h264\n"
      "  --no-auto-match   Capture at the requested size instead of the\n"
      "                    monitor's native mode\n"
      "  --list-monitors   Print online monitors and exit\n"
      "  --dry-run         Capture and encode locally without USB AOA\n"
      "  --headless        Run without GUI window (console service mode)\n");
}

int run_deskmate_host(int argc, char** argv) {
  bool show_help = false;
  const auto options = parse_options(argc, argv, show_help);
  if (show_help) {
    print_usage();
    return 0;
  }
  if (options.list_monitors) {
    for (const auto& monitor : deskmate::windows::list_monitors()) {
      std::puts(monitor.c_str());
    }
    return 0;
  }

  deskmate::windows::HostController controller(options);
  controller.start();

  int exit_code = 0;
  if (options.headless) {
    std::printf("DeskMate Windows host running (headless); connect an Android "
                "or HarmonyOS device over USB-C.\n");
    std::fflush(stdout);
#ifdef _WIN32
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
#else
    std::this_thread::sleep_for(std::chrono::seconds(2));
#endif
  } else {
    exit_code = deskmate::windows::run_studio_ui(controller);
  }
  controller.stop();
  return exit_code;
}

}  // namespace

#ifdef _WIN32

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  // STA COM is required on the UI thread for Direct2D, Shell Tray, and
  // Clipboard; DXGI/Media Foundation capture threads initialize MTA themselves.
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool com_owned = SUCCEEDED(com);

  int wargc = 0;
  LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
  std::vector<std::string> storage;
  std::vector<char*> argv;
  if (wargv != nullptr) {
    storage.reserve(static_cast<std::size_t>(wargc));
    argv.reserve(static_cast<std::size_t>(wargc));
    for (int i = 0; i < wargc; ++i) {
      const int needed = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, nullptr,
                                             0, nullptr, nullptr);
      std::string utf8(needed > 1 ? static_cast<std::size_t>(needed - 1) : 0,
                       '\0');
      if (needed > 1) {
        WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, utf8.data(), needed,
                            nullptr, nullptr);
      }
      storage.push_back(std::move(utf8));
      argv.push_back(storage.back().data());
    }
    LocalFree(wargv);
  } else {
    storage.emplace_back("DeskMateHost.exe");
    argv.push_back(storage.back().data());
  }

  bool needs_console = false;
  for (std::size_t i = 1; i < storage.size(); ++i) {
    if (storage[i] == "--headless" || storage[i] == "--list-monitors" ||
        storage[i] == "--list-displays" || storage[i] == "--help" ||
        storage[i] == "-h") {
      needs_console = true;
      break;
    }
  }
  if (needs_console) {
    if (AttachConsole(ATTACH_PARENT_PROCESS) || AllocConsole()) {
      FILE* dummy = nullptr;
      freopen_s(&dummy, "CONOUT$", "w", stdout);
      freopen_s(&dummy, "CONOUT$", "w", stderr);
    }
  }

  const int status =
      run_deskmate_host(static_cast<int>(argv.size()), argv.data());
  if (com_owned) {
    CoUninitialize();
  }
  return status;
}

#else

int main(int argc, char** argv) { return run_deskmate_host(argc, argv); }

#endif
