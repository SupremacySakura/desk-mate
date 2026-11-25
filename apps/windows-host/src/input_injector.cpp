#include "deskmate/windows/input_injector.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace deskmate::windows {

const char* touch_mode_id(TouchInputMode mode) noexcept {
  switch (mode) {
    case TouchInputMode::NativeTouch:
      return "native_touch";
    case TouchInputMode::VirtualTrackpad:
      return "virtual_trackpad";
    case TouchInputMode::CompatibleGestures:
      return "compatible_gestures";
    case TouchInputMode::SingleTouchOnly:
      return "single_touch_only";
  }
  return "compatible_gestures";
}

TouchInputMode touch_mode_from_id(const std::string& id) noexcept {
  if (id == "native_touch") {
    return TouchInputMode::NativeTouch;
  }
  if (id == "virtual_trackpad") {
    return TouchInputMode::VirtualTrackpad;
  }
  if (id == "single_touch_only") {
    return TouchInputMode::SingleTouchOnly;
  }
  return TouchInputMode::CompatibleGestures;
}

std::string touch_mode_display_name(TouchInputMode mode) {
  switch (mode) {
    case TouchInputMode::NativeTouch:
      return "原生屏幕触控 (实验性)";
    case TouchInputMode::VirtualTrackpad:
      return "虚拟触控板 (实验性)";
    case TouchInputMode::CompatibleGestures:
      return "兼容手势映射";
    case TouchInputMode::SingleTouchOnly:
      return "仅单点触控";
  }
  return "兼容手势映射";
}

std::string touch_mode_short_label(TouchInputMode mode) {
  switch (mode) {
    case TouchInputMode::NativeTouch:
      return "原生触控";
    case TouchInputMode::VirtualTrackpad:
      return "虚拟触控板";
    case TouchInputMode::CompatibleGestures:
      return "兼容手势";
    case TouchInputMode::SingleTouchOnly:
      return "仅单点触控";
  }
  return "兼容手势";
}

std::string touch_mode_summary_text(TouchInputMode mode) {
  switch (mode) {
    case TouchInputMode::NativeTouch:
      return "将客户端触点通过 Windows 触摸注入传给应用，支持最多 10 个独立触点；不可用时自动采用兼容手势映射。";
    case TouchInputMode::VirtualTrackpad:
      return "将客户端触控作为相对触控板输入，用于操作指针、拖拽和执行 Windows 桌面手势。";
    case TouchInputMode::CompatibleGestures:
      return "将客户端触控映射为鼠标操作与 Windows 系统快捷键。";
    case TouchInputMode::SingleTouchOnly:
      return "保留单指点击和拖动，关闭多指手势。";
  }
  return {};
}

std::vector<TouchGestureRow> touch_mode_gesture_rows(TouchInputMode mode) {
  if (mode != TouchInputMode::CompatibleGestures) {
    return {};
  }
  return {
      {"单指", "点击、拖动与指针移动"},
      {"双指", "滚动、右键与前台应用缩放（Ctrl+/-）"},
      {"三指 / 四指", "虚拟桌面切换、任务视图与显示桌面"},
  };
}

std::vector<std::string> touch_mode_trackpad_guidance(TouchInputMode mode) {
  if (mode != TouchInputMode::VirtualTrackpad) {
    return {};
  }
  return {
      "单指快速轻点为左键点击，双指轻点为右键，双指滑动可上下/水平滚动。",
      "单指长按约 0.36 秒后移动即可锁存左键拖拽，抬手自动松开。",
  };
}

