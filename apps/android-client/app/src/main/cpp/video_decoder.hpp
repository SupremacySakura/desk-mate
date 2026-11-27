#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <android/native_window.h>
#include <media/NdkMediaCodec.h>

#include "deskmate/protocol.hpp"

namespace deskmate::android {

struct DecoderConfig {
  std::string mime{"video/avc"};
  std::int32_t width{};
  std::int32_t height{};
  std::int32_t frame_rate{60};
};

// MediaCodec Surface decoder with dedicated input/output threads and lock-free
// atomic telemetry counters that remain readable across worker stalls.
class VideoDecoder {
 public:
  struct Counters {
    std::atomic<std::uint64_t> submitted{0};
    std::atomic<std::uint64_t> rendered{0};
    std::atomic<std::uint64_t> dropped{0};
    std::atomic<std::uint64_t> errors{0};
  };

  VideoDecoder();
  ~VideoDecoder();

  VideoDecoder(const VideoDecoder&) = delete;
  VideoDecoder& operator=(const VideoDecoder&) = delete;

  bool open(const DecoderConfig& config, ANativeWindow* window,
            std::string& error,
            std::shared_ptr<Counters> external_counters = nullptr);
  void close();
  bool push(std::vector<std::uint8_t> access_unit, std::int64_t pts_us,
            bool keyframe);

  struct Stats {
    std::uint64_t submitted{};
    std::uint64_t rendered{};
    std::uint64_t dropped{};
    std::uint64_t errors{};
    std::int32_t width{};
    std::int32_t height{};
  };
  [[nodiscard]] Stats stats() const;
  [[nodiscard]] std::string last_error() const;
  [[nodiscard]] bool open() const {
    return open_.load(std::memory_order_acquire);
  }

 private:
  struct Pending {
    std::vector<std::uint8_t> bytes;
    std::int64_t pts_us{};
    bool keyframe{};
  };

  bool configure(const DecoderConfig& config, ANativeWindow* window,
                 std::string& error);
  void input_loop();
  void output_loop();
  void set_error(std::string message);

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::thread input_thread_;
  std::thread output_thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> open_{false};
  std::deque<Pending> queued_;
  AMediaCodec* codec_{};
  ANativeWindow* window_{};
  DecoderConfig config_{};
  std::int64_t last_render_target_ns_{0};
  // MediaCodec emits nothing until it sees an IDR after configure/flush.
  std::atomic<bool> awaiting_keyframe_{true};
  std::shared_ptr<Counters> counters_;
  std::atomic<std::int32_t> width_{0};
  std::atomic<std::int32_t> height_{0};
  mutable std::mutex error_mutex_;
  std::string error_;
};

}  // namespace deskmate::android
