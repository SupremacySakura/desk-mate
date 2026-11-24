#include "probe_engine.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <utility>

#include <multimedia/player_framework/native_avcapability.h>
#include <multimedia/player_framework/native_avformat.h>
#include <native_window/external_window.h>

namespace deskmate::gate3::probe {

namespace {

constexpr std::uint64_t kInitialFeedLimit = 12;
constexpr std::uint64_t kOutputTimeoutMs = 5000;
constexpr std::uint64_t kRecoveryTimeoutMs = 5000;
constexpr std::uint64_t kShutdownCleanupAttempts = 200;

std::uint64_t SteadyNowNs()
{
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count());
}

const char *Bool(bool value)
{
    return value ? "true" : "false";
}

std::string EscapeJson(const std::string &value)
{
    std::ostringstream output;
    for (const unsigned char character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (character < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<int>(character) << std::dec;
            } else {
                output << static_cast<char>(character);
            }
            break;
        }
    }
    return output.str();
}

std::string IntArray(const std::vector<std::int32_t> &values)
{
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << values[index];
    }
    output << ']';
    return output.str();
}

std::string StagesJson(const std::vector<StageMeasurement> &stages)
{
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < stages.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        output << "{\"stage\":\"" << EscapeJson(stages[index].stage) << "\",\"duration_ns\":"
               << stages[index].duration_ns << ",\"ok\":" << Bool(stages[index].ok) << '}';
    }
    output << ']';
    return output.str();
}

bool ParseUnsignedKey(const std::string &text, std::uint64_t &key)
{
    if (text.empty()) {
        return false;
    }
    const char *begin = text.data();
    const char *end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, key);
    return parsed.ec == std::errc{} && parsed.ptr == end && key != 0;
}

constexpr const char *kAnnexBHostInterop =
    "{\"state\":\"pending\",\"reason\":\"issue 05 has not produced the real Host Annex-B sample\"}";

} // namespace

ProbeEngine &ProbeEngine::Instance()
{
    static ProbeEngine engine;
    return engine;
}

ProbeEngine::ProbeEngine()
    : surfaces_(surface_ops_),
      router_(decoder_),
      cleanup_(ProbeCleanupOperations{
          .stop_decoder = [this] { return decoder_.Stop(); },
          .destroy_decoder = [this] { return decoder_.Destroy(); },
          .release_surface = [this](SurfaceLeaseId lease_id) { return surfaces_.Release(lease_id); },
          .shutdown_surfaces = [this] { return surfaces_.Shutdown(); },
      }),
      lifecycle_(decoder_, router_, surfaces_, [] { return SteadyNowNs(); })
{
    decoder_.SetSink(this);
    worker_ = std::thread([this] {
        worker_thread_id_ = std::this_thread::get_id();
        decoder_.BindControlThread();
        router_.BindControlThread();
        SetStage("control thread ready; waiting for stream and surface");
        WorkerLoop();
    });
}

ProbeEngine::~ProbeEngine()
{
    {
        std::lock_guard lock(worker_mutex_);
        store_shutdown_ = true;
    }
    worker_shutdown_.store(true, std::memory_order_release);
    worker_cv_.notify_all();
    if (worker_.joinable()) {
        worker_.join();
    }
}

void ProbeEngine::NotifyWorker()
{
    worker_cv_.notify_all();
}

void ProbeEngine::SetStage(std::string stage)
{
    std::lock_guard lock(snapshot_mutex_);
    stage_ = std::move(stage);
}

void ProbeEngine::SetTerminalFailure(std::string failure)
{
    std::lock_guard lock(snapshot_mutex_);
    if (failure_.empty()) {
        failure_ = std::move(failure);
        stage_ = failure_;
    }
}

bool ProbeEngine::DrainOutputs(const char *failure_context)
{
    if (router_.Drain()) {
        return true;
    }
    RecordSurfaceCleanupFailure(failure_context);
    return false;
}

void ProbeEngine::RecordSurfaceCleanupFailure(const char *failure_context)
{
    cleanup_.RecordFailure(failure_context);
    SetTerminalFailure(std::string(failure_context) + " failed");
}

bool ProbeEngine::ReleaseSurfaceLease(const SurfaceLease &lease, const char *failure_context)
{
    if (cleanup_.ReleaseSurfaceLease(lease, failure_context)) {
        return true;
    }
    SetTerminalFailure(std::string(failure_context) + " failed");
    return false;
}

void ProbeEngine::ForgetSurfaceLease(SurfaceLeaseId lease_id, SurfaceGeneration generation)
{
    {
        std::lock_guard lock(snapshot_mutex_);
        if (current_lease_.has_value() && current_lease_->id == lease_id) {
            current_lease_.reset();
            current_surface_generation_ = 0;
            current_surface_id_ = 0;
            current_surface_window_ = nullptr;
            surface_ready_ = false;
        }
    }
    if (decoder_.current_instance() == 0 && generation != 0 &&
        !surfaces_.CollectRetiredSurface(generation)) {
        RecordSurfaceCleanupFailure("retired surface collection after retry");
    }
}

void ProbeEngine::RetryPendingSurfaceReleases()
{
    std::vector<SurfaceLease> pending;
    {
        const auto records = cleanup_.PendingSurfaceReleases();
        pending.reserve(records.size());
        for (const PendingSurfaceRelease &record : records) {
            pending.push_back(record.lease);
        }
    }
    if (pending.empty()) {
        return;
    }

    const std::vector<SurfaceLeaseId> released = cleanup_.RetryPendingSurfaceReleases();
    for (const SurfaceLeaseId lease_id : released) {
        const auto lease = std::find_if(pending.begin(), pending.end(), [lease_id](const SurfaceLease &item) {
            return item.id == lease_id;
        });
        if (lease != pending.end()) {
            ForgetSurfaceLease(lease->id, lease->generation);
        }
    }
}