namespace {

double monotonic_seconds() {
  return std::chrono::duration<double>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

float clamp01(float v) { return std::clamp(v, 0.0F, 1.0F); }

#ifdef _WIN32

// Dynamically resolved Win32 touch injection types so both MSVC and older
// MinGW SDK headers compile cleanly without version-gated errors.
#ifndef POINTER_FLAG_NONE
#define POINTER_FLAG_NONE 0x00000000
#define POINTER_FLAG_NEW 0x00000001
#define POINTER_FLAG_INRANGE 0x00000002
#define POINTER_FLAG_INCONTACT 0x00000004
#define POINTER_FLAG_FIRSTBUTTON 0x00000010
#define POINTER_FLAG_PRIMARY 0x00002000
#define POINTER_FLAG_DOWN 0x00010000
#define POINTER_FLAG_UPDATE 0x00020000
#define POINTER_FLAG_UP 0x00040000
#endif

#ifndef TOUCH_FEEDBACK_DEFAULT
#define TOUCH_FEEDBACK_DEFAULT 0x1
#define TOUCH_FEEDBACK_INDIRECT 0x2
#define TOUCH_FEEDBACK_NONE 0x3
#endif

struct DmPointerTouchInfo {
  DWORD pointerType;
  UINT32 pointerId;
  UINT32 frameId;
  UINT32 pointerFlags;
  HANDLE sourceDevice;
  HWND hwndTarget;
  POINT ptPixelLocation;
  POINT ptHimetricLocation;
  POINT ptPixelLocationRaw;
  POINT ptHimetricLocationRaw;
  DWORD dwTime;
  UINT32 historyCount;
  INT32 InputData;
  DWORD dwKeyStates;
  UINT64 PerformanceCount;
  INT32 ButtonChangeType;
  // TOUCH_INFO trailing fields
  UINT32 touchFlags;
  UINT32 touchMask;
  RECT rcContact;
  RECT rcContactRaw;
  UINT32 orientation;
  UINT32 pressure;
};

using FnInitializeTouchInjection = BOOL(WINAPI*)(UINT32 maxCount, DWORD dwMode);
using FnInjectTouchInput = BOOL(WINAPI*)(UINT32 count,
                                         const DmPointerTouchInfo* contacts);

class Win32TouchInputSink final : public TouchInputSink {
 public:
  Win32TouchInputSink() {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 != nullptr) {
      init_touch_ = reinterpret_cast<FnInitializeTouchInjection>(
          reinterpret_cast<void*>(
              GetProcAddress(user32, "InitializeTouchInjection")));
      inject_touch_ = reinterpret_cast<FnInjectTouchInput>(
          reinterpret_cast<void*>(GetProcAddress(user32, "InjectTouchInput")));
      if (init_touch_ != nullptr && inject_touch_ != nullptr) {
        touch_ready_ = (init_touch_(10, TOUCH_FEEDBACK_DEFAULT) != FALSE);
      }
    }
  }

  void warp_cursor(int x, int y) override { SetCursorPos(x, y); }

  bool current_cursor_location(int& x, int& y) override {
    POINT pt{};
    if (!GetCursorPos(&pt)) {
      return false;
    }
    x = pt.x;
    y = pt.y;
    return true;
  }

  bool post_mouse(MouseActionKind action, int x, int y) override {
    int abs_x = 0;
    int abs_y = 0;
    to_virtual_absolute(x, y, abs_x, abs_y);
    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = abs_x;
    input.mi.dy = abs_y;
    input.mi.dwFlags =
        MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK | MOUSEEVENTF_MOVE;
    switch (action) {
      case MouseActionKind::Move:
      case MouseActionKind::LeftDrag:
        break;
      case MouseActionKind::LeftDown:
        input.mi.dwFlags |= MOUSEEVENTF_LEFTDOWN;
        break;
      case MouseActionKind::LeftUp:
        input.mi.dwFlags |= MOUSEEVENTF_LEFTUP;
        break;
      case MouseActionKind::RightDown:
        input.mi.dwFlags |= MOUSEEVENTF_RIGHTDOWN;
        break;
      case MouseActionKind::RightUp:
        input.mi.dwFlags |= MOUSEEVENTF_RIGHTUP;
        break;
    }
    return SendInput(1, &input, sizeof(input)) == 1;
  }

  bool post_scroll(int x, int y, int delta_x, int delta_y) override {
    (void)x;
    (void)y;
    bool ok = true;
    if (delta_y != 0) {
      INPUT v{};
      v.type = INPUT_MOUSE;
      v.mi.dwFlags = MOUSEEVENTF_WHEEL;
      v.mi.mouseData = static_cast<DWORD>(delta_y);
      ok = ok && (SendInput(1, &v, sizeof(v)) == 1);
    }
    if (delta_x != 0) {
      INPUT h{};
      h.type = INPUT_MOUSE;
      h.mi.dwFlags = MOUSEEVENTF_HWHEEL;
      h.mi.mouseData = static_cast<DWORD>(delta_x);
      ok = ok && (SendInput(1, &h, sizeof(h)) == 1);
    }
    return ok;
  }

  bool post_shortcut(ShortcutKey shortcut) override {
    std::vector<INPUT> inputs;
    auto push_key = [&](WORD vk, bool up) {
      INPUT in{};
      in.type = INPUT_KEYBOARD;
      in.ki.wVk = vk;
      in.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0;
      inputs.push_back(in);
    };
    switch (shortcut) {
      case ShortcutKey::ZoomIn:
        push_key(VK_CONTROL, false);
        push_key(VK_OEM_PLUS, false);
        push_key(VK_OEM_PLUS, true);
        push_key(VK_CONTROL, true);
        break;
      case ShortcutKey::ZoomOut:
        push_key(VK_CONTROL, false);
        push_key(VK_OEM_MINUS, false);
        push_key(VK_OEM_MINUS, true);
        push_key(VK_CONTROL, true);
        break;
      case ShortcutKey::PrevDesktop:
        push_key(VK_LWIN, false);
        push_key(VK_CONTROL, false);
        push_key(VK_LEFT, false);
        push_key(VK_LEFT, true);
        push_key(VK_CONTROL, true);
        push_key(VK_LWIN, true);
        break;
      case ShortcutKey::NextDesktop:
        push_key(VK_LWIN, false);
        push_key(VK_CONTROL, false);
        push_key(VK_RIGHT, false);
        push_key(VK_RIGHT, true);
        push_key(VK_CONTROL, true);
        push_key(VK_LWIN, true);
        break;
      case ShortcutKey::TaskView:
        push_key(VK_LWIN, false);
        push_key(VK_TAB, false);
        push_key(VK_TAB, true);
        push_key(VK_LWIN, true);
        break;
      case ShortcutKey::ShowDesktop:
        push_key(VK_LWIN, false);
        push_key('D', false);
        push_key('D', true);
        push_key(VK_LWIN, true);
        break;
    }
    if (inputs.empty()) {
      return false;
    }
    return SendInput(static_cast<UINT>(inputs.size()), inputs.data(),
                     sizeof(INPUT)) == inputs.size();
  }

  bool post_key(std::uint32_t keycode, bool down) override {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = static_cast<WORD>(keycode);
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;
    return SendInput(1, &input, sizeof(input)) == 1;
  }

  DesktopRect desktop_union_bounds(const InjectorTarget& fallback) override {
    const int w = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int h = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (w <= 0 || h <= 0) {
      return {fallback.left, fallback.top, fallback.width, fallback.height};
    }
    return {GetSystemMetrics(SM_XVIRTUALSCREEN),
            GetSystemMetrics(SM_YVIRTUALSCREEN), w, h};
  }

  [[nodiscard]] bool native_touch_available() const override {
    return touch_ready_;
  }

  bool send_native_touch(
      const std::vector<TouchContactSample>& contacts) override {
    if (!touch_ready_ || inject_touch_ == nullptr) {
      return false;
    }
    std::vector<DmPointerTouchInfo> payload;
    std::map<int, TouchContactSample> next_prev;

    for (const auto& c : contacts) {
      if (payload.size() >= 10) {
        break;
      }
      const bool existed = prev_contacts_.count(c.id) != 0;
      DmPointerTouchInfo info{};
      info.pointerType = 0x00000002;  // PT_TOUCH
      info.pointerId = static_cast<UINT32>(c.id);
      info.pointerFlags = POINTER_FLAG_INRANGE | POINTER_FLAG_INCONTACT |
                          (existed ? POINTER_FLAG_UPDATE : POINTER_FLAG_DOWN);
      info.ptPixelLocation.x = c.screen_x;
      info.ptPixelLocation.y = c.screen_y;
      info.touchMask = 0x00000001 | 0x00000002 | 0x00000004;
      info.rcContact = {c.screen_x - 2, c.screen_y - 2, c.screen_x + 2,
                        c.screen_y + 2};
      info.orientation = 90;
      info.pressure = 512;
      payload.push_back(info);
      next_prev[c.id] = c;
    }
    for (const auto& [old_id, old_sample] : prev_contacts_) {
      if (next_prev.count(old_id) == 0 && payload.size() < 10) {
        DmPointerTouchInfo info{};
        info.pointerType = 0x00000002;  // PT_TOUCH
        info.pointerId = static_cast<UINT32>(old_id);
        info.pointerFlags = POINTER_FLAG_UP;
        info.ptPixelLocation.x = old_sample.screen_x;
        info.ptPixelLocation.y = old_sample.screen_y;
        payload.push_back(info);
      }
    }
    prev_contacts_ = std::move(next_prev);
    if (payload.empty()) {
      return true;
    }
    return inject_touch_(static_cast<UINT32>(payload.size()), payload.data()) !=
           FALSE;
  }

  void reset_native_touch() override {
    if (!prev_contacts_.empty()) {
      send_native_touch({});
    }
  }

 private:
  static void to_virtual_absolute(int sx, int sy, int& abs_x, int& abs_y) {
    const int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN) - 1);
    const int vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN) - 1);
    const int vx = GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    abs_x = static_cast<int>(
        std::lround(static_cast<double>(sx - vx) * 65535.0 / vw));
    abs_y = static_cast<int>(
        std::lround(static_cast<double>(sy - vy) * 65535.0 / vh));
  }

  FnInitializeTouchInjection init_touch_{};
  FnInjectTouchInput inject_touch_{};
  bool touch_ready_{false};
  std::map<int, TouchContactSample> prev_contacts_;
};

