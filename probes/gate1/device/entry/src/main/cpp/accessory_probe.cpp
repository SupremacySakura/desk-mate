#define LOG_DOMAIN 0x3201
#define LOG_TAG "Gate1Probe"

#include "accessory_probe.hpp"

#include "probe_protocol.hpp"
#include "probe_status.hpp"

#include <hilog/log.h>

#include <array>
#include <cerrno>
#include <cinttypes>
#include <cstring>
#include <exception>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sstream>
#include <sys/eventfd.h>
#include <unistd.h>
#include <vector>

namespace gate1 {
namespace {

std::int64_t steady_now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void sigusr1_handler(int)
{
}

bool install_signal_handler()
{
    static std::atomic<bool> installed{false};
    if (installed.load()) {
        return true;
    }
    struct sigaction action {};
    action.sa_handler = sigusr1_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    if (sigaction(SIGUSR1, &action, nullptr) != 0) {
        return false;
    }
    installed.store(true);
    return true;
}

void append_u64_le(std::vector<std::uint8_t>& output, std::uint64_t value)
{
    for (int i = 0; i < 8; ++i) {
        output.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFFU));
    }
}

const char* errno_text(int value)
{
    return std::strerror(value);
}

} // namespace

AccessoryProbe::WakeMode AccessoryProbe::parse_mode(const std::string& mode)
{
    if (mode == "eventfd") {
        return WakeMode::kEventFd;
    }
    if (mode == "signal") {
        return WakeMode::kSignal;
    }
    if (mode == "owner-close") {
        return WakeMode::kOwnerClose;
    }
    return WakeMode::kNone;
}

const char* AccessoryProbe::mode_name(WakeMode mode)
{
    switch (mode) {
        case WakeMode::kEventFd:
            return "eventfd";
        case WakeMode::kSignal:
            return "signal";
        case WakeMode::kOwnerClose:
            return "owner-close";
        default:
            return "none";
    }
}

std::string AccessoryProbe::start(int accessory_fd, const std::string& mode)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (running_.load() || worker_.joinable()) {
        return status_json(false, "probe already running or previous worker not joined");
    }
    if (accessory_fd <= 0) {
        return status_json(false, "accessoryFd must be positive");
    }
    const WakeMode requested = parse_mode(mode);
    if (requested == WakeMode::kNone) {
        return status_json(false, "unknown wake mode");
    }

    event_fd_ = -1;
    if (requested == WakeMode::kEventFd) {
        event_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (event_fd_ < 0) {
            return status_json(
                false,
                std::string("eventfd creation failed: ") + errno_text(errno));
        }
    }
    if (requested == WakeMode::kSignal && !install_signal_handler()) {
        return status_json(
            false,
            std::string("sigaction(SIGUSR1) failed: ") + errno_text(errno));
    }

    accessory_fd_ = accessory_fd;
    mode_ = requested;
    stop_requested_.store(false);
    reject_new_work_.store(false);
    stop_requested_ns_.store(0);
    wake_observed_ns_.store(0);
    worker_exit_ns_.store(0);
    last_read_errno_.store(0);
    last_poll_revents_.store(0);
    {
        std::lock_guard<std::mutex> status_lock(status_mutex_);
        exit_reason_ = "running";
        wake_reason_ = "";
    }
    {
        std::lock_guard<std::mutex> join_lock(join_mutex_);
        worker_done_ = false;
    }

    try {
        worker_ = std::thread(&AccessoryProbe::worker_main, this);
    } catch (const std::exception& error) {
        if (event_fd_ >= 0) {
            close(event_fd_);
            event_fd_ = -1;
        }
        return status_json(
            false,
            std::string("worker thread creation failed: ") + error.what());
    }
    running_.store(true);
    OH_LOG_INFO(
        LOG_APP,
        "start accessoryFd=%{public}d mode=%{public}s",
        accessory_fd,
        mode_name(requested));
    return status_json(true, "worker started");
}

