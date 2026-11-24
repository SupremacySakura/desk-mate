#pragma once

#include "deskmate/session.hpp"
#include "video_decoder.hpp"

#include <atomic>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace deskmate::device {

// Owns the device-side session for the whole process lifetime.
//
// The AOA re-enumeration puts the Ability in the background, so nothing here
// depends on ArkTS callbacks firing: the worker thread owns the session, the
// descriptor, the decoder and the timer, and keeps running until ArkTS closes
// the accessory handle.
class DeviceEngine {
public:
    static DeviceEngine &Instance();

    DeviceEngine(const DeviceEngine &) = delete;
    DeviceEngine &operator=(const DeviceEngine &) = delete;
    ~DeviceEngine();

    bool StartSession(int accessory_fd);
    // Non-blocking: sets the stop flag and pokes the wake eventfd. ArkTS must
    // call usbManager.closeAccessory() between RequestStop() and StopSession():
    // only the official close produces the POLLHUP that unblocks poll().
    void RequestStop();
    // Returns once the worker has joined.
    bool StopSession();
    bool Running() const { return running_.load(); }

    bool SurfaceCreated(const std::string &surface_id);
    bool SurfaceDestroyed(const std::string &surface_id);
    void SetNativeWindow(void *window);

    bool SendInput(std::uint8_t kind, std::uint8_t pointer_id,
                   std::uint16_t flags, float x, float y, float dx, float dy,
                   std::uint32_t keycode);

    void NoteLifecycle(const std::string &event);
    std::string SnapshotJson() const;

    // Called by the session boundary on the worker thread.
    bool CommitProfile(const protocol::StreamProfile &profile);
    void ReleaseDecoder();

private:
    DeviceEngine();

    void WorkerMain();
    bool EnsureDecoder();
    void ApplyPendingSurface();
    void DrainVideo();
    void PumpTimer();

    mutable std::mutex mutex_;
    std::thread worker_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    int accessory_fd_ = -1;
    int wake_fd_ = -1;
    std::string surface_id_;
    void *native_window_ = nullptr;
    bool surface_dirty_ = false;
    std::string lifecycle_ = "unknown";
    std::string last_error_;
    std::unique_ptr<protocol::DeviceSession> session_;
    std::unique_ptr<VideoDecoder> decoder_;
    std::optional<protocol::StreamProfile> pending_profile_;
    bool profile_dirty_ = false;
    bool release_requested_ = false;
    std::deque<protocol::InputEvent> input_queue_;
    std::uint64_t frames_received_ = 0;
    std::uint64_t frames_dropped_ = 0;
    std::uint64_t input_events_sent_ = 0;
    std::uint64_t input_events_rejected_ = 0;
    std::uint64_t decoder_opens_ = 0;
    std::uint64_t sessions_started_ = 0;
    std::uint64_t worker_exits_ = 0;
    std::string session_state_ = "idle";
    std::string session_failure_;
    std::string negotiated_;
    VideoDecoder::Stats decoder_stats_{};
};

}  // namespace deskmate::device
