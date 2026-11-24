#pragma once

#include "gate3/annexb.hpp"
#include "gate3/lifecycle_controller.hpp"
#include "gate3/recovery_output.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <multimedia/player_framework/native_avbuffer.h>
#include <multimedia/player_framework/native_avcodec_base.h>

namespace deskmate::gate3::probe {

struct InputReadyEvent {
    DecoderInstanceId decoder = 0;
    std::uint64_t input_epoch = 0;
    std::uint32_t index = 0;
    OH_AVBuffer *buffer = nullptr;
};

struct ErrorEvent {
    DecoderInstanceId decoder = 0;
    std::int32_t code = 0;
};

struct StreamChangedEvent {
    DecoderInstanceId decoder = 0;
    std::int32_t width = 0;
    std::int32_t height = 0;
    std::int32_t pixel_format = 0;
};

class OhosDecoderSink {
public:
    virtual ~OhosDecoderSink() = default;
    virtual void OnInputReady(const InputReadyEvent &event) = 0;
    virtual void OnOutputReady(DecoderInstanceId decoder, std::uint32_t index) = 0;
    virtual void OnDecoderError(const ErrorEvent &event) = 0;
    virtual void OnStreamChanged(const StreamChangedEvent &event) = 0;
};

class OhosDecoder final : public DecoderLifecycleOps, public DecoderOutputOps {
public:
    OhosDecoder();
    // Explicit Destroy() on the control thread is required before returning
    // the native Surface references; the destructor performs no native teardown.
    ~OhosDecoder() override;

    OhosDecoder(const OhosDecoder &) = delete;
    OhosDecoder &operator=(const OhosDecoder &) = delete;

    void SetSink(OhosDecoderSink *sink);
    void SetStream(const AnnexBStream *stream);
    void BindControlThread();
    bool OnControlThread() const;
    bool CreateNext();
    bool Destroy();

    bool Flush() override;
    bool Stop() override;
    bool Reset() override;
    bool Configure() override;
    bool SetSurface(void *native_window) override;
    bool Prepare() override;
    bool Start() override;
    bool DestroyAndCreate() override;
    bool SubmitParameterSetsAndIdr() override;

    bool FreeOutput(DecoderInstanceId decoder, std::uint32_t index) override;
    bool RenderOutput(DecoderInstanceId decoder, std::uint32_t index) override;

    // Called on the control thread after a callback was queued. It is the only
    // place that touches an input buffer pointer.
    bool PushQueuedInput(const InputReadyEvent &event);

    DecoderInstanceId current_instance() const;
    std::uint64_t input_epoch() const;
    std::size_t feed_cursor() const;
    void SetFeedLimit(std::size_t access_unit_count);
    void InvalidateInput();
    void SetInputSize(std::int32_t width, std::int32_t height);
    std::int32_t width() const;
    std::int32_t height() const;
    bool low_latency_requested() const;
    bool low_latency_configured() const;
    bool surface_set() const;
    std::string last_error() const;
    std::uint64_t input_buffers_pushed() const;
    std::uint64_t parameter_set_units_pushed() const;
    std::uint64_t stale_input_callbacks() const;
    std::uint64_t control_thread_violations() const;

private:
    struct CallbackContext {
        OhosDecoderSink *sink = nullptr;
        DecoderInstanceId decoder = 0;
        const std::atomic<std::uint64_t> *input_epoch = nullptr;
    };

    static void OnError(OH_AVCodec *codec, std::int32_t error_code, void *user_data);
    static void OnStreamChanged(OH_AVCodec *codec, OH_AVFormat *format, void *user_data);
    static void OnNeedInputBuffer(OH_AVCodec *codec, std::uint32_t index, OH_AVBuffer *buffer, void *user_data);
    static void OnNewOutputBuffer(OH_AVCodec *codec, std::uint32_t index, OH_AVBuffer *buffer, void *user_data);

    bool DestroyCurrent();
    bool CheckControlThread(const char *operation);
    bool CheckDecoder();
    void NoteResult(const char *operation, OH_AVErrCode result);

    OhosDecoderSink *sink_ = nullptr;
    const AnnexBStream *stream_ = nullptr;
    OH_AVCodec *codec_ = nullptr;
    void *native_window_ = nullptr;
    std::thread::id control_thread_;
    bool control_thread_bound_ = false;
    std::atomic<DecoderInstanceId> current_instance_{0};
    DecoderInstanceId next_instance_ = 1;
    std::atomic<std::uint64_t> input_epoch_{1};
    std::vector<std::unique_ptr<CallbackContext>> callback_contexts_;
    std::int32_t width_ = 640;
    std::int32_t height_ = 360;
    std::size_t feed_cursor_ = 0;
    std::size_t feed_limit_ = 0;
    bool feed_enabled_ = false;
    std::atomic<bool> low_latency_requested_{false};
    std::atomic<bool> low_latency_configured_{false};
    std::atomic<bool> surface_set_{false};
    mutable std::mutex state_mutex_;
    std::string last_error_;
    std::uint64_t input_buffers_pushed_ = 0;
    std::uint64_t parameter_set_units_pushed_ = 0;
    std::uint64_t stale_input_callbacks_ = 0;
    std::uint64_t control_thread_violations_ = 0;
};

} // namespace deskmate::gate3::probe