bool AccessoryProbe::wait_for_worker_join(std::chrono::milliseconds timeout)
{
    std::unique_lock<std::mutex> lock(join_mutex_);
    const bool done = worker_done_cv_.wait_for(
        lock,
        timeout,
        [this]() { return worker_done_; });
    if (!done) {
        return false;
    }

    lock.unlock();
    if (worker_.joinable()) {
        worker_.join();
    }
    lock.lock();
    close_event_fd_if_idle();
    running_.store(false);
    return true;
}

void AccessoryProbe::close_event_fd_if_idle()
{
    if (event_fd_ >= 0) {
        close(event_fd_);
        event_fd_ = -1;
    }
}

std::string AccessoryProbe::stop_and_join(const std::string& mode)
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    const WakeMode requested = parse_mode(mode);
    if (!running_.load()) {
        return status_json(false, "probe is not running");
    }
    if (requested != mode_) {
        return status_json(false, "stop mode does not match active mode");
    }
    if (mode_ == WakeMode::kOwnerClose) {
        return status_json(
            false,
            "owner-close sequence: beginOwnerCloseStop -> closeAccessory -> finishOwnerCloseJoin");
    }

    reject_new_work_.store(true);
    stop_requested_.store(true);
    stop_requested_ns_.store(steady_now_ns());

    if (mode_ == WakeMode::kEventFd) {
        const std::uint64_t one = 1;
        const ssize_t written = write(event_fd_, &one, sizeof(one));
        if (written != static_cast<ssize_t>(sizeof(one))) {
            OH_LOG_ERROR(
                LOG_APP,
                "eventfd wake write failed: %{public}s",
                errno_text(errno));
        }
    } else if (mode_ == WakeMode::kSignal) {
        const int rc = pthread_kill(worker_.native_handle(), SIGUSR1);
        if (rc != 0) {
            OH_LOG_ERROR(
                LOG_APP,
                "pthread_kill(SIGUSR1) failed: %{public}s",
                errno_text(rc));
        }
    }

    if (!wait_for_worker_join(std::chrono::milliseconds(5000))) {
        return status_json(false, "join timed out; handle must remain open");
    }
    OH_LOG_INFO(LOG_APP, "worker joined after %{public}s wake", mode_name(mode_));
    return status_json(true, "worker joined; ArkTS may closeAccessory");
}

std::string AccessoryProbe::begin_owner_close_stop()
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (!running_.load()) {
        return status_json(false, "probe is not running");
    }
    if (mode_ != WakeMode::kOwnerClose) {
        return status_json(false, "active mode is not owner-close");
    }
    reject_new_work_.store(true);
    stop_requested_.store(true);
    stop_requested_ns_.store(steady_now_ns());
    OH_LOG_INFO(LOG_APP, "owner-close stop requested; closeAccessory may wake poll");
    return status_json(
        true,
        "new work rejected; call closeAccessory, then finishOwnerCloseJoin");
}

std::string AccessoryProbe::finish_owner_close_join()
{
    std::lock_guard<std::mutex> lock(control_mutex_);
    if (mode_ != WakeMode::kOwnerClose) {
        return status_json(false, "active mode is not owner-close");
    }
    if (!stop_requested_.load()) {
        return status_json(false, "call beginOwnerCloseStop before joining");
    }
    if (!wait_for_worker_join(std::chrono::milliseconds(5000))) {
        return status_json(false, "owner close did not wake worker before join timeout");
    }
    return status_json(true, "owner-close sequence complete; C++ did not close accessoryFd");
}

