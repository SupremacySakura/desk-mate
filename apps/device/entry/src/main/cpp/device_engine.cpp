#include "device_engine.hpp"

#include "deskmate/clock.hpp"
#include "fd_byte_io.hpp"
#include "video_decoder.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <optional>
#include <poll.h>
#include <random>
#include <sstream>
#include <sys/eventfd.h>
#include <unistd.h>

#include <hilog/log.h>

// OH_LOG_* expands LOG_DOMAIN / LOG_TAG at the use site, and hilog/log.h only
// supplies 0 / NULL defaults when they are undefined. Without this every line
// landed in domain 0 with a null tag and was invisible to `hilog -T`.
#undef LOG_DOMAIN
#undef LOG_TAG
#define LOG_DOMAIN 0x3210
#define LOG_TAG "DeskMateEngine"

namespace deskmate::device {
namespace {

constexpr int kMaxSessionRebuilds = 5;
constexpr int kTimerIntervalMs = 5;
constexpr std::size_t kMaxQueuedInput = 256;

using protocol::BoundaryResult;
using protocol::CapabilitySet;
using protocol::ControlOpcode;
using protocol::DeviceSession;
using protocol::DeviceSessionConfig;
using protocol::DisplayResolution;
using protocol::InputEvent;
using protocol::InputKind;
using protocol::PixelFormat;
using protocol::SessionCommandStatus;
using protocol::SessionState;
using protocol::StreamProfile;
using protocol::VideoEncoder;

std::uint32_t RandomNonce()
{
    std::random_device device;
    std::uint32_t value = 0;
    while (value == 0) {
        value = (static_cast<std::uint32_t>(device()) << 16) ^ device();
    }
    return value;
}

CapabilitySet DeviceCapabilities()
{
    CapabilitySet capabilities;
    capabilities.encoders = {VideoEncoder::Hevc, VideoEncoder::H264};
    capabilities.pixel_formats = {PixelFormat::Nv12};
    capabilities.resolutions = {
        DisplayResolution{2800, 1840, 60},
        DisplayResolution{1920, 1280, 60},
        DisplayResolution{1280, 800, 60},
    };
    return capabilities;
}

const char *StateName(SessionState state)
{
    return protocol::session_state_name(state).data();
}

const char *EncoderName(VideoEncoder encoder)
{
    return encoder == VideoEncoder::Hevc ? "hevc" : "h264";
}

// Timer thread: poll() on the accessory descriptor ignores its timeout on this
// platform (probes/gate1/RUNBOOK.md 5.1 item 3), so periodic work is driven by
// an explicit eventfd tick instead of relying on a poll timeout.
class TickTimer {
public:
    explicit TickTimer(int wake_fd) : wake_fd_(wake_fd) {}
    ~TickTimer() { Stop(); }

    void Start()
    {
        thread_ = std::thread([this] {
            std::unique_lock lock(mutex_);
            while (!stop_) {
                cv_.wait_for(lock, std::chrono::milliseconds(kTimerIntervalMs));
                if (stop_) {
                    break;
                }
                const std::uint64_t value = 1;
                const ssize_t written = ::write(wake_fd_, &value, sizeof(value));
                (void)written;
            }
        });
    }

