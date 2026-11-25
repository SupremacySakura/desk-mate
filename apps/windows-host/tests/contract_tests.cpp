#include "deskmate/platform/session_profile.hpp"
#include "deskmate/windows/aoa_links.hpp"
#include "deskmate/windows/host_session.hpp"
#include "deskmate/windows/host_state.hpp"
#include "deskmate/windows/input_injector.hpp"
#include "deskmate/windows/virtual_display.hpp"

#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__,    \
                   #condition);                                                \
      ++failures;                                                              \
      return;                                                                  \
    }                                                                          \
  } while (false)

class MockTouchSink final : public deskmate::windows::TouchInputSink {
 public:
  bool native_available{false};
  std::vector<std::pair<deskmate::windows::MouseActionKind, std::pair<int, int>>>
      mouse_events;
  std::vector<std::pair<int, int>> scrolls;
  std::vector<deskmate::windows::ShortcutKey> shortcuts;
  std::vector<std::vector<deskmate::windows::TouchContactSample>> native_frames;
  bool native_reset_called{false};

  void warp_cursor(int x, int y) override {
    cursor_x = x;
    cursor_y = y;
  }
  bool current_cursor_location(int& x, int& y) override {
    x = cursor_x;
    y = cursor_y;
    return true;
  }
  bool post_mouse(deskmate::windows::MouseActionKind action, int x,
                  int y) override {
    mouse_events.push_back({action, {x, y}});
    cursor_x = x;
    cursor_y = y;
    return true;
  }
  bool post_scroll(int, int, int dx, int dy) override {
    scrolls.emplace_back(dx, dy);
    return true;
  }
  bool post_shortcut(deskmate::windows::ShortcutKey s) override {
    shortcuts.push_back(s);
    return true;
  }
  bool post_key(std::uint32_t, bool) override { return true; }
  deskmate::windows::DesktopRect desktop_union_bounds(
      const deskmate::windows::InjectorTarget& fallback) override {
    return {fallback.left, fallback.top, fallback.width, fallback.height};
  }
  [[nodiscard]] bool native_touch_available() const override {
    return native_available;
  }
  bool send_native_touch(
      const std::vector<deskmate::windows::TouchContactSample>& contacts)
      override {
    native_frames.push_back(contacts);
    return native_available;
  }
  void reset_native_touch() override { native_reset_called = true; }

  int cursor_x{500};
  int cursor_y{500};
};

deskmate::protocol::InputEvent make_mt(deskmate::protocol::InputKind kind,
                                       std::uint8_t id, float x, float y,
                                       bool commit) {
  deskmate::protocol::InputEvent ev;
  ev.kind = kind;
  ev.pointer_id = id;
  ev.x = x;
  ev.y = y;
  ev.flags = deskmate::protocol::kInputFlagMultiTouchFrame |
             (commit ? deskmate::protocol::kInputFlagFrameCommit : 0U) |
             deskmate::protocol::kInputFlagPrimaryButton;
  return ev;
}