#else

class NullTouchInputSink final : public TouchInputSink {
 public:
  void warp_cursor(int, int) override {}
  bool current_cursor_location(int&, int&) override { return false; }
  bool post_mouse(MouseActionKind, int, int) override { return true; }
  bool post_scroll(int, int, int, int) override { return true; }
  bool post_shortcut(ShortcutKey) override { return true; }
  bool post_key(std::uint32_t, bool) override { return false; }
  DesktopRect desktop_union_bounds(const InjectorTarget& fallback) override {
    return {fallback.left, fallback.top, fallback.width, fallback.height};
  }
  [[nodiscard]] bool native_touch_available() const override { return false; }
  bool send_native_touch(const std::vector<TouchContactSample>&) override {
    return false;
  }
  void reset_native_touch() override {}
};

#endif

}  // namespace

std::unique_ptr<TouchInputSink> make_win32_touch_sink() {
#ifdef _WIN32
  return std::make_unique<Win32TouchInputSink>();
#else
  return std::make_unique<NullTouchInputSink>();
#endif
}

InputInjector::InputInjector(InjectorTarget target, TouchInputSink* custom_sink,
                             TouchInputMode mode)
    : target_(target), mode_(mode) {
  if (custom_sink != nullptr) {
    sink_ = custom_sink;
  } else {
    owned_sink_ = make_win32_touch_sink();
    sink_ = owned_sink_.get();
  }
}

InputInjector::~InputInjector() = default;

void InputInjector::set_target(InjectorTarget target) {
  target_ = target;
  reset();
}

void InputInjector::set_mode(TouchInputMode mode) {
  if (mode == mode_) {
    return;
  }
  reset();
  mode_ = mode;
}

TouchCapabilityStatus InputInjector::capability_status() const {
  const bool native_ok = sink_ != nullptr && sink_->native_touch_available();
  switch (mode_) {
    case TouchInputMode::NativeTouch:
      if (native_ok) {
        return {true, false, "Windows 原生触摸注入 (最多 10 触点)",
                "已启用 InitializeTouchInjection 原生多点触控通路。"};
      }
      return {false, true, "兼容手势映射（回退）",
              "当前环境未初始化 Windows 触摸注入，已自动回退为兼容手势映射。"};
    case TouchInputMode::VirtualTrackpad:
      return {native_ok, false, "相对触控板手势映射",
              "使用相对指针移动 + 长按拖拽锁存与系统桌面手势。"};
    case TouchInputMode::CompatibleGestures:
      return {native_ok, false, "兼容手势映射",
              "使用 SendInput 鼠标/滚轮事件与系统快捷键执行单指与多指操作。"};
    case TouchInputMode::SingleTouchOnly:
      return {native_ok, false, "仅单点触控",
              "仅处理主触点点击与拖动，忽略多指手势。"};
  }
  return {};
}

void InputInjector::reset() {
  if (sink_ != nullptr) {
    if (single_dragging_ || trackpad_latch_dragging_ || primary_down_) {
      const int x =
          has_last_cursor_ ? last_cursor_x_ : target_.left + target_.width / 2;
      const int y =
          has_last_cursor_ ? last_cursor_y_ : target_.top + target_.height / 2;
      sink_->post_mouse(MouseActionKind::LeftUp, x, y);
    }
    if (native_touch_active_) {
      sink_->reset_native_touch();
    }
  }
  contacts_.clear();
  primary_id_ = -1;
  max_fingers_in_gesture_ = 0;
  gesture_cooldown_until_zero_ = false;
  has_pending_single_ = false;
  single_dragging_ = false;
  trackpad_latch_dragging_ = false;
  has_trackpad_cursor_ = false;
  has_trackpad_last_norm_ = false;
  two_finger_active_ = false;
  two_finger_locked_pinch_ = false;
  two_finger_did_scroll_ = false;
  multi_swipe_active_ = false;
  multi_swipe_finger_count_ = 0;
  multi_swipe_triggered_ = false;
  scroll_rem_x_ = 0.0;
  scroll_rem_y_ = 0.0;
  native_touch_active_ = false;
  primary_down_ = false;
  primary_pointer_ = 0xFFU;
}

