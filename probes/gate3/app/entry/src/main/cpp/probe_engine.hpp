#pragma once

#include "gate3/annexb.hpp"
#include "gate3/lifecycle_controller.hpp"
#include "gate3/recovery_output.hpp"
#include "gate3/surface_lease.hpp"
#include "ohos_decoder.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace deskmate::gate3::probe {

class ProbeEngine final : public OhosDecoderSink {
public:
    static ProbeEngine &Instance();

    ProbeEngine(const ProbeEngine &) = delete;
    ProbeEngine &operator=(const ProbeEngine &) = delete;
    ~ProbeEngine() override;

    bool LoadStream(std::vector<std::uint8_t> bytes);
    bool SurfaceCreated(const std::string &surface_id);
    bool SurfaceDestroyed(const std::string &surface_id);
    bool RunSuite();
    bool BeginFreeze();
    bool RecordLastGoodFrame(bool stable);
    bool ResumeAfterFreeze();
    bool SetDeviceInfo(std::string json);
    std::string SnapshotJson() const;

    void OnInputReady(const InputReadyEvent &event) override;
    void OnOutputReady(DecoderInstanceId decoder, std::uint32_t index) override;
    void OnDecoderError(const ErrorEvent &event) override;
    void OnStreamChanged(const StreamChangedEvent &event) override;

private:
    ProbeEngine();

    struct Capability;

    void WorkerLoop();
    void ProcessCallbacks();
    void ProcessSurfaceEvents();
    bool RunSuiteOnWorker();
    bool RunInitialDecode(const SurfaceLease &lease);
    bool DecodeRecovery(const SurfaceLease &lease, RecoveryAction action);
    bool PumpUntil(const std::function<bool()> &predicate, std::uint64_t timeout_ms);
    bool SurfaceStillValid(const SurfaceLease &lease);
    bool EnumerateCapabilities();
    Capability ReadCapability(const char *mime);
    void HandleSurfaceRebuild(const SurfaceLease &lease, SurfaceGeneration previous_generation);
    void SetStage(std::string stage);
    void SetTerminalFailure(std::string failure);
    void CleanupSurfaceIfInvalid();
    std::optional<SurfaceLease> CurrentLease() const;
    std::string BuildReportJson() const;
    std::string CapabilitiesJson() const;
    std::string RecoveriesJson() const;
    std::string DeviceJson() const;
    void NotifyWorker();

    struct Capability {
        std::string mime;
        bool present = false;
        bool hardware = false;
        bool low_latency = false;
        std::string name;
        std::int32_t min_width = 0;
        std::int32_t max_width = 0;
        std::int32_t min_height = 0;
        std::int32_t max_height = 0;
        std::vector<std::int32_t> pixel_formats;
        std::vector<std::int32_t> profiles;
    };

    struct RecoveryMeasurement {
        std::string action;
        bool operation_success = false;
        bool first_output_observed = false;
        bool surface_retained = false;
        bool surface_set_confirmed = false;
        bool parameter_sets_resubmitted = false;
        bool parameter_set_unit_observed = false;
        bool decoder_instance_matched = false;
        bool last_good_frame_state_stable = false;
        std::uint64_t operation_duration_ns = 0;
        std::uint64_t first_output_latency_ns = 0;
        std::uint64_t residual_callbacks = 0;
        std::uint64_t stale_epoch_frees = 0;
        std::uint64_t input_buffers_pushed = 0;
        std::uint64_t parameter_set_units_pushed = 0;
        std::uint64_t stale_input_callbacks = 0;
        DecoderInstanceId decoder = 0;
        SurfaceGeneration surface_generation = 0;
        std::string failure_stage;
        std::vector<StageMeasurement> stages;
    };

    struct SurfaceEvent {
        bool created = false;
        std::uint64_t window_key = 0;
        SurfaceGeneration generation = 0;
        void *native_window = nullptr;
    };

    class OhosSurfaceOps final : public NativeSurfaceOps {
    public:
        std::int32_t Reference(void *object) override;
        std::int32_t Unreference(void *object) override;
    };

    OhosSurfaceOps surface_ops_;
    SurfaceLeaseCoordinator surfaces_;
    OhosDecoder decoder_;
    RecoveryOutputRouter router_;
    RecoveryLifecycleController lifecycle_;

    mutable std::mutex snapshot_mutex_;
    std::string stage_ = "waiting for stream and surface";
    std::string failure_;
    std::string report_json_;
    std::string device_json_ = "{}";
    bool suite_running_ = false;
    bool suite_done_ = false;
    bool suite_passed_ = false;
    bool surface_ready_ = false;
    bool decoder_needs_rebuild_ = false;
    bool stream_loaded_ = false;
    bool freeze_requested_ = false;
    bool resume_requested_ = false;
    bool manual_frozen_ = false;
    std::string manual_lgf_ = "pending";
    std::optional<SurfaceLease> current_lease_;
    std::uint64_t current_surface_id_ = 0;
    SurfaceGeneration current_surface_generation_ = 0;
    void *current_surface_window_ = nullptr;
    std::vector<Capability> capabilities_;
    std::optional<AnnexBStream> stream_;
    std::vector<RecoveryMeasurement> recoveries_;
    bool initial_decode_passed_ = false;
    std::uint64_t initial_rendered_ = 0;
    std::uint64_t initial_feed_count_ = 0;
    std::uint64_t initial_duration_ns_ = 0;
    std::uint64_t initial_parameter_set_units_ = 0;
    std::atomic<std::uint64_t> callback_count_{0};
    std::atomic<bool> callback_off_control_thread_{false};
    std::atomic<bool> worker_shutdown_{false};
    std::atomic<std::int32_t> last_decoder_error_{0};
    std::string stream_change_summary_;
    std::string stream_parse_error_;
    std::thread::id worker_thread_id_;

    mutable std::mutex callback_mutex_;
    std::deque<InputReadyEvent> input_events_;
    std::deque<ErrorEvent> error_events_;
    std::deque<StreamChangedEvent> stream_changes_;

    mutable std::mutex surface_mutex_;
    std::deque<SurfaceEvent> surface_events_;
    std::unordered_map<std::uint64_t, void *> surface_windows_;
    std::unordered_map<std::uint64_t, SurfaceGeneration> surface_generations_;

    mutable std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    bool suite_requested_ = false;
    bool store_shutdown_ = false;
    std::thread worker_;
};

} // namespace deskmate::gate3::probe
