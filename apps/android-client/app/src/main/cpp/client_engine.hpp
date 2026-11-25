#pragma once

#include "accessory_io.hpp"
#include "video_decoder.hpp"

#include "deskmate/platform/input_frames.hpp"
#include "deskmate/session.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace deskmate::android {

// One Android client session. Java owns the accessory fd and the Surface; this
// object only borrows both for the lifetime of start()..stop().
class ClientEngine {
 public:
  ClientEngine();
  ~ClientEngine();

  ClientEngine(const ClientEngine&) = delete;
  ClientEngine& operator=(const ClientEngine&) = delete;

  bool start(int accessory_fd, ANativeWindow* window, int width, int height,
             int refresh_hz, std::string& error);
  // Non-blocking. Java must close the accessory after this returns to wake
  // poll() with POLLHUP on platforms where the driver blocks reads.
  void request_stop();
  void stop();
  [[nodiscard]] bool running() const {
    return running_.load(std::memory_order_acquire);
  }

  void set_surface(ANativeWindow* window);
  void set_viewport(int width, int height);
  void set_preferred_codec(const std::string& codec);
  void set_touch_enabled(bool enabled);

  // A full MotionEvent snapshot. TouchTracker diffs against the previous
  // snapshot, so sending one pointer at a time would report the other contacts
  // as released.
  void touch_snapshot(std::span<const deskmate::platform::TouchSample> samples,
                      bool cancel);
  void touch_down(int pointer_id, float x, float y);
  void touch_move(int pointer_id, float x, float y);
  void touch_up(int pointer_id, float x, float y);
  void touch_cancel();

  struct Snapshot {
    std::string state{"idle"};
    std::string failure;
    std::string profile{"none"};
    std::string codec;
    std::string resolution;
    std::string preferred_codec{"auto"};
    std::string surface{"none"};
    std::string error;
    std::uint64_t frames{};
    std::uint64_t frames_received{};
    std::uint64_t frames_dropped{};
    std::uint64_t decoder_rendered{};
    std::uint64_t decoder_submitted{};
    std::uint64_t decoder_dropped{};
    std::uint64_t decoder_opens{};
    std::uint64_t input_events{};
    std::uint64_t input_rejected{};
    std::uint64_t rx_bytes{};
    std::uint64_t tx_bytes{};
  };
  [[nodiscard]] Snapshot snapshot() const;

 private:
  class Boundary;

  void wake_worker();
  void pump_session_step();
  void worker_main();
  void drain_video();
  void flush_input();
  bool ensure_decoder();

  std::unique_ptr<Boundary> boundary_;
  std::mutex io_mutex_;
  std::unique_ptr<AccessoryIo> io_;
  std::unique_ptr<deskmate::protocol::DeviceSession> session_;
  std::unique_ptr<VideoDecoder> decoder_;
  std::shared_ptr<VideoDecoder::Counters> decoder_counters_;
  // Pending window handed over by Java; adopted by the worker thread, which is
  // the only thread that releases the active window (after closing the decoder
  // that renders into it).
  ANativeWindow* pending_window_{};
  ANativeWindow* active_window_{};
  std::thread worker_;
  std::atomic<int> wake_fd_{-1};
  std::atomic<bool> running_{false};
  std::atomic<bool> stop_requested_{false};
  std::atomic<bool> touch_enabled_{false};
  std::atomic<bool> has_pending_input_{false};
  std::atomic<bool> decoder_dirty_{false};
  std::atomic<bool> decoder_release_{false};
  // set_surface(nullptr): the SurfaceView is going away but the USB session is
  // not. The worker closes the decoder and releases the window; the session
  // keeps running and re-attaches when a new Surface arrives.
  std::atomic<bool> surface_detached_{false};
  std::atomic<bool> surface_changed_{false};
  std::atomic<bool> surface_attached_{false};
  std::optional<deskmate::protocol::StreamProfile> decoder_config_;
  // Profile the currently open decoder was configured for.
  std::optional<deskmate::protocol::StreamProfile> decoder_open_profile_;
  int accessory_fd_{-1};
  int width_{1920};
  int height_{1200};
  int refresh_hz_{60};

  mutable std::mutex mutex_;
  std::mutex surface_mutex_;
  std::string state_{"idle"};
  std::string failure_;
  std::string profile_{"none"};
  std::string active_codec_;
  std::string active_resolution_;
  std::string preferred_codec_{"auto"};
  std::string last_error_;
  std::atomic<std::uint64_t> frames_{0};
  std::atomic<std::uint64_t> frames_received_{0};
  std::atomic<std::uint64_t> frames_dropped_{0};
  std::atomic<std::uint64_t> decoder_opens_{0};
  std::atomic<std::uint64_t> input_events_{0};
  std::atomic<std::uint64_t> input_rejected_{0};
  std::atomic<std::uint64_t> rx_bytes_{0};
  std::atomic<std::uint64_t> tx_bytes_{0};
  std::deque<deskmate::protocol::InputEvent> input_queue_;
  std::mutex input_mutex_;
  deskmate::platform::TouchTracker touches_;
};

}  // namespace deskmate::android
