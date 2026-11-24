#include "gate3/lifecycle_controller.hpp"
#include "gate3/recovery_output.hpp"
#include "gate3/surface_lease.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
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

class FakeSurfaceOps final : public deskmate::gate3::NativeSurfaceOps {
public:
    std::int32_t Reference(void *) override
    {
        return 0;
    }

    std::int32_t Unreference(void *) override
    {
        return 0;
    }
};

class FakeOutputOps final : public deskmate::gate3::DecoderOutputOps {
public:
    bool FreeOutput(deskmate::gate3::DecoderInstanceId, std::uint32_t) override
    {
        ++freed;
        return true;
    }

    bool RenderOutput(deskmate::gate3::DecoderInstanceId, std::uint32_t) override
    {
        ++rendered;
        return true;
    }

    int freed = 0;
    int rendered = 0;
};

class FakeLifecycleOps final : public deskmate::gate3::DecoderLifecycleOps {
public:
    bool Flush() override
    {
        calls.push_back("flush");
        return fail_stage != "flush";
    }

    bool Stop() override
    {
        calls.push_back("stop");
        return fail_stage != "stop";
    }

    bool Reset() override
    {
        calls.push_back("reset");
        return fail_stage != "reset";
    }

    bool Configure() override
    {
        calls.push_back("configure");
        return fail_stage != "configure";
    }

    bool SetSurface(void *native_window) override
    {
        calls.push_back("set-surface");
        surfaces.push_back(native_window);
        return fail_stage != "set-surface";
    }

    bool Prepare() override
    {
        calls.push_back("prepare");
        return fail_stage != "prepare";
    }

    bool Start() override
    {
        calls.push_back("start");
        return fail_stage != "start";
    }

    bool DestroyAndCreate() override
    {
        calls.push_back("destroy-create");
        return fail_stage != "destroy-create";
    }

    bool SubmitParameterSetsAndIdr() override
    {
        calls.push_back("parameter-sets-idr");
        return fail_stage != "parameter-sets-idr";
    }

    std::vector<std::string> calls;
    std::vector<void *> surfaces;
    std::string fail_stage;
};

struct Fixture {
    Fixture()
        : coordinator(surface_ops),
          router(output_ops),
          lifecycle(ops, router, coordinator, [this] { return now_ns += 10; })
    {
        router.BindControlThread();
    }

    deskmate::gate3::SurfaceLease Lease()
    {
        const auto generation = coordinator.OnSurfaceCreated(reinterpret_cast<void *>(0x3000));
        const auto lease = coordinator.Acquire(generation);
        return *lease;
    }

    FakeSurfaceOps surface_ops;
    FakeOutputOps output_ops;
    FakeLifecycleOps ops;
    deskmate::gate3::SurfaceLeaseCoordinator coordinator;
    deskmate::gate3::RecoveryOutputRouter router;
    std::uint64_t now_ns = 0;
    deskmate::gate3::RecoveryLifecycleController lifecycle;
};

bool CallsEqual(const std::vector<std::string> &actual, const std::vector<std::string> &expected)
{
    return actual == expected;
}

void FlushRestartsAndResubmitsParameterSets()
{
    Fixture fixture;
    const auto lease = fixture.Lease();
    Expect(fixture.lifecycle.StartInitial(lease), "initial decoder starts");
    fixture.router.SetSurfaceRenderable(true);
    fixture.router.PostOutputBuffer(fixture.lifecycle.current_decoder(), 1);
    Expect(fixture.router.Drain(), "initial output renders");
    fixture.ops.calls.clear();

    const auto report = fixture.lifecycle.Recover(deskmate::gate3::RecoveryAction::Flush, lease);
    Expect(report.success, "flush recovery succeeds");
    Expect(CallsEqual(fixture.ops.calls, {"flush", "start", "parameter-sets-idr"}),
           "flush calls Start and resends parameter sets");
    Expect(report.parameter_sets_resubmitted, "flush reports parameter-set resend");
    Expect(report.surface_generation == lease.generation, "flush keeps the Surface Lease generation");
    Expect(report.duration_ns == 30, "flush records all measured stage durations");
    Expect(report.stages.size() == 3, "flush records one timing per stage");
    Expect(fixture.router.last_good_frame_visible(), "flush keeps the Last Good Frame");
}

