#pragma once

#include "probe_protocol.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gate1 {

class AccessoryProbe {
public:
    std::string start(int accessory_fd, const std::string& mode);
    std::string stop_and_join(const std::string& mode);
    std::string begin_owner_close_stop();
    std::string finish_owner_close_join();
    std::string stats_json();
    void note_lifecycle(const std::string& event);
    bool running() const;

private:
    enum class WakeMode {
        kNone = 0,
        kEventFd = 1,
        kSignal = 2,
        kOwnerClose = 3,
    };

    void worker_main();
    void process_input(const std::uint8_t* data, std::size_t size);
    void build_frame_response(const struct Frame& frame, std::vector<std::uint8_t>& output);
    void append_stats_response(std::vector<std::uint8_t>& output, std::uint32_t seq);
    void enqueue_output(const std::vector<std::uint8_t>& bytes, std::uint64_t frames);
    bool flush_pending();
    std::size_t pending_output_size() const;
    bool make_accessory_nonblocking();
    void restore_accessory_flags();
    bool write_zero_length();
    void record_wake(const char* reason);
    void record_exit(const char* reason);
    void set_phase(const char* phase);
    bool wait_for_worker_join(std::chrono::milliseconds timeout);
    void close_event_fd_if_idle();
    static WakeMode parse_mode(const std::string& mode);
    static const char* mode_name(WakeMode mode);
    std::string status_json(bool ok, const std::string& message);

    std::mutex control_mutex_;
    mutable std::mutex join_mutex_;
    std::mutex status_mutex_;
    std::condition_variable worker_done_cv_;
    std::thread worker_;
    bool worker_done_ = false;
    std::atomic<bool> running_{false};

    int accessory_fd_ = -1;
    int event_fd_ = -1;
    WakeMode mode_ = WakeMode::kNone;
    std::string exit_reason_;
    std::string wake_reason_;

    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> reject_new_work_{false};
    std::atomic<std::int64_t> stop_requested_ns_{0};
    std::atomic<std::int64_t> wake_observed_ns_{0};
    std::atomic<std::int64_t> worker_exit_ns_{0};
    std::atomic<int> last_read_errno_{0};
    std::atomic<int> last_poll_revents_{0};
    std::atomic<std::size_t> last_read_size_{16384};
    std::mutex phase_mutex_;
    std::string phase_ = "idle";

    std::atomic<std::uint64_t> zero_reads_{0};
    std::atomic<std::uint64_t> poll_wakeups_{0};
    std::atomic<std::uint64_t> bytes_read_{0};
    std::atomic<std::uint64_t> frames_read_{0};
    std::atomic<std::uint64_t> bytes_written_{0};
    std::atomic<std::uint64_t> frames_written_{0};
    std::atomic<std::uint64_t> write_errors_{0};
    std::atomic<std::uint64_t> eventfd_wakeups_{0};
    std::atomic<std::uint64_t> signal_wakeups_{0};
    std::atomic<std::uint64_t> owner_close_wakeups_{0};
    std::atomic<std::uint64_t> lifecycle_background_{0};
    std::atomic<std::uint64_t> lifecycle_foreground_{0};
    std::atomic<std::uint64_t> poll_timeouts_{0};
    std::atomic<std::uint64_t> read_errors_{0};
    std::atomic<std::uint64_t> zero_writes_{0};
    std::atomic<std::uint64_t> parse_errors_{0};
    std::atomic<std::uint64_t> would_block_writes_{0};
    std::atomic<std::uint64_t> backpressure_skips_{0};
    std::atomic<std::uint64_t> frames_dropped_{0};
    std::atomic<std::uint64_t> pending_output_bytes_{0};

    // Output is queued per response batch so that frames_written_ is only
    // credited once a batch's bytes have all reached write(). A single flat
    // buffer cannot do this: a partially drained batch has no frame boundary,
    // and dropping the counter for the whole buffer loses the frames that are
    // still going to be written.
    struct OutputChunk {
        std::vector<std::uint8_t> bytes;
        std::uint64_t frames = 0;
    };
    std::deque<OutputChunk> pending_chunks_;
    std::size_t front_offset_ = 0;
    int saved_accessory_flags_ = -1;
    FrameDecoder decoder_;
};

} // namespace gate1
