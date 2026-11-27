#pragma once

#include "deskmate/protocol.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace deskmate::windows {

enum class TouchInputMode {
  NativeTouch,
  VirtualTrackpad,
  CompatibleGestures,
  SingleTouchOnly,
};

struct TouchGestureRow {
  std::string fingers;
  std::string description;
};

struct TouchCapabilityStatus {
  bool native_touch_available{};
  bool fallback_active{};
  std::string active_backend_label;
  std::string status_detail;
};

[[nodiscard]] const char* touch_mode_id(TouchInputMode mode) noexcept;
[[nodiscard]] TouchInputMode touch_mode_from_id(const std::string& id) noexcept;
[[nodiscard]] std::string touch_mode_display_name(TouchInputMode mode);
[[nodiscard]] std::string touch_mode_short_label(TouchInputMode mode);
[[nodiscard]] std::string touch_mode_summary_text(TouchInputMode mode);
[[nodiscard]] std::vector<TouchGestureRow> touch_mode_gesture_rows(
    TouchInputMode mode);
[[nodiscard]] std::vector<std::string> touch_mode_trackpad_guidance(
    TouchInputMode mode);
[[nodiscard]] inline bool touch_mode_uses_native_when_available(
    TouchInputMode mode) noexcept {
  return mode == TouchInputMode::NativeTouch;
}
[[nodiscard]] TouchCapabilityStatus describe_touch_capability(
    TouchInputMode mode, bool native_available);

struct InjectorTarget {
  // Virtual desktop bounds in physical pixels. Normalized input is mapped into
  // this rectangle so each device controls only its own extended display.
  int left{};
  int top{};
  int width{1920};
  int height{1200};
};

struct TouchContactSample {
  int id{};
  int screen_x{};
  int screen_y{};
};

enum class MouseButtonKind {
  Left,
  Right,
};

enum class MouseActionKind {
  Move,
  LeftDown,
  LeftDrag,
  LeftUp,
  RightDown,
  RightUp,
};

struct DesktopRect {
  int left{};
  int top{};
  int width{1920};
  int height{1200};
};

enum class ShortcutKey {
  ZoomIn,         // Ctrl + '='
  ZoomOut,        // Ctrl + '-'
  PrevDesktop,    // Ctrl + Win + Left
  NextDesktop,    // Ctrl + Win + Right
  TaskView,       // Win + Tab
  ShowDesktop,    // Win + D
};

// Platform sink abstraction so the 4-mode touch state machine can be exercised
// deterministically on any host without calling Win32 SendInput.
class TouchInputSink {
 public:
  virtual ~TouchInputSink() = default;

  virtual void warp_cursor(int x, int y) = 0;
  [[nodiscard]] virtual bool current_cursor_location(int& x, int& y) = 0;
  virtual bool post_mouse(MouseActionKind action, int x, int y) = 0;
  virtual bool post_scroll(int x, int y, int delta_x, int delta_y) = 0;
  virtual bool post_shortcut(ShortcutKey shortcut) = 0;
  virtual bool post_key(std::uint32_t keycode, bool down) = 0;
  [[nodiscard]] virtual DesktopRect desktop_union_bounds(
      const InjectorTarget& fallback) = 0;
  [[nodiscard]] virtual bool native_touch_available() const = 0;
  virtual bool send_native_touch(
      const std::vector<TouchContactSample>& contacts) = 0;
  virtual void reset_native_touch() = 0;
};

[[nodiscard]] std::unique_ptr<TouchInputSink> make_win32_touch_sink();

// Translates normalized client touch/scroll/key events into Windows input via
// the 4 selectable interaction modes (Compatible Gestures, Single-Touch Only,
// Virtual Trackpad, and Native Screen Touch via InitializeTouchInjection).
class InputInjector {
 public:
  explicit InputInjector(InjectorTarget target = {},
                         TouchInputSink* custom_sink = nullptr,
                         TouchInputMode mode = TouchInputMode::CompatibleGestures);
  ~InputInjector();

