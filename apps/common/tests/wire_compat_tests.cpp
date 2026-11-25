// Cross-platform wire compatibility: a Windows-style host session and an
// Android-style device session are joined by an in-memory USB pipe. The test
// pins the handshake, 12 KiB video fragmentation, multi-touch input and
// recovery behavior without touching any platform graphics stack.

#include "deskmate/byte_io.hpp"
#include "deskmate/clock.hpp"
#include "deskmate/session.hpp"
#include "deskmate/platform/session_profile.hpp"
#include "deskmate/platform/input_frames.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <memory>
#include <optional>
#include <span>
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

class BytePipe {
 public:
  deskmate::protocol::IoResult read(std::span<std::uint8_t> destination) {
    if (offset_ == bytes_.size()) {
      return {deskmate::protocol::IoStatus::WouldBlock, 0};
    }
    const std::size_t available = bytes_.size() - offset_;
    const std::size_t take = std::min(destination.size(), available);
    std::copy_n(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_), take,
                destination.begin());
    offset_ += take;
    return {deskmate::protocol::IoStatus::Ok, take};
  }

  deskmate::protocol::IoResult write(
      std::span<const std::uint8_t> source) {
    bytes_.insert(bytes_.end(), source.begin(), source.end());
    return {deskmate::protocol::IoStatus::Ok, source.size()};
  }

  void clear() {
    bytes_.clear();
    offset_ = 0;
  }

  [[nodiscard]] const std::vector<std::uint8_t>& bytes() const { return bytes_; }

 private:
  std::vector<std::uint8_t> bytes_;
  std::size_t offset_{};
};

class DirectedIo final : public deskmate::protocol::ByteIo {
 public:
  DirectedIo(BytePipe& inbound, BytePipe& outbound)
      : inbound_(inbound), outbound_(outbound) {}

  deskmate::protocol::IoResult read(
      std::span<std::uint8_t> destination) override {
    return inbound_.read(destination);
  }
  deskmate::protocol::IoResult write(
      std::span<const std::uint8_t> source) override {
    return outbound_.write(source);
  }

 private:
  BytePipe& inbound_;
  BytePipe& outbound_;
};

class DeviceBoundary final : public deskmate::protocol::DeviceBoundary {
 public:
  deskmate::protocol::BoundaryResult prepare_stream(
      const deskmate::protocol::StreamProfile&, std::uint32_t) override {
    return deskmate::protocol::BoundaryResult::Ok;
  }
  deskmate::protocol::BoundaryResult commit_stream(
      const deskmate::protocol::StreamProfile&, std::uint32_t,
      std::uint32_t) override {
    return deskmate::protocol::BoundaryResult::Ok;
  }
};

struct Pair {
  Pair()
      : host_io(device_to_host, host_to_device),
        device_io(host_to_device, device_to_host) {}

  BytePipe host_to_device;
  BytePipe device_to_host;
  DirectedIo host_io;
  DirectedIo device_io;
  DeviceBoundary boundary;
  std::unique_ptr<deskmate::protocol::HostSession> host;
  std::unique_ptr<deskmate::protocol::DeviceSession> device;

  void start(std::uint16_t width, std::uint16_t height, std::uint16_t hz) {
    using namespace deskmate::platform;
    host = std::make_unique<deskmate::protocol::HostSession>(
        host_io, clock, make_host_session_config(0xA1B2C3D4U, 1U, width,
                                                 height, hz, true, false));
    device = std::make_unique<deskmate::protocol::DeviceSession>(
        device_io, clock, make_device_session_config(0x01020304U, width,
                                                     height, hz, true),
        &boundary);
    host->start();
    device->start();
  }

  void pump(int rounds = 64) {
    for (int i = 0; i < rounds; ++i) {
      host->pump();
      device->pump();
    }
  }

  deskmate::protocol::ManualClock clock;
};

void test_windows_host_android_client_handshake() {
  using namespace deskmate::protocol;
  Pair pair;
  pair.start(1920, 1200, 60);
  pair.pump(256);
  CHECK(pair.host->state() == SessionState::Streaming);
  CHECK(pair.device->state() == SessionState::AwaitingFirstIdr);
  CHECK(pair.host->negotiated_stream().has_value());
  CHECK(pair.device->negotiated_stream().has_value());
  CHECK(pair.host->negotiated_stream()->profile.encoder ==
        pair.device->negotiated_stream()->profile.encoder);
  CHECK(pair.host->negotiated_stream()->profile.resolution.width == 1920);
}

void test_12k_video_fragments_reassemble_on_android() {
  using namespace deskmate::protocol;
  Pair pair;
  pair.start(2800, 1840, 120);
  pair.pump(256);
  CHECK(pair.host->state() == SessionState::Streaming);
  CHECK(pair.device->state() == SessionState::AwaitingFirstIdr);

  std::vector<std::uint8_t> payload(100U * 1024U);
  for (std::size_t i = 0; i < payload.size(); ++i) {
    payload[i] = static_cast<std::uint8_t>(i * 31U);
  }
  CHECK(pair.host->send_video_frame(payload, 12345U, true) ==
        VideoSendStatus::Sent);
  pair.pump(512);
  auto frames = pair.device->take_video_frames();
  CHECK(frames.size() == 1U);
  CHECK(frames[0].payload == payload);
  CHECK((frames[0].header.flags & kFlagKeyFrame) != 0U);
  pair.pump(64);
  CHECK(pair.device->state() == SessionState::Streaming);

  // A frame exactly at the 12 KiB fragment boundary still fits the 64-fragment
  // wire limit and must not be rejected by either side.
  std::vector<std::uint8_t> large(12U * 1024U * 60U, 0x5A);
  CHECK(pair.host->send_video_frame(large, 13000U, false) ==
        VideoSendStatus::Sent);
  pair.pump(2048);
  frames = pair.device->take_video_frames();
  CHECK(frames.size() == 1U);
  CHECK(frames[0].payload.size() == large.size());
}

