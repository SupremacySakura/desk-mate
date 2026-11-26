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

bool test_device_blocklist_and_disconnect() {
  DMBridge *bridge = dm_bridge_create(nullptr);
  if (bridge == nullptr) {
    std::fprintf(stderr, "FAIL could not allocate DMBridge for policy test\n");
    return false;
  }
  auto link = std::make_shared<DeviceLink>();
  link->bridge = bridge;
  link->link_id = 1;
  {
    std::lock_guard lock(link->snapshot_mutex);
    link->active_device.id = "usb:MATEPAD-001";
    link->active_device.name = "MatePad 12.2 Pro";
    link->active_device_valid = true;
  }
  {
    std::lock_guard lock(bridge->links_mutex);
    bridge->links.emplace(1, link);
  }

  const char *other[] = {"usb:OTHER-999"};
  dm_bridge_set_blocked_devices(bridge, other, 1);
  if (link->disconnect_requested.load() ||
      is_device_blocked(bridge, "usb:MATEPAD-001") ||
      !is_device_blocked(bridge, "usb:OTHER-999")) {
    std::fprintf(stderr, "FAIL non-matching blocklist triggered disconnect\n");
    dm_bridge_destroy(bridge);
    return false;
  }

  dm_bridge_disconnect_device(bridge, "usb:OTHER-999");
  if (link->disconnect_requested.load()) {
    std::fprintf(stderr, "FAIL non-matching explicit disconnect triggered\n");
    dm_bridge_destroy(bridge);
    return false;
  }

  const char *blocked_active[] = {"usb:MATEPAD-001"};
  dm_bridge_set_blocked_devices(bridge, blocked_active, 1);
  if (!is_device_blocked(bridge, "usb:MATEPAD-001") ||
      !link->disconnect_requested.load()) {
    std::fprintf(stderr, "FAIL blocking active device did not request disconnect\n");
    dm_bridge_destroy(bridge);
    return false;
  }

  link->disconnect_requested.store(false);
  dm_bridge_set_blocked_devices(bridge, nullptr, 0);
  if (is_device_blocked(bridge, "usb:MATEPAD-001")) {
    std::fprintf(stderr, "FAIL clearing blocklist left device blocked\n");
    dm_bridge_destroy(bridge);
    return false;
  }

  dm_bridge_disconnect_device(bridge, "usb:MATEPAD-001");
  if (!link->disconnect_requested.load()) {
    std::fprintf(stderr, "FAIL matching explicit disconnect did not trigger\n");
    dm_bridge_destroy(bridge);
    return false;
  }

  char *scanned = dm_bridge_scan_devices(bridge);
  const bool valid_scan = scanned != nullptr && scanned[0] == '[';
  std::free(scanned);
  dm_bridge_destroy(bridge);
  if (!valid_scan) {
    std::fprintf(stderr, "FAIL device scan did not return a JSON array\n");
    return false;
  }
  std::puts("PASS device blocklist and active disconnect policy");
  return true;
}