bool InputInjector::inject(const deskmate::protocol::InputEvent& event,
                           double now_seconds) {
  using deskmate::protocol::InputKind;
  if (!std::isfinite(event.x) || !std::isfinite(event.y) ||
      !std::isfinite(event.dx) || !std::isfinite(event.dy)) {
    ++dropped_;
    return false;
  }
  if (target_.width <= 0 || target_.height <= 0 || sink_ == nullptr) {
    ++dropped_;
    return false;
  }
  const double now = now_seconds >= 0.0 ? now_seconds : monotonic_seconds();

  if (event.kind == InputKind::Key) {
    const bool down =
        (event.flags & deskmate::protocol::kInputFlagPrimaryButton) != 0;
    const bool ok = sink_->post_key(event.keycode, down);
    ok ? ++injected_ : ++dropped_;
    return ok;
  }

  const bool is_multitouch =
      (event.flags & deskmate::protocol::kInputFlagMultiTouchFrame) != 0;
  if (!is_multitouch) {
    return process_legacy(event, now);
  }

  const float nx = clamp01(event.x);
  const float ny = clamp01(event.y);
  const int id = static_cast<int>(event.pointer_id);

  switch (event.kind) {
    case InputKind::TouchDown: {
      contacts_[id] =
          ActiveContact{id, nx, ny, nx, ny, now, nx, ny};
      if (primary_id_ < 0) {
        primary_id_ = id;
      }
      break;
    }
    case InputKind::TouchMove: {
      auto it = contacts_.find(id);
      if (it != contacts_.end()) {
        it->second.last_norm_x = it->second.norm_x;
        it->second.last_norm_y = it->second.norm_y;
        it->second.norm_x = nx;
        it->second.norm_y = ny;
      } else {
        contacts_[id] =
            ActiveContact{id, nx, ny, nx, ny, now, nx, ny};
        if (primary_id_ < 0) {
          primary_id_ = id;
        }
      }
      break;
    }
    case InputKind::TouchUp:
    case InputKind::TouchCancel:
      contacts_.erase(id);
      break;
    case InputKind::Scroll:
    case InputKind::Key:
      ++dropped_;
      return false;
  }

  max_fingers_in_gesture_ =
      std::max(max_fingers_in_gesture_, static_cast<int>(contacts_.size()));

  const bool commit =
      (event.flags & deskmate::protocol::kInputFlagFrameCommit) != 0;
  if (!commit) {
    ++injected_;
    return true;
  }
  const bool ok = evaluate_frame(now);
  ok ? ++injected_ : ++dropped_;
  return ok;
}

void InputInjector::tick(double now_seconds) {
  if (sink_ == nullptr || target_.width <= 0 || target_.height <= 0) {
    return;
  }
  if (gesture_cooldown_until_zero_ || contacts_.size() != 1) {
    return;
  }
  const double now = now_seconds >= 0.0 ? now_seconds : monotonic_seconds();

  switch (mode_) {
    case TouchInputMode::CompatibleGestures:
      if (has_pending_single_ && (now - pending_single_.start_time) >= 0.045) {
        commit_single_press_if_needed(now);
      }
      break;
    case TouchInputMode::NativeTouch:
      if (!sink_->native_touch_available() && has_pending_single_ &&
          (now - pending_single_.start_time) >= 0.045) {
        commit_single_press_if_needed(now);
      }
      break;
    case TouchInputMode::VirtualTrackpad:
      if (has_pending_single_ && !trackpad_latch_dragging_ &&
          !pending_single_.moved_exceeds_tap &&
          (now - pending_single_.start_time) >= 0.36) {
        trackpad_latch_dragging_ = true;
        int cx = 0;
        int cy = 0;
        current_trackpad_cursor(cx, cy);
        trackpad_cursor_x_ = cx;
        trackpad_cursor_y_ = cy;
        has_trackpad_cursor_ = true;
        sink_->post_mouse(MouseActionKind::LeftDown, cx, cy);
        single_dragging_ = true;
        primary_down_ = true;
        last_cursor_x_ = cx;
        last_cursor_y_ = cy;
        has_last_cursor_ = true;
      }
      break;
    case TouchInputMode::SingleTouchOnly:
      break;
  }
}

bool InputInjector::evaluate_frame(double now) {
  switch (mode_) {
    case TouchInputMode::NativeTouch:
      if (sink_->native_touch_available()) {
        return dispatch_native_touch();
      }
      return evaluate_compatible_gestures(now);
    case TouchInputMode::VirtualTrackpad:
      return evaluate_relative_trackpad(now);
    case TouchInputMode::CompatibleGestures:
      return evaluate_compatible_gestures(now);
    case TouchInputMode::SingleTouchOnly:
      return evaluate_single_touch_only(now);
  }
  return false;
}

bool InputInjector::dispatch_native_touch() {
  std::vector<TouchContactSample> samples;
  samples.reserve(std::min<std::size_t>(10, contacts_.size()));
  for (const auto& [id, c] : contacts_) {
    if (samples.size() >= 10) {
      break;
    }
    int sx = 0;
    int sy = 0;
    map_point(c.norm_x, c.norm_y, sx, sy);
    samples.push_back(TouchContactSample{id, sx, sy});
  }
  native_touch_active_ = !samples.empty();
  if (contacts_.empty()) {
    max_fingers_in_gesture_ = 0;
    primary_id_ = -1;
  }
  return sink_->send_native_touch(samples);
}