void AccessoryProbe::worker_main()
{
    if (mode_ == WakeMode::kSignal) {
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGUSR1);
        pthread_sigmask(SIG_UNBLOCK, &set, nullptr);
    }

    std::vector<std::uint8_t> buffer(65536);
    const char* exit_reason = "stop-observed";

    while (true) {
        std::array<pollfd, 2> fds {};
        fds[0].fd = accessory_fd_;
        fds[0].events = POLLIN;
        nfds_t count = 1;
        if (event_fd_ >= 0) {
            fds[1].fd = event_fd_;
            fds[1].events = POLLIN;
            count = 2;
        }

        const int poll_result = poll(fds.data(), count, 250);
        if (poll_result < 0) {
            const int poll_errno = errno;
            if (errno == EINTR) {
                if (stop_requested_.load() && mode_ == WakeMode::kSignal) {
                    record_wake("signal");
                    exit_reason = "signal";
                    break;
                }
                continue;
            }
            if (stop_requested_.load() && mode_ == WakeMode::kOwnerClose) {
                record_wake("owner-close");
                exit_reason = "owner-close";
                break;
            }
            read_errors_.fetch_add(1);
            last_read_errno_.store(poll_errno);
            last_poll_revents_.store(0);
            OH_LOG_ERROR(
                LOG_APP,
                "poll failed: %{public}s",
                probe_error_context(
                    poll_errno,
                    accessory_fd_,
                    0,
                    mode_name(mode_)).c_str());
            exit_reason = "poll-error";
            break;
        }
        if (poll_result == 0) {
            poll_timeouts_.fetch_add(1);
            continue;
        }
        poll_wakeups_.fetch_add(1);

        if (event_fd_ >= 0 && (fds[1].revents & POLLIN) != 0) {
            std::uint64_t value = 0;
            const ssize_t bytes = read(event_fd_, &value, sizeof(value));
            if (bytes == static_cast<ssize_t>(sizeof(value)) &&
                stop_requested_.load()) {
                record_wake("eventfd");
                exit_reason = "eventfd";
                break;
            }
            continue;
        }

        const short errors = static_cast<short>(POLLERR | POLLHUP | POLLNVAL);
        if ((fds[0].revents & errors) != 0) {
            last_poll_revents_.store(fds[0].revents);
            last_read_errno_.store(0);
            if (stop_requested_.load() && mode_ == WakeMode::kOwnerClose) {
                record_wake("owner-close");
                exit_reason = "owner-close";
            } else {
                read_errors_.fetch_add(1);
                OH_LOG_ERROR(
                    LOG_APP,
                    "accessory poll error: %{public}s",
                    probe_error_context(
                        0,
                        accessory_fd_,
                        fds[0].revents,
                        mode_name(mode_)).c_str());
                exit_reason = "accessory-error";
            }
            break;
        }

        if ((fds[0].revents & POLLIN) != 0) {
            const ssize_t bytes = read(accessory_fd_, buffer.data(), buffer.size());
            if (bytes > 0) {
                bytes_read_.fetch_add(static_cast<std::uint64_t>(bytes));
                if (!reject_new_work_.load()) {
                    process_input(buffer.data(), static_cast<std::size_t>(bytes));
                }
                continue;
            }
            if (bytes == 0) {
                zero_reads_.fetch_add(1);
                continue;
            }
            const int read_errno = errno;
            last_read_errno_.store(read_errno);
            last_poll_revents_.store(fds[0].revents);
            if (read_errno == EINTR) {
                if (stop_requested_.load() && mode_ == WakeMode::kSignal) {
                    record_wake("signal");
                    exit_reason = "signal";
                    break;
                }
                continue;
            }
            if (read_errno == EAGAIN || read_errno == EWOULDBLOCK) {
                continue;
            }
            if (stop_requested_.load() && mode_ == WakeMode::kOwnerClose) {
                record_wake("owner-close");
                exit_reason = "owner-close";
            } else {
                read_errors_.fetch_add(1);
                OH_LOG_ERROR(
                    LOG_APP,
                    "read failed: %{public}s",
                    probe_error_context(
                        read_errno,
                        accessory_fd_,
                        fds[0].revents,
                        mode_name(mode_)).c_str());
                exit_reason = "read-error";
            }
            break;
        }
    }

    record_exit(exit_reason);
}

