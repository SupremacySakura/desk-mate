#include "ohos_decoder.hpp"

#include <algorithm>
#include <cstring>
#include <sstream>

#include <multimedia/player_framework/native_avcodec_videodecoder.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_window/external_window.h>

namespace deskmate::gate3::probe {

namespace {

constexpr std::int64_t kPtsStepUs = 33333;

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

} // namespace

OhosDecoder::OhosDecoder() = default;

OhosDecoder::~OhosDecoder()
{
    if (codec_ != nullptr && (!control_thread_bound_ || OnControlThread())) {
        (void)OH_VideoDecoder_Destroy(codec_);
        codec_ = nullptr;
    }
}

void OhosDecoder::SetSink(OhosDecoderSink *sink)
{
    sink_ = sink;
}

void OhosDecoder::SetStream(const AnnexBStream *stream)
{
    stream_ = stream;
}

void OhosDecoder::BindControlThread()
{
    control_thread_ = std::this_thread::get_id();
    control_thread_bound_ = true;
}

bool OhosDecoder::OnControlThread() const
{
    return control_thread_bound_ && std::this_thread::get_id() == control_thread_;
}

bool OhosDecoder::CheckControlThread(const char *operation)
{
    if (OnControlThread()) {
        return true;
    }
    std::lock_guard lock(state_mutex_);
    last_error_ = std::string(operation) + " called outside the control thread";
    ++control_thread_violations_;
    return false;
}

bool OhosDecoder::CheckDecoder()
{
    if (codec_ != nullptr) {
        return true;
    }
    std::lock_guard lock(state_mutex_);
    last_error_ = "decoder is not created";
    return false;
}

void OhosDecoder::NoteResult(const char *operation, OH_AVErrCode result)
{
    if (result == AV_ERR_OK) {
        return;
    }
    std::lock_guard lock(state_mutex_);
    std::ostringstream message;
    message << operation << " failed: " << ErrorName(result) << " (" << static_cast<int>(result) << ")";
    last_error_ = message.str();
}

bool OhosDecoder::CreateNext()
{
    if (!CheckControlThread("CreateNext") || codec_ != nullptr) {
        return false;
    }
    OH_AVCodec *codec = OH_VideoDecoder_CreateByMime(OH_AVCODEC_MIMETYPE_VIDEO_AVC);
    if (codec == nullptr) {
        std::lock_guard lock(state_mutex_);
        last_error_ = "OH_VideoDecoder_CreateByMime returned null";
        return false;
    }

    auto context = std::make_unique<CallbackContext>();
    context->sink = sink_;
    context->decoder = next_instance_++;
    context->input_epoch = &input_epoch_;
    callback_contexts_.push_back(std::move(context));
    CallbackContext *context_ptr = callback_contexts_.back().get();

    OH_AVCodecCallback callbacks{
        &OhosDecoder::OnError,
        &OhosDecoder::OnStreamChanged,
        &OhosDecoder::OnNeedInputBuffer,
        &OhosDecoder::OnNewOutputBuffer,
    };
    const OH_AVErrCode registered =
        OH_VideoDecoder_RegisterCallback(codec, callbacks, static_cast<void *>(context_ptr));
    if (registered != AV_ERR_OK) {
        NoteResult("OH_VideoDecoder_RegisterCallback", registered);
        (void)OH_VideoDecoder_Destroy(codec);
        return false;
    }

    codec_ = codec;
    current_instance_ = context_ptr->decoder;
    return true;
}

bool OhosDecoder::DestroyCurrent()
{
    if (!CheckControlThread("Destroy") || !CheckDecoder()) {
        return false;
    }
    InvalidateInput();
    const OH_AVErrCode result = OH_VideoDecoder_Destroy(codec_);
    NoteResult("OH_VideoDecoder_Destroy", result);
    if (result == AV_ERR_OK) {
        codec_ = nullptr;
        surface_set_ = false;
    }
    return result == AV_ERR_OK;
}

bool OhosDecoder::DestroyAndCreate()
{
    if (!DestroyCurrent()) {
        return false;
    }
    return CreateNext();
}

bool OhosDecoder::Destroy()
{
    return DestroyCurrent();
}

bool OhosDecoder::Configure()
{
    if (!CheckControlThread("Configure") || !CheckDecoder()) {
        return false;
    }
    OH_AVFormat *format = OH_AVFormat_CreateVideoFormat(OH_AVCODEC_MIMETYPE_VIDEO_AVC, width_, height_);
    if (format == nullptr) {
        std::lock_guard lock(state_mutex_);
        last_error_ = "OH_AVFormat_CreateVideoFormat returned null";
        return false;
    }

    low_latency_requested_ = true;
    low_latency_configured_ = OH_AVFormat_SetIntValue(format, OH_MD_KEY_VIDEO_ENABLE_LOW_LATENCY, 1);
    (void)OH_AVFormat_SetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, AV_PIXEL_FORMAT_SURFACE_FORMAT);
    (void)OH_AVFormat_SetIntValue(format, OH_MD_KEY_FRAME_RATE, 30);
    (void)OH_AVFormat_SetIntValue(format, OH_MD_KEY_MAX_INPUT_SIZE, 1024 * 1024);