void test_12k_budget_is_shared() {
  const auto config = deskmate::platform::make_host_session_config(
      1U, 1U, 1920, 1200, 60, true, true);
  CHECK(config.limits.video_fragment_payload == 12U * 1024U);
  CHECK(config.scheduler.max_video_urbs == 8U);
  CHECK(config.limits.stage_timeout_ns == 20'000'000'000ULL);
}

void test_injector_rejects_non_finite_input() {
  deskmate::windows::InputInjector injector;
  deskmate::protocol::InputEvent event;
  event.kind = deskmate::protocol::InputKind::TouchMove;
  event.x = std::numeric_limits<float>::quiet_NaN();
  event.y = 0.5F;
  CHECK(!injector.inject(event));
  CHECK(injector.dropped_events() == 1U);
}

void test_injector_target_can_be_updated() {
  deskmate::windows::InputInjector injector;
  deskmate::windows::InjectorTarget target;
  target.left = 1920;
  target.width = 1280;
  target.height = 800;
  injector.set_target(target);
  CHECK(injector.target().left == 1920);
  CHECK(injector.target().width == 1280);
}

// Regression: every TouchMove used to re-send LEFTDOWN (one click per frame),
// and any finger's TouchUp released the drag. The primary button must be held
// by the contact that pressed it and released only by that contact.
void test_primary_button_is_owned_by_the_first_contact() {
  MockTouchSink sink;
  deskmate::windows::InputInjector injector({0, 0, 1920, 1080}, &sink);
  deskmate::protocol::InputEvent down;
  down.kind = deskmate::protocol::InputKind::TouchDown;
  down.pointer_id = 0;
  down.flags = deskmate::protocol::kInputFlagPrimaryButton;
  down.x = 0.5F;
  down.y = 0.5F;
  CHECK(injector.inject(down));
  CHECK(injector.primary_down());

  deskmate::protocol::InputEvent move;
  move.kind = deskmate::protocol::InputKind::TouchMove;
  move.pointer_id = 0;
  move.flags = deskmate::protocol::kInputFlagPrimaryButton;
  move.x = 0.6F;
  move.y = 0.6F;
  CHECK(injector.inject(move));
  CHECK(injector.primary_down());

  // A secondary finger lifting must not end the drag.
  deskmate::protocol::InputEvent secondary_up;
  secondary_up.kind = deskmate::protocol::InputKind::TouchUp;
  secondary_up.pointer_id = 1;
  CHECK(injector.inject(secondary_up));
  CHECK(injector.primary_down());

  deskmate::protocol::InputEvent primary_up;
  primary_up.kind = deskmate::protocol::InputKind::TouchUp;
  primary_up.pointer_id = 0;
  CHECK(injector.inject(primary_up));
  CHECK(!injector.primary_down());

  // Cancel must release a stuck button.
  CHECK(injector.inject(down));
  CHECK(injector.primary_down());
  deskmate::protocol::InputEvent cancel;
  cancel.kind = deskmate::protocol::InputKind::TouchCancel;
  CHECK(injector.inject(cancel));
  CHECK(!injector.primary_down());
}

void test_compatible_gestures_tap_scroll_pinch_and_swipe() {
  using namespace deskmate::windows;
  using deskmate::protocol::InputKind;
  MockTouchSink sink;
  InputInjector inj({1000, 0, 1000, 1000}, &sink,
                    TouchInputMode::CompatibleGestures);

  // 1. Quick single-finger tap -> LeftDown + LeftUp at mapped coordinates.
  CHECK(inj.inject(make_mt(InputKind::TouchDown, 0, 0.5F, 0.5F, true), 1.00));
  CHECK(inj.inject(make_mt(InputKind::TouchUp, 0, 0.5F, 0.5F, true), 1.02));
  CHECK(sink.mouse_events.size() >= 3U);
  CHECK(sink.mouse_events[sink.mouse_events.size() - 2].first ==
        MouseActionKind::LeftDown);
  CHECK(sink.mouse_events.back().first == MouseActionKind::LeftUp);
  CHECK(sink.mouse_events.back().second.first == 1500);

  // 2. Two-finger tap -> RightDown + RightUp.
  sink.mouse_events.clear();
  CHECK(inj.inject(make_mt(InputKind::TouchDown, 0, 0.4F, 0.4F, false), 2.00));
  CHECK(inj.inject(make_mt(InputKind::TouchDown, 1, 0.45F, 0.4F, true), 2.00));
  CHECK(inj.inject(make_mt(InputKind::TouchUp, 0, 0.4F, 0.4F, false), 2.05));
  CHECK(inj.inject(make_mt(InputKind::TouchUp, 1, 0.45F, 0.4F, true), 2.05));
  CHECK(sink.mouse_events.size() >= 2U);
  CHECK(sink.mouse_events[sink.mouse_events.size() - 2].first ==
        MouseActionKind::RightDown);
  CHECK(sink.mouse_events.back().first == MouseActionKind::RightUp);

  // 3. Two-finger pinch out -> ZoomIn shortcut.
  sink.shortcuts.clear();
  CHECK(inj.inject(make_mt(InputKind::TouchDown, 0, 0.45F, 0.5F, false), 3.00));
  CHECK(inj.inject(make_mt(InputKind::TouchDown, 1, 0.55F, 0.5F, true), 3.00));
  CHECK(inj.inject(make_mt(InputKind::TouchMove, 0, 0.30F, 0.5F, false), 3.02));
  CHECK(inj.inject(make_mt(InputKind::TouchMove, 1, 0.70F, 0.5F, true), 3.02));
  CHECK(inj.inject(make_mt(InputKind::TouchUp, 0, 0.30F, 0.5F, false), 3.05));
  CHECK(inj.inject(make_mt(InputKind::TouchUp, 1, 0.70F, 0.5F, true), 3.05));
  CHECK(!sink.shortcuts.empty());
  CHECK(sink.shortcuts.front() == ShortcutKey::ZoomIn);

  // 4. Three-finger swipe up -> TaskView shortcut.
  sink.shortcuts.clear();
  CHECK(inj.inject(make_mt(InputKind::TouchDown, 0, 0.3F, 0.6F, false), 4.00));
  CHECK(inj.inject(make_mt(InputKind::TouchDown, 1, 0.5F, 0.6F, false), 4.00));
  CHECK(inj.inject(make_mt(InputKind::TouchDown, 2, 0.7F, 0.6F, true), 4.00));
  CHECK(inj.inject(make_mt(InputKind::TouchMove, 0, 0.3F, 0.4F, false), 4.02));
  CHECK(inj.inject(make_mt(InputKind::TouchMove, 1, 0.5F, 0.4F, false), 4.02));
  CHECK(inj.inject(make_mt(InputKind::TouchMove, 2, 0.7F, 0.4F, true), 4.02));
  CHECK(inj.inject(make_mt(InputKind::TouchUp, 0, 0.3F, 0.4F, false), 4.05));
  CHECK(inj.inject(make_mt(InputKind::TouchUp, 1, 0.5F, 0.4F, false), 4.05));
  CHECK(inj.inject(make_mt(InputKind::TouchUp, 2, 0.7F, 0.4F, true), 4.05));
  CHECK(sink.shortcuts.size() == 1U);
  CHECK(sink.shortcuts.front() == ShortcutKey::TaskView);
}

void test_virtual_trackpad_long_press_drag_and_native_touch() {
  using namespace deskmate::windows;
  using deskmate::protocol::InputKind;
  MockTouchSink sink;
  InputInjector trackpad({0, 0, 1000, 1000}, &sink,
                         TouchInputMode::VirtualTrackpad);
  CHECK(trackpad.inject(make_mt(InputKind::TouchDown, 0, 0.5F, 0.5F, true),
                        10.00));
  CHECK(!trackpad.primary_down());
  trackpad.tick(10.40);
  CHECK(trackpad.primary_down());
  CHECK(trackpad.inject(make_mt(InputKind::TouchUp, 0, 0.5F, 0.5F, true),
                        10.45));
  CHECK(!trackpad.primary_down());

  // Native touch mode forwards contacts when available, and falls back when not.
  sink.native_available = true;
  InputInjector native_inj({100, 200, 800, 600}, &sink,
                           TouchInputMode::NativeTouch);
  CHECK(!native_inj.capability_status().fallback_active);
  CHECK(native_inj.inject(make_mt(InputKind::TouchDown, 3, 0.5F, 0.5F, true),
                          1.0));
  CHECK(sink.native_frames.size() == 1U);
  CHECK(sink.native_frames[0].size() == 1U);
  CHECK(sink.native_frames[0][0].screen_x == 500);
  CHECK(sink.native_frames[0][0].screen_y == 500);

  sink.native_available = false;
  CHECK(native_inj.capability_status().fallback_active);
}

void test_settings_store_round_trip() {
  using namespace deskmate::windows;
#ifdef _WIN32
  const std::string tmp =
      (std::filesystem::temp_directory_path() / "deskmate_test_settings.json")
          .string();
#else
  const std::string tmp = "/tmp/deskmate_test_settings.json";
#endif
  std::error_code ec;
  std::filesystem::remove(tmp, ec);

  {
    SettingsStore store(tmp);
    store.prefs().auto_virtual_display = false;
    store.prefs().default_position = DisplayPosition::Left;
    store.prefs().allow_input = false;
    store.prefs().touch_input_mode = TouchInputMode::VirtualTrackpad;
    store.prefs().auto_start_service = false;
    store.prefs().preferred_monitor_name = "\\\\.\\DISPLAY2";
    store.record_seen_device("usb:SN123", "HUAWEI MatePad", "HUAWEI", "SN123",
                             "HEVC 2800X1840@120", true);
    store.rename("usb:SN123", "我的副屏");
    store.set_allowed("usb:SN123", false);
    store.set_preferred_position("usb:SN123", DisplayPosition::Top);
    CHECK(store.save());
  }

  {
    SettingsStore reloaded(tmp);
    CHECK(!reloaded.prefs().auto_virtual_display);
    CHECK(reloaded.prefs().default_position == DisplayPosition::Left);
    CHECK(!reloaded.prefs().allow_input);
    CHECK(reloaded.prefs().touch_input_mode == TouchInputMode::VirtualTrackpad);
    CHECK(!reloaded.prefs().auto_start_service);
    CHECK(reloaded.prefs().preferred_monitor_name == "\\\\.\\DISPLAY2");
    CHECK(reloaded.prefs().paired_devices.size() == 1U);
    CHECK(reloaded.display_name_for("usb:SN123") == "我的副屏");
    CHECK(!reloaded.is_allowed("usb:SN123"));
    CHECK(reloaded.preferred_position_for("usb:SN123") ==
          DisplayPosition::Top);
  }
  std::filesystem::remove(tmp);
}

void test_session_link_restart_increments_reconnects() {
  class InertTransport final : public deskmate::windows::LinkTransport {
   public:
    deskmate::protocol::IoResult read(std::span<std::uint8_t>) override {
      return {deskmate::protocol::IoStatus::WouldBlock, 0};
    }
    deskmate::protocol::IoResult write(
        std::span<const std::uint8_t> source) override {
      return {deskmate::protocol::IoStatus::Ok, source.size()};
    }
    [[nodiscard]] bool alive() const override { return true; }
  };

  deskmate::platform::AoaIdentity id;
  id.product = "MatePad";
  id.port_path = "1:2";
  deskmate::windows::SessionLink link(std::make_unique<InertTransport>(), id,
                                      1U, 100U, 1U, 1920, 1200, 60, true);
  CHECK(link.reconnect_count() == 0U);
  link.restart_session();
  CHECK(link.reconnect_count() == 1U);
  CHECK(link.state() == deskmate::protocol::SessionState::AwaitingClientHello);
  link.stop();
}

void test_session_link_pumps_terminal_transport_and_reports_error() {
  class FailingTransport final : public deskmate::windows::LinkTransport {
   public:
    deskmate::protocol::IoResult read(std::span<std::uint8_t>) override {
      if (alive_now) {
        return {deskmate::protocol::IoStatus::WouldBlock, 0};
      }
      return {close_instead ? deskmate::protocol::IoStatus::Closed
                            : deskmate::protocol::IoStatus::Error,
              0};
    }
    deskmate::protocol::IoResult write(
        std::span<const std::uint8_t> source) override {
      if (!alive_now) {
        return {deskmate::protocol::IoStatus::Error, 0};
      }
      return {deskmate::protocol::IoStatus::Ok, source.size()};
    }
    [[nodiscard]] bool alive() const override { return alive_now; }
    [[nodiscard]] std::string last_error() const override { return err; }

    bool alive_now{true};
    bool close_instead{false};
    std::string err;
  };

  for (bool close_instead : {false, true}) {
    auto transport = std::make_unique<FailingTransport>();
    transport->close_instead = close_instead;
    transport->err = close_instead ? "device unplugged" : "bulk IN failed (13): LIBUSB_ERROR_PIPE";
    auto* raw = transport.get();

    deskmate::platform::AoaIdentity id;
    id.product = "MatePad";
    id.port_path = "1:1";
    deskmate::windows::SessionLink link(std::move(transport), id, 1U, 100U, 1U,
                                        1920, 1200, 60, false);
    CHECK(!link.is_terminal());
    raw->alive_now = false;
    link.pump();
    CHECK(link.is_terminal());
    CHECK(link.failure_reason() == "io_error");
    CHECK(link.transport_last_error() == raw->err);
  }
}

void test_virtual_display_adapter_matching_and_mode_selection() {
  using namespace deskmate::windows;
  CHECK(is_virtual_display_adapter_name("Parsec Virtual Display Adapter"));
  CHECK(is_virtual_display_adapter_name("MONITOR\\PSCN0001\\{4d36e96e}"));
  CHECK(is_virtual_display_adapter_name("Virtual Display Driver (MttVDD)"));
  CHECK(is_virtual_display_adapter_name("IddSampleDriver Device"));
  CHECK(is_virtual_display_adapter_name("SudoMaker Virtual Display"));
  CHECK(!is_virtual_display_adapter_name("NVIDIA GeForce RTX 4070"));
  CHECK(!is_virtual_display_adapter_name("Intel(R) UHD Graphics"));
  CHECK(!is_virtual_display_adapter_name("Dell U2723QE"));

  const std::vector<DisplayModeCandidate> modes{
      {1920, 1080, 60},
      {1920, 1200, 60},
      {2560, 1440, 60},
      {2560, 1440, 120},
      {2848, 1320, 60},
      {2848, 1320, 120},
  };

  const auto exact = pick_best_display_mode(modes, 2848, 1320, 60);
  CHECK(exact.has_value());
  CHECK(exact->width == 2848 && exact->height == 1320 && exact->refresh_hz == 60);

  const auto closest_hz = pick_best_display_mode(modes, 2560, 1440, 90);
  CHECK(closest_hz.has_value());
  CHECK(closest_hz->width == 2560 && closest_hz->height == 1440);

  const std::vector<DisplayModeCandidate> limited{
      {1920, 1080, 60},
      {1920, 1200, 60},
      {2560, 1200, 60},
  };
  const auto aspect_fallback = pick_best_display_mode(limited, 2848, 1320, 60);
  CHECK(aspect_fallback.has_value());
  CHECK(aspect_fallback->width == 2560 && aspect_fallback->height == 1200);

  VirtualDisplayManager mgr;
  const auto status = mgr.probe_status();
  CHECK(!status.driver_installed);
}

}  // namespace

int main() {
  test_12k_budget_is_shared();
  test_injector_rejects_non_finite_input();
  test_injector_target_can_be_updated();
  test_primary_button_is_owned_by_the_first_contact();
  test_compatible_gestures_tap_scroll_pinch_and_swipe();
  test_virtual_trackpad_long_press_drag_and_native_touch();
  test_settings_store_round_trip();
  test_session_link_restart_increments_reconnects();
  test_session_link_pumps_terminal_transport_and_reports_error();
  test_virtual_display_adapter_matching_and_mode_selection();
  if (failures != 0) {
    return 1;
  }
  std::puts("all Windows contract tests passed");
  return 0;
}
