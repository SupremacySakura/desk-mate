#include "deskmate/platform/annex_b.hpp"
#include "deskmate/platform/aoa.hpp"
#include "deskmate/platform/input_frames.hpp"
#include "deskmate/platform/session_profile.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
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

void test_aoa_identity_prefers_serial_then_port() {
  using deskmate::platform::AoaIdentity;
  using deskmate::platform::aoa_device_id;
  using deskmate::platform::aoa_display_name;

  AoaIdentity serial_device;
  serial_device.bus = 2;
  serial_device.address = 7;
  serial_device.port_path = "2:1.1";
  serial_device.serial = "MATEPAD-001";
  serial_device.product = "MatePad Pro";
  CHECK(aoa_device_id(serial_device) == "usb:MATEPAD-001");
  CHECK(aoa_display_name(serial_device) == "MatePad Pro");

  AoaIdentity port_device;
  port_device.bus = 2;
  port_device.address = 8;
  port_device.port_path = "2:1.2";
  port_device.product = "Mate 80";
  CHECK(aoa_device_id(port_device) == "usb:port:2:1.2");
  CHECK(aoa_device_id(serial_device) != aoa_device_id(port_device));
}

void test_aoa_ids_and_endpoint_selection() {
  using namespace deskmate::platform;
  CHECK(is_aoa_hdc_candidate(0x12D1, 0x5000));
  CHECK(!is_aoa_hdc_candidate(0x18D1, 0x2D00));
  CHECK(is_aoa_accessory(0x18D1, 0x2D00));
  CHECK(is_aoa_accessory(0x18D1, 0x2D05));
  CHECK(!is_aoa_accessory(0x18D1, 0x2D06));

  const std::array<EndpointDescriptorView, 5> endpoints{{
      {0, 0x03, 0x81},
      {1, 0x02, 0x02},
      {1, 0x02, 0x82},
      {1, 0x02, 0x83},
      {2, 0x02, 0x04},
  }};
  EndpointPair pair;
  CHECK(select_bulk_endpoints(endpoints, pair));
  CHECK(pair.interface_number == 1);
  CHECK(pair.endpoint_in == 0x82);
  CHECK(pair.endpoint_out == 0x02);

  const auto encoded = encode_aoa_string("DM");
  const std::vector<std::uint8_t> expected{0x44, 0x00, 0x4D, 0x00, 0x00, 0x00};
  CHECK(encoded == expected);
}