    const OH_AVErrCode result = OH_VideoDecoder_Configure(codec_, format);
    OH_AVFormat_Destroy(format);
    NoteResult("OH_VideoDecoder_Configure", result);
    if (result == AV_ERR_OK) {
        low_latency_configured_ = low_latency_configured_ && true;
    }
    return result == AV_ERR_OK;
}

bool OhosDecoder::SetSurface(void *native_window)
{
    if (!CheckControlThread("SetSurface") || !CheckDecoder() || native_window == nullptr) {
        return false;
    }
    const std::int32_t geometry =
        OH_NativeWindow_NativeWindowHandleOpt(static_cast<OHNativeWindow *>(native_window), SET_BUFFER_GEOMETRY,
                                              width_, height_);
    if (geometry != 0) {
        std::lock_guard lock(state_mutex_);
        last_error_ = "SET_BUFFER_GEOMETRY failed: " + std::to_string(geometry);
    }
    const OH_AVErrCode result = OH_VideoDecoder_SetSurface(codec_, static_cast<OHNativeWindow *>(native_window));
    NoteResult("OH_VideoDecoder_SetSurface", result);
    if (result == AV_ERR_OK) {
        native_window_ = native_window;
        surface_set_ = true;
    }
    return result == AV_ERR_OK;
}

bool OhosDecoder::Prepare()
{
    if (!CheckControlThread("Prepare") || !CheckDecoder()) {
        return false;
    }
    const OH_AVErrCode result = OH_VideoDecoder_Prepare(codec_);
    NoteResult("OH_VideoDecoder_Prepare", result);
    return result == AV_ERR_OK;
}

bool OhosDecoder::Start()
{
    if (!CheckControlThread("Start") || !CheckDecoder()) {
        return false;
    }
    const OH_AVErrCode result = OH_VideoDecoder_Start(codec_);
    NoteResult("OH_VideoDecoder_Start", result);
    return result == AV_ERR_OK;
}

bool OhosDecoder::Flush()
{
    if (!CheckControlThread("Flush") || !CheckDecoder()) {
        return false;
    }
    InvalidateInput();
    const OH_AVErrCode result = OH_VideoDecoder_Flush(codec_);
    NoteResult("OH_VideoDecoder_Flush", result);
    return result == AV_ERR_OK;
}

bool OhosDecoder::Stop()
{
    if (!CheckControlThread("Stop") || !CheckDecoder()) {
        return false;
    }
    InvalidateInput();
    const OH_AVErrCode result = OH_VideoDecoder_Stop(codec_);
    NoteResult("OH_VideoDecoder_Stop", result);
    return result == AV_ERR_OK;
}

bool OhosDecoder::Reset()
{
    if (!CheckControlThread("Reset") || !CheckDecoder()) {
        return false;
    }
    InvalidateInput();
    const OH_AVErrCode result = OH_VideoDecoder_Reset(codec_);
    NoteResult("OH_VideoDecoder_Reset", result);
    if (result == AV_ERR_OK) {
        surface_set_ = false;
    }
    return result == AV_ERR_OK;
}

