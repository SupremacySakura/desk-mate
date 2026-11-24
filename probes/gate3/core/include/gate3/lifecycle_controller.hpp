#pragma once

#include "gate3/recovery_output.hpp"
#include "gate3/surface_lease.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace deskmate::gate3 {

enum class RecoveryAction {
    Flush,
    Stop,
    Reset,
    Rebuild,
};

struct StageMeasurement {
    std::string stage;
    std::uint64_t duration_ns = 0;
    bool ok = false;
};

struct RecoveryReport {
    RecoveryAction action = RecoveryAction::Flush;
    bool success = false;
    bool surface_retained = false;
    bool parameter_sets_resubmitted = false;
    std::uint64_t duration_ns = 0;
    SurfaceGeneration surface_generation = 0;
    DecoderInstanceId decoder = 0;
    std::string failure_stage;
    std::vector<StageMeasurement> stages;
};

class DecoderLifecycleOps {
public:
    virtual ~DecoderLifecycleOps() = default;

    virtual bool Flush() = 0;
    virtual bool Stop() = 0;
    virtual bool Reset() = 0;
    virtual bool Configure() = 0;
    virtual bool SetSurface(void *native_window) = 0;
    virtual bool Prepare() = 0;
    virtual bool Start() = 0;
    virtual bool DestroyAndCreate() = 0;
    virtual bool SubmitParameterSetsAndIdr() = 0;
};

class RecoveryLifecycleController {
public:
    using Clock = std::function<std::uint64_t()>;

    RecoveryLifecycleController(DecoderLifecycleOps &operations, RecoveryOutputRouter &router,
                                SurfaceLeaseCoordinator &surfaces, Clock clock);

    RecoveryLifecycleController(const RecoveryLifecycleController &) = delete;
    RecoveryLifecycleController &operator=(const RecoveryLifecycleController &) = delete;

    bool StartInitial(const SurfaceLease &lease);
    RecoveryReport Recover(RecoveryAction action, const SurfaceLease &lease);

    DecoderInstanceId current_decoder() const;

private:
    bool RunStage(RecoveryReport &report, const std::string &name,
                  const std::function<bool()> &operation);

    DecoderLifecycleOps &operations_;
    RecoveryOutputRouter &router_;
    SurfaceLeaseCoordinator &surfaces_;
    Clock clock_;
    DecoderInstanceId current_decoder_ = 0;
    DecoderInstanceId next_decoder_ = 1;
};

const char *RecoveryActionName(RecoveryAction action);

} // namespace deskmate::gate3
