#include "video_decoder.hpp"

#include <android/log.h>
#include <pthread.h>
#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cstring>

#define LOG_TAG "DeskMateDecoder"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace deskmate::android {
namespace {

constexpr std::size_t kMaxQueued = 12;

AMediaFormat* build_format(const DecoderConfig& config,
                           bool low_latency_hints) {
  AMediaFormat* format = AMediaFormat_new();
  AMediaFormat_setString(format, AMEDIAFORMAT_KEY_MIME, config.mime.c_str());
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_WIDTH, config.width);
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_HEIGHT, config.height);
  // One access unit is at most kMaxVideoFragments * kMaxPacketPayload, so the
  // codec input buffer must be able to hold the protocol's largest frame.
  AMediaFormat_setInt32(
      format, AMEDIAFORMAT_KEY_MAX_INPUT_SIZE,
      static_cast<std::int32_t>(deskmate::protocol::kMaxVideoFramePayload));
  AMediaFormat_setInt32(format, AMEDIAFORMAT_KEY_FRAME_RATE, config.frame_rate);
  if (low_latency_hints) {
    AMediaFormat_setInt32(format, "low-latency", 1);
    AMediaFormat_setInt32(format, "priority", 0);
  }
  return format;
}

}  // namespace

VideoDecoder::VideoDecoder()
    : counters_(std::make_shared<Counters>()) {}

VideoDecoder::~VideoDecoder() { close(); }

void VideoDecoder::set_error(std::string message) {
  std::lock_guard lock(error_mutex_);
  error_ = std::move(message);
}

bool VideoDecoder::open(const DecoderConfig& config, ANativeWindow* window,
                        std::string& error,
                        std::shared_ptr<Counters> external_counters) {
  close();
  counters_ = external_counters ? std::move(external_counters)
                                : std::make_shared<Counters>();
  if (window == nullptr) {
    error = "decoder surface is not available";
    return false;
  }
  if (!configure(config, window, error)) {
    return false;
  }
  awaiting_keyframe_.store(true, std::memory_order_release);
  stop_.store(false, std::memory_order_release);
  open_.store(true, std::memory_order_release);
  input_thread_ = std::thread([this] { input_loop(); });
  output_thread_ = std::thread([this] { output_loop(); });
  return true;
}

bool VideoDecoder::configure(const DecoderConfig& config, ANativeWindow* window,
                             std::string& error) {
  codec_ = AMediaCodec_createDecoderByType(config.mime.c_str());
  if (codec_ == nullptr) {
    error = "AMediaCodec_createDecoderByType failed for " + config.mime;
    return false;
  }

  AMediaFormat* format = build_format(config, true);
  media_status_t status =
      AMediaCodec_configure(codec_, format, window, nullptr, 0);
  AMediaFormat_delete(format);

  if (status != AMEDIA_OK) {
    LOGW("AMediaCodec_configure with low-latency hints failed (%d), retrying baseline",
         static_cast<int>(status));
    AMediaCodec_delete(codec_);
    codec_ = AMediaCodec_createDecoderByType(config.mime.c_str());
    if (codec_ == nullptr) {
      error = "AMediaCodec_createDecoderByType failed for " + config.mime;
      return false;
    }
    AMediaFormat* baseline = build_format(config, false);
    status = AMediaCodec_configure(codec_, baseline, window, nullptr, 0);
    AMediaFormat_delete(baseline);
  }

  if (status != AMEDIA_OK) {
    error = "AMediaCodec_configure failed: " + std::to_string(status);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
    return false;
  }
  const media_status_t started = AMediaCodec_start(codec_);
  if (started != AMEDIA_OK) {
    error = "AMediaCodec_start failed: " + std::to_string(started);
    AMediaCodec_delete(codec_);
    codec_ = nullptr;
    return false;
  }
  window_ = window;
  config_ = config;
  width_.store(config.width, std::memory_order_relaxed);
  height_.store(config.height, std::memory_order_relaxed);
  set_error({});
  LOGI("decoder open %s %dx%d@%d", config.mime.c_str(), config.width,
       config.height, config.frame_rate);
  return true;
}

void VideoDecoder::close() {
  open_.store(false, std::memory_order_release);
  stop_.store(true, std::memory_order_release);
  {
    std::lock_guard lock(mutex_);
    queued_.clear();
  }
  cv_.notify_all();
  if (input_thread_.joinable()) {
    input_thread_.join();
  }
  if (output_thread_.joinable()) {
    output_thread_.join();
  }
  if (codec_ != nullptr) {
    (void)AMediaCodec_stop(codec_);
    (void)AMediaCodec_delete(codec_);
    codec_ = nullptr;
  }
  window_ = nullptr;
}