void AccessoryProbe::process_input(const std::uint8_t* data, std::size_t size)
{
    std::vector<Frame> frames;
    std::string error;
    if (!decoder_.push(data, size, frames, error)) {
        parse_errors_.fetch_add(1);
        OH_LOG_ERROR(LOG_APP, "frame parse failed: %{public}s", error.c_str());
        decoder_.reset();
        return;
    }

    std::vector<std::uint8_t> output;
    std::uint64_t response_frames = 0;
    bool split_write = false;
    for (const Frame& frame : frames) {
        frames_read_.fetch_add(1);
        build_frame_response(frame, output);
        if (frame.type == FrameType::kHello ||
            frame.type == FrameType::kConfig ||
            frame.type == FrameType::kStatsRequest ||
            frame.type == FrameType::kHeartbeat ||
            frame.type == FrameType::kData) {
            ++response_frames;
        }
        if (frame.type == FrameType::kData && frame.seq % 5U == 0U) {
            split_write = true;
        }
    }
    if (output.empty()) {
        return;
    }

    bool ok = true;
    if (split_write && output.size() > 7U) {
        ok = write_all(output.data(), 7U) &&
             write_all(output.data() + 7U, output.size() - 7U);
    } else {
        ok = write_all(output.data(), output.size());
    }
    if (ok) {
        frames_written_.fetch_add(response_frames);
    }
}

void AccessoryProbe::build_frame_response(
    const Frame& frame,
    std::vector<std::uint8_t>& output)
{
    switch (frame.type) {
        case FrameType::kHello: {
            const std::vector<std::uint8_t> response =
                encode_frame(FrameType::kHelloAck, 0, frame.seq, {});
            output.insert(output.end(), response.begin(), response.end());
            return;
        }
        case FrameType::kConfig: {
            const std::vector<std::uint8_t> response =
                encode_frame(FrameType::kConfigAck, 0, frame.seq, {});
            output.insert(output.end(), response.begin(), response.end());
            return;
        }
        case FrameType::kStatsRequest:
            append_stats_response(output, frame.seq);
            return;
        case FrameType::kHeartbeat: {
            const std::vector<std::uint8_t> response =
                encode_frame(FrameType::kHeartbeatAck, 0, frame.seq, {});
            output.insert(output.end(), response.begin(), response.end());
            return;
        }
        case FrameType::kData: {
            if (frame.seq % 8U == 0U) {
                write_zero_length();
            }
            const std::vector<std::uint8_t> response =
                encode_frame(FrameType::kDataEcho, frame.flags, frame.seq, frame.payload);
            output.insert(output.end(), response.begin(), response.end());
            return;
        }
        case FrameType::kError:
            OH_LOG_WARN(LOG_APP, "host sent ERROR frame; ignored");
            return;
        default:
            OH_LOG_WARN(
                LOG_APP,
                "unknown frame type=%{public}d",
                static_cast<int>(frame.type));
            return;
    }
}

void AccessoryProbe::append_stats_response(
    std::vector<std::uint8_t>& output,
    std::uint32_t seq)
{
    std::vector<std::uint8_t> payload;
    payload.reserve(kDeviceStatsFieldCount * sizeof(std::uint64_t));
    append_u64_le(payload, zero_reads_.load());
    append_u64_le(payload, poll_wakeups_.load());
    append_u64_le(payload, bytes_read_.load());
    append_u64_le(payload, frames_read_.load());
    append_u64_le(payload, bytes_written_.load());
    append_u64_le(payload, frames_written_.load());
    append_u64_le(payload, write_errors_.load());
    append_u64_le(payload, eventfd_wakeups_.load());
    append_u64_le(payload, signal_wakeups_.load());
    append_u64_le(payload, owner_close_wakeups_.load());
    append_u64_le(payload, lifecycle_background_.load());
    append_u64_le(payload, lifecycle_foreground_.load());
    append_u64_le(payload, poll_timeouts_.load());
    append_u64_le(payload, read_errors_.load());
    append_u64_le(payload, zero_writes_.load());
    append_u64_le(payload, parse_errors_.load());
    const std::vector<std::uint8_t> response =
        encode_frame(FrameType::kStatsResponse, 0, seq, payload);
    output.insert(output.end(), response.begin(), response.end());
}

