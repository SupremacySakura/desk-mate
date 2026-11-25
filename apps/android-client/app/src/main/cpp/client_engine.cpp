#include "client_engine.hpp"

#include "deskmate/clock.hpp"
#include "deskmate/platform/session_profile.hpp"

#include <android/log.h>
#include <poll.h>
#include <pthread.h>
#include <sys/resource.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <random>

#define LOG_TAG "DeskMateClient"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace deskmate::android {
namespace {

constexpr int kMaxSessionRebuilds = 5;

std::uint32_t random_nonzero_u32() {
  std::random_device device;
  std::uint32_t value = 0;
  while (value == 0) {
    value = (static_cast<std::uint32_t>(device()) << 16U) ^ device();
  }
  return value;
}

std::string normalize_codec_preference(const std::string& codec) {
  if (codec == "hevc" || codec == "h264") {
    return codec;
  }
  return "auto";
}

}  // namespace

class ClientEngine::Boundary final : public deskmate::protocol::DeviceBoundary {
 public:
  explicit Boundary(ClientEngine& owner) : owner_(owner) {}

  deskmate::protocol::BoundaryResult prepare_stream(
      const deskmate::protocol::StreamProfile& profile,
      std::uint32_t) override {
    if (profile.pixel_format != deskmate::protocol::PixelFormat::Nv12 ||
        profile.resolution.width == 0 || profile.resolution.height == 0) {
      return deskmate::protocol::BoundaryResult::Failed;
    }
    return deskmate::protocol::BoundaryResult::Ok;
  }

  deskmate::protocol::BoundaryResult commit_stream(
      const deskmate::protocol::StreamProfile& profile, std::uint32_t,
      std::uint32_t) override {
    const std::string codec_label =
        profile.encoder == deskmate::protocol::VideoEncoder::Hevc ? "HEVC"
                                                                  : "H.264";
    const std::string res_label =
        std::to_string(profile.resolution.width) + "x" +
        std::to_string(profile.resolution.height) + "@" +
        std::to_string(profile.resolution.refresh_hz);
    const std::string prof_label =
        (profile.encoder == deskmate::protocol::VideoEncoder::Hevc ? "hevc "
                                                                   : "h264 ") +
        res_label;
    {
      std::lock_guard lock(owner_.mutex_);
      owner_.profile_ = prof_label;
      owner_.active_codec_ = codec_label;
      owner_.active_resolution_ = res_label;
      owner_.decoder_config_ = profile;
    }
    owner_.decoder_dirty_.store(true, std::memory_order_release);
    return deskmate::protocol::BoundaryResult::Ok;
  }

  void release_stream(std::uint32_t, std::uint32_t) override {
    owner_.decoder_release_.store(true, std::memory_order_release);
  }

 private:
  ClientEngine& owner_;
};

ClientEngine::ClientEngine()
    : boundary_(std::make_unique<Boundary>(*this)),
      decoder_counters_(std::make_shared<VideoDecoder::Counters>()) {}

ClientEngine::~ClientEngine() { stop(); }

void ClientEngine::wake_worker() {
  const int fd = wake_fd_.load(std::memory_order_acquire);
  if (fd >= 0) {
    const std::uint64_t value = 1;
    const ssize_t written = ::write(fd, &value, sizeof(value));
    (void)written;
  }
}

bool ClientEngine::start(int accessory_fd, ANativeWindow* window, int width,
                         int height, int refresh_hz, std::string& error) {
  if (accessory_fd <= 0) {
    error = "invalid accessory descriptor";
    return false;
  }
  if (running_.load(std::memory_order_acquire)) {
    return true;
  }
  if (worker_.joinable()) {
    worker_.join();
  }
  accessory_fd_ = accessory_fd;
  if (window != nullptr) {
    std::lock_guard lock(surface_mutex_);
    if (pending_window_ != nullptr) {
      ANativeWindow_release(pending_window_);
    }
    pending_window_ = window;
  }
  const int safe_w = width > 0 ? width : 1920;
  const int safe_h = height > 0 ? height : 1200;
  width_ = std::max(640, std::max(safe_w, safe_h) & ~1);
  height_ = std::max(480, std::min(safe_w, safe_h) & ~1);
  refresh_hz_ = refresh_hz > 0 ? refresh_hz : 60;
  {
    std::lock_guard lock(input_mutex_);
    touches_.set_viewport(static_cast<std::uint32_t>(safe_w),
                          static_cast<std::uint32_t>(safe_h));
    input_queue_.clear();
  }
  has_pending_input_.store(false, std::memory_order_release);
  surface_attached_.store(false, std::memory_order_release);
  frames_.store(0, std::memory_order_relaxed);
  frames_received_.store(0, std::memory_order_relaxed);
  frames_dropped_.store(0, std::memory_order_relaxed);
  decoder_opens_.store(0, std::memory_order_relaxed);
  input_events_.store(0, std::memory_order_relaxed);
  input_rejected_.store(0, std::memory_order_relaxed);
  rx_bytes_.store(0, std::memory_order_relaxed);
  tx_bytes_.store(0, std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    decoder_counters_ = std::make_shared<VideoDecoder::Counters>();
    failure_.clear();
    last_error_.clear();
    profile_ = "none";
    active_codec_.clear();
    active_resolution_.clear();
    decoder_config_.reset();
  }
  stop_requested_.store(false, std::memory_order_release);
  running_.store(true, std::memory_order_release);
  worker_ = std::thread([this] { worker_main(); });
  return true;
}