  InputInjector(const InputInjector&) = delete;
  InputInjector& operator=(const InputInjector&) = delete;

  void set_target(InjectorTarget target);
  [[nodiscard]] const InjectorTarget& target() const { return target_; }

  void set_mode(TouchInputMode mode);
  [[nodiscard]] TouchInputMode mode() const { return mode_; }
  [[nodiscard]] TouchCapabilityStatus capability_status() const;

  bool inject(const deskmate::protocol::InputEvent& event,
              double now_seconds = -1.0);
  void tick(double now_seconds = -1.0);
  void reset();

  [[nodiscard]] bool primary_down() const { return primary_down_; }
  [[nodiscard]] std::uint64_t injected_events() const { return injected_; }
  [[nodiscard]] std::uint64_t dropped_events() const { return dropped_; }

 private:
  struct ActiveContact {
    int id{};
    float norm_x{};
    float norm_y{};
    float down_x{};
    float down_y{};
    double down_time{};
    float last_norm_x{};
    float last_norm_y{};
  };

  struct PendingSinglePress {
    int id{};
    int start_x{};
    int start_y{};
    float start_norm_x{};
    float start_norm_y{};
    double start_time{};
    bool moved_exceeds_tap{};
  };

  bool process_legacy(const deskmate::protocol::InputEvent& event, double now);
  bool evaluate_frame(double now);
  bool dispatch_native_touch();
  bool evaluate_relative_trackpad(double now);
  bool evaluate_compatible_gestures(double now);
  bool evaluate_single_touch_only(double now);
  bool handle_two_finger_frame(bool use_trackpad_cursor, double now);
  bool handle_multi_finger_swipe(int count);
  void commit_single_press_if_needed(double now);
  void perform_left_click(int x, int y, double now);
  void perform_right_click(int x, int y);
  void reset_gesture_tracking_only();
  void current_trackpad_cursor(int& x, int& y);
  void map_point(float nx, float ny, int& x, int& y) const;
  void clamp_to_rect(int& x, int& y, const DesktopRect& rect) const;

  InjectorTarget target_;
  std::unique_ptr<TouchInputSink> owned_sink_;
  TouchInputSink* sink_{};
  TouchInputMode mode_{TouchInputMode::CompatibleGestures};

  std::map<int, ActiveContact> contacts_;
  int primary_id_{-1};
  int max_fingers_in_gesture_{0};
  bool gesture_cooldown_until_zero_{false};

  bool has_last_cursor_{false};
  int last_cursor_x_{};
  int last_cursor_y_{};

  bool has_pending_single_{false};
  PendingSinglePress pending_single_{};
  bool single_dragging_{false};
  bool trackpad_latch_dragging_{false};
  bool has_trackpad_cursor_{false};
  int trackpad_cursor_x_{};
  int trackpad_cursor_y_{};
  bool has_trackpad_last_norm_{false};
  float trackpad_last_norm_x_{};
  float trackpad_last_norm_y_{};

  bool two_finger_active_{false};
  double two_finger_start_time_{};
  float two_finger_start_mid_x_{};
  float two_finger_start_mid_y_{};
  float two_finger_prev_mid_x_{};
  float two_finger_prev_mid_y_{};
  double two_finger_start_dist_px_{};
  float two_finger_pinch_ref_dist_norm_{};
  bool two_finger_locked_pinch_{false};
  bool two_finger_did_scroll_{false};

  double scroll_rem_x_{0.0};
  double scroll_rem_y_{0.0};

  bool multi_swipe_active_{false};
  int multi_swipe_finger_count_{0};
  float multi_swipe_start_cx_{};
  float multi_swipe_start_cy_{};
  bool multi_swipe_triggered_{false};

  bool native_touch_active_{false};

  // Legacy non-multitouch wire fallback state.
  bool primary_down_{false};
  std::uint8_t primary_pointer_{0xFFU};
  std::uint64_t injected_{};
  std::uint64_t dropped_{};
};

}  // namespace deskmate::windows