    void Stop()
    {
        {
            std::lock_guard lock(mutex_);
            stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

private:
    int wake_fd_ = -1;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::thread thread_;
};

class EngineBoundary final : public protocol::DeviceBoundary {
public:
    explicit EngineBoundary(DeviceEngine *engine) : engine_(engine) {}

    BoundaryResult prepare_stream(const StreamProfile &profile,
                                  std::uint32_t config_id) override
    {
        OH_LOG_INFO(LOG_APP,
                    "prepare_stream config=%{public}u encoder=%{public}s "
                    "%{public}dx%{public}d@%{public}u nv12=%{public}d",
                    config_id, EncoderName(profile.encoder),
                    profile.resolution.width, profile.resolution.height,
                    profile.resolution.refresh_hz,
                    profile.pixel_format == PixelFormat::Nv12 ? 1 : 0);
        if (profile.encoder != VideoEncoder::Hevc &&
            profile.encoder != VideoEncoder::H264) {
            OH_LOG_ERROR(LOG_APP, "prepare_stream rejected: encoder");
            return BoundaryResult::Failed;
        }
        if (profile.pixel_format != PixelFormat::Nv12) {
            OH_LOG_ERROR(LOG_APP, "prepare_stream rejected: pixel format");
            return BoundaryResult::Failed;
        }
        if (profile.resolution.width == 0 || profile.resolution.height == 0) {
            OH_LOG_ERROR(LOG_APP, "prepare_stream rejected: empty resolution");
            return BoundaryResult::Failed;
        }
        OH_LOG_INFO(LOG_APP, "prepare_stream accepted config=%{public}u",
                    config_id);
        return BoundaryResult::Ok;
    }

    BoundaryResult commit_stream(const StreamProfile &profile,
                                 std::uint32_t config_id,
                                 std::uint32_t video_stream_id) override
    {
        const bool ok = engine_->CommitProfile(profile);
        OH_LOG_INFO(LOG_APP,
                    "commit_stream config=%{public}u stream=%{public}u "
                    "encoder=%{public}s %{public}dx%{public}d@%{public}u ok=%{public}d",
                    config_id, video_stream_id, EncoderName(profile.encoder),
                    profile.resolution.width, profile.resolution.height,
                    profile.resolution.refresh_hz, ok ? 1 : 0);
        return ok ? BoundaryResult::Ok : BoundaryResult::Failed;
    }

    void release_stream(std::uint32_t config_id,
                        std::uint32_t video_stream_id) override
    {
        OH_LOG_INFO(LOG_APP,
                    "release_stream config=%{public}u stream=%{public}u",
                    config_id, video_stream_id);
        engine_->ReleaseDecoder();
    }

private:
    DeviceEngine *engine_;
};

}  // namespace

DeviceEngine &DeviceEngine::Instance()
{
    static DeviceEngine engine;
    return engine;
}

DeviceEngine::DeviceEngine() = default;

DeviceEngine::~DeviceEngine()
{
    (void)StopSession();
}

bool DeviceEngine::StartSession(int accessory_fd)
{
    if (accessory_fd <= 0) {
        std::lock_guard lock(mutex_);
        last_error_ = "invalid accessory fd";
        return false;
    }
    if (running_.load()) {
        return true;
    }
    // A worker that exited on its own (accessory unplugged) is still joinable;
    // assigning over a joinable thread would terminate the process.
    if (worker_.joinable()) {
        worker_.join();
    }
    {
        std::lock_guard lock(mutex_);
        accessory_fd_ = accessory_fd;
        stop_requested_.store(false);
        last_error_.clear();
        session_failure_.clear();
        session_state_ = "starting";
        ++sessions_started_;
    }
    running_.store(true);
    worker_ = std::thread([this] { WorkerMain(); });
    OH_LOG_INFO(LOG_APP, "session worker started fd=%{public}d", accessory_fd);
    return true;
}

bool DeviceEngine::StopSession()
{
    if (!worker_.joinable()) {
        running_.store(false);
        return true;
    }
    RequestStop();
    worker_.join();
    running_.store(false);
    OH_LOG_INFO(LOG_APP, "session worker joined");
    return true;
}

void DeviceEngine::RequestStop()
{
    stop_requested_.store(true);
    if (wake_fd_ >= 0) {
        const std::uint64_t value = 1;
        const ssize_t written = ::write(wake_fd_, &value, sizeof(value));
        (void)written;
    }
    OH_LOG_INFO(LOG_APP, "session stop requested");
}

bool DeviceEngine::SurfaceCreated(const std::string &surface_id)
{
    std::lock_guard lock(mutex_);
    surface_id_ = surface_id;
    surface_dirty_ = true;
    return true;
}

bool DeviceEngine::SurfaceDestroyed(const std::string &surface_id)
{
    std::lock_guard lock(mutex_);
    if (surface_id_ == surface_id) {
        surface_id_.clear();
        native_window_ = nullptr;
        surface_dirty_ = true;
    }
    return true;
}

void DeviceEngine::SetNativeWindow(void *window)
{
    std::lock_guard lock(mutex_);
    native_window_ = window;
    surface_dirty_ = true;
}

void DeviceEngine::NoteLifecycle(const std::string &event)
{
    std::lock_guard lock(mutex_);
    lifecycle_ = event;
}

bool DeviceEngine::SendInput(std::uint8_t kind, std::uint8_t pointer_id,
                             std::uint16_t flags, float x, float y, float dx,
                             float dy, std::uint32_t keycode)
{
    std::lock_guard lock(mutex_);
    if (input_queue_.size() >= kMaxQueuedInput) {
        input_queue_.pop_front();
        ++input_events_rejected_;
    }
    InputEvent event;
    event.kind = static_cast<InputKind>(kind);
    event.pointer_id = pointer_id;
    event.flags = flags;
    event.x = x;
    event.y = y;
    event.dx = dx;
    event.dy = dy;
    event.keycode = keycode;
    input_queue_.push_back(event);
    return true;
}

bool DeviceEngine::CommitProfile(const StreamProfile &profile)
{
    std::lock_guard lock(mutex_);
    pending_profile_ = profile;
    profile_dirty_ = true;
    return true;
}

void DeviceEngine::ReleaseDecoder()
{
    std::lock_guard lock(mutex_);
    release_requested_ = true;
}

bool DeviceEngine::EnsureDecoder()
{
    std::optional<StreamProfile> profile;
    void *window = nullptr;
    {
        std::lock_guard lock(mutex_);
        surface_dirty_ = false;
        if (!profile_dirty_ && decoder_ != nullptr) {
            return decoder_ != nullptr;
        }
        profile = pending_profile_;
        window = native_window_;
    }
    if (!profile.has_value()) {
        return false;
    }

    DecoderConfig config;
    config.mime = profile->encoder == VideoEncoder::Hevc
                      ? OH_AVCODEC_MIMETYPE_VIDEO_HEVC
                      : OH_AVCODEC_MIMETYPE_VIDEO_AVC;
    config.width = profile->resolution.width;
    config.height = profile->resolution.height;
    config.frame_rate = profile->resolution.refresh_hz;

    if (decoder_ == nullptr) {
        decoder_ = std::make_unique<VideoDecoder>(wake_fd_);
    }
    const bool ok = decoder_->Open(config, window);
    {
        std::lock_guard lock(mutex_);
        profile_dirty_ = false;
        if (ok) {
            ++decoder_opens_;
            negotiated_ = std::string(config.mime) + " " +
                          std::to_string(config.width) + "x" +
                          std::to_string(config.height) + "@" +
                          std::to_string(config.frame_rate);
        } else {
            last_error_ = decoder_->last_error();
        }
    }
    return ok;
}

// COMMIT can arrive before the XComponent surface exists (AOA re-enumeration
// moves the Ability to the background). When the window shows up later the
// decoder is reopened against it instead of staying surface-less forever.
void DeviceEngine::ApplyPendingSurface()
{
    bool dirty = false;
    void *window = nullptr;
    {
        std::lock_guard lock(mutex_);
        dirty = surface_dirty_;
        window = native_window_;
    }
    if (!dirty) {
        return;
    }
    if (decoder_ == nullptr) {
        std::lock_guard lock(mutex_);
        surface_dirty_ = false;
        return;
    }
    if (window == nullptr) {
        if (decoder_->surface_attached()) {
            // The surface is gone: freeze on the last frame and reopen later.
            decoder_->Close();
            decoder_.reset();
            std::lock_guard lock(mutex_);
            profile_dirty_ = true;
            surface_dirty_ = false;
        } else {
            std::lock_guard lock(mutex_);
            surface_dirty_ = false;
        }
        return;
    }
    if (decoder_->surface_attached()) {
        std::lock_guard lock(mutex_);
        surface_dirty_ = false;
        return;
    }
    decoder_->Close();
    decoder_.reset();
    {
        std::lock_guard lock(mutex_);
        profile_dirty_ = true;
        surface_dirty_ = false;
    }
    (void)EnsureDecoder();
    OH_LOG_INFO(LOG_APP, "decoder attached to late surface");
}

void DeviceEngine::DrainVideo()
{
    if (session_ == nullptr || decoder_ == nullptr) {
        return;
    }
    auto frames = session_->take_video_frames();
    // A paused stream keeps the last rendered frame on screen: the frames that
    // keep arriving are consumed from the session but never pushed, otherwise
    // the surface would keep advancing while the host believes it is frozen.
    const bool paused = session_->state() == SessionState::Paused;
    for (auto &frame : frames) {
        if (paused) {
            std::lock_guard lock(mutex_);
            ++frames_dropped_;
            continue;
        }
        const bool keyframe =
            (frame.header.flags & protocol::kFlagKeyFrame) != 0;
        if (decoder_->Push(frame.payload, static_cast<std::int64_t>(frame.header.timestamp_us),
                           keyframe)) {
            std::lock_guard lock(mutex_);
            ++frames_received_;
        } else {
            std::lock_guard lock(mutex_);
            ++frames_dropped_;
        }
    }
}

void DeviceEngine::PumpTimer()
{
    // Drain the eventfd counter so the next tick blocks again.
    std::uint64_t value = 0;
    while (::read(wake_fd_, &value, sizeof(value)) > 0) {
    }
}

void DeviceEngine::WorkerMain()
{
    wake_fd_ = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wake_fd_ < 0) {
        std::lock_guard lock(mutex_);
        last_error_ = "eventfd creation failed";
        running_.store(false);
        return;
    }
    FdByteIo io(accessory_fd_);
    protocol::SteadyClock clock;
    EngineBoundary boundary(this);
    TickTimer timer(wake_fd_);
    timer.Start();