void ClientEngine::request_stop() {
  stop_requested_.store(true, std::memory_order_release);
  {
    std::lock_guard lock(io_mutex_);
    if (io_ != nullptr) {
      io_->cancel();
    }
  }
  wake_worker();
}

void ClientEngine::stop() {
  request_stop();
  if (worker_.joinable()) {
    worker_.join();
  }
  running_.store(false, std::memory_order_release);
  if (decoder_ != nullptr) {
    decoder_->close();
    decoder_.reset();
  }
  decoder_open_profile_.reset();
  surface_attached_.store(false, std::memory_order_release);
  {
    // The worker is joined, so the active window can be released safely now.
    std::lock_guard lock(surface_mutex_);
    if (active_window_ != nullptr) {
      ANativeWindow_release(active_window_);
      active_window_ = nullptr;
    }
    if (pending_window_ != nullptr) {
      ANativeWindow_release(pending_window_);
      pending_window_ = nullptr;
    }
  }
  session_.reset();
  wake_fd_.store(-1, std::memory_order_release);
  {
    std::lock_guard lock(io_mutex_);
    io_.reset();
  }
  {
    std::lock_guard lock(mutex_);
    state_ = "idle";
  }
}

void ClientEngine::set_surface(ANativeWindow* window) {
  ANativeWindow* previous = nullptr;
  {
    std::lock_guard lock(surface_mutex_);
    previous = pending_window_;
    pending_window_ = window;
  }
  // Java transferred a reference to us; drop the superseded one only after the
  // swap so the decoder never sees a window that is already released.
  if (previous != nullptr) {
    ANativeWindow_release(previous);
  }
  if (window == nullptr) {
    surface_detached_.store(true, std::memory_order_release);
  } else {
    surface_changed_.store(true, std::memory_order_release);
  }
  decoder_dirty_.store(true, std::memory_order_release);
  wake_worker();
}

void ClientEngine::set_viewport(int width, int height) {
  if (width <= 0 || height <= 0) {
    return;
  }
  std::lock_guard lock(input_mutex_);
  touches_.set_viewport(static_cast<std::uint32_t>(width),
                        static_cast<std::uint32_t>(height));
}

void ClientEngine::set_preferred_codec(const std::string& codec) {
  std::lock_guard lock(mutex_);
  preferred_codec_ = normalize_codec_preference(codec);
}

void ClientEngine::set_touch_enabled(bool enabled) {
  const bool was_enabled =
      touch_enabled_.exchange(enabled, std::memory_order_acq_rel);
  if (was_enabled && !enabled) {
    touch_cancel();
  }
}

void ClientEngine::touch_snapshot(
    std::span<const deskmate::platform::TouchSample> samples, bool cancel) {
  if (!touch_enabled_.load(std::memory_order_acquire) && !cancel) {
    return;
  }
  bool queued = false;
  {
    std::lock_guard lock(input_mutex_);
    for (auto& event : touches_.update(samples, cancel)) {
      input_queue_.push_back(event);
      queued = true;
    }
  }
  if (queued) {
    has_pending_input_.store(true, std::memory_order_release);
    wake_worker();
  }
}

void ClientEngine::touch_down(int pointer_id, float x, float y) {
  const std::array<deskmate::platform::TouchSample, 1> sample{{
      {pointer_id, x, y, true},
  }};
  touch_snapshot(sample, false);
}

void ClientEngine::touch_move(int pointer_id, float x, float y) {
  touch_down(pointer_id, x, y);
}