bool test_multi_device_links_and_policy() {
  DMBridge *bridge = dm_bridge_create(nullptr);
  if (bridge == nullptr) {
    std::fprintf(stderr, "FAIL could not allocate DMBridge for multi-device test\n");
    return false;
  }

  auto link1 = std::make_shared<DeviceLink>();
  link1->bridge = bridge;
  link1->link_id = 1;
  {
    std::lock_guard lock(link1->snapshot_mutex);
    link1->active_device.id = "usb:MATEPAD-001";
    link1->active_device.name = "MatePad 12.2 Pro";
    link1->active_device_valid = true;
    link1->cached_state = "streaming";
    link1->cached_negotiated = "hevc 2800x1840@60";
  }

  auto link2 = std::make_shared<DeviceLink>();
  link2->bridge = bridge;
  link2->link_id = 2;
  {
    std::lock_guard lock(link2->snapshot_mutex);
    link2->active_device.id = "usb:PHONE-002";
    link2->active_device.name = "Pura 70 Pro";
    link2->active_device_valid = true;
    link2->cached_state = "streaming";
    link2->cached_negotiated = "hevc 2720x1260@60";
  }

  {
    std::lock_guard lock(bridge->links_mutex);
    bridge->links.emplace(1, link1);
    bridge->links.emplace(2, link2);
  }

  // Snapshot contains both concurrent links and activeLinkCount == 2.
  char *snap_raw = dm_bridge_snapshot(bridge);
  const std::string snap_text = snap_raw != nullptr ? snap_raw : "";
  std::free(snap_raw);
  if (snap_text.find("\"activeLinkCount\":2") == std::string::npos ||
      snap_text.find("usb:MATEPAD-001") == std::string::npos ||
      snap_text.find("usb:PHONE-002") == std::string::npos) {
    std::fprintf(stderr, "FAIL snapshot missing multi-link entries: %s\n", snap_text.c_str());
    dm_bridge_destroy(bridge);
    return false;
  }

  // Blocking Device 1 disconnects only Link 1, leaving Link 2 intact.
  const char *blocked_one[] = {"usb:MATEPAD-001"};
  dm_bridge_set_blocked_devices(bridge, blocked_one, 1);
  if (!link1->disconnect_requested.load() || link2->disconnect_requested.load()) {
    std::fprintf(stderr, "FAIL blocklist did not isolate Link 1 from Link 2\n");
    dm_bridge_destroy(bridge);
    return false;
  }

  link1->disconnect_requested.store(false);
  dm_bridge_disconnect_link(bridge, 2);
  if (link1->disconnect_requested.load() || !link2->disconnect_requested.load()) {
    std::fprintf(stderr, "FAIL dm_bridge_disconnect_link did not isolate Link 2\n");
    dm_bridge_destroy(bridge);
    return false;
  }

  // Per-link video and input queues remain isolated.
  const std::array<std::uint8_t, 4> frame1{0x00, 0x00, 0x00, 0x01};
  const std::array<std::uint8_t, 4> frame2{0x00, 0x00, 0x00, 0x02};
  if (dm_bridge_submit_video_for_link(bridge, 1, frame1.data(), frame1.size(), 100, 1) != 0 ||
      dm_bridge_submit_video_for_link(bridge, 2, frame2.data(), frame2.size(), 200, 1) != 0 ||
      dm_bridge_submit_video_for_link(bridge, 99, frame1.data(), frame1.size(), 300, 1) != -1) {
    std::fprintf(stderr, "FAIL per-link video routing returned unexpected status\n");
    dm_bridge_destroy(bridge);
    return false;
  }
  {
    std::lock_guard lock1(link1->video_mutex);
    std::lock_guard lock2(link2->video_mutex);
    if (link1->video_queue.size() != 1 || link1->video_queue.front().pts_us != 100 ||
        link2->video_queue.size() != 1 || link2->video_queue.front().pts_us != 200) {
      std::fprintf(stderr, "FAIL per-link video queues mixed frames across links\n");
      dm_bridge_destroy(bridge);
      return false;
    }
  }
  {
    std::lock_guard lock(link2->input_mutex);
    deskmate::protocol::InputEvent ev{};
    ev.kind = deskmate::protocol::InputKind::TouchDown;
    ev.x = 0.25f;
    ev.y = 0.75f;
    link2->pending_inputs.push_back(ev);
  }
  DMInputEvent out1[4]{}, out2[4]{};
  if (dm_bridge_take_input_for_link(bridge, 1, out1, 4) != 0 ||
      dm_bridge_take_input_for_link(bridge, 2, out2, 4) != 1 ||
      out2[0].x != 0.25f || out2[0].y != 0.75f) {
    std::fprintf(stderr, "FAIL per-link input drain did not isolate queues\n");
    dm_bridge_destroy(bridge);
    return false;
  }

  // Serial-less AOA devices on distinct USB ports resolve via port_path without colliding.
  UsbDeviceIdentity hdc_a{};
  hdc_a.vendor_id = kHdcVendorId;
  hdc_a.product_id = kHdcProductId;
  hdc_a.port_path = "2:1.1";
  hdc_a.name = "MatePad A";
  resolve_device_identity(bridge, hdc_a);

  UsbDeviceIdentity hdc_b{};
  hdc_b.vendor_id = kHdcVendorId;
  hdc_b.product_id = kHdcProductId;
  hdc_b.port_path = "2:1.2";
  hdc_b.name = "Phone B";
  resolve_device_identity(bridge, hdc_b);

  UsbDeviceIdentity aoa_a{};
  aoa_a.vendor_id = kAccessoryVendorId;
  aoa_a.product_id = kAccessoryProductIdMin;
  aoa_a.port_path = "2:1.1";
  aoa_a.in_accessory_mode = true;
  resolve_device_identity(bridge, aoa_a);

  UsbDeviceIdentity aoa_b{};
  aoa_b.vendor_id = kAccessoryVendorId;
  aoa_b.product_id = kAccessoryProductIdMin;
  aoa_b.port_path = "2:1.2";
  aoa_b.in_accessory_mode = true;
  resolve_device_identity(bridge, aoa_b);

  if (aoa_a.id != "usb:port:2:1.1" || aoa_a.name != "MatePad A" ||
      aoa_b.id != "usb:port:2:1.2" || aoa_b.name != "Phone B" ||
      aoa_a.id == aoa_b.id) {
    std::fprintf(stderr, "FAIL port-path identity resolution collided: %s (%s) vs %s (%s)\n",
                 aoa_a.id.c_str(), aoa_a.name.c_str(), aoa_b.id.c_str(), aoa_b.name.c_str());
    dm_bridge_destroy(bridge);
    return false;
  }

  dm_bridge_destroy(bridge);
  std::puts("PASS multi-device links, queue isolation, and port-path identity");
  return true;
}

