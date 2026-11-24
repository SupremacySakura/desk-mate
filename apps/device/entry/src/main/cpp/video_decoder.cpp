#include "video_decoder.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <sstream>
#include <unistd.h>

#include <multimedia/player_framework/native_avformat.h>
#include <native_window/external_window.h>

// OH_LOG_* expands LOG_DOMAIN / LOG_TAG; hilog/log.h defaults them to 0 / NULL.
// They must be defined before the include for the macros to pick them up.
#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3210
#define LOG_TAG "DeskMateDecoder"

#include <hilog/log.h>

namespace deskmate::device {
namespace {

const char *ErrorName(OH_AVErrCode code)
{
    switch (code) {
        case AV_ERR_OK:
            return "AV_ERR_OK";
        case AV_ERR_NO_MEMORY:
            return "AV_ERR_NO_MEMORY";
        case AV_ERR_OPERATE_NOT_PERMIT:
            return "AV_ERR_OPERATE_NOT_PERMIT";
        case AV_ERR_INVALID_VAL:
            return "AV_ERR_INVALID_VAL";
        case AV_ERR_IO:
            return "AV_ERR_IO";
        case AV_ERR_TIMEOUT:
            return "AV_ERR_TIMEOUT";
        case AV_ERR_UNKNOWN:
            return "AV_ERR_UNKNOWN";
        case AV_ERR_SERVICE_DIED:
            return "AV_ERR_SERVICE_DIED";
        case AV_ERR_INVALID_STATE:
            return "AV_ERR_INVALID_STATE";
        case AV_ERR_UNSUPPORT:
            return "AV_ERR_UNSUPPORT";
        case AV_ERR_TRY_AGAIN_LATER:
            return "AV_ERR_TRY_AGAIN_LATER";
        default:
            return "AV_ERR_OTHER";
    }
}

constexpr std::size_t kMaxQueuedFrames = 8;

std::int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

VideoDecoder::VideoDecoder(int wake_fd) : wake_fd_(wake_fd) {}

VideoDecoder::~VideoDecoder()
{
    Close();
}

void VideoDecoder::SetError(std::string message)
{
    std::lock_guard lock(mutex_);
    last_error_ = std::move(message);
}

void VideoDecoder::NoteResult(const char *operation, OH_AVErrCode result)
{
    if (result == AV_ERR_OK) {
        return;
    }
    std::ostringstream message;
    message << operation << " failed: " << ErrorName(result) << " ("
            << static_cast<int>(result) << ")";
    SetError(message.str());
    OH_LOG_ERROR(LOG_APP, "%{public}s", message.str().c_str());
}

void VideoDecoder::Wake()
{
    const std::uint64_t value = 1;
    const ssize_t written = ::write(wake_fd_, &value, sizeof(value));
    (void)written;
}

void VideoDecoder::Enqueue(Event event)
{
    {
        std::lock_guard lock(mutex_);
        if (events_.size() > 256) {
            events_.pop_front();
            ++stats_.stale_callbacks;
        }
        events_.push_back(event);
    }
    Wake();
}

void VideoDecoder::OnError(OH_AVCodec *, std::int32_t error_code, void *user_data)
{
    auto *self = static_cast<VideoDecoder *>(user_data);
    if (self == nullptr) {
        return;
    }
    Event event;
    event.kind = Event::Kind::Error;
    event.code = error_code;
    self->Enqueue(event);
}

void VideoDecoder::OnStreamChanged(OH_AVCodec *, OH_AVFormat *format,
                                   void *user_data)
{
    auto *self = static_cast<VideoDecoder *>(user_data);
    if (self == nullptr || format == nullptr) {
        return;
    }
    Event event;
    event.kind = Event::Kind::StreamChanged;
    (void)OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_PIC_WIDTH,
                                  &event.width);
    (void)OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_PIC_HEIGHT,
                                  &event.height);
    if (event.width == 0) {
        (void)OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &event.width);
    }
    if (event.height == 0) {
        (void)OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &event.height);
    }
    (void)OH_AVFormat_GetIntValue(format, OH_MD_KEY_PIXEL_FORMAT,
                                  &event.pixel_format);
    self->Enqueue(event);
}