bool InputInjector::evaluate_relative_trackpad(double now) {
  const int count = static_cast<int>(contacts_.size());
  if (count == 0) {
    if (trackpad_latch_dragging_ || single_dragging_) {
      int cx = 0;
      int cy = 0;
      current_trackpad_cursor(cx, cy);
      sink_->post_mouse(MouseActionKind::LeftUp, cx, cy);
      trackpad_latch_dragging_ = false;
      single_dragging_ = false;
      primary_down_ = false;
    } else if (has_pending_single_ && !pending_single_.moved_exceeds_tap &&
               !gesture_cooldown_until_zero_ && max_fingers_in_gesture_ <= 1 &&
               (now - pending_single_.start_time) < 0.26) {
      int cx = 0;
      int cy = 0;
      current_trackpad_cursor(cx, cy);
      perform_left_click(cx, cy, now);
    } else if (two_finger_active_ && max_fingers_in_gesture_ == 2 &&
               !gesture_cooldown_until_zero_ && !two_finger_did_scroll_ &&
               !two_finger_locked_pinch_ &&
               (now - two_finger_start_time_) < 0.30) {
      int cx = 0;
      int cy = 0;
      current_trackpad_cursor(cx, cy);
      perform_right_click(cx, cy);
    }
    reset_gesture_tracking_only();
    return true;
  }

  if (gesture_cooldown_until_zero_) {
    return true;
  }

  if (count >= 3) {
    if (trackpad_latch_dragging_ || single_dragging_) {
      int cx = 0;
      int cy = 0;
      current_trackpad_cursor(cx, cy);
      sink_->post_mouse(MouseActionKind::LeftUp, cx, cy);
      trackpad_latch_dragging_ = false;
      single_dragging_ = false;
      primary_down_ = false;
    }
    has_pending_single_ = false;
    two_finger_active_ = false;
    two_finger_locked_pinch_ = false;
    return handle_multi_finger_swipe(count);
  }

  if (count == 2) {
    if (max_fingers_in_gesture_ > 2) {
      gesture_cooldown_until_zero_ = true;
      return true;
    }
    has_pending_single_ = false;
    if (trackpad_latch_dragging_ || single_dragging_) {
      int cx = 0;
      int cy = 0;
      current_trackpad_cursor(cx, cy);
      sink_->post_mouse(MouseActionKind::LeftUp, cx, cy);
      trackpad_latch_dragging_ = false;
      single_dragging_ = false;
      primary_down_ = false;
    }
    return handle_two_finger_frame(true, now);
  }

  // count == 1
  if (max_fingers_in_gesture_ > 1) {
    two_finger_active_ = false;
    two_finger_locked_pinch_ = false;
    gesture_cooldown_until_zero_ = true;
    return true;
  }

  const auto& c = contacts_.begin()->second;
  const DesktopRect desktop = sink_->desktop_union_bounds(target_);

  if (has_pending_single_ && pending_single_.id == c.id) {
    int prev_x = 0;
    int prev_y = 0;
    current_trackpad_cursor(prev_x, prev_y);
    const float prev_nx =
        has_trackpad_last_norm_ ? trackpad_last_norm_x_ : c.last_norm_x;
    const float prev_ny =
        has_trackpad_last_norm_ ? trackpad_last_norm_y_ : c.last_norm_y;
    trackpad_last_norm_x_ = c.norm_x;
    trackpad_last_norm_y_ = c.norm_y;
    has_trackpad_last_norm_ = true;

    constexpr double kGain = 1.25;
    int next_x = prev_x + static_cast<int>(std::lround(
                              static_cast<double>(c.norm_x - prev_nx) *
                              target_.width * kGain));
    int next_y = prev_y + static_cast<int>(std::lround(
                              static_cast<double>(c.norm_y - prev_ny) *
                              target_.height * kGain));
    clamp_to_rect(next_x, next_y, desktop);
    trackpad_cursor_x_ = next_x;
    trackpad_cursor_y_ = next_y;
    has_trackpad_cursor_ = true;
    last_cursor_x_ = next_x;
    last_cursor_y_ = next_y;
    has_last_cursor_ = true;

    const double total_move = std::hypot(
        static_cast<double>(c.norm_x - pending_single_.start_norm_x) *
            target_.width,
        static_cast<double>(c.norm_y - pending_single_.start_norm_y) *
            target_.height);
    if (total_move >= 10.0) {
      pending_single_.moved_exceeds_tap = true;
    }

    if (!trackpad_latch_dragging_ && !pending_single_.moved_exceeds_tap &&
        (now - pending_single_.start_time) >= 0.36) {
      trackpad_latch_dragging_ = true;
      sink_->post_mouse(MouseActionKind::LeftDown, next_x, next_y);
      single_dragging_ = true;
      primary_down_ = true;
    } else if (trackpad_latch_dragging_ || single_dragging_) {
      if (next_x != prev_x || next_y != prev_y) {
        sink_->post_mouse(MouseActionKind::LeftDrag, next_x, next_y);
      }
    } else if (next_x != prev_x || next_y != prev_y) {
      sink_->post_mouse(MouseActionKind::Move, next_x, next_y);
    }
  } else {
    int cx = 0;
    int cy = 0;
    current_trackpad_cursor(cx, cy);
    trackpad_cursor_x_ = cx;
    trackpad_cursor_y_ = cy;
    has_trackpad_cursor_ = true;
    last_cursor_x_ = cx;
    last_cursor_y_ = cy;
    has_last_cursor_ = true;
    trackpad_last_norm_x_ = c.norm_x;
    trackpad_last_norm_y_ = c.norm_y;
    has_trackpad_last_norm_ = true;
    int sx = 0;
    int sy = 0;
    map_point(c.norm_x, c.norm_y, sx, sy);
    pending_single_ = PendingSinglePress{
        c.id, sx, sy, c.norm_x, c.norm_y, now, false};
    has_pending_single_ = true;
  }
  return true;
}