void test_android_multitouch_reaches_windows_host() {
  using namespace deskmate::protocol;
  using deskmate::platform::TouchSample;
  using deskmate::platform::TouchTracker;

  Pair pair;
  pair.start(1920, 1200, 60);
  pair.pump(256);
  CHECK(pair.host->state() == SessionState::Streaming);
  const std::array<std::uint8_t, 8> keyframe{0, 0, 0, 1, 0x65, 1, 2, 3};
  CHECK(pair.host->send_video_frame(keyframe, 1U, true) ==
        VideoSendStatus::Sent);
  pair.pump(128);
  CHECK(pair.device->state() == SessionState::Streaming);

  TouchTracker tracker;
  tracker.set_viewport(1920, 1200);
  const std::array<TouchSample, 2> samples{{
      {4, 480.0F, 300.0F, true},
      {9, 1440.0F, 900.0F, true},
  }};
  for (const auto& event : tracker.update(samples)) {
    CHECK(pair.device->send_input(event) == SessionCommandStatus::Ok);
  }
  pair.pump(64);
  const auto received = pair.host->take_input_events();
  CHECK(received.size() == 2U);
  CHECK(received[0].kind == InputKind::TouchDown);
  CHECK(received[1].kind == InputKind::TouchDown);
  CHECK((received[1].flags & kInputFlagFrameCommit) != 0U);
  CHECK(((received[1].flags & kInputFlagContactCountMask) >>
         kInputFlagContactCountShift) == 2U);
  CHECK(received[0].x > 0.2F && received[0].x < 0.3F);
  CHECK(received[1].y > 0.7F && received[1].y < 0.8F);
}

void test_host_and_device_capability_intersection() {
  using namespace deskmate::protocol;
  const auto host = deskmate::platform::make_host_capabilities(1920, 1200, 60,
                                                              true);
  const auto device = deskmate::platform::make_device_capabilities(
      2800, 1840, 120, true);
  CHECK(!host.resolutions.empty());
  CHECK(!device.resolutions.empty());
  CHECK(std::find(device.resolutions.begin(), device.resolutions.end(),
                  DisplayResolution{1920, 1200, 60}) != device.resolutions.end());
}

// The Windows host encodes at the negotiated profile, and the Android decoder
// configures MediaCodec from the same profile. They must agree exactly, or the
// bitstream is rejected by the device codec.
void test_negotiated_profile_matches_both_ends() {
  using namespace deskmate::protocol;
  Pair pair;
  pair.start(2800, 1840, 120);
  pair.pump(256);
  CHECK(pair.host->state() == SessionState::Streaming);
  CHECK(pair.device->negotiated_stream().has_value());
  CHECK(pair.host->negotiated_stream().has_value());
  const auto host_profile = pair.host->negotiated_stream()->profile;
  const auto device_profile = pair.device->negotiated_stream()->profile;
  CHECK(host_profile == device_profile);
  CHECK(host_profile.pixel_format == PixelFormat::Nv12);
  CHECK(host_profile.resolution.width > 0);
  CHECK(host_profile.resolution.height > 0);
  CHECK(host_profile.resolution.refresh_hz > 0);
}

// A frame just under the 64-fragment limit must reassemble, and one fragment
// past it must be rejected rather than silently truncated.
void test_video_fragment_limit_is_enforced() {
  using namespace deskmate::protocol;
  const std::size_t fragment = deskmate::platform::kVideoFragmentPayload;
  CHECK(fragment == 12U * 1024U);
  const std::size_t max_fragments = kMaxVideoFragments;

  VideoFrameToSend frame;
  frame.session_nonce = 0x11223344U;
  frame.video_stream_id = 7U;
  frame.seq = 1U;
  frame.payload.assign(fragment * max_fragments, 0x33);
  const auto at_limit = encode_video_fragments(frame, fragment);
  CHECK(at_limit.has_value());
  CHECK(at_limit.value().size() == max_fragments);
  const auto header = decode_header(at_limit.value().front());
  CHECK(header.has_value());
  CHECK(header.value().payload_len == fragment);
  CHECK(header.value().fragment_total == max_fragments);

  frame.payload.push_back(0x44);
  const auto over_limit = encode_video_fragments(frame, fragment);
  CHECK(!over_limit.has_value());
}

}  // namespace

int main() {
  test_windows_host_android_client_handshake();
  test_12k_video_fragments_reassemble_on_android();
  test_android_multitouch_reaches_windows_host();
  test_host_and_device_capability_intersection();
  test_negotiated_profile_matches_both_ends();
  test_video_fragment_limit_is_enforced();
  if (failures != 0) {
    std::fprintf(stderr, "%d wire compatibility test(s) failed\n", failures);
    return 1;
  }
  std::puts("all wire compatibility tests passed");
  return 0;
}
