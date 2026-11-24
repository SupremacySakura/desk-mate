#include "gate3/lifecycle_controller.hpp"

#include <utility>

namespace deskmate::gate3 {

RecoveryLifecycleController::RecoveryLifecycleController(DecoderLifecycleOps &operations,
                                                         RecoveryOutputRouter &router,
                                                         SurfaceLeaseCoordinator &surfaces, Clock clock)
    : operations_(operations), router_(router), surfaces_(surfaces), clock_(std::move(clock))
{
}

bool RecoveryLifecycleController::StartInitial(const SurfaceLease &lease)
{
    if (!surfaces_.CanRender(lease)) {
        return false;
    }
    current_decoder_ = next_decoder_++;
    if (!router_.StartDecoder(current_decoder_)) {
        current_decoder_ = 0;
        return false;
    }
    return router_.SetSurfaceRenderable(true);
}

RecoveryReport RecoveryLifecycleController::Recover(RecoveryAction action, const SurfaceLease &lease)
{
    RecoveryReport report;
    report.action = action;
    report.surface_generation = lease.generation;

    if (!surfaces_.CanRender(lease)) {
        (void)router_.SetSurfaceRenderable(false);
        report.failure_stage = "surface-lease";
        return report;
    }

    const DecoderInstanceId decoder =
        action == RecoveryAction::Rebuild ? next_decoder_++ : current_decoder_;
    if (decoder == 0 || !router_.BeginRecovery(decoder)) {
        report.failure_stage = "control-thread";
        current_decoder_ = decoder;
        return report;
    }
    current_decoder_ = decoder;
    report.decoder = decoder;

    const auto run = [this, &report](const std::string &name, const std::function<bool()> &operation) {
        return RunStage(report, name, operation);
    };

    bool ok = true;
    switch (action) {
    case RecoveryAction::Flush:
        ok = run("flush", [this] { return operations_.Flush(); });
        ok = ok && run("start", [this] { return operations_.Start(); });
        break;
    case RecoveryAction::Stop:
        ok = run("stop", [this] { return operations_.Stop(); });
        ok = ok && run("start", [this] { return operations_.Start(); });
        break;
    case RecoveryAction::Reset:
        ok = run("reset", [this] { return operations_.Reset(); });
        ok = ok && run("configure", [this] { return operations_.Configure(); });
        ok = ok && run("set-surface", [this, &lease] { return operations_.SetSurface(lease.native_window); });
        ok = ok && run("prepare", [this] { return operations_.Prepare(); });
        ok = ok && run("start", [this] { return operations_.Start(); });
        break;
    case RecoveryAction::Rebuild:
        ok = run("destroy-create", [this] { return operations_.DestroyAndCreate(); });
        ok = ok && run("configure", [this] { return operations_.Configure(); });
        ok = ok && run("set-surface", [this, &lease] { return operations_.SetSurface(lease.native_window); });
        ok = ok && run("prepare", [this] { return operations_.Prepare(); });
        ok = ok && run("start", [this] { return operations_.Start(); });
        break;
    }

    ok = ok && run("parameter-sets-idr", [this] { return operations_.SubmitParameterSetsAndIdr(); });
    report.parameter_sets_resubmitted = ok;
    report.surface_retained = surfaces_.CanRender(lease);
    report.success = ok && report.surface_retained;
    if (report.success) {
        report.success = router_.OpenRecoveryOutput(decoder);
    }
    if (!report.success && report.failure_stage.empty()) {
        report.failure_stage = !report.surface_retained ? "surface-lease-lost" : "open-output";
    }
    for (const auto &stage : report.stages) {
        report.duration_ns += stage.duration_ns;
    }
    return report;
}

DecoderInstanceId RecoveryLifecycleController::current_decoder() const
{
    return current_decoder_;
}

bool RecoveryLifecycleController::RunStage(RecoveryReport &report, const std::string &name,
                                           const std::function<bool()> &operation)
{
    const std::uint64_t started = clock_();
    const bool ok = operation();
    const std::uint64_t finished = clock_();
    report.stages.push_back({name, finished - started, ok});
    if (!ok && report.failure_stage.empty()) {
        report.failure_stage = name;
    }
    return ok;
}

const char *RecoveryActionName(RecoveryAction action)
{
    switch (action) {
    case RecoveryAction::Flush:
        return "flush";
    case RecoveryAction::Stop:
        return "stop";
    case RecoveryAction::Reset:
        return "reset";
    case RecoveryAction::Rebuild:
        return "rebuild";
    }
    return "unknown";
}

} // namespace deskmate::gate3