bool VideoDecoder::push(std::vector<std::uint8_t> access_unit,
                        std::int64_t pts_us, bool keyframe) {
  if (!open_.load(std::memory_order_acquire) || access_unit.empty()) {
    return false;
  }
  if (!keyframe && awaiting_keyframe_.load(std::memory_order_acquire)) {
    // Feeding a P-frame before the first IDR produces no output and can
    // wedge some decoders; drop it instead.
    counters_->dropped.fetch_add(1, std::memory_order_relaxed);
    return false;
  }
  {
    std::lock_guard lock(mutex_);
    if (!keyframe && awaiting_keyframe_.load(std::memory_order_acquire)) {
      counters_->dropped.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    if (keyframe) {
      awaiting_keyframe_.store(false, std::memory_order_release);
      if (queued_.size() > 2) {
        counters_->dropped.fetch_add(queued_.size(), std::memory_order_relaxed);
        queued_.clear();
      }
    } else if (queued_.size() >= kMaxQueued) {
      queued_.pop_front();
      counters_->dropped.fetch_add(1, std::memory_order_relaxed);
    }
    queued_.push_back(Pending{std::move(access_unit), pts_us, keyframe});
  }
  counters_->submitted.fetch_add(1, std::memory_order_relaxed);
  cv_.notify_one();
  return true;
}

void VideoDecoder::input_loop() {
  (void)pthread_setname_np(pthread_self(), "dm-dec-in");
  (void)setpriority(PRIO_PROCESS, 0, -8);

  for (;;) {
    Pending frame;
    {
      std::unique_lock lock(mutex_);
      cv_.wait(lock, [this] {
        return stop_.load(std::memory_order_acquire) || !queued_.empty();
      });
      if (stop_.load(std::memory_order_acquire)) {
        return;
      }
      frame = std::move(queued_.front());
      queued_.pop_front();
    }
    if (codec_ == nullptr) {
      return;
    }

    while (!stop_.load(std::memory_order_acquire)) {
      const ssize_t index = AMediaCodec_dequeueInputBuffer(codec_, 5000);
      if (index >= 0) {
        std::size_t capacity = 0;
        std::uint8_t* buffer = AMediaCodec_getInputBuffer(
            codec_, static_cast<std::size_t>(index), &capacity);
        if (buffer != nullptr && capacity >= frame.bytes.size()) {
          std::memcpy(buffer, frame.bytes.data(), frame.bytes.size());
          const std::uint32_t flags =
              frame.keyframe ? AMEDIACODEC_BUFFER_FLAG_KEY_FRAME : 0;
          const media_status_t queued = AMediaCodec_queueInputBuffer(
              codec_, static_cast<std::size_t>(index), 0, frame.bytes.size(),
              static_cast<std::uint64_t>(frame.pts_us), flags);
          if (queued != AMEDIA_OK) {
            counters_->errors.fetch_add(1, std::memory_order_relaxed);
            counters_->dropped.fetch_add(1, std::memory_order_relaxed);
            awaiting_keyframe_.store(true, std::memory_order_release);
            set_error("AMediaCodec_queueInputBuffer failed: " +
                      std::to_string(queued));
          }
        } else {
          counters_->errors.fetch_add(1, std::memory_order_relaxed);
          counters_->dropped.fetch_add(1, std::memory_order_relaxed);
          awaiting_keyframe_.store(true, std::memory_order_release);
          set_error("decoder input buffer unavailable or too small");
        }
        break;
      }
      if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER) {
        continue;
      }
      counters_->errors.fetch_add(1, std::memory_order_relaxed);
      counters_->dropped.fetch_add(1, std::memory_order_relaxed);
      set_error("AMediaCodec_dequeueInputBuffer failed: " +
                std::to_string(index));
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
      break;
    }
  }
}

void VideoDecoder::output_loop() {
  (void)pthread_setname_np(pthread_self(), "dm-dec-out");
  (void)setpriority(PRIO_PROCESS, 0, -8);

  while (!stop_.load(std::memory_order_acquire)) {
    if (codec_ == nullptr) {
      return;
    }
    AMediaCodecBufferInfo info{};
    const ssize_t output =
        AMediaCodec_dequeueOutputBuffer(codec_, &info, 10000);
    if (output >= 0) {
      if ((info.flags & AMEDIACODEC_BUFFER_FLAG_END_OF_STREAM) == 0) {
        AMediaCodec_releaseOutputBuffer(codec_, static_cast<std::size_t>(output),
                                        true);
        counters_->rendered.fetch_add(1, std::memory_order_relaxed);
        awaiting_keyframe_.store(false, std::memory_order_release);
      } else {
        AMediaCodec_releaseOutputBuffer(
            codec_, static_cast<std::size_t>(output), false);
        awaiting_keyframe_.store(true, std::memory_order_release);
      }
      continue;
    }
    if (output == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED) {
      AMediaFormat* format = AMediaCodec_getOutputFormat(codec_);
      if (format != nullptr) {
        std::int32_t width = 0;
        std::int32_t height = 0;
        (void)AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_WIDTH, &width);
        (void)AMediaFormat_getInt32(format, AMEDIAFORMAT_KEY_HEIGHT, &height);
        AMediaFormat_delete(format);
        if (width > 0) {
          width_.store(width, std::memory_order_relaxed);
        }
        if (height > 0) {
          height_.store(height, std::memory_order_relaxed);
        }
      }
      continue;
    }
    if (output == AMEDIACODEC_INFO_TRY_AGAIN_LATER ||
        output == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED) {
      continue;
    }
    counters_->errors.fetch_add(1, std::memory_order_relaxed);
    set_error("AMediaCodec_dequeueOutputBuffer failed: " +
              std::to_string(output));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
}

VideoDecoder::Stats VideoDecoder::stats() const {
  Stats copy;
  copy.submitted = counters_->submitted.load(std::memory_order_relaxed);
  copy.rendered = counters_->rendered.load(std::memory_order_relaxed);
  copy.dropped = counters_->dropped.load(std::memory_order_relaxed);
  copy.errors = counters_->errors.load(std::memory_order_relaxed);
  copy.width = width_.load(std::memory_order_relaxed);
  copy.height = height_.load(std::memory_order_relaxed);
  return copy;
}

std::string VideoDecoder::last_error() const {
  std::lock_guard lock(error_mutex_);
  return error_;
}

}  // namespace deskmate::android