std::optional<SurfaceLease> ProbeEngine::CurrentLease() const
{
    std::lock_guard lock(snapshot_mutex_);
    return current_lease_;
}

bool ProbeEngine::LoadStream(std::vector<std::uint8_t> bytes)
{
    std::string error;
    auto parsed = ParseAnnexB(std::move(bytes), error);
    if (!parsed.has_value()) {
        {
            std::lock_guard lock(snapshot_mutex_);
            stream_parse_error_ = error;
            stage_ = "Annex-B parse failed";
            failure_ = error;
        }
        return false;
    }

    {
        std::lock_guard lock(snapshot_mutex_);
        if (suite_running_ || suite_done_) {
            failure_ = "stream cannot be replaced while the suite is running or complete";
            return false;
        }
        stream_ = std::move(parsed);
        stream_loaded_ = true;
        stream_parse_error_.clear();
        failure_.clear();
        stage_ = "Annex-B stream loaded; waiting for surface";
    }
    decoder_.SetStream(&*stream_);
    NotifyWorker();
    return true;
}

bool ProbeEngine::SurfaceCreated(const std::string &surface_id)
{
    std::uint64_t key = 0;
    if (!ParseUnsignedKey(surface_id, key)) {
        SetTerminalFailure("surfaceCreated received a non-numeric native window key");
        return false;
    }

    {
        std::lock_guard lock(surface_mutex_);
        const auto existing = surface_generations_.find(key);
        if (existing != surface_generations_.end() && surfaces_.CanRender(existing->second)) {
            return true;
        }
    }

    void *const window = reinterpret_cast<void *>(static_cast<std::uintptr_t>(key));
    const SurfaceGeneration generation = surfaces_.OnSurfaceCreated(window);
    if (generation == 0) {
        SetTerminalFailure("OH_NativeWindow_NativeObjectReference failed for the XComponent surface");
        return false;
    }

    {
        std::lock_guard lock(surface_mutex_);
        surface_windows_[key] = window;
        surface_generations_[key] = generation;
        surface_events_.push_back({true, key, generation, window});
    }
    SetStage("surface created; acquiring a serialized lease");
    NotifyWorker();
    return true;
}

bool ProbeEngine::SurfaceDestroyed(const std::string &surface_id)
{
    std::uint64_t key = 0;
    if (!ParseUnsignedKey(surface_id, key)) {
        return false;
    }

    SurfaceGeneration generation = 0;
    void *window = nullptr;
    {
        std::lock_guard lock(surface_mutex_);
        const auto found = surface_generations_.find(key);
        if (found == surface_generations_.end()) {
            return false;
        }
        generation = found->second;
        const auto window_found = surface_windows_.find(key);
        window = window_found == surface_windows_.end() ? nullptr : window_found->second;
        surface_generations_.erase(found);
        surface_windows_.erase(key);
    }

    const bool was_active = surfaces_.active_generation() == generation;
    surfaces_.OnSurfaceDestroyed(generation);
    if (was_active) {
        // This callback performs no decoder operation. It closes output and
        // invalidates the Surface generation before the control thread drains.
        router_.InvalidateSurfaceFromCallback();
    }

    {
        std::lock_guard lock(surface_mutex_);
        surface_events_.push_back({false, key, generation, window});
    }
    SetStage("surface destroyed; rendering stopped immediately");
    NotifyWorker();
    return true;
}

bool ProbeEngine::RunSuite()
{
    {
        std::lock_guard lock(snapshot_mutex_);
        if (!stream_loaded_) {
            stage_ = "runSuite rejected: load the Annex-B stream first";
            return false;
        }
        if (suite_running_) {
            stage_ = "runSuite rejected: suite already running";
            return false;
        }
        if (suite_done_) {
            stage_ = "runSuite rejected: this process runs the suite once";
            return false;
        }
        suite_running_ = true;
        suite_passed_ = false;
        failure_.clear();
        stage_ = "suite queued on the control thread";
    }
    {
        std::lock_guard lock(worker_mutex_);
        suite_requested_ = true;
    }
    NotifyWorker();
    return true;
}

bool ProbeEngine::BeginFreeze()
{
    {
        std::lock_guard lock(snapshot_mutex_);
        if (!suite_done_ || !initial_decode_passed_) {
            stage_ = "freeze rejected: run the suite first";
            return false;
        }
        if (freeze_requested_ || manual_frozen_) {
            return false;
        }
        freeze_requested_ = true;
        manual_lgf_ = "pending";
        stage_ = "freeze queued on the control thread";
    }
    NotifyWorker();
    return true;
}

bool ProbeEngine::RecordLastGoodFrame(bool stable)
{
    std::lock_guard lock(snapshot_mutex_);
    if (!manual_frozen_) {
        stage_ = "Last Good Frame observation rejected: freeze first";
        return false;
    }
    manual_lgf_ = stable ? "stable" : "not-stable";
    stage_ = stable ? "manual observation recorded: Last Good Frame stable"
                    : "manual observation recorded: Last Good Frame not stable";
    return true;
}

bool ProbeEngine::ResumeAfterFreeze()
{
    {
        std::lock_guard lock(snapshot_mutex_);
        if (!manual_frozen_ || !current_lease_.has_value()) {
            stage_ = "resume rejected: freeze and verify the held frame first";
            return false;
        }
        if (resume_requested_) {
            return false;
        }
        resume_requested_ = true;
        stage_ = "resume queued on the control thread";
    }
    NotifyWorker();
    return true;
}

bool ProbeEngine::SetDeviceInfo(std::string json)
{
    if (json.size() < 2 || json.front() != '{' || json.back() != '}') {
        return false;
    }
    std::lock_guard lock(snapshot_mutex_);
    device_json_ = std::move(json);
    return true;
}