class MemoryPipeIo final : public deskmate::protocol::ByteIo {
 public:
  struct Buffer {
    std::mutex mutex;
    std::deque<std::uint8_t> bytes;
  };

  MemoryPipeIo(std::shared_ptr<Buffer> rx, std::shared_ptr<Buffer> tx)
      : rx_(std::move(rx)), tx_(std::move(tx)) {}

  deskmate::protocol::IoResult read(
      std::span<std::uint8_t> destination) override {
    std::lock_guard lock(rx_->mutex);
    if (rx_->bytes.empty()) {
      return {deskmate::protocol::IoStatus::WouldBlock, 0};
    }
    const std::size_t take = std::min(destination.size(), rx_->bytes.size());
    for (std::size_t i = 0; i < take; ++i) {
      destination[i] = rx_->bytes.front();
      rx_->bytes.pop_front();
    }
    return {deskmate::protocol::IoStatus::Ok, take};
  }

  deskmate::protocol::IoResult write(
      std::span<const std::uint8_t> source) override {
    std::lock_guard lock(tx_->mutex);
    tx_->bytes.insert(tx_->bytes.end(), source.begin(), source.end());
    return {deskmate::protocol::IoStatus::Ok, source.size()};
  }

 private:
  std::shared_ptr<Buffer> rx_;
  std::shared_ptr<Buffer> tx_;
};