void ResetRetainsLeaseAndReconfiguresSurface()
{
    Fixture fixture;
    const auto lease = fixture.Lease();
    Expect(fixture.lifecycle.StartInitial(lease), "initial decoder starts");
    fixture.ops.calls.clear();

    const auto report = fixture.lifecycle.Recover(deskmate::gate3::RecoveryAction::Reset, lease);
    Expect(report.success, "reset recovery succeeds");
    Expect(CallsEqual(fixture.ops.calls,
                      {"reset", "configure", "set-surface", "prepare", "start", "parameter-sets-idr"}),
           "reset reconfigures and restores the surface before starting");
    Expect(!fixture.ops.surfaces.empty() && fixture.ops.surfaces.front() == lease.native_window,
           "reset reuses the leased native window");
    Expect(report.surface_retained, "reset keeps the Surface Lease valid");
    Expect(report.parameter_sets_resubmitted, "reset resends parameter sets");
}

void StopAndRebuildUseTheirOwnRecoverySequences()
{
    Fixture fixture;
    const auto lease = fixture.Lease();
    Expect(fixture.lifecycle.StartInitial(lease), "initial decoder starts");

    fixture.ops.calls.clear();
    const auto stop = fixture.lifecycle.Recover(deskmate::gate3::RecoveryAction::Stop, lease);
    Expect(stop.success, "stop recovery succeeds");
    Expect(CallsEqual(fixture.ops.calls, {"stop", "start", "parameter-sets-idr"}),
           "stop restarts the same decoder and resends parameter sets");

    fixture.ops.calls.clear();
    fixture.ops.surfaces.clear();
    const auto rebuild = fixture.lifecycle.Recover(deskmate::gate3::RecoveryAction::Rebuild, lease);
    Expect(rebuild.success, "rebuild recovery succeeds");
    Expect(CallsEqual(fixture.ops.calls,
                      {"destroy-create", "configure", "set-surface", "prepare", "start", "parameter-sets-idr"}),
           "rebuild creates and fully configures a new decoder");
    Expect(fixture.ops.surfaces == std::vector<void *>{lease.native_window},
           "rebuild attaches the same leased native window");
    Expect(rebuild.surface_retained, "rebuild keeps the Surface Lease valid");
}

void FailedRecoveryKeepsOutputClosedAndLastGoodFrame()
{
    Fixture fixture;
    const auto lease = fixture.Lease();
    Expect(fixture.lifecycle.StartInitial(lease), "initial decoder starts");
    fixture.router.SetSurfaceRenderable(true);
    fixture.router.PostOutputBuffer(fixture.lifecycle.current_decoder(), 10);
    Expect(fixture.router.Drain(), "establish Last Good Frame");

    fixture.ops.fail_stage = "prepare";
    const auto report = fixture.lifecycle.Recover(deskmate::gate3::RecoveryAction::Reset, lease);
    Expect(!report.success, "failed reset is reported as failure");
    Expect(!fixture.router.output_open(), "failed recovery never opens output");
    Expect(fixture.router.last_good_frame_visible(), "failed recovery preserves Last Good Frame");
    fixture.router.PostOutputBuffer(fixture.lifecycle.current_decoder(), 11);
    Expect(fixture.router.Drain(), "drain output while recovery failed");
    Expect(fixture.output_ops.rendered == 1, "failed recovery does not render newer output");
    Expect(fixture.output_ops.freed == 1, "failed recovery releases newer output");
}

void ExpiredSurfaceRejectsRecoveryWithoutCallingDecoder()
{
    Fixture fixture;
    const auto lease = fixture.Lease();
    Expect(fixture.lifecycle.StartInitial(lease), "initial decoder starts");
    fixture.coordinator.OnSurfaceDestroyed(lease.generation);
    fixture.ops.calls.clear();

    const auto report = fixture.lifecycle.Recover(deskmate::gate3::RecoveryAction::Flush, lease);
    Expect(!report.success, "recovery with destroyed Surface fails");
    Expect(fixture.ops.calls.empty(), "destroyed Surface never reaches the decoder");
    Expect(!fixture.router.last_good_frame_visible(), "destroyed Surface has no visible frame");
}

} // namespace

int main()
{
    FlushRestartsAndResubmitsParameterSets();
    ResetRetainsLeaseAndReconfiguresSurface();
    StopAndRebuildUseTheirOwnRecoverySequences();
    FailedRecoveryKeepsOutputClosedAndLastGoodFrame();
    ExpiredSurfaceRejectsRecoveryWithoutCallingDecoder();
    if (failures == 0) {
        std::puts("lifecycle controller tests: PASS");
        return 0;
    }
    std::fprintf(stderr, "lifecycle controller tests: %d failure(s)\n", failures);
    return 1;
}