void ClientEngine::touch_up(int pointer_id, float x, float y) {
  const std::array<deskmate::platform::TouchSample, 1> sample{{
      {pointer_id, x, y, false},
  }};
  touch_snapshot(sample, false);
}

void ClientEngine::touch_cancel() {
  bool queued = false;
  {
    std::lock_guard lock(input_mutex_);
    for (auto& event : touches_.release_all(true)) {
      input_queue_.push_back(event);
      queued = true;
    }
  }
  if (queued) {
    has_pending_input_.store(true, std::memory_order_release);
    wake_worker();
  }
}

void ClientEngine::flush_input() {
  std::deque<deskmate::protocol::InputEvent> pending;
  {
    std::lock_guard lock(input_mutex_);
    pending.swap(input_queue_);
  }
  if (pending.empty() || session_ == nullptr) {
    return;
  }
  std::uint64_t sent = 0;
  std::uint64_t rejected = 0;
  for (const auto& event : pending) {
    if (session_->send_input(event) ==
        deskmate::protocol::SessionCommandStatus::Ok) {
      ++sent;
    } else {
      ++rejected;
    }
  }
  if (sent > 0) {
    input_events_.fetch_add(sent, std::memory_order_relaxed);
  }
  if (rejected > 0) {
    input_rejected_.fetch_add(rejected, std::memory_order_relaxed);
  }
}

bool ClientEngine::ensure_decoder() {
  std::optional<deskmate::protocol::StreamProfile> profile;
  std::shared_ptr<VideoDecoder::Counters> counters;
  {
    std::lock_guard lock(mutex_);
    profile = decoder_config_;
    counters = decoder_counters_;
  }
  if (surface_detached_.exchange(false, std::memory_order_acq_rel)) {
    if (decoder_ != nullptr) {
      decoder_->close();
      decoder_.reset();
    }
    decoder_open_profile_.reset();
    surface_attached_.store(false, std::memory_order_release);
    ANativeWindow* retired = nullptr;
    {
      std::lock_guard lock(surface_mutex_);
      retired = active_window_;
      active_window_ = nullptr;
    }
    if (retired != nullptr) {
      ANativeWindow_release(retired);
    }
  }
  ANativeWindow* retired = nullptr;
  ANativeWindow* redundant = nullptr;
  ANativeWindow* window = nullptr;
  (void)surface_changed_.exchange(false, std::memory_order_acq_rel);
  {
    std::lock_guard lock(surface_mutex_);
    // Adopt a newly supplied Surface on the worker thread. If it points to
    // the already-active ANativeWindow, drop the duplicate reference instead
    // of tearing down MediaCodec.
    if (pending_window_ != nullptr) {
      if (pending_window_ == active_window_) {
        redundant = pending_window_;
        pending_window_ = nullptr;
      } else {
        retired = active_window_;
        active_window_ = pending_window_;
        pending_window_ = nullptr;
      }
    }
    window = active_window_;
  }
  if (redundant != nullptr) {
    ANativeWindow_release(redundant);
  }
  if (retired != nullptr) {
    if (decoder_ != nullptr) {
      decoder_->close();
      decoder_.reset();
    }
    decoder_open_profile_.reset();
    surface_attached_.store(false, std::memory_order_release);
    ANativeWindow_release(retired);
  }
  if (!profile.has_value() || window == nullptr) {
    surface_attached_.store(false, std::memory_order_release);
    return false;
  }
  // A renegotiation can change codec or resolution; a decoder built for the old
  // profile must be torn down instead of silently decoding the new bitstream.
  if (decoder_ != nullptr && decoder_->open() &&
      decoder_open_profile_ == profile) {
    surface_attached_.store(true, std::memory_order_release);
    return true;
  }
  if (decoder_ != nullptr) {
    decoder_->close();
    decoder_.reset();
    decoder_open_profile_.reset();
    surface_attached_.store(false, std::memory_order_release);
  }
  DecoderConfig config;
  config.mime = profile->encoder == deskmate::protocol::VideoEncoder::Hevc
                    ? "video/hevc"
                    : "video/avc";
  config.width = profile->resolution.width;
  config.height = profile->resolution.height;
  config.frame_rate = profile->resolution.refresh_hz;
  decoder_ = std::make_unique<VideoDecoder>();
  std::string error;
  if (!decoder_->open(config, window, error, std::move(counters))) {
    LOGE("decoder open failed: %s", error.c_str());
    {
      std::lock_guard lock(mutex_);
      last_error_ = error;
    }
    decoder_.reset();
    surface_attached_.store(false, std::memory_order_release);
    return false;
  }
  decoder_opens_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard lock(mutex_);
    last_error_.clear();
  }
  decoder_open_profile_ = profile;
  surface_attached_.store(true, std::memory_order_release);
  return true;
}