void test_host_profile_keeps_12k_fragment_budget() {
  using namespace deskmate::platform;
  const auto config = make_host_session_config(0x11223344U, 7U, 2800, 1840, 120,
                                               true, true);
  CHECK(config.limits.video_fragment_payload == 12U * 1024U);
  CHECK(config.limits.stage_timeout_ns == 20'000'000'000ULL);
  CHECK(config.capabilities.encoders.size() == 2);
  CHECK(config.capabilities.resolutions.front().width == 2800);
  CHECK(config.accept_device_resolution);

  const auto device = make_device_session_config(0x55667788U, 2800, 1840, 120,
                                                 true);
  CHECK(device.capabilities.encoders.size() == 2);
  CHECK(device.capabilities.resolutions.size() <=
        deskmate::protocol::kMaxCapabilityItems);
}

void test_touch_tracker_emits_committed_multitouch_frames() {
  using namespace deskmate::platform;
  TouchTracker tracker;
  tracker.set_viewport(1000, 500);
  const std::array<TouchSample, 1> down{{{11, 250.0F, 125.0F, true}}};
  auto events = tracker.update(down);
  CHECK(events.size() == 1U);
  CHECK(events[0].kind == deskmate::protocol::InputKind::TouchDown);
  CHECK(events[0].pointer_id == 0U);
  CHECK(std::fabs(events[0].x - 0.25F) < 0.0001F);
  CHECK(std::fabs(events[0].y - 0.25F) < 0.0001F);
  CHECK((events[0].flags & deskmate::protocol::kInputFlagFrameCommit) != 0U);
  CHECK((events[0].flags & deskmate::protocol::kInputFlagPrimaryButton) != 0U);
  CHECK(((events[0].flags & deskmate::protocol::kInputFlagContactCountMask) >>
         deskmate::protocol::kInputFlagContactCountShift) == 1U);

  const std::array<TouchSample, 2> both{{
      {11, 500.0F, 250.0F, true},
      {12, 750.0F, 400.0F, true},
  }};
  events = tracker.update(both);
  CHECK(events.size() == 2U);
  CHECK(events[0].kind == deskmate::protocol::InputKind::TouchMove);
  CHECK(events[1].kind == deskmate::protocol::InputKind::TouchDown);
  CHECK((events.back().flags & deskmate::protocol::kInputFlagFrameCommit) != 0U);
  CHECK(((events.back().flags & deskmate::protocol::kInputFlagContactCountMask) >>
         deskmate::protocol::kInputFlagContactCountShift) == 2U);

  const std::array<TouchSample, 1> one_left{{{11, 500.0F, 250.0F, true}}};
  events = tracker.update(one_left);
  CHECK(events.size() == 1U);
  CHECK(events[0].kind == deskmate::protocol::InputKind::TouchUp);
  CHECK(events[0].pointer_id == 1U);

  const auto release = tracker.release_all(true);
  CHECK(release.size() == 1U);
  CHECK(release[0].kind == deskmate::protocol::InputKind::TouchCancel);
  CHECK(tracker.active_contacts() == 0U);
}

void test_touch_tracker_round_trips_wire_payload() {
  using namespace deskmate::platform;
  TouchTracker tracker;
  tracker.set_viewport(1920, 1080);
  const std::array<TouchSample, 1> sample{{{3, 960.0F, 540.0F, true}}};
  const auto events = tracker.update(sample);
  CHECK(events.size() == 1U);
  const auto payload = deskmate::protocol::encode_input_payload(events[0]);
  CHECK(payload.has_value());
  CHECK(payload.value().size() == 28U);
  const auto decoded =
      deskmate::protocol::decode_input_payload(payload.value());
  CHECK(decoded.has_value());
  CHECK(decoded.value().flags == events[0].flags);
  CHECK(std::fabs(decoded.value().x - 0.5F) < 0.0001F);
}

// Regression: the Android client used to forward one pointer per MotionEvent.
// TouchTracker diffs snapshots, so a one-pointer "move" reported every other
// contact as lifted. A full snapshot must keep them pressed.
void test_touch_tracker_keeps_other_contacts_across_a_move() {
  using namespace deskmate::platform;
  TouchTracker tracker;
  tracker.set_viewport(1000, 1000);

  const std::array<TouchSample, 2> two_down{{
      {0, 100.0F, 100.0F, true},
      {1, 900.0F, 900.0F, true},
  }};
  auto events = tracker.update(two_down);
  CHECK(events.size() == 2U);
  CHECK(tracker.active_contacts() == 2U);

  // Only pointer 0 moves; pointer 1 is still down in the snapshot.
  const std::array<TouchSample, 2> one_moved{{
      {0, 200.0F, 200.0F, true},
      {1, 900.0F, 900.0F, true},
  }};
  events = tracker.update(one_moved);
  CHECK(events.size() == 1U);
  CHECK(events[0].kind == deskmate::protocol::InputKind::TouchMove);
  CHECK(events[0].pointer_id == 0U);
  CHECK(tracker.active_contacts() == 2U);

  // Now simulate the old per-pointer behaviour: a snapshot with only pointer 1.
  // The tracker must report pointer 0 as released, which is exactly the bug the
  // full-snapshot API prevents.
  const std::array<TouchSample, 1> only_second{{{1, 900.0F, 900.0F, true}}};
  events = tracker.update(only_second);
  CHECK(events.size() == 1U);
  CHECK(events[0].kind == deskmate::protocol::InputKind::TouchUp);
  CHECK(events[0].pointer_id == 0U);
  CHECK(tracker.active_contacts() == 1U);
}

void test_annex_b_parameter_set_collection() {
  using namespace deskmate::platform;
  const std::vector<std::uint8_t> h264{
      0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1F,
      0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,
      0x00, 0x00, 0x00, 0x01, 0x65, 0x88, 0x84,
  };
  CHECK(looks_like_annex_b(h264));
  const auto units = split_annex_b(h264);
  CHECK(units.size() == 3U);
  const auto sets = collect_parameter_sets(h264, false);
  const std::vector<std::uint8_t> expected{
      0x00, 0x00, 0x00, 0x01, 0x67, 0x42, 0x00, 0x1F,
      0x00, 0x00, 0x00, 0x01, 0x68, 0xCE, 0x3C, 0x80,
  };
  CHECK(sets == expected);
  CHECK(is_keyframe_nal(0x65, false));
  CHECK(!is_keyframe_nal(0x41, false));
}

void test_avcc_conversion_to_annex_b() {
  using namespace deskmate::platform;
  const std::vector<std::uint8_t> avcc{
      0x00, 0x00, 0x00, 0x03, 0x67, 0x42, 0x00,
      0x00, 0x00, 0x00, 0x02, 0x68, 0xCE,
      0x00, 0x00, 0x00, 0x02, 0x65, 0x88,
  };
  const auto annex_b = to_annex_b(avcc, false);
  CHECK(annex_b.size() == avcc.size());
  CHECK(looks_like_annex_b(annex_b));
  CHECK(split_annex_b(annex_b).size() == 3U);
  const auto sets = collect_parameter_sets(annex_b, false);
  CHECK(!sets.empty());
}

}  // namespace

int main() {
  test_aoa_identity_prefers_serial_then_port();
  test_aoa_ids_and_endpoint_selection();
  test_host_profile_keeps_12k_fragment_budget();
  test_touch_tracker_emits_committed_multitouch_frames();
  test_touch_tracker_round_trips_wire_payload();
  test_touch_tracker_keeps_other_contacts_across_a_move();
  test_annex_b_parameter_set_collection();
  test_avcc_conversion_to_annex_b();
  if (failures != 0) {
    std::fprintf(stderr, "%d platform test(s) failed\n", failures);
    return 1;
  }
  std::puts("all platform tests passed");
  return 0;
}
