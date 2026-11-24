#include "../Sources/Bridge.cpp"

#include <array>

namespace {
constexpr std::array<std::uint8_t, 4> kPayload{1, 2, 3, 4};
unsigned transfer_count = 0;
bool correct_suffix = false;
deskmate::protocol::ManualClock *stream_clock = nullptr;
}  // namespace

extern "C" int LIBUSB_CALL libusb_bulk_transfer(
    libusb_device_handle *, unsigned char endpoint, unsigned char *data, int length,
    int *transferred, unsigned int timeout_ms) {
  if (stream_clock != nullptr) {
    if ((endpoint & LIBUSB_ENDPOINT_IN) != 0) {
      stream_clock->advance_ns(timeout_ms * 1'000'000ULL);
      *transferred = 0;
      return LIBUSB_ERROR_TIMEOUT;
    }
    stream_clock->advance_ns(750'000);
    *transferred = length;
    return LIBUSB_SUCCESS;
  }
  ++transfer_count;
  if (transfer_count == 1) {
    *transferred = 2;
    return LIBUSB_ERROR_TIMEOUT;
  }
  if (transfer_count == 2) {
    correct_suffix = length == 2 && data[0] == 3 && data[1] == 4;
    *transferred = 0;
    return LIBUSB_ERROR_TIMEOUT;
  }
  *transferred = length;
  return LIBUSB_SUCCESS;
}

bool test_steady_video_with_no_reverse_input() {
  using namespace deskmate::protocol;
  ManualClock clock;
  std::atomic<std::uint64_t> rx{}, tx{}, errors{};
  UsbByteIo io(nullptr, 0x81, 0x01, &rx, &tx, &errors, nullptr, nullptr);
  HostSendScheduler scheduler(io, clock);
  std::array<std::uint8_t, 4096> read_buffer{};
  constexpr std::uint32_t kFrames = 60;
  constexpr std::uint64_t kFrameIntervalNs = 1'000'000'000 / kFrames;
  constexpr std::size_t kFragmentsPerFrame = 7;
  constexpr std::size_t kFrameBytes = kFragmentsPerFrame * kVideoFragmentPayload;
  std::uint32_t submitted = 0;
  bool recovered = false;
  stream_clock = &clock;
  while (clock.now_ns() < 1'100'000'000ULL) {
    while (submitted < kFrames && clock.now_ns() >= submitted * kFrameIntervalNs) {
      VideoFrameToSend frame;
      frame.session_nonce = 1;
      frame.video_stream_id = 1;
      frame.seq = ++submitted;
      frame.flags = submitted == 1 ? kFlagKeyFrame : 0;
      frame.payload.resize(kFrameBytes);
      recovered |= scheduler.enqueue_video_frame(std::move(frame),
                                                 kVideoFragmentPayload) !=
          SchedulerQueueStatus::Queued;
    }
    (void)io.read(read_buffer);
    scheduler.pump();
    recovered |= scheduler.idr_request().has_value();
    clock.advance_ns(1'000'000);
  }
  stream_clock = nullptr;
  const auto full_stream_bytes =
      kFrames * (kFrameBytes + kFragmentsPerFrame * kWireHeaderSize);
  if (recovered || tx != full_stream_bytes) {
    std::fprintf(stderr, "FAIL idle USB IN polling starves 60 fps video: sent %llu of %zu bytes\n",
                 static_cast<unsigned long long>(tx.load()), full_stream_bytes);
    return false;
  }
  std::puts("PASS idle USB IN polling leaves time to send 60 fps video");
  return true;
}

bool test_sustained_120fps_video_with_keyframes_and_tuned_host_scheduler() {
  using namespace deskmate::protocol;
  ManualClock clock;
  std::atomic<std::uint64_t> rx{}, tx{}, errors{};
  UsbByteIo io(nullptr, 0x81, 0x01, &rx, &tx, &errors, nullptr, nullptr);
  SendSchedulerConfig config;
  config.max_video_urbs = kHostSchedulerMaxVideoUrbs;
  config.max_pending_video_bytes = kHostSchedulerMaxPendingVideoBytes;
  config.max_oldest_unsent_frame_age_ns = kHostSchedulerMaxUnsentFrameAgeNs;
  HostSendScheduler scheduler(io, clock, config);
  constexpr std::uint32_t kFrames = 120;
  constexpr std::uint64_t kFrameIntervalNs = 1'000'000'000 / kFrames;
  constexpr std::size_t kKeyframeFragments = 24;  // 288 KiB keyframe
  constexpr std::size_t kPFrameFragments = 4;     // 48 KiB P-frame
  std::uint32_t submitted = 0;
  std::size_t expected_bytes = 0;
  bool recovered = false;
  stream_clock = &clock;
  // Simulate full-duplex TX (IN runs on its own thread, so TX pumps directly
  // without sleeping while scheduler has pending output).
  while (clock.now_ns() < 1'100'000'000ULL) {
    while (submitted < kFrames && clock.now_ns() >= submitted * kFrameIntervalNs) {
      const bool keyframe = (submitted == 0);
      const std::size_t frags = keyframe ? kKeyframeFragments : kPFrameFragments;
      const std::size_t payload_bytes = frags * kVideoFragmentPayload;
      VideoFrameToSend frame;
      frame.session_nonce = 1;
      frame.video_stream_id = 1;
      frame.seq = ++submitted;
      frame.flags = keyframe ? kFlagKeyFrame : 0;
      frame.payload.resize(payload_bytes);
      expected_bytes += payload_bytes + frags * kWireHeaderSize;
      recovered |= scheduler.enqueue_video_frame(std::move(frame),
                                                 kVideoFragmentPayload) !=
          SchedulerQueueStatus::Queued;
    }
    scheduler.pump();
    recovered |= scheduler.idr_request().has_value();
    if (scheduler.idle()) {
      clock.advance_ns(1'000'000);
    }
  }
  stream_clock = nullptr;
  if (recovered || tx != expected_bytes) {
    std::fprintf(stderr,
                 "FAIL tuned host scheduler could not sustain 120 fps: recovered=%d sent %llu of %zu bytes\n",
                 recovered ? 1 : 0, static_cast<unsigned long long>(tx.load()),
                 expected_bytes);
    return false;
  }
  std::puts("PASS tuned host scheduler sustains 120 fps with 288 KiB keyframes");
  return true;
}

bool test_partial_read_on_timeout() {
  using deskmate::protocol::IoStatus;
  transfer_count = 0;
  std::atomic<std::uint64_t> rx{}, tx{}, errors{};
  UsbByteIo io(nullptr, 0x81, 0x01, &rx, &tx, &errors, nullptr, nullptr);
  auto buffer = kPayload;
  const auto partial = io.read(buffer);
  if (partial.status != IoStatus::Ok || partial.bytes != 2 || rx != 2) {
    std::fprintf(stderr, "FAIL timeout discarded partial USB read progress\n");
    return false;
  }
  const auto empty = io.read(std::span(buffer).subspan(2));
  if (empty.status != IoStatus::WouldBlock || empty.bytes != 0 || rx != 2 ||
      errors != 0) {
    std::fprintf(stderr, "FAIL empty USB read timeout or byte accounting\n");
    return false;
  }
  std::puts("PASS partial USB read progress survives timeout");
  return true;
}

int main() {
  using deskmate::protocol::IoStatus;
  std::atomic<std::uint64_t> rx{}, tx{}, errors{};
  std::uint64_t timeouts = 0, timeout_bytes = 0;
  UsbByteIo io(nullptr, 0x81, 0x01, &rx, &tx, &errors, &timeouts,
               &timeout_bytes);
  const auto partial = io.write(kPayload);
  if (partial.status != IoStatus::Ok || partial.bytes != 2 || tx != 2) {
    std::fprintf(stderr, "FAIL timeout discarded partial USB write progress\n");
    return 1;
  }
  const auto suffix = std::span(kPayload).subspan(partial.bytes);
  const auto blocked = io.write(suffix);
  const auto complete = io.write(suffix);
  if (!correct_suffix || blocked.status != IoStatus::WouldBlock ||
      blocked.bytes != 0 || complete.status != IoStatus::Ok ||
      complete.bytes != 2 || tx != kPayload.size() || timeouts != 2 ||
      timeout_bytes != 2 || errors != 0) {
    std::fprintf(stderr, "FAIL USB retry or byte accounting\n");
    return 1;
  }
  std::puts("PASS partial USB write progress survives timeout");
  const bool partial_read = test_partial_read_on_timeout();
  const bool streaming = test_steady_video_with_no_reverse_input();
  const bool high_fps = test_sustained_120fps_video_with_keyframes_and_tuned_host_scheduler();
  return partial_read && streaming && high_fps ? 0 : 1;
}