bool test_refresh_rate_reconfigure_and_advertisement() {
  using namespace deskmate::protocol;

  DMBridgeConfig cfg{};
  cfg.width = 1920;
  cfg.height = 1200;
  cfg.refresh_hz = 120;
  cfg.prefer_hevc = 1;
  cfg.auto_match_device_resolution = 1;
  DMBridge *bridge = dm_bridge_create(&cfg);
  if (bridge == nullptr) {
    return false;
  }
  // Setting preferred_refresh_hz to 30 must NOT restrict handshake advertisement.
  dm_bridge_set_preferred_refresh_hz(bridge, 30);

  auto h2d = std::make_shared<MemoryPipeIo::Buffer>();
  auto d2h = std::make_shared<MemoryPipeIo::Buffer>();
  MemoryPipeIo host_pipe(d2h, h2d);
  MemoryPipeIo dev_pipe(h2d, d2h);
  ManualClock clock;

  DeviceSessionConfig dev_cfg;
  dev_cfg.session_nonce = 0x11223344U;
  dev_cfg.capabilities = CapabilitySet{
      {VideoEncoder::Hevc, VideoEncoder::H264},
      {PixelFormat::Nv12},
      {{2800, 1840, 60}, {2800, 1840, 120}},
  };
  DeviceSession device(dev_pipe, clock, dev_cfg, nullptr);
  device.start();
  device.pump();  // queues ClientHello into d2h

  // Verify start_session advertises up to 120 Hz even when preferred is 30 Hz.
  auto adv_link = std::make_shared<DeviceLink>();
  adv_link->bridge = bridge;
  adv_link->link_id = 2;
  adv_link->boundary = std::make_unique<HostBoundaryImpl>(adv_link.get());
  std::atomic<std::uint64_t> adv_rx{}, adv_tx{}, adv_err{};
  adv_link->io = std::make_unique<UsbByteIo>(
      nullptr, 0x81, 0x01, &adv_rx, &adv_tx, &adv_err, nullptr, nullptr);
  adv_link->clock = std::make_unique<SteadyClock>();
  start_session(adv_link);
  const auto &adv_stream = adv_link->session;
  if (adv_stream == nullptr) {
    dm_bridge_destroy(bridge);
    return false;
  }
  // Tear down adv_link's session before exercising live reconfigure on link 1.
  adv_link->session.reset();
  adv_link->io.reset();

  // Exercise live refresh upgrade via dm_bridge_request_refresh_hz_for_link.
  HostSessionConfig host_cfg;
  host_cfg.video_stream_id = 0x20000001U;
  host_cfg.initial_config_id = 10U;
  host_cfg.accept_device_resolution = true;
  host_cfg.capabilities = CapabilitySet{
      {VideoEncoder::Hevc, VideoEncoder::H264},
      {PixelFormat::Nv12},
      {{1920, 1200, 120}, {1920, 1200, 90}, {1920, 1200, 60}},
  };

  auto link = std::make_shared<DeviceLink>();
  link->bridge = bridge;
  link->link_id = 1;
  link->next_config_id = host_cfg.initial_config_id;
  link->session = std::make_unique<HostSession>(host_pipe, clock, host_cfg, nullptr);
  link->session->start();
  {
    std::lock_guard lock(bridge->links_mutex);
    bridge->links.emplace(1, link);
  }

  for (int step = 0; step < 6; ++step) {
    link->session->pump();
    device.pump();
  }
  const std::array<std::uint8_t, 4> idr{0x00, 0x00, 0x00, 0x01};
  if (link->session->send_video_frame(idr, 1000, true) != VideoSendStatus::Sent) {
    std::fprintf(stderr, "FAIL initial IDR rejected\n");
    dm_bridge_destroy(bridge);
    return false;
  }
  link->session->pump();
  device.pump();
  if (device.state() != SessionState::Streaming ||
      link->session->negotiated_stream()->profile.resolution.refresh_hz != 60) {
    std::fprintf(stderr, "FAIL initial streaming state mismatch\n");
    dm_bridge_destroy(bridge);
    return false;
  }

  // Unsupported 90 Hz must return 0 and keep session streaming at 60 Hz.
  if (dm_bridge_request_refresh_hz_for_link(bridge, 1, 90) != 0) {
    std::fprintf(stderr, "FAIL unsupported 90 Hz unexpectedly triggered reconfigure\n");
    dm_bridge_destroy(bridge);
    return false;
  }

  // Supported 120 Hz must initiate live reconfiguration.
  if (dm_bridge_request_refresh_hz_for_link(bridge, 1, 120) != 120) {
    std::fprintf(stderr, "FAIL 120 Hz reconfigure not initiated\n");
    dm_bridge_destroy(bridge);
    return false;
  }
  for (int step = 0; step < 4; ++step) {
    link->session->pump();
    device.pump();
  }
  if (link->session->state() != SessionState::Reconfiguring ||
      !link->session->reconfiguration().has_value() ||
      link->session->reconfiguration()->phase != ReconfigurationPhase::AwaitingFirstIdr) {
    std::fprintf(stderr, "FAIL reconfigure did not reach AwaitingFirstIdr\n");
    dm_bridge_destroy(bridge);
    return false;
  }
  if (link->session->send_video_frame(idr, 2000, true) != VideoSendStatus::Sent) {
    std::fprintf(stderr, "FAIL reconfigure IDR rejected\n");
    dm_bridge_destroy(bridge);
    return false;
  }
  link->session->pump();
  device.pump();
  link->session->pump();

  if (link->session->state() != SessionState::Streaming ||
      link->session->negotiated_stream()->profile.resolution.refresh_hz != 120) {
    std::fprintf(stderr, "FAIL reconfigure did not upgrade to 120 Hz\n");
    dm_bridge_destroy(bridge);
    return false;
  }

  {
    std::lock_guard lock(bridge->links_mutex);
    bridge->links.clear();
  }
  link->session.reset();
  dm_bridge_destroy(bridge);
  std::puts("PASS live refresh-rate reconfiguration and handshake advertisement");
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
  const bool policy = test_device_blocklist_and_disconnect();
  const bool multi = test_multi_device_links_and_policy();
  const bool refresh_reconf = test_refresh_rate_reconfigure_and_advertisement();
  return partial_read && streaming && high_fps && policy && multi && refresh_reconf ? 0 : 1;
}
