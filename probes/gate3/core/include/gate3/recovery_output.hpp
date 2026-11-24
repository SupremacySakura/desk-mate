#pragma once

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <thread>

namespace deskmate::gate3 {

using DecoderInstanceId = std::uint64_t;
using RecoveryEpoch = std::uint64_t;

class DecoderOutputOps {
public:
    virtual ~DecoderOutputOps() = default;
    virtual bool FreeOutput(DecoderInstanceId decoder, std::uint32_t index) = 0;
    virtual bool RenderOutput(DecoderInstanceId decoder, std::uint32_t index) = 0;
};

struct OutputRouterStats {
    std::uint64_t callbacks = 0;
    std::uint64_t rendered = 0;
    std::uint64_t freed_stale_epoch = 0;
    std::uint64_t freed_surface_invalid = 0;
    std::uint64_t residual_decoder_callbacks = 0;
    std::uint64_t free_failures = 0;
    std::uint64_t render_failures = 0;
};

class RecoveryOutputRouter {
public:
    explicit RecoveryOutputRouter(DecoderOutputOps &operations);

    RecoveryOutputRouter(const RecoveryOutputRouter &) = delete;
    RecoveryOutputRouter &operator=(const RecoveryOutputRouter &) = delete;

    // Bind once from the dedicated control thread before any lifecycle command.
    bool BindControlThread();

    // Lifecycle state changes are rejected from decoder/surface callback
    // threads. Callbacks only use PostOutputBuffer().
    bool StartDecoder(DecoderInstanceId decoder);
    bool BeginRecovery(DecoderInstanceId decoder);
    bool OpenRecoveryOutput(DecoderInstanceId decoder);
    bool SetSurfaceRenderable(bool renderable);

    // Surface callbacks may invalidate rendering immediately. This does not
    // touch the decoder: it closes output, advances the recovery epoch, and
    // waits for any in-flight native render to return before relinquishing the
    // callback.
    void InvalidateSurfaceFromCallback();

    bool Drain();

    void PostOutputBuffer(DecoderInstanceId decoder, std::uint32_t index);

    RecoveryEpoch recovery_epoch() const;
    DecoderInstanceId current_decoder() const;
    bool output_open() const;
    bool last_good_frame_visible() const;
    OutputRouterStats stats() const;

private:
    struct Command {
        DecoderInstanceId decoder = 0;
        std::uint32_t index = 0;
        RecoveryEpoch epoch = 0;
        bool accepted_when_posted = false;
    };

    bool IsControlThreadLocked() const;

    DecoderOutputOps &operations_;
    mutable std::mutex mutex_;
    std::deque<Command> commands_;
    std::thread::id control_thread_;
    bool control_thread_bound_ = false;
    std::atomic<bool> surface_renderable_fast_{false};
    std::mutex render_gate_;
    DecoderInstanceId current_decoder_ = 0;
    RecoveryEpoch recovery_epoch_ = 0;
    bool output_open_ = false;
    bool surface_renderable_ = false;
    bool last_good_frame_visible_ = false;
    OutputRouterStats stats_;
};

} // namespace deskmate::gate3
