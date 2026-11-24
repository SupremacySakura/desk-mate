#include "gate3/probe_cleanup.hpp"

#include <algorithm>
#include <utility>

namespace deskmate::gate3 {

ProbeCleanupCoordinator::ProbeCleanupCoordinator(ProbeCleanupOperations operations)
    : operations_(std::move(operations))
{
}

void ProbeCleanupCoordinator::RecordFailure(std::string stage)
{
    std::lock_guard lock(mutex_);
    RecordFailureLocked(std::move(stage));
}

void ProbeCleanupCoordinator::RecordFailureLocked(std::string stage)
{
    failed_ = true;
    if (failure_stage_.empty()) {
        failure_stage_ = std::move(stage);
    }
}

bool ProbeCleanupCoordinator::Failed() const
{
    std::lock_guard lock(mutex_);
    return failed_;
}

std::string ProbeCleanupCoordinator::FailureStage() const
{
    std::lock_guard lock(mutex_);
    return failure_stage_;
}

bool ProbeCleanupCoordinator::EffectiveSuitePassed(bool suite_passed) const
{
    std::lock_guard lock(mutex_);
    return suite_passed && !failed_;
}

const char *ProbeCleanupCoordinator::Verification(bool suite_done, bool suite_passed) const
{
    std::lock_guard lock(mutex_);
    if (!suite_done) {
        return "not executed";
    }
    return suite_passed && !failed_ ? "passed" : "failed";
}

bool ProbeCleanupCoordinator::ReleaseSurfaceLease(const SurfaceLease &lease, std::string failure_stage)
{
    const std::string stage = failure_stage;
    {
        std::lock_guard lock(mutex_);
        const auto pending = std::find_if(
            pending_surface_releases_.begin(), pending_surface_releases_.end(),
            [&lease](const PendingSurfaceRelease &release) { return release.lease.id == lease.id; });
        if (pending == pending_surface_releases_.end()) {
            pending_surface_releases_.push_back({lease, std::move(failure_stage)});
        }
    }

    const bool released = operations_.release_surface && operations_.release_surface(lease.id);
    if (released) {
        std::lock_guard lock(mutex_);
        std::erase_if(pending_surface_releases_, [&lease](const PendingSurfaceRelease &release) {
            return release.lease.id == lease.id;
        });
        released_surface_lease_ids_.push_back(lease.id);
        return true;
    }
    RecordFailure(stage);
    return false;
}

std::vector<SurfaceLeaseId> ProbeCleanupCoordinator::RetryPendingSurfaceReleases()
{
    std::vector<PendingSurfaceRelease> pending;
    {
        std::lock_guard lock(mutex_);
        pending = pending_surface_releases_;
    }

    std::vector<SurfaceLeaseId> released;
    for (const PendingSurfaceRelease &release : pending) {
        if (operations_.release_surface && operations_.release_surface(release.lease.id)) {
            released.push_back(release.lease.id);
            std::lock_guard lock(mutex_);
            std::erase_if(pending_surface_releases_, [&release](const PendingSurfaceRelease &item) {
                return item.lease.id == release.lease.id;
            });
            released_surface_lease_ids_.push_back(release.lease.id);
        } else {
            RecordFailure(release.failure_stage);
        }
    }
    return released;
}

std::vector<PendingSurfaceRelease> ProbeCleanupCoordinator::PendingSurfaceReleases() const
{
    std::lock_guard lock(mutex_);
    return pending_surface_releases_;
}

bool ProbeCleanupCoordinator::HasPendingSurfaceRelease(SurfaceLeaseId lease_id) const
{
    std::lock_guard lock(mutex_);
    return std::any_of(pending_surface_releases_.begin(), pending_surface_releases_.end(),
                       [lease_id](const PendingSurfaceRelease &release) {
                           return release.lease.id == lease_id;
                       });
}

bool ProbeCleanupCoordinator::Shutdown(bool decoder_present, const std::optional<SurfaceLease> &lease)
{
    {
        std::lock_guard lock(mutex_);
        if (shutdown_complete_) {
            return true;
        }
    }

    if (decoder_present) {
        bool stop_confirmed = false;
        bool destroy_confirmed = false;
        {
            std::lock_guard lock(mutex_);
            stop_confirmed = decoder_stop_confirmed_;
            destroy_confirmed = decoder_destroy_confirmed_;
        }
        if (!destroy_confirmed) {
            if (!stop_confirmed) {
                if (!operations_.stop_decoder || !operations_.stop_decoder()) {
                    RecordFailure("decoder-stop");
                    return false;
                }
                std::lock_guard lock(mutex_);
                decoder_stop_confirmed_ = true;
            }
            if (!operations_.destroy_decoder || !operations_.destroy_decoder()) {
                RecordFailure("decoder-destroy");
                return false;
            }
            std::lock_guard lock(mutex_);
            decoder_destroy_confirmed_ = true;
        }
    }

    (void)RetryPendingSurfaceReleases();
    {
        std::lock_guard lock(mutex_);
        if (!pending_surface_releases_.empty()) {
            return false;
        }
    }

    if (lease.has_value()) {
        bool already_released = false;
        {
            std::lock_guard lock(mutex_);
            already_released =
                std::find(released_surface_lease_ids_.begin(), released_surface_lease_ids_.end(),
                          lease->id) != released_surface_lease_ids_.end();
        }
        if (!already_released) {
            if (!ReleaseSurfaceLease(*lease, "shutdown surface lease release")) {
                return false;
            }
        }
    }

    if (!operations_.shutdown_surfaces || !operations_.shutdown_surfaces()) {
        RecordFailure("surface-shutdown");
        return false;
    }
    {
        std::lock_guard lock(mutex_);
        shutdown_complete_ = true;
    }
    return true;
}

bool ProbeCleanupCoordinator::DecoderDestroyed() const
{
    std::lock_guard lock(mutex_);
    return decoder_destroy_confirmed_;
}

bool ProbeCleanupCoordinator::DecoderStopped() const
{
    std::lock_guard lock(mutex_);
    return decoder_stop_confirmed_;
}

void ProbeCleanupCoordinator::ConfirmDecoderStopped()
{
    std::lock_guard lock(mutex_);
    decoder_stop_confirmed_ = true;
}

void ProbeCleanupCoordinator::DecoderStarted()
{
    std::lock_guard lock(mutex_);
    decoder_stop_confirmed_ = false;
    decoder_destroy_confirmed_ = false;
}

} // namespace deskmate::gate3
