#include "gate3/probe_cleanup.hpp"

#include <cstdio>
#include <cstring>
#include <optional>
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

void CleanupFailureAfterSuiteCompletionFailsVerification()
{
    deskmate::gate3::ProbeCleanupCoordinator cleanup;
    Expect(cleanup.EffectiveSuitePassed(true), "clean suite completion remains passed");
    Expect(std::strcmp(cleanup.Verification(true, true), "passed") == 0,
           "clean suite reports passed verification");
    Expect(std::strcmp(cleanup.Verification(false, false), "not executed") == 0,
           "unfinished suite reports not executed");

    cleanup.RecordFailure("decoder-destroy");
    Expect(cleanup.Failed(), "cleanup failure is retained");
    Expect(cleanup.FailureStage() == "decoder-destroy",
           "cleanup failure reports the failed stage");
    Expect(!cleanup.EffectiveSuitePassed(true),
           "suite completion cannot hide a cleanup failure");
    Expect(std::strcmp(cleanup.Verification(true, true), "failed") == 0,
           "cleanup failure makes verification failed");
}

void FailedSurfaceReleaseRemainsPendingUntilRetry()
{
    bool release_succeeds = false;
    std::vector<deskmate::gate3::SurfaceLeaseId> attempts;
    deskmate::gate3::ProbeCleanupCoordinator cleanup(
        deskmate::gate3::ProbeCleanupOperations{
            .release_surface =
                [&](deskmate::gate3::SurfaceLeaseId lease_id) {
                    attempts.push_back(lease_id);
                    return release_succeeds;
                },
        });

    std::optional<deskmate::gate3::SurfaceLease> current =
        deskmate::gate3::SurfaceLease{11, 42, reinterpret_cast<void *>(0x4200)};
    Expect(!cleanup.ReleaseSurfaceLease(*current, "surface-destroyed"),
           "failed Surface release is reported");
    Expect(current.has_value(), "caller retains the current lease while release is pending");
    const auto pending = cleanup.PendingSurfaceReleases();
    Expect(pending.size() == 1 && pending.front().lease.id == 42,
           "failed lease ID remains in the pending cleanup queue");
    Expect(pending.front().failure_stage == "surface-destroyed",
           "pending release retains its failure stage");

    Expect(cleanup.RetryPendingSurfaceReleases().empty(),
           "failed retry leaves the lease pending");
    release_succeeds = true;
    const auto released = cleanup.RetryPendingSurfaceReleases();
    Expect(released == std::vector<deskmate::gate3::SurfaceLeaseId>{42},
           "successful retry reports the released lease ID");
    if (!released.empty() && current->id == released.front()) {
        current.reset();
    }
    Expect(!current.has_value(), "current lease is cleared only after a successful release");
    Expect(cleanup.PendingSurfaceReleases().empty(), "released lease leaves the pending queue");
    Expect(attempts == (std::vector<deskmate::gate3::SurfaceLeaseId>{42, 42, 42}),
           "release is attempted once and retried on every worker pass");
}

void DecoderDestroyFailureKeepsLeaseUntilRetry()
{
    bool stop_succeeds = false;
    bool destroy_succeeds = false;
    bool release_called = false;
    bool shutdown_called = false;
    std::vector<std::string> calls;
    deskmate::gate3::ProbeCleanupCoordinator cleanup(
        deskmate::gate3::ProbeCleanupOperations{
            .stop_decoder =
                [&] {
                    calls.push_back("stop");
                    return stop_succeeds;
                },
            .destroy_decoder =
                [&] {
                    calls.push_back("destroy");
                    return destroy_succeeds;
                },
            .release_surface =
                [&](deskmate::gate3::SurfaceLeaseId) {
                    calls.push_back("release");
                    release_called = true;
                    return true;
                },
            .shutdown_surfaces =
                [&] {
                    calls.push_back("shutdown");
                    shutdown_called = true;
                    return true;
                },
        });

    const std::optional<deskmate::gate3::SurfaceLease> lease =
        deskmate::gate3::SurfaceLease{12, 43, reinterpret_cast<void *>(0x4300)};
    Expect(!cleanup.Shutdown(true, lease), "failed decoder Stop blocks shutdown");
    Expect(!release_called && !shutdown_called,
           "Surface lease is retained while decoder Stop is pending");
    Expect(!cleanup.DecoderStopped(), "failed Stop is not recorded as confirmed");
    Expect(calls == (std::vector<std::string>{"stop"}), "Stop is attempted before Destroy");

    stop_succeeds = true;
    Expect(!cleanup.Shutdown(true, lease), "failed decoder Destroy blocks shutdown");
    Expect(cleanup.DecoderStopped(), "successful Stop is confirmed before Destroy retry");
    Expect(!release_called && !shutdown_called,
           "Surface lease is retained while decoder Destroy is pending");
    Expect(calls == (std::vector<std::string>{"stop", "stop", "destroy"}),
           "successful Stop is not repeated before retrying Destroy");

    destroy_succeeds = true;
    Expect(cleanup.Shutdown(true, lease), "shutdown completes after decoder Destroy succeeds");
    Expect(calls == (std::vector<std::string>{"stop", "stop", "destroy", "destroy", "release", "shutdown"}),
           "lease release and Surface shutdown happen only after Destroy succeeds");
    Expect(cleanup.DecoderDestroyed(), "decoder Destroy success is recorded");
    Expect(cleanup.FailureStage() == "decoder-stop", "the first cleanup failure stage is retained");

    cleanup.DecoderStarted();
    Expect(!cleanup.DecoderStopped(), "a restarted decoder requires Stop again before teardown");
    Expect(!cleanup.DecoderDestroyed(), "a restarted decoder requires Destroy again before teardown");
}

void ShutdownRetryDoesNotReleaseTheSameLeaseTwice()
{
    bool release_succeeds = false;
    int release_attempts = 0;
    int shutdown_calls = 0;
    deskmate::gate3::ProbeCleanupCoordinator cleanup(
        deskmate::gate3::ProbeCleanupOperations{
            .release_surface =
                [&](deskmate::gate3::SurfaceLeaseId) {
                    ++release_attempts;
                    return release_succeeds;
                },
            .shutdown_surfaces =
                [&] {
                    ++shutdown_calls;
                    return true;
                },
        });

    const std::optional<deskmate::gate3::SurfaceLease> lease =
        deskmate::gate3::SurfaceLease{13, 44, reinterpret_cast<void *>(0x4400)};
    Expect(!cleanup.Shutdown(false, lease), "failed lease release keeps shutdown incomplete");
    Expect(cleanup.PendingSurfaceReleases().size() == 1,
           "failed shutdown release is retryable");
    Expect(shutdown_calls == 0, "Surface shutdown waits for lease release");

    release_succeeds = true;
    Expect(cleanup.Shutdown(false, lease), "shutdown succeeds after pending release recovers");
    Expect(release_attempts == 2, "pending release is retried exactly once before shutdown");
    Expect(shutdown_calls == 1, "Surface shutdown runs after the retried release");
    Expect(cleanup.PendingSurfaceReleases().empty(), "successful shutdown drains pending cleanup");
}

} // namespace

int main()
{
    CleanupFailureAfterSuiteCompletionFailsVerification();
    FailedSurfaceReleaseRemainsPendingUntilRetry();
    DecoderDestroyFailureKeepsLeaseUntilRetry();
    ShutdownRetryDoesNotReleaseTheSameLeaseTwice();
    if (failures == 0) {
        std::puts("probe cleanup tests: PASS");
        return 0;
    }
    std::fprintf(stderr, "probe cleanup tests: %d failure(s)\n", failures);
    return 1;
}