std::string ProbeEngine::SnapshotJson() const
{
    std::lock_guard lock(snapshot_mutex_);
    return BuildReportJson();
}

void ProbeEngine::OnInputReady(const InputReadyEvent &event)
{
    callback_count_.fetch_add(1, std::memory_order_relaxed);
    if (worker_thread_id_ != std::thread::id{} && std::this_thread::get_id() != worker_thread_id_) {
        callback_off_control_thread_.store(true, std::memory_order_relaxed);
    }
    {
        std::lock_guard lock(callback_mutex_);
        input_events_.push_back(event);
    }
    NotifyWorker();
}

void ProbeEngine::OnOutputReady(DecoderInstanceId decoder, std::uint32_t index)
{
    callback_count_.fetch_add(1, std::memory_order_relaxed);
    if (worker_thread_id_ != std::thread::id{} && std::this_thread::get_id() != worker_thread_id_) {
        callback_off_control_thread_.store(true, std::memory_order_relaxed);
    }
    // Generation-tagged queue only. No native output API is called here.
    router_.PostOutputBuffer(decoder, index);
    NotifyWorker();
}

void ProbeEngine::OnDecoderError(const ErrorEvent &event)
{
    callback_count_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(callback_mutex_);
        error_events_.push_back(event);
    }
    NotifyWorker();
}

void ProbeEngine::OnStreamChanged(const StreamChangedEvent &event)
{
    callback_count_.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard lock(callback_mutex_);
        stream_changes_.push_back(event);
    }
    NotifyWorker();
}

void ProbeEngine::ProcessCallbacks()
{
    std::deque<InputReadyEvent> inputs;
    std::deque<ErrorEvent> errors;
    std::deque<StreamChangedEvent> changes;
    {
        std::lock_guard lock(callback_mutex_);
        inputs.swap(input_events_);
        errors.swap(error_events_);
        changes.swap(stream_changes_);
    }

    for (const InputReadyEvent &event : inputs) {
        (void)decoder_.PushQueuedInput(event);
    }
    for (const ErrorEvent &event : errors) {
        if (event.decoder == decoder_.current_instance()) {
            last_decoder_error_.store(event.code, std::memory_order_release);
            SetTerminalFailure("decoder error callback: " + std::to_string(event.code));
        }
    }
    for (const StreamChangedEvent &event : changes) {
        std::ostringstream summary;
        summary << "{\"decoder\":" << event.decoder << ",\"width\":" << event.width
                << ",\"height\":" << event.height << ",\"pixel_format\":" << event.pixel_format << '}';
        std::lock_guard lock(snapshot_mutex_);
        stream_change_summary_ = summary.str();
    }
    (void)DrainOutputs("decoder callback output drain");
}

void ProbeEngine::ProcessSurfaceEvents()
{
    std::deque<SurfaceEvent> events;
    {
        std::lock_guard lock(surface_mutex_);
        events.swap(surface_events_);
    }

    for (const SurfaceEvent &event : events) {
        if (!event.created) {
            SurfaceGeneration current = 0;
            {
                std::lock_guard lock(snapshot_mutex_);
                current = current_surface_generation_;
            }
            if (event.generation != current) {
                continue;
            }

            (void)router_.SetSurfaceRenderable(false);
            bool decoder_stopped = true;
            if (decoder_.current_instance() != 0) {
                (void)router_.BeginRecovery(decoder_.current_instance());
                if (!cleanup_.DecoderStopped() && !decoder_.Stop()) {
                    decoder_stopped = false;
                    RecordSurfaceCleanupFailure("surface decoder stop");
                } else {
                    cleanup_.ConfirmDecoderStopped();
                    std::lock_guard lock(snapshot_mutex_);
                    decoder_needs_rebuild_ = true;
                }
            }

            std::optional<SurfaceLease> lease;
            {
                std::lock_guard lock(snapshot_mutex_);
                surface_ready_ = false;
                current_surface_generation_ = 0;
                current_surface_id_ = 0;
                current_surface_window_ = nullptr;
                lease = current_lease_;
            }
            if (!decoder_stopped) {
                SetStage("surface destroyed; decoder stop is pending cleanup retry");
                continue;
            }
            if (lease.has_value() && ReleaseSurfaceLease(*lease, "surface lease release")) {
                ForgetSurfaceLease(lease->id, lease->generation);
            }
            SetStage("surface destroyed; decoder stopped on the control thread");
            continue;
        }

        const auto lease = surfaces_.Acquire(event.generation);
        if (!lease.has_value()) {
            SetTerminalFailure("surface lease acquisition failed or the Surface generation is stale");
            continue;
        }

        std::optional<SurfaceLease> previous_lease;
        SurfaceGeneration previous_generation = 0;
        bool rebuild = false;
        {
            std::lock_guard lock(snapshot_mutex_);
            previous_lease = current_lease_;
            previous_generation = current_surface_generation_;
            current_lease_ = lease;
            current_surface_generation_ = event.generation;
            current_surface_id_ = event.window_key;
            current_surface_window_ = event.native_window;
            surface_ready_ = true;
            rebuild = decoder_.current_instance() != 0 && (decoder_needs_rebuild_ || suite_done_);
        }

        if (previous_lease.has_value() && previous_lease->id != lease->id) {
            if (ReleaseSurfaceLease(*previous_lease, "previous surface lease release")) {
                ForgetSurfaceLease(previous_lease->id, previous_lease->generation);
            }
        }

        if (rebuild) {
            HandleSurfaceRebuild(*lease, previous_generation);
        } else {
            if (previous_lease.has_value() && decoder_.current_instance() == 0) {
                if (!surfaces_.CollectRetiredSurface(previous_generation)) {
                    RecordSurfaceCleanupFailure("previous retired surface collection");
                }
            }
            SetStage("surface lease ready on the control thread");
        }
    }
}