bool AccessoryProbe::write_all(const std::uint8_t* data, std::size_t size)
{
    std::size_t offset = 0;
    while (offset < size) {
        const ssize_t written = write(accessory_fd_, data + offset, size - offset);
        if (written > 0) {
            offset += static_cast<std::size_t>(written);
            bytes_written_.fetch_add(static_cast<std::uint64_t>(written));
            continue;
        }
        if (written < 0 && errno == EINTR) {
            continue;
        }
        write_errors_.fetch_add(1);
        OH_LOG_ERROR(LOG_APP, "write failed: %{public}s", errno_text(errno));
        return false;
    }
    return true;
}

bool AccessoryProbe::write_zero_length()
{
    std::uint8_t dummy = 0;
    const ssize_t written = write(accessory_fd_, &dummy, 0);
    if (written == 0) {
        zero_writes_.fetch_add(1);
        return true;
    }
    write_errors_.fetch_add(1);
    OH_LOG_ERROR(LOG_APP, "zero-length write failed: %{public}s", errno_text(errno));
    return false;
}

void AccessoryProbe::record_wake(const char* reason)
{
    std::int64_t expected = 0;
    const std::int64_t now = steady_now_ns();
    if (!wake_observed_ns_.compare_exchange_strong(expected, now)) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        wake_reason_ = reason;
    }
    if (std::strcmp(reason, "eventfd") == 0) {
        eventfd_wakeups_.fetch_add(1);
    } else if (std::strcmp(reason, "signal") == 0) {
        signal_wakeups_.fetch_add(1);
    } else if (std::strcmp(reason, "owner-close") == 0) {
        owner_close_wakeups_.fetch_add(1);
    }
    OH_LOG_INFO(LOG_APP, "poll wake observed: %{public}s", reason);
}

void AccessoryProbe::record_exit(const char* reason)
{
    worker_exit_ns_.store(steady_now_ns());
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        exit_reason_ = reason;
    }
    {
        std::lock_guard<std::mutex> lock(join_mutex_);
        worker_done_ = true;
    }
    worker_done_cv_.notify_all();
    OH_LOG_INFO(LOG_APP, "worker exit: %{public}s", reason);
}

std::string AccessoryProbe::status_json(bool ok, const std::string& message)
{
    const std::int64_t stop_ns = stop_requested_ns_.load();
    const std::int64_t wake_ns = wake_observed_ns_.load();
    const std::int64_t exit_ns = worker_exit_ns_.load();
    bool worker_done = false;
    {
        std::lock_guard<std::mutex> lock(join_mutex_);
        worker_done = worker_done_;
    }
    const bool worker_exited = worker_done || exit_ns > 0;
    const bool reported_running =
        probe_reports_running(running_.load(), worker_exited);
    std::string exit_reason;
    std::string wake_reason;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        exit_reason = exit_reason_;
        wake_reason = wake_reason_;
    }
    std::ostringstream json;
    json << "{\"ok\":" << (ok ? "true" : "false")
         << ",\"message\":\"" << message << "\""
         << ",\"mode\":\"" << mode_name(mode_) << "\""
         << ",\"running\":" << (reported_running ? "true" : "false")
         << ",\"stop_to_wake_us\":"
         << (stop_ns > 0 && wake_ns >= stop_ns ? (wake_ns - stop_ns) / 1000 : -1)
         << ",\"stop_to_worker_exit_ms\":"
         << (stop_ns > 0 && exit_ns >= stop_ns ? (exit_ns - stop_ns) / 1000000 : -1)
         << ",\"last_read_errno\":" << last_read_errno_.load()
         << ",\"last_poll_revents\":" << last_poll_revents_.load()
         << ",\"wake_reason\":\"" << wake_reason << "\""
         << ",\"exit_reason\":\"" << exit_reason << "\""
         << ",\"zero_reads\":" << zero_reads_.load()
         << ",\"zero_writes\":" << zero_writes_.load()
         << ",\"read_errors\":" << read_errors_.load()
         << ",\"write_errors\":" << write_errors_.load()
         << ",\"parse_errors\":" << parse_errors_.load()
         << ",\"eventfd_wakeups\":" << eventfd_wakeups_.load()
         << ",\"signal_wakeups\":" << signal_wakeups_.load()
         << ",\"owner_close_wakeups\":" << owner_close_wakeups_.load()
         << "}";
    return json.str();
}