void VideoDecoder::OnNeedInputBuffer(OH_AVCodec *, std::uint32_t index,
                                     OH_AVBuffer *buffer, void *user_data)
{
    auto *self = static_cast<VideoDecoder *>(user_data);
    if (self == nullptr) {
        return;
    }
    {
        std::lock_guard lock(self->mutex_);
        ++self->stats_.input_callbacks;
    }
    Event event;
    event.kind = Event::Kind::InputReady;
    event.index = index;
    event.buffer = buffer;
    self->Enqueue(event);
}

void VideoDecoder::OnNewOutputBuffer(OH_AVCodec *, std::uint32_t index,
                                     OH_AVBuffer *, void *user_data)
{
    auto *self = static_cast<VideoDecoder *>(user_data);
    if (self == nullptr) {
        return;
    }
    {
        std::lock_guard lock(self->mutex_);
        ++self->stats_.output_callbacks;
    }
    Event event;
    event.kind = Event::Kind::OutputReady;
    event.index = index;
    self->Enqueue(event);
}

bool VideoDecoder::CreateCodec(const DecoderConfig &config)
{
    codec_ = OH_VideoDecoder_CreateByMime(config.mime.c_str());
    if (codec_ == nullptr) {
        SetError("OH_VideoDecoder_CreateByMime returned null for " + config.mime);
        return false;
    }

    OH_AVCodecCallback callbacks{
        &VideoDecoder::OnError,
        &VideoDecoder::OnStreamChanged,
        &VideoDecoder::OnNeedInputBuffer,
        &VideoDecoder::OnNewOutputBuffer,
    };
    OH_AVErrCode result =
        OH_VideoDecoder_RegisterCallback(codec_, callbacks, static_cast<void *>(this));
    if (result != AV_ERR_OK) {
        NoteResult("OH_VideoDecoder_RegisterCallback", result);
        (void)OH_VideoDecoder_Destroy(codec_);
        codec_ = nullptr;
        return false;
    }

    OH_AVFormat *format = OH_AVFormat_CreateVideoFormat(
        config.mime.c_str(), config.width, config.height);
    if (format == nullptr) {
        SetError("OH_AVFormat_CreateVideoFormat returned null");
        (void)OH_VideoDecoder_Destroy(codec_);
        codec_ = nullptr;
        return false;
    }
    (void)OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 1);
    (void)OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT,
                                  AV_PIXEL_FORMAT_SURFACE_FORMAT);
    (void)OH_AVFormat_SetIntValue(format, OH_MD_KEY_FRAME_RATE,
                                  config.frame_rate);
    (void)OH_AVFormat_SetIntValue(format, OH_MD_KEY_MAX_INPUT_SIZE, 2 * 1024 * 1024);

    result = OH_VideoDecoder_Configure(codec_, format);
    OH_AVFormat_Destroy(format);
    if (result != AV_ERR_OK) {
        NoteResult("OH_VideoDecoder_Configure", result);
        (void)OH_VideoDecoder_Destroy(codec_);
        codec_ = nullptr;
        return false;
    }
    config_ = config;
    return true;
}

bool VideoDecoder::DestroyCodec()
{
    if (codec_ == nullptr) {
        return true;
    }
    const OH_AVErrCode result = OH_VideoDecoder_Destroy(codec_);
    NoteResult("OH_VideoDecoder_Destroy", result);
    codec_ = nullptr;
    surface_set_ = false;
    native_window_ = nullptr;
    {
        std::lock_guard lock(mutex_);
        queued_.clear();
        input_ready_.clear();
    }
    return result == AV_ERR_OK;
}