void ClientEngine::drain_video() {
  if (session_ == nullptr) {
    return;
  }
  auto frames = session_->take_video_frames();
  if (frames.empty()) {
    return;
  }
  const bool paused =
      session_->state() == deskmate::protocol::SessionState::Paused;
  std::uint64_t received = 0;
  std::uint64_t pre_dropped = 0;
  std::uint64_t pushed = 0;

  for (auto& frame : frames) {
    ++received;
    if (paused || decoder_ == nullptr) {
      ++pre_dropped;
      continue;
    }
    const bool keyframe =
        (frame.header.flags & deskmate::protocol::kFlagKeyFrame) != 0;
    // VideoDecoder::push counts decoder-side drops in its own counters.
    if (decoder_->push(std::move(frame.payload),
                       static_cast<std::int64_t>(frame.header.timestamp_us),
                       keyframe)) {
      ++pushed;
    }
  }
  frames_received_.fetch_add(received, std::memory_order_relaxed);
  if (pre_dropped > 0) {
    frames_dropped_.fetch_add(pre_dropped, std::memory_order_relaxed);
  }
  if (pushed > 0) {
    frames_.fetch_add(pushed, std::memory_order_relaxed);
  }
}

void ClientEngine::pump_session_step() {
  if (has_pending_input_.exchange(false, std::memory_order_acq_rel)) {
    flush_input();
  }
  session_->pump();
  if (decoder_release_.exchange(false, std::memory_order_acq_rel)) {
    if (decoder_ != nullptr) {
      decoder_->close();
      decoder_.reset();
      decoder_open_profile_.reset();
      surface_attached_.store(false, std::memory_order_release);
    }
  }
  if (decoder_dirty_.exchange(false, std::memory_order_acq_rel)) {
    (void)ensure_decoder();
  }
  drain_video();
}