std::string AccessoryProbe::stats_json()
{
    std::string exit_reason;
    std::string wake_reason;
    {
        std::lock_guard<std::mutex> lock(status_mutex_);
        exit_reason = exit_reason_;
        wake_reason = wake_reason_;
    }
    const std::int64_t stop_ns = stop_requested_ns_.load();
    const std::int64_t wake_ns = wake_observed_ns_.load();
    const std::int64_t exit_ns = worker_exit_ns_.load();
    bool worker_done = false;
    {
        std::lock_guard<std::mutex> lock(join_mutex_);
        worker_done = worker_done_;
    }
    const bool worker_exited = worker_done || exit_ns > 0;
    const bool reported_running =
        probe_reports_running(running_.load(), worker_exited);
    std::ostringstream json;
    json << "{\"running\":" << (reported_running ? "true" : "false")
         << ",\"worker_exited\":" << (worker_exited ? "true" : "false")
         << ",\"mode\":\"" << mode_name(mode_) << "\""
         << ",\"wake_reason\":\"" << wake_reason << "\""
         << ",\"exit_reason\":\"" << exit_reason << "\""
         << ",\"stop_to_wake_us\":"
         << (stop_ns > 0 && wake_ns >= stop_ns ? (wake_ns - stop_ns) / 1000 : -1)
         << ",\"stop_to_worker_exit_ms\":"
         << (stop_ns > 0 && exit_ns >= stop_ns ? (exit_ns - stop_ns) / 1000000 : -1)
         << ",\"last_read_errno\":" << last_read_errno_.load()
         << ",\"last_poll_revents\":" << last_poll_revents_.load()
         << ",\"zero_reads\":" << zero_reads_.load()
         << ",\"poll_wakeups\":" << poll_wakeups_.load()
         << ",\"bytes_read\":" << bytes_read_.load()
         << ",\"frames_read\":" << frames_read_.load()
         << ",\"bytes_written\":" << bytes_written_.load()
         << ",\"frames_written\":" << frames_written_.load()
         << ",\"write_errors\":" << write_errors_.load()
         << ",\"eventfd_wakeups\":" << eventfd_wakeups_.load()
         << ",\"signal_wakeups\":" << signal_wakeups_.load()
         << ",\"owner_close_wakeups\":" << owner_close_wakeups_.load()
         << ",\"lifecycle_background\":" << lifecycle_background_.load()
         << ",\"lifecycle_foreground\":" << lifecycle_foreground_.load()
         << ",\"poll_timeouts\":" << poll_timeouts_.load()
         << ",\"read_errors\":" << read_errors_.load()
         << ",\"zero_writes\":" << zero_writes_.load()
         << ",\"parse_errors\":" << parse_errors_.load()
         << "}";
    return json.str();
}

void AccessoryProbe::note_lifecycle(const std::string& event)
{
    if (event == "background") {
        lifecycle_background_.fetch_add(1);
    } else if (event == "foreground") {
        lifecycle_foreground_.fetch_add(1);
    }
    OH_LOG_INFO(LOG_APP, "lifecycle: %{public}s", event.c_str());
}

bool AccessoryProbe::running() const
{
    std::lock_guard<std::mutex> lock(join_mutex_);
    return probe_reports_running(
        running_.load(),
        worker_done_ || worker_exit_ns_.load() > 0);
}

} // namespace gate1