bool InputInjector::evaluate_compatible_gestures(double now) {
  const int count = static_cast<int>(contacts_.size());
  if (count == 0) {
    if (single_dragging_) {
      const int x =
          has_last_cursor_ ? last_cursor_x_ : target_.left + target_.width / 2;
      const int y =
          has_last_cursor_ ? last_cursor_y_ : target_.top + target_.height / 2;
      sink_->post_mouse(MouseActionKind::LeftUp, x, y);
      single_dragging_ = false;
      primary_down_ = false;
    } else if (has_pending_single_ && !gesture_cooldown_until_zero_ &&
               max_fingers_in_gesture_ <= 1) {
      perform_left_click(pending_single_.start_x, pending_single_.start_y, now);
    } else if (two_finger_active_ && max_fingers_in_gesture_ == 2 &&
               !gesture_cooldown_until_zero_ && !two_finger_did_scroll_ &&
               !two_finger_locked_pinch_ &&
               (now - two_finger_start_time_) < 0.30) {
      int rx = 0;
      int ry = 0;
      map_point(two_finger_start_mid_x_, two_finger_start_mid_y_, rx, ry);
      perform_right_click(rx, ry);
    }
    reset_gesture_tracking_only();
    return true;
  }

  if (gesture_cooldown_until_zero_) {
    return true;
  }

  if (count >= 3) {
    if (single_dragging_) {
      const int x =
          has_last_cursor_ ? last_cursor_x_ : target_.left + target_.width / 2;
      const int y =
          has_last_cursor_ ? last_cursor_y_ : target_.top + target_.height / 2;
      sink_->post_mouse(MouseActionKind::LeftUp, x, y);
      single_dragging_ = false;
      primary_down_ = false;
    }
    has_pending_single_ = false;
    two_finger_active_ = false;
    two_finger_locked_pinch_ = false;
    return handle_multi_finger_swipe(count);
  }

  if (count == 2) {
    if (max_fingers_in_gesture_ > 2) {
      gesture_cooldown_until_zero_ = true;
      return true;
    }
    if (single_dragging_) {
      const int x =
          has_last_cursor_ ? last_cursor_x_ : target_.left + target_.width / 2;
      const int y =
          has_last_cursor_ ? last_cursor_y_ : target_.top + target_.height / 2;
      sink_->post_mouse(MouseActionKind::LeftUp, x, y);
      single_dragging_ = false;
      primary_down_ = false;
    }
    has_pending_single_ = false;
    return handle_two_finger_frame(false, now);
  }

  // count == 1
  if (max_fingers_in_gesture_ > 1) {
    two_finger_active_ = false;
    two_finger_locked_pinch_ = false;
    gesture_cooldown_until_zero_ = true;
    return true;
  }

  const auto& c = contacts_.begin()->second;
  int px = 0;
  int py = 0;
  map_point(c.norm_x, c.norm_y, px, py);

  if (single_dragging_) {
    last_cursor_x_ = px;
    last_cursor_y_ = py;
    has_last_cursor_ = true;
    sink_->post_mouse(MouseActionKind::LeftDrag, px, py);
  } else if (has_pending_single_ && pending_single_.id == c.id) {
    const double travel = std::hypot(px - pending_single_.start_x,
                                     py - pending_single_.start_y);
    if (travel >= 8.0 || (now - pending_single_.start_time) >= 0.045) {
      commit_single_press_if_needed(pending_single_.start_time);
      if (single_dragging_ &&
          (px != pending_single_.start_x || py != pending_single_.start_y)) {
        last_cursor_x_ = px;
        last_cursor_y_ = py;
        has_last_cursor_ = true;
        sink_->post_mouse(MouseActionKind::LeftDrag, px, py);
      }
    }
  } else {
    pending_single_ = PendingSinglePress{
        c.id, px, py, c.norm_x, c.norm_y, now, false};
    has_pending_single_ = true;
  }
  return true;
}

void InputInjector::commit_single_press_if_needed(double now) {
  (void)now;
  if (!has_pending_single_ || single_dragging_) {
    return;
  }
  last_cursor_x_ = pending_single_.start_x;
  last_cursor_y_ = pending_single_.start_y;
  has_last_cursor_ = true;
  sink_->warp_cursor(pending_single_.start_x, pending_single_.start_y);
  sink_->post_mouse(MouseActionKind::Move, pending_single_.start_x,
                    pending_single_.start_y);
  sink_->post_mouse(MouseActionKind::LeftDown, pending_single_.start_x,
                    pending_single_.start_y);
  single_dragging_ = true;
  primary_down_ = true;
  has_pending_single_ = false;
}

