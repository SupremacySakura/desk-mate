#pragma once

namespace deskmate::windows {

class HostController;

// Runs the native Win32 + Direct2D Alabaster Studio desktop UI and System Tray
// popover on Windows. Returns the process exit code (0 on clean quit).
int run_studio_ui(HostController& controller);

}  // namespace deskmate::windows
