#pragma once

#include "deskmate/windows/aoa_links.hpp"

#include "deskmate/clock.hpp"

#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

namespace deskmate::windows {

// HostSession adapter. This is the only place the Windows host binds the shared
// core to a USB transport; capture, encoding and input injection stay outside.
//
// Frames arrive on the encoder thread and are queued; `pump()` owns every
// HostSession call, so the encoder never races the USB pump. A separate mutex
// serializes HostSession access because disconnect() may stop a link while the
// pump thread is still inside pump().
class SessionLink final : public LinkSession {
 public:
  SessionLink(std::unique_ptr<LinkTransport> transport,
              deskmate::platform::AoaIdentity identity, std::uint64_t link_id,
              std::uint32_t video_stream_id, std::uint32_t config_id,
              std::uint16_t width, std::uint16_t height, std::uint16_t hz,
              bool allow_hevc, bool accept_device_resolution = true);
  ~SessionLink() override;

  [[nodiscard]] std::uint64_t id() const override { return link_id_; }
  [[nodiscard]] const deskmate::platform::AoaIdentity& identity()
      const override {
    return identity_;
  }
  [[nodiscard]] deskmate::protocol::SessionState state() const override;
  [[nodiscard]] std::string negotiated_summary() const override;
  [[nodiscard]] LinkStats stats() const override;

  void submit_video(std::vector<std::uint8_t> annex_b, std::uint64_t pts_us,
                    bool keyframe) override;
  [[nodiscard]] std::vector<deskmate::protocol::InputEvent> take_input()
      override;
  [[nodiscard]] std::uint32_t take_keyframe_request() override;
  [[nodiscard]] bool capture_requested() const override {
    return capture_intent_.load(std::memory_order_acquire);
  }
  [[nodiscard]] bool input_requested() const override {
    return input_intent_.load(std::memory_order_acquire);
  }
  [[nodiscard]] std::optional<deskmate::protocol::StreamProfile>
  negotiated_profile() const override;

  [[nodiscard]] bool has_pending_output() const override;
  [[nodiscard]] bool is_terminal() const override;
  [[nodiscard]] std::string failure_reason() const override;
  [[nodiscard]] std::string transport_last_error() const override;
  [[nodiscard]] std::uint64_t reconnect_count() const override {
    return reconnects_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t rx_bytes() const override;
  [[nodiscard]] std::uint64_t tx_bytes() const override;
  [[nodiscard]] bool transport_alive() const override;
  [[nodiscard]] bool consume_write_blocked() override;
  void set_rx_callback(std::function<void()> callback) override;
  void set_accept_device_resolution(bool enabled) override;
  void restart_session() override;
  [[nodiscard]] deskmate::protocol::SessionCommandStatus handle_host_event(
      deskmate::protocol::HostEvent event) override;

  void pump() override;
  void stop() override;
  // Releases the USB handle. Must run after the pump loop stopped calling
  // pump(), because the reader thread owns the handle until then.
  void close_transport() override;

 private:
  class Boundary;

  void start_new_session(std::uint32_t video_stream_id,
                         std::uint32_t config_id);

  static constexpr std::size_t kMaxPendingVideoFrames = 16;

  std::uint64_t link_id_;
  deskmate::platform::AoaIdentity identity_;
  std::uint16_t width_;
  std::uint16_t height_;
  std::uint16_t hz_;
  bool allow_hevc_;
  std::atomic<bool> accept_device_resolution_;
  std::unique_ptr<LinkTransport> transport_;
  deskmate::protocol::SteadyClock clock_;
  std::unique_ptr<Boundary> boundary_;
  std::unique_ptr<deskmate::protocol::HostSession> session_;
  mutable std::mutex session_mutex_;
  std::atomic<bool> capture_intent_{false};
  std::atomic<bool> input_intent_{false};
  mutable std::mutex video_mutex_;
  std::deque<PendingVideo> video_queue_;
  std::atomic<std::uint64_t> submitted_{0};
  std::atomic<std::uint64_t> sent_{0};
  std::atomic<std::uint64_t> dropped_{0};
  std::atomic<std::uint64_t> input_events_{0};
  std::atomic<std::uint64_t> reconnects_{0};
  std::atomic<std::uint32_t> requested_idr_{0};
};

}  // namespace deskmate::windows
