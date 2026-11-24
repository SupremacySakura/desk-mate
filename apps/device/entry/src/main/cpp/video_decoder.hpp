#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>
#include <multimedia/player_framework/native_avcodec_videodecoder.h>

namespace deskmate::device {

struct DecoderConfig {
    std::string mime;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t frame_rate = 60;
};

// Streaming H.264/HEVC decoder onto a single native window.
//
// Every codec call happens on the owner's control thread. The AVCodec
// callbacks only enqueue events and poke an eventfd, exactly like the Gate 3
// probe, so no teardown ever runs inside a callback.
class VideoDecoder {
public:
    explicit VideoDecoder(int wake_fd);
    ~VideoDecoder();

    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    bool Open(const DecoderConfig &config, void *native_window);
    bool Reconfigure(const DecoderConfig &config);
    void Close();

    bool Push(const std::vector<std::uint8_t> &access_unit, std::int64_t pts_us,
              bool keyframe);
    void SetSurface(void *native_window);

    // Drains queued callback work. Must be called from the control thread.
    void ProcessEvents();

    struct Stats {
        std::uint64_t frames_submitted = 0;
        std::uint64_t frames_rendered = 0;
        std::uint64_t frames_dropped = 0;
        std::uint64_t input_buffers = 0;
        std::uint64_t stale_callbacks = 0;
        std::uint64_t output_errors = 0;
        std::uint64_t stream_changes = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
        std::int32_t pixel_format = 0;
    };

    [[nodiscard]] Stats stats() const;
    [[nodiscard]] std::string last_error() const;
    [[nodiscard]] bool open() const { return codec_ != nullptr; }

private:
    struct PendingFrame {
        std::vector<std::uint8_t> bytes;
        std::int64_t pts_us = 0;
        bool keyframe = false;
    };

    struct Event {
        enum class Kind { InputReady, OutputReady, Error, StreamChanged } kind =
            Kind::InputReady;
        std::uint32_t index = 0;
        OH_AVBuffer *buffer = nullptr;
        std::int32_t code = 0;
        std::int32_t width = 0;
        std::int32_t height = 0;
        std::int32_t pixel_format = 0;
    };

    static void OnError(OH_AVCodec *, std::int32_t error_code, void *user_data);
    static void OnStreamChanged(OH_AVCodec *, OH_AVFormat *format,
                                void *user_data);
    static void OnNeedInputBuffer(OH_AVCodec *, std::uint32_t index,
                                  OH_AVBuffer *buffer, void *user_data);
    static void OnNewOutputBuffer(OH_AVCodec *, std::uint32_t index,
                                  OH_AVBuffer *, void *user_data);

    bool CreateCodec(const DecoderConfig &config);
    bool DestroyCodec();
    bool PushOne(const PendingFrame &frame, std::uint32_t index,
                 OH_AVBuffer *buffer);
    void PumpFrames();
    void NoteResult(const char *operation, OH_AVErrCode result);
    void Enqueue(Event event);
    void Wake();
    void SetError(std::string message);

    int wake_fd_ = -1;
    OH_AVCodec *codec_ = nullptr;
    DecoderConfig config_{};
    void *native_window_ = nullptr;
    bool surface_set_ = false;
    std::uint64_t generation_ = 0;

    mutable std::mutex mutex_;
    std::deque<PendingFrame> queued_;
    // Input buffers the codec handed us but we had no frame for yet. Dropping
    // one would permanently shrink the codec's buffer pool and eventually
    // stop all input callbacks, so they are held until a frame is available.
    std::deque<std::pair<std::uint32_t, OH_AVBuffer *>> input_ready_;
    std::deque<Event> events_;
    Stats stats_;
    std::string last_error_;
};

}  // namespace deskmate::device
