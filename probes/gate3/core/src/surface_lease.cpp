#include "gate3/surface_lease.hpp"

#include <algorithm>
#include <utility>

namespace deskmate::gate3 {

SurfaceLeaseCoordinator::SurfaceLeaseCoordinator(NativeSurfaceOps &operations) : operations_(operations) {}

SurfaceLeaseCoordinator::~SurfaceLeaseCoordinator()
{
    Shutdown();
}

SurfaceGeneration SurfaceLeaseCoordinator::OnSurfaceCreated(void *native_window)
{
    if (native_window == nullptr) {
        return 0;
    }

    std::lock_guard lock(mutex_);
    if (shutdown_) {
        return 0;
    }
    if (operations_.Reference(native_window) != 0) {
        return 0;
    }

    if (active_surface_valid_) {
        active_surface_valid_ = false;
    }
    active_generation_ = next_generation_++;
    owner_references_.push_back({native_window, active_generation_});
    active_surface_valid_ = true;
    return active_generation_;
}

void SurfaceLeaseCoordinator::OnSurfaceDestroyed(SurfaceGeneration generation)
{
    std::lock_guard lock(mutex_);
    if (generation != 0 && generation == active_generation_) {
        active_surface_valid_ = false;
    }
}

std::optional<SurfaceLease> SurfaceLeaseCoordinator::Acquire(SurfaceGeneration expected_generation)
{
    std::lock_guard lock(mutex_);
    if (shutdown_ || !active_surface_valid_ || expected_generation != active_generation_) {
        return std::nullopt;
    }

    auto owner = std::find_if(owner_references_.begin(), owner_references_.end(),
                              [expected_generation](const OwnedWindow &window) {
                                  return window.generation == expected_generation;
                              });
    if (owner == owner_references_.end()) {
        return std::nullopt;
    }
    if (operations_.Reference(owner->native_window) != 0) {
        return std::nullopt;
    }

    const SurfaceLeaseId lease_id = next_lease_id_++;
    leases_.push_back({lease_id, expected_generation, owner->native_window});
    return SurfaceLease{expected_generation, lease_id, owner->native_window};
}

bool SurfaceLeaseCoordinator::Release(SurfaceLeaseId lease_id)
{
    std::lock_guard lock(mutex_);
    const auto lease = std::find_if(leases_.begin(), leases_.end(), [lease_id](const LeaseRecord &record) {
        return record.id == lease_id;
    });
    if (lease == leases_.end()) {
        return false;
    }

    void *const native_window = lease->native_window;
    leases_.erase(lease);
    return operations_.Unreference(native_window) == 0;
}

bool SurfaceLeaseCoordinator::CollectRetiredSurface(SurfaceGeneration generation)
{
    std::lock_guard lock(mutex_);
    if (generation == 0 || (active_surface_valid_ && generation == active_generation_)) {
        return false;
    }
    if (std::any_of(leases_.begin(), leases_.end(),
                    [generation](const LeaseRecord &lease) { return lease.generation == generation; })) {
        return false;
    }
    const auto owner = std::find_if(owner_references_.begin(), owner_references_.end(),
                                    [generation](const OwnedWindow &window) {
                                        return window.generation == generation;
                                    });
    if (owner == owner_references_.end()) {
        return false;
    }

    void *const native_window = owner->native_window;
    owner_references_.erase(owner);
    return operations_.Unreference(native_window) == 0;
}

bool SurfaceLeaseCoordinator::CanRender(SurfaceGeneration generation) const
{
    std::lock_guard lock(mutex_);
    return !shutdown_ && active_surface_valid_ && generation != 0 && generation == active_generation_;
}

bool SurfaceLeaseCoordinator::CanRender(const SurfaceLease &lease) const
{
    std::lock_guard lock(mutex_);
    if (shutdown_ || !active_surface_valid_ || lease.generation != active_generation_) {
        return false;
    }
    return std::any_of(leases_.begin(), leases_.end(), [&lease](const LeaseRecord &record) {
        return record.id == lease.id && record.generation == lease.generation;
    });
}

void SurfaceLeaseCoordinator::Shutdown()
{
    std::lock_guard lock(mutex_);
    if (shutdown_) {
        return;
    }
    shutdown_ = true;
    active_surface_valid_ = false;
    for (const auto &lease : leases_) {
        (void)operations_.Unreference(lease.native_window);
    }
    for (const auto &owner : owner_references_) {
        (void)operations_.Unreference(owner.native_window);
    }
    leases_.clear();
    owner_references_.clear();
}

SurfaceGeneration SurfaceLeaseCoordinator::active_generation() const
{
    std::lock_guard lock(mutex_);
    return active_surface_valid_ ? active_generation_ : 0;
}

} // namespace deskmate::gate3