void ClientEngine::worker_main() {
  (void)pthread_setname_np(pthread_self(), "dm-usb-worker");
  (void)setpriority(PRIO_PROCESS, 0, -8);

  {
    std::lock_guard lock(io_mutex_);
    io_ = std::make_unique<AccessoryIo>(accessory_fd_);
  }
  wake_fd_.store(io_->wake_fd(), std::memory_order_release);
  deskmate::protocol::SteadyClock clock;
  int rebuilds = 0;

  while (!stop_requested_.load(std::memory_order_acquire)) {
    std::string codec_pref;
    {
      std::lock_guard lock(mutex_);
      codec_pref = preferred_codec_;
      decoder_config_.reset();
    }
    const bool prefer_hevc = (codec_pref != "h264");
    auto config = deskmate::platform::make_device_session_config(
        random_nonzero_u32(), static_cast<std::uint16_t>(width_),
        static_cast<std::uint16_t>(height_),
        static_cast<std::uint16_t>(refresh_hz_), prefer_hevc);
    if (codec_pref == "hevc") {
      config.capabilities.encoders = {deskmate::protocol::VideoEncoder::Hevc};
    }

    session_ = std::make_unique<deskmate::protocol::DeviceSession>(
        *io_, clock, std::move(config), boundary_.get());
    session_->start();
    deskmate::protocol::SessionState last_state = session_->state();
    {
      std::lock_guard lock(mutex_);
      state_ = std::string(deskmate::protocol::session_state_name(last_state));
      failure_.clear();
    }
    LOGI("session state -> %s",
         deskmate::protocol::session_state_name(last_state).data());
    (void)ensure_decoder();

    bool rebuild = false;
    bool is_io_failure = false;

    while (!stop_requested_.load(std::memory_order_acquire)) {
      pollfd fds[2] = {
          {accessory_fd_, POLLIN, 0},
          {io_->wake_fd(), POLLIN, 0},
      };
      const int ready = ::poll(fds, 2, 16);
      if (ready < 0 && errno != EINTR) {
        is_io_failure = true;
        break;
      }
      if ((fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
        io_->note_poll(fds[0].revents);
        is_io_failure = true;
        break;
      }
      io_->note_poll(fds[0].revents);
      if ((fds[1].revents & POLLIN) != 0) {
        std::uint64_t value = 0;
        while (::read(fds[1].fd, &value, sizeof(value)) > 0) {
        }
      }

      // Single pass per poll(): /dev/usb_accessory has no .poll handler and
      // always reports POLLIN, so zero-timeout burst polling would call read()
      // synchronously with no message staged and block inside the kernel.
      pump_session_step();

      const auto io_stats = io_->stats();
      rx_bytes_.store(io_stats.bytes_read, std::memory_order_relaxed);
      tx_bytes_.store(io_stats.bytes_written, std::memory_order_relaxed);

      const auto current_state = session_->state();
      if (current_state == deskmate::protocol::SessionState::Streaming) {
        rebuilds = 0;
      }
      const bool terminal = session_->is_terminal();
      const std::string dec_error =
          decoder_ != nullptr ? decoder_->last_error() : std::string{};
      if (current_state != last_state) {
        LOGI("session state -> %s failure=%s",
             deskmate::protocol::session_state_name(current_state).data(),
             session_->failure().has_value()
                 ? deskmate::protocol::session_failure_name(
                       *session_->failure())
                       .data()
                 : "none");
      }
      if (terminal || current_state != last_state || !dec_error.empty()) {
        last_state = current_state;
        std::lock_guard lock(mutex_);
        state_ = std::string(
            deskmate::protocol::session_state_name(current_state));
        if (session_->failure().has_value()) {
          failure_ = std::string(deskmate::protocol::session_failure_name(
              *session_->failure()));
        }
        if (!dec_error.empty()) {
          last_error_ = dec_error;
        } else if (current_state ==
                   deskmate::protocol::SessionState::Streaming) {
          last_error_.clear();
        }
      }
      if (terminal) {
        const auto failure = session_->failure();
        is_io_failure =
            !failure.has_value() ||
            *failure == deskmate::protocol::SessionFailure::IoError;
        rebuild = !io_->hung_up() && !stop_requested_.load();
        break;
      }
    }

    if (decoder_ != nullptr) {
      decoder_->close();
      decoder_.reset();
      decoder_open_profile_.reset();
    }
    surface_attached_.store(false, std::memory_order_release);
    session_.reset();
    if (!rebuild || stop_requested_.load(std::memory_order_acquire) ||
        io_->hung_up()) {
      break;
    }
    if (is_io_failure) {
      if (++rebuilds > kMaxSessionRebuilds) {
        std::lock_guard lock(mutex_);
        last_error_ = "session rebuild limit reached";
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
    } else {
      rebuilds = 0;
      std::this_thread::sleep_for(std::chrono::milliseconds(400));
    }
  }

  if (decoder_ != nullptr) {
    decoder_->close();
    decoder_.reset();
    decoder_open_profile_.reset();
  }
  surface_attached_.store(false, std::memory_order_release);
  session_.reset();
  wake_fd_.store(-1, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    state_ = "stopped";
  }
  running_.store(false, std::memory_order_release);
}

ClientEngine::Snapshot ClientEngine::snapshot() const {
  Snapshot copy;
  std::shared_ptr<VideoDecoder::Counters> counters;
  {
    std::lock_guard lock(mutex_);
    copy.state = state_;
    copy.failure = failure_;
    copy.profile = profile_;
    copy.codec = active_codec_;
    copy.resolution = active_resolution_;
    copy.preferred_codec = preferred_codec_;
    copy.error = last_error_;
    counters = decoder_counters_;
  }
  copy.surface =
      surface_attached_.load(std::memory_order_acquire) ? "attached" : "none";
  copy.frames = frames_.load(std::memory_order_relaxed);
  copy.frames_received = frames_received_.load(std::memory_order_relaxed);
  copy.frames_dropped = frames_dropped_.load(std::memory_order_relaxed);
  if (counters != nullptr) {
    copy.decoder_rendered = counters->rendered.load(std::memory_order_relaxed);
    copy.decoder_submitted =
        counters->submitted.load(std::memory_order_relaxed);
    copy.decoder_dropped = counters->dropped.load(std::memory_order_relaxed);
  }
  copy.decoder_opens = decoder_opens_.load(std::memory_order_relaxed);
  copy.input_events = input_events_.load(std::memory_order_relaxed);
  copy.input_rejected = input_rejected_.load(std::memory_order_relaxed);
  copy.rx_bytes = rx_bytes_.load(std::memory_order_relaxed);
  copy.tx_bytes = tx_bytes_.load(std::memory_order_relaxed);
  return copy;
}

}  // namespace deskmate::android