bool OhosDecoder::SubmitParameterSetsAndIdr()
{
    if (!CheckControlThread("SubmitParameterSetsAndIdr")) {
        return false;
    }
    if (stream_ == nullptr || stream_->access_units.empty()) {
        std::lock_guard lock(state_mutex_);
        last_error_ = "no Annex-B access units loaded";
        return false;
    }
    const AnnexBAccessUnit &first = stream_->access_units.front();
    if (!first.keyframe || !first.contains_sps || !first.contains_pps) {
        std::lock_guard lock(state_mutex_);
        last_error_ = "first access unit is not an IDR with SPS and PPS";
        return false;
    }
    feed_cursor_ = 0;
    if (feed_limit_ == 0) {
        feed_limit_ = stream_->access_units.size();
    }
    feed_enabled_ = true;
    return true;
}

bool OhosDecoder::PushQueuedInput(const InputReadyEvent &event)
{
    if (!CheckControlThread("PushQueuedInput")) {
        return false;
    }
    if (stream_ == nullptr || event.decoder != current_instance_ || event.input_epoch != input_epoch_.load() ||
        !feed_enabled_ || event.buffer == nullptr) {
        std::lock_guard lock(state_mutex_);
        ++stale_input_callbacks_;
        return false;
    }
    if (feed_cursor_ >= stream_->access_units.size() || feed_cursor_ >= feed_limit_) {
        return false;
    }

    const AnnexBAccessUnit &access_unit = stream_->access_units[feed_cursor_];
    const auto bytes = stream_->View(access_unit);
    const std::int32_t capacity = OH_AVBuffer_GetCapacity(event.buffer);
    if (capacity < 0 || bytes.size() > static_cast<std::size_t>(capacity)) {
        std::lock_guard lock(state_mutex_);
        last_error_ = "input buffer capacity is smaller than the access unit";
        return false;
    }
    std::uint8_t *destination = OH_AVBuffer_GetAddr(event.buffer);
    if (destination == nullptr) {
        std::lock_guard lock(state_mutex_);
        last_error_ = "OH_AVBuffer_GetAddr returned null for input";
        return false;
    }
    std::memcpy(destination, bytes.data(), bytes.size());

    OH_AVCodecBufferAttr attr{};
    attr.pts = static_cast<std::int64_t>(feed_cursor_) * kPtsStepUs;
    attr.size = static_cast<std::int32_t>(bytes.size());
    attr.offset = 0;
    attr.flags = access_unit.keyframe ? AVCODEC_BUFFER_FLAGS_SYNC_FRAME : AVCODEC_BUFFER_FLAGS_NONE;
    OH_AVErrCode result = OH_AVBuffer_SetBufferAttr(event.buffer, &attr);
    NoteResult("OH_AVBuffer_SetBufferAttr(input)", result);
    if (result != AV_ERR_OK) {
        return false;
    }
    result = OH_VideoDecoder_PushInputBuffer(codec_, event.index);
    NoteResult("OH_VideoDecoder_PushInputBuffer", result);
    if (result == AV_ERR_OK) {
        ++feed_cursor_;
        std::lock_guard lock(state_mutex_);
        ++input_buffers_pushed_;
        if (access_unit.contains_sps && access_unit.contains_pps) {
            ++parameter_set_units_pushed_;
        }
    }
    return result == AV_ERR_OK;
}

bool OhosDecoder::FreeOutput(DecoderInstanceId decoder, std::uint32_t index)
{
    if (!CheckControlThread("FreeOutput") || !CheckDecoder() || decoder != current_instance_) {
        return false;
    }
    const OH_AVErrCode result = OH_VideoDecoder_FreeOutputBuffer(codec_, index);
    NoteResult("OH_VideoDecoder_FreeOutputBuffer", result);
    return result == AV_ERR_OK;
}

bool OhosDecoder::RenderOutput(DecoderInstanceId decoder, std::uint32_t index)
{
    if (!CheckControlThread("RenderOutput") || !CheckDecoder() || decoder != current_instance_) {
        return false;
    }
    const OH_AVErrCode result = OH_VideoDecoder_RenderOutputBuffer(codec_, index);
    NoteResult("OH_VideoDecoder_RenderOutputBuffer", result);
    return result == AV_ERR_OK;
}

