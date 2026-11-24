#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <vector>

namespace deskmate::gate3 {

using SurfaceGeneration = std::uint64_t;
using SurfaceLeaseId = std::uint64_t;

class NativeSurfaceOps {
public:
    virtual ~NativeSurfaceOps() = default;

    // OH_NativeWindow_NativeObjectReference/Unreference are not thread-safe.
    virtual std::int32_t Reference(void *object) = 0;
    virtual std::int32_t Unreference(void *object) = 0;
};

struct SurfaceLease {
    SurfaceGeneration generation = 0;
    SurfaceLeaseId id = 0;
    void *native_window = nullptr;
};

class SurfaceLeaseCoordinator {
public:
    explicit SurfaceLeaseCoordinator(NativeSurfaceOps &operations);
    ~SurfaceLeaseCoordinator();

    SurfaceLeaseCoordinator(const SurfaceLeaseCoordinator &) = delete;
    SurfaceLeaseCoordinator &operator=(const SurfaceLeaseCoordinator &) = delete;

    // Creation is serialized with every reference operation. Destruction only
    // invalidates the generation; the already-held owner reference is returned
    // later from the control thread.
    SurfaceGeneration OnSurfaceCreated(void *native_window);
    void OnSurfaceDestroyed(SurfaceGeneration generation);

    std::optional<SurfaceLease> Acquire(SurfaceGeneration expected_generation);
    bool Release(SurfaceLeaseId lease_id);

    // Returns the owner reference for a destroyed generation after all leases
    // from that generation have been released.
    bool CollectRetiredSurface(SurfaceGeneration generation);

    bool CanRender(SurfaceGeneration generation) const;
    bool CanRender(const SurfaceLease &lease) const;

    // Safe point after decoder output is drained. Returns all outstanding native
    // references exactly once.
    void Shutdown();

    SurfaceGeneration active_generation() const;

private:
    struct OwnedWindow {
        void *native_window = nullptr;
        SurfaceGeneration generation = 0;
    };

    struct LeaseRecord {
        SurfaceLeaseId id = 0;
        SurfaceGeneration generation = 0;
        void *native_window = nullptr;
    };

    NativeSurfaceOps &operations_;
    mutable std::mutex mutex_;
    std::vector<OwnedWindow> owner_references_;
    std::vector<LeaseRecord> leases_;
    SurfaceGeneration next_generation_ = 1;
    SurfaceLeaseId next_lease_id_ = 1;
    SurfaceGeneration active_generation_ = 0;
    bool active_surface_valid_ = false;
    bool shutdown_ = false;
};

} // namespace deskmate::gate3