void ProbeEngine::CleanupSurfaceIfInvalid()
{
    const auto lease = CurrentLease();
    if (!lease.has_value() || surfaces_.CanRender(*lease)) {
        return;
    }

    (void)router_.SetSurfaceRenderable(false);
    if (decoder_.current_instance() != 0) {
        (void)router_.BeginRecovery(decoder_.current_instance());
        if (!cleanup_.DecoderStopped() && !decoder_.Stop()) {
            RecordSurfaceCleanupFailure("invalid surface decoder stop");
            SetStage("invalid Surface Lease retained; decoder stop is pending cleanup retry");
            return;
        }
        cleanup_.ConfirmDecoderStopped();
        std::lock_guard lock(snapshot_mutex_);
        decoder_needs_rebuild_ = true;
    }

    {
        std::lock_guard lock(snapshot_mutex_);
        if (current_lease_.has_value() && current_lease_->id == lease->id) {
            current_surface_generation_ = 0;
            current_surface_id_ = 0;
            current_surface_window_ = nullptr;
            surface_ready_ = false;
        }
    }
    if (cleanup_.HasPendingSurfaceRelease(lease->id)) {
        SetStage("invalid Surface Lease retained; release is pending cleanup retry");
        return;
    }
    if (ReleaseSurfaceLease(*lease, "invalid surface lease release")) {
        ForgetSurfaceLease(lease->id, lease->generation);
    }
    SetStage("invalid Surface Lease drained; decoder awaits a rebuild");
}