DecoderInstanceId OhosDecoder::current_instance() const
{
    return current_instance_.load(std::memory_order_acquire);
}

std::uint64_t OhosDecoder::input_epoch() const
{
    return input_epoch_.load();
}

std::size_t OhosDecoder::feed_cursor() const
{
    return feed_cursor_;
}

void OhosDecoder::SetFeedLimit(std::size_t access_unit_count)
{
    feed_limit_ = stream_ == nullptr ? 0 : std::min(access_unit_count, stream_->access_units.size());
}

void OhosDecoder::InvalidateInput()
{
    feed_enabled_ = false;
    ++input_epoch_;
}

void OhosDecoder::SetInputSize(std::int32_t width, std::int32_t height)
{
    if (width > 0 && height > 0) {
        width_ = width;
        height_ = height;
    }
}

std::int32_t OhosDecoder::width() const
{
    return width_;
}

std::int32_t OhosDecoder::height() const
{
    return height_;
}

bool OhosDecoder::low_latency_requested() const
{
    return low_latency_requested_.load(std::memory_order_acquire);
}

bool OhosDecoder::low_latency_configured() const
{
    return low_latency_configured_.load(std::memory_order_acquire);
}

bool OhosDecoder::surface_set() const
{
    return surface_set_.load(std::memory_order_acquire);
}

std::string OhosDecoder::last_error() const
{
    std::lock_guard lock(state_mutex_);
    return last_error_;
}

std::uint64_t OhosDecoder::input_buffers_pushed() const
{
    std::lock_guard lock(state_mutex_);
    return input_buffers_pushed_;
}

std::uint64_t OhosDecoder::parameter_set_units_pushed() const
{
    std::lock_guard lock(state_mutex_);
    return parameter_set_units_pushed_;
}

std::uint64_t OhosDecoder::stale_input_callbacks() const
{
    std::lock_guard lock(state_mutex_);
    return stale_input_callbacks_;
}

std::uint64_t OhosDecoder::control_thread_violations() const
{
    std::lock_guard lock(state_mutex_);
    return control_thread_violations_;
}

void OhosDecoder::OnError(OH_AVCodec *, std::int32_t error_code, void *user_data)
{
    auto *context = static_cast<CallbackContext *>(user_data);
    if (context != nullptr && context->sink != nullptr) {
        context->sink->OnDecoderError({context->decoder, error_code});
    }
}

void OhosDecoder::OnStreamChanged(OH_AVCodec *, OH_AVFormat *format, void *user_data)
{
    auto *context = static_cast<CallbackContext *>(user_data);
    if (context == nullptr || context->sink == nullptr || format == nullptr) {
        return;
    }
    StreamChangedEvent event;
    event.decoder = context->decoder;
    (void)OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_PIC_WIDTH, &event.width);
    (void)OH_AVFormat_GetIntValue(format, OH_MD_KEY_VIDEO_PIC_HEIGHT, &event.height);
    if (event.width == 0) {
        (void)OH_AVFormat_GetIntValue(format, OH_MD_KEY_WIDTH, &event.width);
    }
    if (event.height == 0) {
        (void)OH_AVFormat_GetIntValue(format, OH_MD_KEY_HEIGHT, &event.height);
    }
    (void)OH_AVFormat_GetIntValue(format, OH_MD_KEY_PIXEL_FORMAT, &event.pixel_format);
    context->sink->OnStreamChanged(event);
}

void OhosDecoder::OnNeedInputBuffer(OH_AVCodec *, std::uint32_t index, OH_AVBuffer *buffer, void *user_data)
{
    auto *context = static_cast<CallbackContext *>(user_data);
    if (context != nullptr && context->sink != nullptr) {
        context->sink->OnInputReady({context->decoder, context->input_epoch->load(), index, buffer});
    }
}

void OhosDecoder::OnNewOutputBuffer(OH_AVCodec *, std::uint32_t index, OH_AVBuffer *, void *user_data)
{
    auto *context = static_cast<CallbackContext *>(user_data);
    if (context != nullptr && context->sink != nullptr) {
        context->sink->OnOutputReady(context->decoder, index);
    }
}

} // namespace deskmate::gate3::probe
