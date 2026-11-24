#pragma once

#include "gate3/surface_lease.hpp"

#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace deskmate::gate3 {

struct ProbeCleanupOperations {
    std::function<bool()> stop_decoder;
    std::function<bool()> destroy_decoder;
    std::function<bool(SurfaceLeaseId)> release_surface;
    std::function<bool()> shutdown_surfaces;
};

struct PendingSurfaceRelease {
    SurfaceLease lease;
    std::string failure_stage;
};

class ProbeCleanupCoordinator {
public:
    explicit ProbeCleanupCoordinator(ProbeCleanupOperations operations = {});

    void RecordFailure(std::string stage);

    bool Failed() const;
    std::string FailureStage() const;
    bool EffectiveSuitePassed(bool suite_passed) const;
    const char *Verification(bool suite_done, bool suite_passed) const;

    bool ReleaseSurfaceLease(const SurfaceLease &lease, std::string failure_stage);
    std::vector<SurfaceLeaseId> RetryPendingSurfaceReleases();
    std::vector<PendingSurfaceRelease> PendingSurfaceReleases() const;
    bool HasPendingSurfaceRelease(SurfaceLeaseId lease_id) const;
    bool Shutdown(bool decoder_present, const std::optional<SurfaceLease> &lease);
    bool DecoderDestroyed() const;
    bool DecoderStopped() const;
    void ConfirmDecoderStopped();
    void DecoderStarted();

private:
    void RecordFailureLocked(std::string stage);

    ProbeCleanupOperations operations_;
    mutable std::mutex mutex_;
    bool failed_ = false;
    std::string failure_stage_;
    std::vector<PendingSurfaceRelease> pending_surface_releases_;
    bool decoder_stop_confirmed_ = false;
    bool decoder_destroy_confirmed_ = false;
    std::vector<SurfaceLeaseId> released_surface_lease_ids_;
    bool shutdown_complete_ = false;
};

} // namespace deskmate::gate3