bool InputInjector::handle_two_finger_frame(bool use_trackpad_cursor,
                                            double now) {
  if (contacts_.size() < 2) {
    return false;
  }
  auto it = contacts_.begin();
  const auto& c0 = it->second;
  ++it;
  const auto& c1 = it->second;

  const float mx = (c0.norm_x + c1.norm_x) * 0.5F;
  const float my = (c0.norm_y + c1.norm_y) * 0.5F;
  const float norm_dist = std::hypot(c0.norm_x - c1.norm_x, c0.norm_y - c1.norm_y);
  const double dist_px =
      std::hypot(static_cast<double>(c0.norm_x - c1.norm_x) * target_.width,
                 static_cast<double>(c0.norm_y - c1.norm_y) * target_.height);

  if (!two_finger_active_) {
    two_finger_active_ = true;
    two_finger_start_time_ = now;
    two_finger_start_mid_x_ = mx;
    two_finger_start_mid_y_ = my;
    two_finger_prev_mid_x_ = mx;
    two_finger_prev_mid_y_ = my;
    two_finger_start_dist_px_ = dist_px;
    two_finger_pinch_ref_dist_norm_ = norm_dist;
    two_finger_locked_pinch_ = false;
    two_finger_did_scroll_ = false;
    scroll_rem_x_ = 0.0;
    scroll_rem_y_ = 0.0;
    return true;
  }

  const double center_px = std::hypot(
      static_cast<double>(mx - two_finger_start_mid_x_) * target_.width,
      static_cast<double>(my - two_finger_start_mid_y_) * target_.height);
  const double dist_change_px = std::abs(dist_px - two_finger_start_dist_px_);

  if (!two_finger_did_scroll_ &&
      (two_finger_locked_pinch_ ||
       (dist_change_px > 22.0 && dist_change_px > center_px * 1.25))) {
    two_finger_locked_pinch_ = true;
    if (two_finger_pinch_ref_dist_norm_ > 0.001F) {
      const float ratio = norm_dist / two_finger_pinch_ref_dist_norm_;
      const float delta = norm_dist - two_finger_pinch_ref_dist_norm_;
      if (ratio >= 1.11F || delta >= 0.042F) {
        sink_->post_shortcut(ShortcutKey::ZoomIn);
        two_finger_pinch_ref_dist_norm_ = norm_dist;
      } else if (ratio <= 0.90F || delta <= -0.042F) {
        sink_->post_shortcut(ShortcutKey::ZoomOut);
        two_finger_pinch_ref_dist_norm_ = norm_dist;
      }
    } else {
      two_finger_pinch_ref_dist_norm_ = norm_dist;
    }
    return true;
  }

  if (!two_finger_locked_pinch_ &&
      (two_finger_did_scroll_ || center_px > 10.0)) {
    const float dx_norm = mx - two_finger_prev_mid_x_;
    const float dy_norm = my - two_finger_prev_mid_y_;
    two_finger_prev_mid_x_ = mx;
    two_finger_prev_mid_y_ = my;

    int sx = 0;
    int sy = 0;
    if (use_trackpad_cursor) {
      current_trackpad_cursor(sx, sy);
    } else {
      map_point(mx, my, sx, sy);
      if (!has_last_cursor_ || last_cursor_x_ < target_.left ||
          last_cursor_x_ >= target_.left + target_.width ||
          last_cursor_y_ < target_.top ||
          last_cursor_y_ >= target_.top + target_.height) {
        sink_->warp_cursor(sx, sy);
        sink_->post_mouse(MouseActionKind::Move, sx, sy);
        last_cursor_x_ = sx;
        last_cursor_y_ = sy;
        has_last_cursor_ = true;
      }
    }

    // Scale normalized motion to Win32 WHEEL_DELTA (120) notches.
    scroll_rem_y_ += static_cast<double>(dy_norm) * target_.height * 0.75;
    scroll_rem_x_ += static_cast<double>(dx_norm) * target_.width * 0.75;
    const int vert = static_cast<int>(std::trunc(scroll_rem_y_));
    const int horiz = static_cast<int>(std::trunc(scroll_rem_x_));
    scroll_rem_y_ -= vert;
    scroll_rem_x_ -= horiz;
    if (vert != 0 || horiz != 0) {
      sink_->post_scroll(sx, sy, -horiz, vert);
      two_finger_did_scroll_ = true;
    }
  }
  return true;
}

bool InputInjector::handle_multi_finger_swipe(int count) {
  if (count < 3 || contacts_.empty()) {
    return false;
  }
  float sum_x = 0.0F;
  float sum_y = 0.0F;
  for (const auto& [id, c] : contacts_) {
    (void)id;
    sum_x += c.norm_x;
    sum_y += c.norm_y;
  }
  const float cx = sum_x / static_cast<float>(contacts_.size());
  const float cy = sum_y / static_cast<float>(contacts_.size());

  if (!multi_swipe_active_ || multi_swipe_finger_count_ != count) {
    multi_swipe_active_ = true;
    multi_swipe_finger_count_ = count;
    multi_swipe_start_cx_ = cx;
    multi_swipe_start_cy_ = cy;
    return true;
  }
  if (multi_swipe_triggered_) {
    return true;
  }

  const float dx = cx - multi_swipe_start_cx_;
  const float dy = cy - multi_swipe_start_cy_;
  const float abs_x = std::abs(dx);
  const float abs_y = std::abs(dy);
  constexpr float kThreshold = 0.055F;

  if (abs_x >= kThreshold && abs_x >= abs_y * 1.2F) {
    sink_->post_shortcut(dx < 0.0F ? ShortcutKey::NextDesktop
                                   : ShortcutKey::PrevDesktop);
    multi_swipe_triggered_ = true;
    gesture_cooldown_until_zero_ = true;
  } else if (abs_y >= kThreshold && abs_y >= abs_x * 1.2F) {
    sink_->post_shortcut(dy < 0.0F ? ShortcutKey::TaskView
                                   : ShortcutKey::ShowDesktop);
    multi_swipe_triggered_ = true;
    gesture_cooldown_until_zero_ = true;
  }
  return true;
}

