#include "gate3/surface_lease.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
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

class RecordingSurfaceOps final : public deskmate::gate3::NativeSurfaceOps {
public:
    int32_t Reference(void *object) override
    {
        ++reference_calls;
        if (object == expected_object) {
            ++expected_references;
        }
        return 0;
    }

    int32_t Unreference(void *object) override
    {
        ++unreference_calls;
        if (object == expected_object && expected_references > 0) {
            --expected_references;
        }
        return 0;
    }

    void *expected_object = reinterpret_cast<void *>(0x1000);
    int reference_calls = 0;
    int unreference_calls = 0;
    int expected_references = 0;
};

void SurfaceReferenceIsPairedAndDestroyStopsRendering()
{
    RecordingSurfaceOps ops;
    deskmate::gate3::SurfaceLeaseCoordinator coordinator(ops);
    const auto generation = coordinator.OnSurfaceCreated(ops.expected_object);

    Expect(generation != 0, "surface creation returns a non-zero generation");
    Expect(ops.expected_references == 1, "owner takes one surface reference");
    Expect(coordinator.CanRender(generation), "created surface is renderable");

    const auto lease = coordinator.Acquire(generation);
    Expect(lease.has_value(), "active surface can be leased");
    Expect(ops.expected_references == 2, "lease takes its own reference");
    Expect(coordinator.CanRender(*lease), "valid lease is renderable");

    coordinator.OnSurfaceDestroyed(generation);
    Expect(!coordinator.CanRender(generation), "destroyed surface is not renderable");
    Expect(!coordinator.CanRender(*lease), "existing lease stops rendering with its surface");
    Expect(!coordinator.Acquire(generation).has_value(), "destroyed surface cannot be leased");
    Expect(ops.expected_references == 2, "destruction does not touch the native reference");

    Expect(coordinator.Release(lease->id), "lease release succeeds once");
    Expect(!coordinator.Release(lease->id), "duplicate lease release is ignored");
    coordinator.Shutdown();
    Expect(ops.expected_references == 0, "owner and lease references are paired");
}

void StaleLeaseCannotRenderOnReplacementSurface()
{
    RecordingSurfaceOps ops;
    deskmate::gate3::SurfaceLeaseCoordinator coordinator(ops);
    const auto first_generation = coordinator.OnSurfaceCreated(ops.expected_object);
    const auto first_lease = coordinator.Acquire(first_generation);
    Expect(first_lease.has_value(), "first surface can be leased");

    coordinator.OnSurfaceDestroyed(first_generation);
    const auto second_generation = coordinator.OnSurfaceCreated(ops.expected_object);
    Expect(second_generation != first_generation, "replacement surface has a new generation");
    Expect(!coordinator.CanRender(*first_lease), "old lease cannot render on replacement surface");

    Expect(coordinator.Release(first_lease->id), "old lease can be released");
    coordinator.Shutdown();
}

void DestroyedSurfaceReferenceIsCollectedAfterLeaseDrain()
{
    RecordingSurfaceOps ops;
    deskmate::gate3::SurfaceLeaseCoordinator coordinator(ops);
    const auto first_generation = coordinator.OnSurfaceCreated(ops.expected_object);
    const auto first_lease = coordinator.Acquire(first_generation);
    coordinator.OnSurfaceDestroyed(first_generation);
    const auto second_generation = coordinator.OnSurfaceCreated(ops.expected_object);
    const auto second_lease = coordinator.Acquire(second_generation);
    Expect(first_lease.has_value() && second_lease.has_value(), "both surfaces have leases");
    Expect(!coordinator.CollectRetiredSurface(first_generation),
           "retired surface reference is not collected while a lease remains");

    Expect(coordinator.Release(first_lease->id), "old lease releases");
    Expect(coordinator.CollectRetiredSurface(first_generation),
           "retired surface reference is collected after lease drain");
    Expect(!coordinator.CollectRetiredSurface(first_generation),
           "retired surface reference is collected only once");
    Expect(coordinator.CanRender(*second_lease), "replacement surface remains renderable");

    coordinator.Shutdown();
    Expect(ops.expected_references == 0, "replacement references remain paired");
}

class SerializedSurfaceOps final : public deskmate::gate3::NativeSurfaceOps {
public:
    int32_t Reference(void *) override
    {
        TrackCall();
        return 0;
    }

    int32_t Unreference(void *) override
    {
        TrackCall();
        return 0;
    }

    std::atomic<int> max_concurrent_calls{0};

private:
    void TrackCall()
    {
        const int active = active_calls.fetch_add(1) + 1;
        int observed = max_concurrent_calls.load();
        while (active > observed && !max_concurrent_calls.compare_exchange_weak(observed, active)) {
        }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
        active_calls.fetch_sub(1);
    }

    std::atomic<int> active_calls{0};
};

void SurfaceReferenceOperationsAreSerialized()
{
    SerializedSurfaceOps ops;
    deskmate::gate3::SurfaceLeaseCoordinator coordinator(ops);
    const auto generation = coordinator.OnSurfaceCreated(reinterpret_cast<void *>(0x2000));

    constexpr int thread_count = 6;
    constexpr int iterations = 40;
    std::vector<std::thread> threads;
    threads.reserve(thread_count);
    for (int thread_index = 0; thread_index < thread_count; ++thread_index) {
        threads.emplace_back([&coordinator, generation] {
            for (int iteration = 0; iteration < iterations; ++iteration) {
                const auto lease = coordinator.Acquire(generation);
                if (lease.has_value()) {
                    (void)coordinator.Release(lease->id);
                }
            }
        });
    }
    for (auto &thread : threads) {
        thread.join();
    }

    Expect(ops.max_concurrent_calls == 1, "surface reference calls never overlap");
    coordinator.OnSurfaceDestroyed(generation);
    coordinator.Shutdown();
}

} // namespace

int main()
{
    SurfaceReferenceIsPairedAndDestroyStopsRendering();
    StaleLeaseCannotRenderOnReplacementSurface();
    DestroyedSurfaceReferenceIsCollectedAfterLeaseDrain();
    SurfaceReferenceOperationsAreSerialized();
    if (failures == 0) {
        std::puts("surface lease tests: PASS");
        return 0;
    }
    std::fprintf(stderr, "surface lease tests: %d failure(s)\n", failures);
    return 1;
}