bool VideoDecoder::Open(const DecoderConfig &config, void *native_window)
{
    Close();
    if (!CreateCodec(config)) {
        return false;
    }
    ++generation_;
    if (native_window != nullptr) {
        SetSurface(native_window);
    }
    OH_AVErrCode result = OH_VideoDecoder_Prepare(codec_);
    if (result != AV_ERR_OK) {
        NoteResult("OH_VideoDecoder_Prepare", result);
        (void)DestroyCodec();
        return false;
    }
    result = OH_VideoDecoder_Start(codec_);
    if (result != AV_ERR_OK) {
        NoteResult("OH_VideoDecoder_Start", result);
        (void)DestroyCodec();
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        stats_.width = config.width;
        stats_.height = config.height;
        last_error_.clear();
    }
    return true;
}

bool VideoDecoder::Reconfigure(const DecoderConfig &config)
{
    const void *window = native_window_;
    Close();
    return Open(config, const_cast<void *>(window));
}

void VideoDecoder::Close()
{
    if (codec_ == nullptr) {
        return;
    }
    (void)OH_VideoDecoder_Stop(codec_);
    (void)DestroyCodec();
}

void VideoDecoder::SetSurface(void *native_window)
{
    if (codec_ == nullptr || native_window == nullptr) {
        return;
    }
    native_window_ = native_window;
    const std::int32_t geometry = OH_NativeWindow_NativeWindowHandleOpt(
        static_cast<OHNativeWindow *>(native_window), SET_BUFFER_GEOMETRY,
        config_.width, config_.height);
    if (geometry != 0) {
        std::ostringstream message;
        message << "SET_BUFFER_GEOMETRY failed: " << geometry;
        SetError(message.str());
        OH_LOG_ERROR(LOG_APP, "%{public}s", message.str().c_str());
    }
    const OH_AVErrCode result =
        OH_VideoDecoder_SetSurface(codec_, static_cast<OHNativeWindow *>(native_window));
    NoteResult("OH_VideoDecoder_SetSurface", result);
    surface_set_ = result == AV_ERR_OK;
    OH_LOG_INFO(LOG_APP,
                "surface set %{public}dx%{public}d geometry=%{public}d "
                "result=%{public}d",
                config_.width, config_.height, geometry,
                static_cast<std::int32_t>(result));
}

bool VideoDecoder::Push(const std::vector<std::uint8_t> &access_unit,
                        std::int64_t pts_us, bool keyframe)
{
    if (codec_ == nullptr) {
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        if (queued_.size() >= kMaxQueuedFrames) {
            queued_.pop_front();
            ++stats_.frames_dropped;
        }
        queued_.push_back(PendingFrame{access_unit, pts_us, keyframe});
        ++stats_.frames_submitted;
    }
    // The codec hands over its whole input pool right after Start(), which is
    // usually before the first access unit arrives. Those buffers wait in
    // input_ready_ and only get filled from PumpFrames(), which used to run
    // solely on OnNeedInputBuffer. With nothing pushed the codec never asks
    // for another buffer, so the pool stayed empty and no frame was ever
    // decoded: the surface stayed black for both HEVC and H.264.
    PumpFrames();
    return true;
}