bool InputInjector::evaluate_single_touch_only(double now) {
  (void)now;
  if (contacts_.empty()) {
    if (single_dragging_) {
      const int x =
          has_last_cursor_ ? last_cursor_x_ : target_.left + target_.width / 2;
      const int y =
          has_last_cursor_ ? last_cursor_y_ : target_.top + target_.height / 2;
      sink_->post_mouse(MouseActionKind::LeftUp, x, y);
      single_dragging_ = false;
      primary_down_ = false;
    }
    primary_id_ = -1;
    gesture_cooldown_until_zero_ = false;
    max_fingers_in_gesture_ = 0;
    return true;
  }
  if (gesture_cooldown_until_zero_) {
    return true;
  }
  if (primary_id_ < 0) {
    primary_id_ = contacts_.begin()->first;
  }
  auto it = contacts_.find(primary_id_);
  if (it == contacts_.end()) {
    if (single_dragging_) {
      const int x =
          has_last_cursor_ ? last_cursor_x_ : target_.left + target_.width / 2;
      const int y =
          has_last_cursor_ ? last_cursor_y_ : target_.top + target_.height / 2;
      sink_->post_mouse(MouseActionKind::LeftUp, x, y);
      single_dragging_ = false;
      primary_down_ = false;
    }
    gesture_cooldown_until_zero_ = true;
    return true;
  }

  int px = 0;
  int py = 0;
  map_point(it->second.norm_x, it->second.norm_y, px, py);
  if (!single_dragging_) {
    last_cursor_x_ = px;
    last_cursor_y_ = py;
    has_last_cursor_ = true;
    sink_->warp_cursor(px, py);
    sink_->post_mouse(MouseActionKind::Move, px, py);
    sink_->post_mouse(MouseActionKind::LeftDown, px, py);
    single_dragging_ = true;
    primary_down_ = true;
  } else if (!has_last_cursor_ || last_cursor_x_ != px ||
             last_cursor_y_ != py) {
    last_cursor_x_ = px;
    last_cursor_y_ = py;
    has_last_cursor_ = true;
    sink_->post_mouse(MouseActionKind::LeftDrag, px, py);
  }
  return true;
}

bool InputInjector::process_legacy(const deskmate::protocol::InputEvent& event,
                                   double now) {
  (void)now;
  using deskmate::protocol::InputKind;
  if (event.kind == InputKind::Scroll) {
    if (mode_ == TouchInputMode::SingleTouchOnly) {
      ++dropped_;
      return false;
    }
    const int delta_y = static_cast<int>(
        std::lround(-static_cast<double>(event.dy) * target_.height * 0.5));
    const int delta_x = static_cast<int>(
        std::lround(static_cast<double>(event.dx) * target_.width * 0.5));
    int sx = 0;
    int sy = 0;
    map_point(event.x, event.y, sx, sy);
    const bool ok = sink_->post_scroll(sx, sy, delta_x, delta_y);
    ok ? ++injected_ : ++dropped_;
    return ok;
  }
  if (event.kind == InputKind::TouchCancel) {
    reset();
    ++injected_;
    return true;
  }

  int sx = 0;
  int sy = 0;
  map_point(event.x, event.y, sx, sy);
  last_cursor_x_ = sx;
  last_cursor_y_ = sy;
  has_last_cursor_ = true;

  const bool primary =
      (event.flags & deskmate::protocol::kInputFlagPrimaryButton) != 0;
  MouseActionKind action = MouseActionKind::Move;
  if (event.kind == InputKind::TouchDown && primary && !primary_down_) {
    action = MouseActionKind::LeftDown;
    primary_down_ = true;
    primary_pointer_ = event.pointer_id;
  } else if (event.kind == InputKind::TouchUp && primary_down_ &&
             event.pointer_id == primary_pointer_) {
    action = MouseActionKind::LeftUp;
    primary_down_ = false;
    primary_pointer_ = 0xFFU;
  } else if (primary_down_) {
    action = MouseActionKind::LeftDrag;
  }

  const bool ok = sink_->post_mouse(action, sx, sy);
  ok ? ++injected_ : ++dropped_;
  return ok;
}

void InputInjector::perform_left_click(int x, int y, double now) {
  (void)now;
  last_cursor_x_ = x;
  last_cursor_y_ = y;
  has_last_cursor_ = true;
  sink_->warp_cursor(x, y);
  sink_->post_mouse(MouseActionKind::Move, x, y);
  sink_->post_mouse(MouseActionKind::LeftDown, x, y);
  sink_->post_mouse(MouseActionKind::LeftUp, x, y);
}

void InputInjector::perform_right_click(int x, int y) {
  last_cursor_x_ = x;
  last_cursor_y_ = y;
  has_last_cursor_ = true;
  sink_->warp_cursor(x, y);
  sink_->post_mouse(MouseActionKind::Move, x, y);
  sink_->post_mouse(MouseActionKind::RightDown, x, y);
  sink_->post_mouse(MouseActionKind::RightUp, x, y);
}

void InputInjector::reset_gesture_tracking_only() {
  has_pending_single_ = false;
  has_trackpad_last_norm_ = false;
  two_finger_active_ = false;
  two_finger_locked_pinch_ = false;
  two_finger_did_scroll_ = false;
  multi_swipe_active_ = false;
  multi_swipe_finger_count_ = 0;
  multi_swipe_triggered_ = false;
  gesture_cooldown_until_zero_ = false;
  max_fingers_in_gesture_ = 0;
  primary_id_ = -1;
  scroll_rem_x_ = 0.0;
  scroll_rem_y_ = 0.0;
}

void InputInjector::current_trackpad_cursor(int& x, int& y) {
  if (has_trackpad_cursor_) {
    x = trackpad_cursor_x_;
    y = trackpad_cursor_y_;
    return;
  }
  if (sink_->current_cursor_location(x, y)) {
    return;
  }
  if (has_last_cursor_) {
    x = last_cursor_x_;
    y = last_cursor_y_;
    return;
  }
  x = target_.left + target_.width / 2;
  y = target_.top + target_.height / 2;
}

void InputInjector::map_point(float nx, float ny, int& x, int& y) const {
  x = target_.left +
      static_cast<int>(std::lround(clamp01(nx) * target_.width));
  y = target_.top +
      static_cast<int>(std::lround(clamp01(ny) * target_.height));
}

void InputInjector::clamp_to_rect(int& x, int& y,
                                  const DesktopRect& rect) const {
  const int max_x = std::max(rect.left, rect.left + rect.width - 1);
  const int max_y = std::max(rect.top, rect.top + rect.height - 1);
  x = std::clamp(x, rect.left, max_x);
  y = std::clamp(y, rect.top, max_y);
}

}  // namespace deskmate::windows