    int rebuilds = 0;
    auto last_report_ns = std::chrono::steady_clock::now();
    while (!stop_requested_.load()) {
        {
            std::lock_guard lock(mutex_);
            session_ = std::make_unique<DeviceSession>(
                io, clock, DeviceSessionConfig{RandomNonce(), DeviceCapabilities()},
                &boundary);
        }
        session_->start();

        bool rebuild = false;
        while (!stop_requested_.load()) {
            // poll() on the accessory descriptor ignores its timeout on this
            // platform, so this loop is driven by the 5 ms eventfd tick; the
            // descriptor is only watched for readable/hangup transitions. A
            // NAK'd OUT endpoint is retried on the next tick rather than via
            // POLLOUT, which would otherwise make poll() spin when writable.
            pollfd fds[2] = {
                {accessory_fd_, POLLIN, 0},
                {wake_fd_, POLLIN, 0},
            };
            const int ready = ::poll(fds, 2, 1000);
            if (ready < 0 && errno != EINTR) {
                std::lock_guard lock(mutex_);
                last_error_ = std::string("poll failed: ") + std::strerror(errno);
                rebuild = true;
                break;
            }
            if ((fds[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0) {
                io.note_poll_revents(fds[0].revents);
                std::lock_guard lock(mutex_);
                last_error_ = "accessory descriptor hung up";
                rebuild = true;
                break;
            }
            io.note_poll_revents(fds[0].revents);
            if ((fds[1].revents & POLLIN) != 0) {
                PumpTimer();
            }
            session_->pump();

            bool dirty = false;
            bool release = false;
            {
                std::lock_guard lock(mutex_);
                dirty = profile_dirty_;
                release = release_requested_;
                release_requested_ = false;
            }
            if (release && decoder_ != nullptr) {
                decoder_->Close();
                decoder_.reset();
            }
            ApplyPendingSurface();
            if (dirty) {
                (void)EnsureDecoder();
            }
            if (decoder_ != nullptr) {
                decoder_->ProcessEvents();
            }
            DrainVideo();

            {
                std::lock_guard lock(mutex_);
                if (decoder_ != nullptr) {
                    decoder_stats_ = decoder_->stats();
                }
                while (!input_queue_.empty() && session_ != nullptr) {
                    const InputEvent event = input_queue_.front();
                    input_queue_.pop_front();
                    if (session_->send_input(event) == SessionCommandStatus::Ok) {
                        ++input_events_sent_;
                    } else {
                        ++input_events_rejected_;
                    }
                }
                if (session_ != nullptr) {
                    session_state_ = StateName(session_->state());
                    if (session_->failure().has_value()) {
                        session_failure_ =
                            std::string(protocol::session_failure_name(
                                *session_->failure()));
                    }
                }
            }

            if (session_->is_terminal()) {
                const auto failure = session_->failure();
                OH_LOG_ERROR(LOG_APP,
                             "session terminal state=%{public}s "
                             "failure=%{public}s detail=%{public}s "
                             "rebuilds=%{public}d",
                             StateName(session_->state()),
                             failure.has_value()
                                 ? protocol::session_failure_name(*failure)
                                       .data()
                                 : "none",
                             std::string(session_->failure_detail()).c_str(),
                             rebuilds);
                rebuild = true;
                break;
            }

            // One line per second so a stalled handshake can be diagnosed from
            // hilog alone: which state the session sits in and what the
            // descriptor actually did.
            const auto now = std::chrono::steady_clock::now();
            if (now - last_report_ns >= std::chrono::seconds(1)) {
                last_report_ns = now;
                const auto stats = io.stats();
                const auto state = session_->state();
                const bool surface = surface_id_.size() > 0;
                const bool window = native_window_ != nullptr;
                const bool decoder_open = decoder_ != nullptr && decoder_->open();
                const bool decoder_surface =
                    decoder_ != nullptr && decoder_->surface_attached();
                OH_LOG_INFO(
                    LOG_APP,
                    "state=%{public}s failure=%{public}s detail=%{public}s "
                    "reads=%{public}llu bytes=%{public}llu read_err=%{public}llu "
                    "errno=%{public}d zero=%{public}llu "
                    "write_err=%{public}llu werrno=%{public}d "
                    "tx=%{public}llu pending=%{public}zu einval=%{public}llu "
                    "wblk=%{public}llu partial=%{public}llu "
                    "revents=0x%{public}x nonce=0x%{public}x "
                    "rebuilds=%{public}d stop=%{public}d "
                    "surface=%{public}d window=%{public}d decoder=%{public}d "
                    "dopen=%{public}d dsurface=%{public}d "
                    "rx=%{public}llu dropped=%{public}llu "
                    "dsubmit=%{public}llu dinput_cb=%{public}llu "
                    "dout_cb=%{public}llu dinput=%{public}llu "
                    "drendered=%{public}llu ddrop=%{public}llu "
                    "derr=%{public}llu dstale=%{public}llu dstream=%{public}llu "
                    "width=%{public}d height=%{public}d fmt=%{public}d "
                    "derror=%{public}s",
                    StateName(state),
                    session_->failure().has_value()
                        ? protocol::session_failure_name(*session_->failure())
                              .data()
                        : "none",
                    std::string(session_->failure_detail()).c_str(),
                    static_cast<unsigned long long>(stats.reads),
                    static_cast<unsigned long long>(stats.bytes_read),
                    static_cast<unsigned long long>(stats.read_errors),
                    stats.last_read_errno,
                    static_cast<unsigned long long>(stats.zero_reads),
                    static_cast<unsigned long long>(stats.write_errors),
                    stats.last_write_errno,
                    static_cast<unsigned long long>(stats.bytes_written),
                    stats.pending_write_bytes,
                    static_cast<unsigned long long>(stats.einval_shrinks),
                    static_cast<unsigned long long>(stats.write_would_block),
                    static_cast<unsigned long long>(stats.write_partials),
                    stats.last_poll_revents, session_->session_nonce(), rebuilds,
                    stop_requested_.load() ? 1 : 0, surface ? 1 : 0,
                    window ? 1 : 0, decoder_ != nullptr ? 1 : 0,
                    decoder_open ? 1 : 0, decoder_surface ? 1 : 0,
                    static_cast<unsigned long long>(frames_received_),
                    static_cast<unsigned long long>(frames_dropped_),
                    static_cast<unsigned long long>(decoder_stats_.frames_submitted),
                    static_cast<unsigned long long>(decoder_stats_.input_callbacks),
                    static_cast<unsigned long long>(decoder_stats_.output_callbacks),
                    static_cast<unsigned long long>(decoder_stats_.input_buffers),
                    static_cast<unsigned long long>(decoder_stats_.frames_rendered),
                    static_cast<unsigned long long>(decoder_stats_.frames_dropped),
                    static_cast<unsigned long long>(decoder_stats_.output_errors),
                    static_cast<unsigned long long>(decoder_stats_.stale_callbacks),
                    static_cast<unsigned long long>(decoder_stats_.stream_changes),
                    decoder_stats_.width, decoder_stats_.height,
                    decoder_stats_.pixel_format,
                    decoder_ != nullptr ? decoder_->last_error().c_str() : "none");
            }
        }

        if (decoder_ != nullptr) {
            decoder_->Close();
            decoder_.reset();
        }
        session_.reset();
        if (!rebuild || stop_requested_.load()) {
            break;
        }
        if (++rebuilds > kMaxSessionRebuilds) {
            std::lock_guard lock(mutex_);
            last_error_ = "session rebuild limit reached";
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    timer.Stop();
    if (decoder_ != nullptr) {
        decoder_->Close();
        decoder_.reset();
    }
    session_.reset();
    if (wake_fd_ >= 0) {
        ::close(wake_fd_);
        wake_fd_ = -1;
    }
    {
        std::lock_guard lock(mutex_);
        ++worker_exits_;
        session_state_ = "stopped";
    }
    running_.store(false);
    OH_LOG_INFO(LOG_APP, "session worker exited");
}

std::string DeviceEngine::SnapshotJson() const
{
    std::lock_guard lock(mutex_);
    std::ostringstream json;
    const VideoDecoder::Stats decoder_stats = decoder_stats_;
    json << "{"
         << "\"session\":\"" << session_state_ << "\","
         << "\"failure\":\"" << session_failure_ << "\","
         << "\"profile\":\"" << negotiated_ << "\","
         << "\"lifecycle\":\"" << lifecycle_ << "\","
         << "\"surface\":\"" << (surface_id_.empty() ? "none" : "attached") << "\","
         << "\"framesReceived\":" << frames_received_ << ","
         << "\"framesDropped\":" << frames_dropped_ << ","
         << "\"decoderRendered\":" << decoder_stats.frames_rendered << ","
         << "\"decoderSubmitted\":" << decoder_stats.frames_submitted << ","
         << "\"decoderDropped\":" << decoder_stats.frames_dropped << ","
         << "\"decoderOpens\":" << decoder_opens_ << ","
         << "\"inputSent\":" << input_events_sent_ << ","
         << "\"inputRejected\":" << input_events_rejected_ << ","
         << "\"sessions\":" << sessions_started_ << ","
         << "\"workerExits\":" << worker_exits_ << ","
         << "\"error\":\"" << last_error_ << "\""
         << "}";
    return json.str();
}

}  // namespace deskmate::device