bool VideoDecoder::PushOne(const PendingFrame &frame, std::uint32_t index,
                           OH_AVBuffer *buffer)
{
    if (buffer == nullptr) {
        std::lock_guard lock(mutex_);
        ++stats_.stale_callbacks;
        return false;
    }
    const std::int32_t capacity = OH_AVBuffer_GetCapacity(buffer);
    if (capacity < 0 ||
        frame.bytes.size() > static_cast<std::size_t>(capacity)) {
        SetError("input buffer capacity is smaller than the access unit");
        std::lock_guard lock(mutex_);
        ++stats_.output_errors;
        return false;
    }
    std::uint8_t *destination = OH_AVBuffer_GetAddr(buffer);
    if (destination == nullptr) {
        SetError("OH_AVBuffer_GetAddr returned null for input");
        std::lock_guard lock(mutex_);
        ++stats_.output_errors;
        return false;
    }
    std::memcpy(destination, frame.bytes.data(), frame.bytes.size());

    OH_AVCodecBufferAttr attr{};
    attr.pts = frame.pts_us;
    attr.size = static_cast<std::int32_t>(frame.bytes.size());
    attr.offset = 0;
    attr.flags = frame.keyframe ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME
                                : AVCODEC_BUFFER_FLAGS_NONE;
    OH_AVErrCode result = OH_AVBuffer_SetBufferAttr(buffer, &attr);
    if (result != AV_ERR_OK) {
        NoteResult("OH_AVBuffer_SetBufferAttr(input)", result);
        std::lock_guard lock(mutex_);
        ++stats_.output_errors;
        return false;
    }
    result = OH_VideoDecoder_PushInputBuffer(codec_, index);
    if (result != AV_ERR_OK) {
        NoteResult("OH_VideoDecoder_PushInputBuffer", result);
        std::lock_guard lock(mutex_);
        ++stats_.output_errors;
        return false;
    }
    std::lock_guard lock(mutex_);
    ++stats_.input_buffers;
    return true;
}

void VideoDecoder::ProcessEvents()
{
    std::deque<Event> events;
    {
        std::lock_guard lock(mutex_);
        events.swap(events_);
    }
    for (const Event &event : events) {
        if (codec_ == nullptr) {
            break;
        }
        switch (event.kind) {
            case Event::Kind::InputReady: {
                {
                    std::lock_guard lock(mutex_);
                    if (input_ready_.size() > 32) {
                        input_ready_.pop_front();
                        ++stats_.stale_callbacks;
                    }
                    input_ready_.emplace_back(event.index, event.buffer);
                }
                PumpFrames();
                break;
            }
            case Event::Kind::OutputReady: {
                // Timestamped render lets the display drop frames on its own
                // when we run ahead of the panel, instead of blocking.
                const OH_AVErrCode result = OH_VideoDecoder_RenderOutputBufferAtTime(
                    codec_, event.index, NowNs());
                if (result != AV_ERR_OK) {
                    NoteResult("OH_VideoDecoder_RenderOutputBufferAtTime", result);
                    std::lock_guard lock(mutex_);
                    ++stats_.output_errors;
                } else {
                    std::lock_guard lock(mutex_);
                    ++stats_.frames_rendered;
                }
                break;
            }
            case Event::Kind::Error: {
                std::ostringstream message;
                message << "decoder error: " << event.code;
                SetError(message.str());
                OH_LOG_ERROR(LOG_APP, "decoder error %{public}d", event.code);
                break;
            }
            case Event::Kind::StreamChanged: {
                std::lock_guard lock(mutex_);
                ++stats_.stream_changes;
                if (event.width > 0) {
                    stats_.width = event.width;
                }
                if (event.height > 0) {
                    stats_.height = event.height;
                }
                stats_.pixel_format = event.pixel_format;
                OH_LOG_INFO(LOG_APP,
                            "stream changed %{public}dx%{public}d format=%{public}d",
                            stats_.width, stats_.height, stats_.pixel_format);
                break;
            }
        }
    }
}

void VideoDecoder::PumpFrames()
{
    for (;;) {
        PendingFrame frame;
        std::uint32_t index = 0;
        OH_AVBuffer *buffer = nullptr;
        {
            std::lock_guard lock(mutex_);
            if (queued_.empty() || input_ready_.empty()) {
                return;
            }
            frame = std::move(queued_.front());
            queued_.pop_front();
            index = input_ready_.front().first;
            buffer = input_ready_.front().second;
            input_ready_.pop_front();
        }
        if (!PushOne(frame, index, buffer)) {
            return;
        }
    }
}

VideoDecoder::Stats VideoDecoder::stats() const
{
    std::lock_guard lock(mutex_);
    return stats_;
}

std::string VideoDecoder::last_error() const
{
    std::lock_guard lock(mutex_);
    return last_error_;
}

}  // namespace deskmate::device
