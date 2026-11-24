#include "gate3/recovery_output.hpp"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <thread>
#include <vector>

namespace {

int failures = 0;

void Expect(bool condition, const char *message)
{
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

class RecordingOutputOps final : public deskmate::gate3::DecoderOutputOps {
public:
    bool FreeOutput(deskmate::gate3::DecoderInstanceId, std::uint32_t index) override
    {
        freed.push_back(index);
        return free_result;
    }

    bool RenderOutput(deskmate::gate3::DecoderInstanceId, std::uint32_t index) override
    {
        rendered.push_back(index);
        if (on_render) {
            return on_render();
        }
        return render_result;
    }

    std::vector<std::uint32_t> freed;
    std::vector<std::uint32_t> rendered;
    std::function<bool()> on_render;
    bool free_result = true;
    bool render_result = true;
};

void CallbackOnlyQueuesAndControlThreadRenders()
{
    RecordingOutputOps ops;
    deskmate::gate3::RecoveryOutputRouter router(ops);
    router.BindControlThread();
    Expect(router.StartDecoder(7), "control thread can start decoder");
    router.SetSurfaceRenderable(true);

    std::thread callback_thread([&router] { router.PostOutputBuffer(7, 11); });
    callback_thread.join();

    Expect(ops.freed.empty() && ops.rendered.empty(), "decoder callback performs no buffer operation");
    Expect(router.Drain(), "control thread drains queued outputs");
    Expect(ops.rendered == std::vector<std::uint32_t>{11}, "current output is rendered");
    Expect(router.stats().callbacks == 1, "output callback is counted");
    Expect(router.last_good_frame_visible(), "rendering establishes the Last Good Frame");
}

void RecoveryFreesQueuedAndLateEpochOutputsWithoutRendering()
{
    RecordingOutputOps ops;
    deskmate::gate3::RecoveryOutputRouter router(ops);
    router.BindControlThread();
    Expect(router.StartDecoder(41), "start initial decoder");
    router.SetSurfaceRenderable(true);
    router.PostOutputBuffer(41, 1);
    Expect(router.Drain(), "drain initial output");
    Expect(router.last_good_frame_visible(), "initial Last Good Frame is visible");

    Expect(router.BeginRecovery(41), "enter recovery on control thread");
    router.PostOutputBuffer(41, 2);
    router.PostOutputBuffer(41, 3);
    Expect(router.last_good_frame_visible(), "last good frame remains visible during recovery");
    Expect(router.Drain(), "drain recovery outputs");

    Expect(ops.rendered == std::vector<std::uint32_t>{1}, "no old epoch output is rendered");
    Expect(ops.freed == (std::vector<std::uint32_t>{2, 3}), "old epoch outputs are released");
    Expect(router.stats().freed_stale_epoch == 2, "stale epoch releases are reported");
    Expect(router.last_good_frame_visible(), "recovery does not clear Last Good Frame");

    Expect(router.OpenRecoveryOutput(41), "new IDR opens recovery output");
    router.PostOutputBuffer(41, 4);
    Expect(router.Drain(), "drain recovered output");
    Expect(ops.rendered == (std::vector<std::uint32_t>{1, 4}), "new epoch output is rendered");
    Expect(ops.freed == (std::vector<std::uint32_t>{2, 3}), "new epoch output is not released");
}

void DestroyedDecoderResidualCallbacksAreNotPassedToNative()
{
    RecordingOutputOps ops;
    deskmate::gate3::RecoveryOutputRouter router(ops);
    router.BindControlThread();
    Expect(router.StartDecoder(1), "start first decoder");
    router.SetSurfaceRenderable(true);
    router.PostOutputBuffer(1, 20);

    Expect(router.BeginRecovery(2), "replacement decoder starts a new recovery");
    Expect(router.OpenRecoveryOutput(2), "replacement decoder opens output");
    Expect(router.Drain(), "drain residual and replacement outputs");

    Expect(ops.freed.empty(), "destroyed decoder index is never passed to a new native instance");
    Expect(ops.rendered.empty(), "residual old decoder output is not rendered");
    Expect(router.stats().residual_decoder_callbacks == 1, "residual callback is measured");
}

void SurfaceLossStopsRenderingImmediately()
{
    RecordingOutputOps ops;
    deskmate::gate3::RecoveryOutputRouter router(ops);
    router.BindControlThread();
    Expect(router.StartDecoder(5), "start decoder");
    router.SetSurfaceRenderable(true);
    router.PostOutputBuffer(5, 30);
    Expect(router.Drain(), "render while surface exists");
    Expect(router.last_good_frame_visible(), "frame is visible");

    router.SetSurfaceRenderable(false);
    router.PostOutputBuffer(5, 31);
    Expect(router.Drain(), "drain after surface loss");
    Expect(ops.rendered == std::vector<std::uint32_t>{30}, "destroyed surface is not rendered");
    Expect(ops.freed == std::vector<std::uint32_t>{31}, "surface-bound output is released");
    Expect(!router.last_good_frame_visible(), "surface loss clears visible Last Good Frame");
}

void SurfaceCallbackInvalidatesEpochWithoutRunningCleanup()
{
    RecordingOutputOps ops;
    deskmate::gate3::RecoveryOutputRouter router(ops);
    router.BindControlThread();
    Expect(router.StartDecoder(8), "control thread starts decoder");
    router.SetSurfaceRenderable(true);
    router.PostOutputBuffer(8, 40);

    std::thread surface_callback([&router] { router.InvalidateSurfaceFromCallback(); });
    surface_callback.join();

    Expect(ops.freed.empty() && ops.rendered.empty(), "surface callback performs no buffer operation");
    Expect(router.Drain(), "control thread drains after surface callback");
    Expect(ops.freed == std::vector<std::uint32_t>{40}, "pre-loss output is released");
    Expect(ops.rendered.empty(), "pre-loss output is not rendered on a replaced surface");
    Expect(router.stats().freed_stale_epoch == 1, "surface callback advances the tagged epoch");
    Expect(!router.last_good_frame_visible(), "surface callback clears visible Last Good Frame");
    Expect(!router.output_open(), "surface callback closes output");
}

void LifecycleCommandsRejectCallbackThreads()
{
    RecordingOutputOps ops;
    deskmate::gate3::RecoveryOutputRouter router(ops);
    router.BindControlThread();
    Expect(router.StartDecoder(1), "control thread starts decoder");

    bool start_from_callback = true;
    bool recovery_from_callback = true;
    bool drain_from_callback = true;
    std::thread callback_thread([&] {
        start_from_callback = router.StartDecoder(2);
        recovery_from_callback = router.BeginRecovery(1);
        drain_from_callback = router.Drain();
    });
    callback_thread.join();

    Expect(!start_from_callback, "callback thread cannot replace decoder");
    Expect(!recovery_from_callback, "callback thread cannot begin recovery");
    Expect(!drain_from_callback, "callback thread cannot issue cleanup commands");
    Expect(router.recovery_epoch() == 1, "rejected commands do not advance recovery epoch");
    Expect(router.current_decoder() == 1, "rejected commands do not replace decoder");
}

void DrainReportsNativeOperationFailures()
{
    {
        RecordingOutputOps ops;
        deskmate::gate3::RecoveryOutputRouter router(ops);
        router.BindControlThread();
        Expect(router.StartDecoder(1), "start decoder before free failure");
        router.SetSurfaceRenderable(true);
        router.PostOutputBuffer(1, 50);
        ops.render_result = false;
        Expect(!router.Drain(), "render failure makes Drain fail");
        Expect(router.stats().render_failures == 1, "render failure is counted");
        Expect(!router.last_good_frame_visible(),
               "failed render does not publish a Last Good Frame");
    }
    {
        RecordingOutputOps ops;
        deskmate::gate3::RecoveryOutputRouter router(ops);
        router.BindControlThread();
        Expect(router.StartDecoder(2), "start decoder before free failure");
        router.SetSurfaceRenderable(true);
        Expect(router.BeginRecovery(2), "close output before stale callback");
        router.PostOutputBuffer(2, 51);
        ops.free_result = false;
        Expect(!router.Drain(), "free failure makes Drain fail");
        Expect(router.stats().free_failures == 1, "free failure is counted");
    }
}

void SurfaceLossDuringRenderFreesTheRejectedOutput()
{
    RecordingOutputOps ops;
    deskmate::gate3::RecoveryOutputRouter router(ops);
    router.BindControlThread();
    Expect(router.StartDecoder(9), "start decoder before in-gate Surface loss");
    router.SetSurfaceRenderable(true);
    router.PostOutputBuffer(9, 60);

    std::atomic<bool> render_started{false};
    ops.on_render = [&] {
        render_started.store(true, std::memory_order_release);
        while (router.surface_renderable()) {
            std::this_thread::yield();
        }
        return false;
    };
    std::thread surface_callback([&] {
        while (!render_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        router.InvalidateSurfaceFromCallback();
    });

    Expect(router.Drain(), "in-gate Surface loss is cleaned up");
    surface_callback.join();
    Expect(ops.rendered == std::vector<std::uint32_t>{60}, "render was attempted before loss");
    Expect(ops.freed == std::vector<std::uint32_t>{60},
           "failed output is freed after the render gate");
    Expect(router.stats().freed_surface_invalid == 1,
           "only the completed Surface-invalid free is counted");
    Expect(router.stats().render_failures == 0,
           "Surface-invalid output is not reported as a render failure");
}

void FailedFreeRemainsPendingForTheNextDrain()
{
    RecordingOutputOps ops;
    deskmate::gate3::RecoveryOutputRouter router(ops);
    router.BindControlThread();
    Expect(router.StartDecoder(10), "start decoder before retryable free");
    router.SetSurfaceRenderable(true);
    router.PostOutputBuffer(10, 70);
    router.SetSurfaceRenderable(false);

    ops.free_result = false;
    Expect(!router.Drain(), "failed free keeps Drain failed");
    Expect(router.stats().free_failures == 1, "failed free is counted");
    Expect(router.stats().freed_surface_invalid == 0,
           "failed free is not reported as a completed release");
    Expect(router.stats().pending_free_buffers == 1, "failed output is retained in the retry queue");

    ops.free_result = true;
    Expect(router.Drain(), "retry succeeds after the native free recovers");
    Expect(ops.freed == (std::vector<std::uint32_t>{70, 70}),
           "the failed output is retried instead of being lost");
    Expect(router.stats().freed_surface_invalid == 1,
           "retry counts the release only after it succeeds");
    Expect(router.stats().pending_free_buffers == 0, "successful retry drains the pending queue");
}

} // namespace

int main()
{
    CallbackOnlyQueuesAndControlThreadRenders();
    RecoveryFreesQueuedAndLateEpochOutputsWithoutRendering();
    DestroyedDecoderResidualCallbacksAreNotPassedToNative();
    SurfaceLossStopsRenderingImmediately();
    SurfaceCallbackInvalidatesEpochWithoutRunningCleanup();
    LifecycleCommandsRejectCallbackThreads();
    DrainReportsNativeOperationFailures();
    SurfaceLossDuringRenderFreesTheRejectedOutput();
    FailedFreeRemainsPendingForTheNextDrain();
    if (failures == 0) {
        std::puts("recovery output tests: PASS");
        return 0;
    }
    std::fprintf(stderr, "recovery output tests: %d failure(s)\n", failures);
    return 1;
}