void ProbeEngine::WorkerLoop()
{
    std::uint64_t shutdown_cleanup_attempts = 0;
    for (;;) {
        bool run_suite = false;
        {
            std::unique_lock lock(worker_mutex_);
            worker_cv_.wait_for(lock, std::chrono::milliseconds(10), [this] {
                return worker_shutdown_.load(std::memory_order_acquire) || store_shutdown_ ||
                       suite_requested_;
            });
            run_suite = suite_requested_;
            suite_requested_ = false;
        }

        ProcessSurfaceEvents();
        ProcessCallbacks();
        RetryPendingSurfaceReleases();
        CleanupSurfaceIfInvalid();

        if (worker_shutdown_.load(std::memory_order_acquire) || store_shutdown_) {
            bool cleanup_ready = true;
            if (!router_.SetSurfaceRenderable(false)) {
                RecordSurfaceCleanupFailure("shutdown surface output close");
                cleanup_ready = false;
            }
            if (decoder_.current_instance() != 0) {
                if (!router_.BeginRecovery(decoder_.current_instance())) {
                    RecordSurfaceCleanupFailure("shutdown recovery close");
                    cleanup_ready = false;
                }
                cleanup_ready = DrainOutputs("shutdown decoder output drain") && cleanup_ready;
            }
            bool shutdown_complete = false;
            if (cleanup_ready) {
                shutdown_complete = cleanup_.Shutdown(decoder_.current_instance() != 0, CurrentLease());
            }
            if (shutdown_complete) {
                return;
            }
            SetTerminalFailure("shutdown cleanup pending at " + cleanup_.FailureStage());
            if (++shutdown_cleanup_attempts >= kShutdownCleanupAttempts) {
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        bool freeze = false;
        {
            std::lock_guard lock(snapshot_mutex_);
            freeze = freeze_requested_;
            freeze_requested_ = false;
        }
        if (freeze) {
            decoder_.SetFeedLimit(decoder_.feed_cursor());
            (void)DrainOutputs("freeze output drain");
            {
                std::lock_guard lock(snapshot_mutex_);
                manual_frozen_ = true;
                manual_lgf_ = "pending";
                stage_ = "frozen: visually verify the Last Good Frame, then record the result";
            }
        }

        if (run_suite) {
            (void)RunSuiteOnWorker();
        }

        bool resume = false;
        {
            std::lock_guard lock(snapshot_mutex_);
            resume = resume_requested_;
            resume_requested_ = false;
        }
        if (resume) {
            const auto lease = CurrentLease();
            if (lease.has_value()) {
                const bool recovered = DecodeRecovery(*lease, RecoveryAction::Flush);
                {
                    std::lock_guard lock(snapshot_mutex_);
                    if (!recoveries_.empty()) {
                        recoveries_.back().action = "manual-freeze-resume";
                    }
                    manual_frozen_ = false;
                    stage_ = recovered
                                 ? "resumed after freeze; verify whether the held frame remained stable"
                                 : "resume recovery failed; see recovery evidence";
                }
            }
        }
    }
}

bool ProbeEngine::EnumerateCapabilities()
{
    std::vector<Capability> capabilities;
    capabilities.push_back(ReadCapability(OH_AVCODEC_MIMETYPE_VIDEO_AVC));
    capabilities.push_back(ReadCapability(OH_AVCODEC_MIMETYPE_VIDEO_HEVC));
    std::lock_guard lock(snapshot_mutex_);
    capabilities_ = std::move(capabilities);
    return !capabilities_.empty();
}

ProbeEngine::Capability ProbeEngine::ReadCapability(const char *mime)
{
    Capability capability;
    capability.mime = mime == nullptr ? "" : mime;
    OH_AVCapability *const raw = OH_AVCodec_GetCapability(mime, false);
    if (raw == nullptr) {
        return capability;
    }

    capability.present = true;
    capability.hardware = OH_AVCapability_IsHardware(raw);
    capability.low_latency = OH_AVCapability_IsFeatureSupported(raw, VIDEO_LOW_LATENCY);
    const char *const name = OH_AVCapability_GetName(raw);
    capability.name = name == nullptr ? "" : name;

    OH_AVRange range{};
    if (OH_AVCapability_GetVideoWidthRange(raw, &range) == AV_ERR_OK) {
        capability.min_width = range.minVal;
        capability.max_width = range.maxVal;
    }
    if (OH_AVCapability_GetVideoHeightRange(raw, &range) == AV_ERR_OK) {
        capability.min_height = range.minVal;
        capability.max_height = range.maxVal;
    }

    const std::int32_t *pixel_formats = nullptr;
    std::uint32_t pixel_format_count = 0;
    if (OH_AVCapability_GetVideoSupportedPixelFormats(raw, &pixel_formats, &pixel_format_count) == AV_ERR_OK &&
        pixel_formats != nullptr) {
        capability.pixel_formats.assign(pixel_formats, pixel_formats + pixel_format_count);
    }

    const std::int32_t *profiles = nullptr;
    std::uint32_t profile_count = 0;
    if (OH_AVCapability_GetSupportedProfiles(raw, &profiles, &profile_count) == AV_ERR_OK &&
        profiles != nullptr) {
        capability.profiles.assign(profiles, profiles + profile_count);
    }
    return capability;
}

bool ProbeEngine::SurfaceStillValid(const SurfaceLease &lease)
{
    return surfaces_.CanRender(lease) && decoder_.surface_set();
}

bool ProbeEngine::PumpUntil(const std::function<bool()> &predicate, std::uint64_t timeout_ms)
{
    const std::uint64_t deadline = SteadyNowNs() + timeout_ms * 1000000ULL;
    while (SteadyNowNs() < deadline) {
        ProcessCallbacks();
        CleanupSurfaceIfInvalid();
        if (predicate()) {
            return true;
        }
        if (last_decoder_error_.load(std::memory_order_acquire) != 0) {
            return false;
        }
        if (cleanup_.Failed()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ProcessCallbacks();
    return predicate() && !cleanup_.Failed();
}

bool ProbeEngine::RunInitialDecode(const SurfaceLease &lease)
{
    SetStage("initial decode: creating and configuring the H.264 Surface decoder");
    if (!stream_.has_value() || !stream_->access_units.front().keyframe ||
        !stream_->access_units.front().contains_sps || !stream_->access_units.front().contains_pps) {
        SetTerminalFailure("test stream does not start with an IDR carrying SPS and PPS");
        return false;
    }

    const std::uint64_t started = SteadyNowNs();
    if (!decoder_.CreateNext()) {
        SetTerminalFailure("OH_VideoDecoder_CreateByMime failed: " + decoder_.last_error());
        return false;
    }
    if (!decoder_.Configure()) {
        SetTerminalFailure("decoder configure failed: " + decoder_.last_error());
        return false;
    }
    if (!decoder_.SetSurface(lease.native_window)) {
        SetTerminalFailure("decoder Surface attach failed: " + decoder_.last_error());
        return false;
    }
    if (!decoder_.Prepare() || !decoder_.Start()) {
        SetTerminalFailure("decoder prepare/start failed: " + decoder_.last_error());
        return false;
    }
    cleanup_.DecoderStarted();
    if (!lifecycle_.StartInitial(lease)) {
        SetTerminalFailure("control-thread recovery router did not start");
        return false;
    }
    if (decoder_.current_instance() != lifecycle_.current_decoder()) {
        SetTerminalFailure("decoder instance id diverged from the recovery controller");
        return false;
    }

    const std::uint64_t baseline = router_.stats().rendered;
    decoder_.SetFeedLimit(kInitialFeedLimit);
    if (!decoder_.SubmitParameterSetsAndIdr()) {
        SetTerminalFailure("initial SPS/PPS/IDR submission failed: " + decoder_.last_error());
        return false;
    }

    SetStage("initial decode: waiting for the first rendered Surface output");
    const bool first_output = PumpUntil(
        [this, baseline] { return router_.stats().rendered > baseline; }, kOutputTimeoutMs);
    const std::uint64_t finished = SteadyNowNs();
    decoder_.SetFeedLimit(decoder_.feed_cursor());
    const bool drained = DrainOutputs("initial decode output drain");

    const OutputRouterStats stats = router_.stats();
    const std::uint64_t parameter_set_units = decoder_.parameter_set_units_pushed();
    const bool passed = first_output && drained && SurfaceStillValid(lease) &&
                        parameter_set_units > 0 &&
                        stats.rendered > baseline &&
                        last_decoder_error_.load(std::memory_order_acquire) == 0;
    {
        std::lock_guard lock(snapshot_mutex_);
        initial_decode_passed_ = passed;
        initial_rendered_ = stats.rendered - baseline;
        initial_feed_count_ = decoder_.feed_cursor();
        initial_parameter_set_units_ = parameter_set_units;
        initial_duration_ns_ = finished - started;
    }
    if (!passed) {
        SetTerminalFailure(
            first_output ? "initial decode did not establish a valid Surface-backed Last Good Frame"
                         : "initial decode produced no rendered output before timeout");
    } else {
        SetStage("initial decode passed; Last Good Frame state established");
    }
    return passed;
}

bool ProbeEngine::DecodeRecovery(const SurfaceLease &lease, RecoveryAction action)
{
    RecoveryMeasurement measurement;
    measurement.action = RecoveryActionName(action);
    measurement.surface_generation = lease.generation;

    if (!surfaces_.CanRender(lease)) {
        measurement.failure_stage = "surface-lease";
        std::lock_guard lock(snapshot_mutex_);
        recoveries_.push_back(std::move(measurement));
        return false;
    }

    if (!DrainOutputs("pre-recovery output drain")) {
        measurement.failure_stage = "drain-before";
        std::lock_guard lock(snapshot_mutex_);
        recoveries_.push_back(std::move(measurement));
        return false;
    }
    const OutputRouterStats before = router_.stats();
    const std::uint64_t feed_before = decoder_.feed_cursor();
    const std::uint64_t parameter_units_before = decoder_.parameter_set_units_pushed();
    const std::uint64_t input_pushed_before = decoder_.input_buffers_pushed();
    const std::uint64_t stale_input_before = decoder_.stale_input_callbacks();
    const bool last_good_before = router_.last_good_frame_visible();
    const std::uint64_t started = SteadyNowNs();

    const RecoveryReport report = lifecycle_.Recover(action, lease);
    const bool last_good_after_operation = router_.last_good_frame_visible();
    measurement.operation_duration_ns = report.duration_ns;
    measurement.decoder = report.decoder;
    measurement.surface_retained = report.surface_retained && surfaces_.CanRender(lease);
    measurement.parameter_sets_resubmitted = report.parameter_sets_resubmitted;
    measurement.failure_stage = report.failure_stage;
    measurement.stages = report.stages;

    bool first_output = false;
    if (report.success) {
        const std::uint64_t rendered_before = before.rendered;
        decoder_.SetFeedLimit(std::max<std::uint64_t>(decoder_.feed_cursor() + 1, kInitialFeedLimit));
        first_output = PumpUntil(
            [this, rendered_before] { return router_.stats().rendered > rendered_before; },
            kRecoveryTimeoutMs);
        decoder_.SetFeedLimit(decoder_.feed_cursor());
        if (!DrainOutputs("recovery output drain")) {
            measurement.failure_stage = "drain";
            first_output = false;
        }
    }
    const std::uint64_t finished = SteadyNowNs();
    const OutputRouterStats after = router_.stats();

    measurement.first_output_observed = first_output;
    measurement.first_output_latency_ns = finished - started;
    measurement.residual_callbacks = after.residual_decoder_callbacks - before.residual_decoder_callbacks;
    measurement.stale_epoch_frees = after.freed_stale_epoch - before.freed_stale_epoch;
    measurement.input_buffers_pushed = decoder_.input_buffers_pushed() - input_pushed_before;
    measurement.parameter_set_units_pushed =
        decoder_.parameter_set_units_pushed() - parameter_units_before;
    measurement.stale_input_callbacks = decoder_.stale_input_callbacks() - stale_input_before;
    measurement.parameter_set_unit_observed = measurement.parameter_set_units_pushed > 0;
    measurement.decoder_instance_matched =
        report.decoder != 0 && decoder_.current_instance() == report.decoder;
    measurement.surface_set_confirmed = decoder_.surface_set();
    measurement.last_good_frame_state_stable =
        last_good_before && last_good_after_operation && router_.last_good_frame_visible() && first_output;

    measurement.operation_success =
        report.success && first_output && measurement.surface_retained &&
        measurement.surface_set_confirmed && measurement.parameter_set_unit_observed &&
        measurement.decoder_instance_matched && measurement.last_good_frame_state_stable &&
        last_decoder_error_.load(std::memory_order_acquire) == 0;
    if (!measurement.operation_success && measurement.failure_stage.empty()) {
        if (!first_output) {
            measurement.failure_stage = "first-output";
        } else if (!measurement.parameter_set_unit_observed) {
            measurement.failure_stage = "parameter-sets";
        } else if (!measurement.decoder_instance_matched) {
            measurement.failure_stage = "decoder-id";
        } else {
            measurement.failure_stage = "evidence";
        }
    }

    // The feed cursor resets during resubmission. Remember where the bounded
    // feed stopped so later recoveries cannot continue the old generation.
    if (feed_before > decoder_.feed_cursor()) {
        decoder_.SetFeedLimit(decoder_.feed_cursor());
    }

    const bool success = measurement.operation_success;
    if (std::any_of(report.stages.begin(), report.stages.end(),
                    [](const StageMeasurement &stage) { return stage.stage == "start" && stage.ok; })) {
        cleanup_.DecoderStarted();
    }
    {
        std::lock_guard lock(snapshot_mutex_);
        recoveries_.push_back(std::move(measurement));
    }
    return success;
}

bool ProbeEngine::RunSuiteOnWorker()
{
    const auto lease = CurrentLease();
    if (!lease.has_value() || !surfaces_.CanRender(*lease)) {
        SetTerminalFailure("suite requires a live Surface Lease before it starts");
        std::lock_guard lock(snapshot_mutex_);
        suite_running_ = false;
        suite_done_ = true;
        suite_passed_ = false;
        return false;
    }

    if (!EnumerateCapabilities()) {
        SetTerminalFailure("no H.264/HEVC decoder capability was reported");
    }
    SetStage("suite: enumerating codec capabilities and running initial decode");

    const bool initial = RunInitialDecode(*lease);
    bool passed = initial;
    if (initial) {
        const RecoveryAction actions[] = {
            RecoveryAction::Flush,
            RecoveryAction::Stop,
            RecoveryAction::Reset,
            RecoveryAction::Rebuild,
        };
        for (const RecoveryAction action : actions) {
            if (!surfaces_.CanRender(*lease)) {
                passed = false;
                break;
            }
            SetStage(std::string("suite: recovering with ") + RecoveryActionName(action));
            const bool recovered = DecodeRecovery(*lease, action);
            passed = passed && recovered;
            if (!recovered && !surfaces_.CanRender(*lease)) {
                break;
            }
        }
    }

    const std::uint64_t violations = decoder_.control_thread_violations();
    passed = passed && violations == 0 &&
             last_decoder_error_.load(std::memory_order_acquire) == 0 &&
             !cleanup_.Failed() &&
             router_.last_good_frame_visible();
    {
        std::lock_guard lock(snapshot_mutex_);
        suite_running_ = false;
        suite_done_ = true;
        suite_passed_ = passed;
        if (passed) {
            failure_.clear();
            stage_ = "suite passed: all Surface recovery paths produced current-epoch output";
        } else if (failure_.empty()) {
            failure_ = "suite did not satisfy every recovery evidence field";
            stage_ = failure_;
        }
    }
    return passed;
}

void ProbeEngine::HandleSurfaceRebuild(const SurfaceLease &lease, SurfaceGeneration previous_generation)
{
    SetStage("replacement Surface: rebuilding the decoder on the control thread");
    const bool recovered = DecodeRecovery(lease, RecoveryAction::Rebuild);
    {
        std::lock_guard lock(snapshot_mutex_);
        if (!recoveries_.empty()) {
            recoveries_.back().action = "surface-rebuild";
        }
        decoder_needs_rebuild_ = !recovered;
        if (recovered) {
            stage_ = "replacement Surface decoded with a rebuilt decoder";
        } else {
            failure_ = "replacement Surface decoder rebuild failed";
            stage_ = failure_;
        }
    }
    if (recovered && previous_generation != 0) {
        if (!surfaces_.CollectRetiredSurface(previous_generation)) {
            RecordSurfaceCleanupFailure("rebuilt retired surface collection");
        }
    }
}

std::string ProbeEngine::CapabilitiesJson() const
{
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < capabilities_.size(); ++index) {
        const Capability &capability = capabilities_[index];
        if (index != 0) {
            output << ',';
        }
        const bool surface_format =
            std::find(capability.pixel_formats.begin(), capability.pixel_formats.end(),
                      static_cast<std::int32_t>(AV_PIXEL_FORMAT_SURFACE_FORMAT)) !=
            capability.pixel_formats.end();
        output << "{\"mime\":\"" << EscapeJson(capability.mime) << "\""
               << ",\"present\":" << Bool(capability.present)
               << ",\"name\":\"" << EscapeJson(capability.name) << "\""
               << ",\"hardware\":" << Bool(capability.hardware)
               << ",\"low_latency_feature\":" << Bool(capability.low_latency)
               << ",\"surface_pixel_format\":" << Bool(surface_format)
               << ",\"width_range\":{\"min\":" << capability.min_width
               << ",\"max\":" << capability.max_width << '}'
               << ",\"height_range\":{\"min\":" << capability.min_height
               << ",\"max\":" << capability.max_height << '}'
               << ",\"pixel_formats\":" << IntArray(capability.pixel_formats)
               << ",\"profiles\":" << IntArray(capability.profiles) << '}';
    }
    output << ']';
    return output.str();
}

std::string ProbeEngine::RecoveriesJson() const
{
    std::ostringstream output;
    output << '[';
    for (std::size_t index = 0; index < recoveries_.size(); ++index) {
        const RecoveryMeasurement &recovery = recoveries_[index];
        if (index != 0) {
            output << ',';
        }
        output << "{\"action\":\"" << EscapeJson(recovery.action) << "\""
               << ",\"operation_success\":" << Bool(recovery.operation_success)
               << ",\"first_output_observed\":" << Bool(recovery.first_output_observed)
               << ",\"surface_retained\":" << Bool(recovery.surface_retained)
               << ",\"surface_set_confirmed\":" << Bool(recovery.surface_set_confirmed)
               << ",\"parameter_sets_resubmitted\":" << Bool(recovery.parameter_sets_resubmitted)
               << ",\"parameter_set_unit_observed\":" << Bool(recovery.parameter_set_unit_observed)
               << ",\"decoder_instance_matched\":" << Bool(recovery.decoder_instance_matched)
               << ",\"last_good_frame_state_stable\":"
               << Bool(recovery.last_good_frame_state_stable)
               << ",\"operation_duration_ns\":" << recovery.operation_duration_ns
               << ",\"first_output_latency_ns\":" << recovery.first_output_latency_ns
               << ",\"residual_callbacks\":" << recovery.residual_callbacks
               << ",\"stale_epoch_frees\":" << recovery.stale_epoch_frees
               << ",\"input_buffers_pushed\":" << recovery.input_buffers_pushed
               << ",\"parameter_set_units_pushed\":" << recovery.parameter_set_units_pushed
               << ",\"stale_input_callbacks\":" << recovery.stale_input_callbacks
               << ",\"decoder\":" << recovery.decoder
               << ",\"surface_generation\":" << recovery.surface_generation
               << ",\"failure_stage\":\"" << EscapeJson(recovery.failure_stage) << "\""
               << ",\"stages\":" << StagesJson(recovery.stages) << '}';
    }
    output << ']';
    return output.str();
}

std::string ProbeEngine::DeviceJson() const
{
    return device_json_.empty() ? "{}" : device_json_;
}

std::string ProbeEngine::BuildReportJson() const
{
    const OutputRouterStats stats = router_.stats();
    const bool lease_valid = current_lease_.has_value() && surfaces_.CanRender(*current_lease_);
    const bool cleanup_failed = cleanup_.Failed();
    const bool effective_suite_passed = cleanup_.EffectiveSuitePassed(suite_passed_);
    const char *const verification = cleanup_.Verification(suite_done_, suite_passed_);
    const std::vector<PendingSurfaceRelease> pending_releases = cleanup_.PendingSurfaceReleases();

    std::size_t keyframes = 0;
    std::size_t sps = 0;
    std::size_t pps = 0;
    if (stream_.has_value()) {
        for (const AnnexBAccessUnit &unit : stream_->access_units) {
            if (unit.keyframe) {
                ++keyframes;
            }
            if (unit.contains_sps) {
                ++sps;
            }
            if (unit.contains_pps) {
                ++pps;
            }
        }
    }

    std::ostringstream output;
    output << "{\"schema\":\"deskmate.gate3.device-probe.v1\""
           << ",\"verification\":\"" << verification << "\""
           << ",\"stage\":\"" << EscapeJson(stage_) << "\""
           << ",\"failure\":\"" << EscapeJson(failure_) << "\""
           << ",\"suite\":{\"state\":\""
           << (suite_running_ ? "running" : (suite_done_ ? "complete" : "not-started"))
           << "\",\"passed\":" << Bool(effective_suite_passed) << '}'
           << ",\"cleanup\":{\"failed\":" << Bool(cleanup_failed)
           << ",\"failure_stage\":\"" << EscapeJson(cleanup_.FailureStage()) << "\""
           << ",\"decoder_destroyed\":" << Bool(cleanup_.DecoderDestroyed())
           << ",\"pending_surface_releases\":[";
    for (std::size_t index = 0; index < pending_releases.size(); ++index) {
        if (index != 0) {
            output << ',';
        }
        const PendingSurfaceRelease &release = pending_releases[index];
        output << "{\"lease_id\":" << release.lease.id
               << ",\"generation\":" << release.lease.generation
               << ",\"failure_stage\":\"" << EscapeJson(release.failure_stage) << "\"}";
    }
    output << "]}"
           << ",\"device\":" << DeviceJson()
           << ",\"capabilities\":" << CapabilitiesJson()
           << ",\"stream\":";
    if (stream_.has_value()) {
        output << "{\"bytes\":" << stream_->bytes.size()
               << ",\"nals\":" << stream_->nals.size()
               << ",\"access_units\":" << stream_->access_units.size()
               << ",\"keyframes\":" << keyframes
               << ",\"access_units_with_sps\":" << sps
               << ",\"access_units_with_pps\":" << pps
               << ",\"first_access_unit_is_idr_with_sps_pps\":"
               << Bool(stream_->access_units.front().keyframe &&
                       stream_->access_units.front().contains_sps &&
                       stream_->access_units.front().contains_pps)
               << '}';
    } else {
        output << "null";
    }
    output << ",\"surface\":{\"ready\":" << Bool(surface_ready_)
           << ",\"generation\":" << current_surface_generation_
           << ",\"window_key\":" << current_surface_id_
           << ",\"lease_id\":" << (current_lease_.has_value() ? current_lease_->id : 0)
           << ",\"lease_valid\":" << Bool(lease_valid)
           << ",\"active_generation\":" << surfaces_.active_generation()
           << ",\"renderer_surface_set\":" << Bool(decoder_.surface_set())
           << ",\"reference_operations_serialized\":true"
           << ",\"decoder_needs_rebuild\":" << Bool(decoder_needs_rebuild_) << '}'
           << ",\"initial_decode\":{\"passed\":" << Bool(initial_decode_passed_)
           << ",\"duration_ns\":" << initial_duration_ns_
           << ",\"rendered_outputs\":" << initial_rendered_
           << ",\"input_access_units\":" << initial_feed_count_
           << ",\"parameter_set_units\":" << initial_parameter_set_units_
           << ",\"low_latency_key_requested\":" << Bool(decoder_.low_latency_requested())
           << ",\"low_latency_key_accepted\":" << Bool(decoder_.low_latency_configured())
           << ",\"decoder\":" << decoder_.current_instance()
           << ",\"input_buffers_pushed\":" << decoder_.input_buffers_pushed() << '}'
           << ",\"recoveries\":" << RecoveriesJson()
           << ",\"callback_audit\":{\"output_callbacks\":" << stats.callbacks
           << ",\"total_callback_events\":" << callback_count_.load(std::memory_order_relaxed)
           << ",\"callbacks_observed_off_control_thread\":"
           << Bool(callback_off_control_thread_.load(std::memory_order_relaxed))
           << ",\"residual_decoder_callbacks\":" << stats.residual_decoder_callbacks
           << ",\"stale_epoch_frees\":" << stats.freed_stale_epoch
           << ",\"surface_invalid_frees\":" << stats.freed_surface_invalid
           << ",\"free_failures\":" << stats.free_failures
           << ",\"render_failures\":" << stats.render_failures
           << ",\"pending_free_buffers\":" << stats.pending_free_buffers
           << ",\"stale_input_callbacks\":" << decoder_.stale_input_callbacks()
           << ",\"control_thread_violations\":" << decoder_.control_thread_violations()
           << ",\"callback_body\":\"queue-only\""
           << ",\"stream_change\":" << (stream_change_summary_.empty() ? "null" : stream_change_summary_)
           << '}'
           << ",\"last_good_frame\":{\"state_visible\":"
           << Bool(router_.last_good_frame_visible())
           << ",\"manual_observation\":\"" << EscapeJson(manual_lgf_) << "\""
           << ",\"frozen\":" << Bool(manual_frozen_)
           << ",\"evidence\":\"router state plus retained Surface Lease; no CPU pixel readback\"}"
           << ",\"annexb_host_sample_interop\":" << kAnnexBHostInterop
           << ",\"no_cpu_pixel_readback\":true"
           << ",\"limitations\":["
              "\"last_good_frame.state_visible and manual_observation are not a pixel capture\","
              "\"low_latency_key_accepted proves the configure key was accepted, not decoder scheduling\","
              "\"surface_retained uses lease validity and SetSurface success, not display scanout proof\","
              "\"Annex-B Host sample interoperability remains pending issue 05\"]"
           << '}';
    return output.str();
}

std::int32_t ProbeEngine::OhosSurfaceOps::Reference(void *object)
{
    return OH_NativeWindow_NativeObjectReference(object);
}

std::int32_t ProbeEngine::OhosSurfaceOps::Unreference(void *object)
{
    return OH_NativeWindow_NativeObjectUnreference(object);
}

} // namespace deskmate::gate3::probe
