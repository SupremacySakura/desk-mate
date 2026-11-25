#include "deskmate/windows/aoa_links.hpp"
#include "deskmate/windows/host_session.hpp"

#include "deskmate/clock.hpp"
#include "deskmate/platform/session_profile.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <optional>
#include <random>
#include <sstream>
#include <utility>

namespace deskmate::windows {

std::uint32_t random_nonzero_u32() {
  std::random_device device;
  std::uint32_t value = 0;
  while (value == 0) {
    value = (static_cast<std::uint32_t>(device()) << 16U) ^ device();
  }
  return value;
}

class SessionLink::Boundary final : public deskmate::protocol::HostBoundary {
 public:
  explicit Boundary(SessionLink& owner) : owner_(owner) {}

  void set_capture_encoding_intent(bool enabled) override {
    owner_.capture_intent_.store(enabled, std::memory_order_release);
  }
  void set_input_injection_intent(bool enabled) override {
    owner_.input_intent_.store(enabled, std::memory_order_release);
  }

 private:
  SessionLink& owner_;
};

SessionLink::SessionLink(std::unique_ptr<LinkTransport> transport,
                         deskmate::platform::AoaIdentity identity,
                         std::uint64_t link_id,
                         std::uint32_t video_stream_id,
                         std::uint32_t config_id, std::uint16_t width,
                         std::uint16_t height, std::uint16_t hz,
                         bool allow_hevc, bool accept_device_resolution)
    : link_id_(link_id),
      identity_(std::move(identity)),
      width_(width),
      height_(height),
      hz_(hz),
      allow_hevc_(allow_hevc),
      accept_device_resolution_(accept_device_resolution),
      transport_(std::move(transport)),
      boundary_(std::make_unique<Boundary>(*this)) {
  start_new_session(video_stream_id, config_id);
}

SessionLink::~SessionLink() = default;

void SessionLink::start_new_session(std::uint32_t video_stream_id,
                                    std::uint32_t config_id) {
  auto next = std::make_unique<deskmate::protocol::HostSession>(
      *transport_, clock_,
      deskmate::platform::make_host_session_config(
          video_stream_id, config_id, width_, height_, hz_, allow_hevc_,
          accept_device_resolution_.load(std::memory_order_acquire)),
      boundary_.get());
  next->start();
  std::lock_guard lock(session_mutex_);
  capture_intent_.store(false, std::memory_order_release);
  input_intent_.store(false, std::memory_order_release);
  requested_idr_.store(0, std::memory_order_release);
  idr_wait_rejects_.store(0, std::memory_order_relaxed);
  session_ = std::move(next);
}

deskmate::protocol::SessionState SessionLink::state() const {
  std::lock_guard lock(session_mutex_);
  return session_ != nullptr ? session_->state()
                             : deskmate::protocol::SessionState::Stopped;
}

std::string SessionLink::negotiated_summary() const {
  std::lock_guard lock(session_mutex_);
  if (session_ == nullptr) {
    return "none";
  }
  const auto& stream = session_->negotiated_stream();
  if (!stream.has_value()) {
    return "none";
  }
  std::ostringstream out;
  out << (stream->profile.encoder == deskmate::protocol::VideoEncoder::Hevc
              ? "hevc"
              : "h264")
      << ' ' << stream->profile.resolution.width << 'x'
      << stream->profile.resolution.height << '@'
      << stream->profile.resolution.refresh_hz;
  return out.str();
}

std::optional<deskmate::protocol::StreamProfile>
SessionLink::negotiated_profile() const {
  std::lock_guard lock(session_mutex_);
  if (session_ == nullptr) {
    return std::nullopt;
  }
  const auto& stream = session_->negotiated_stream();
  if (!stream.has_value()) {
    return std::nullopt;
  }
  return stream->profile;
}

LinkStats SessionLink::stats() const {
  LinkStats stats;
  stats.frames_submitted = submitted_.load(std::memory_order_relaxed);
  stats.frames_sent = sent_.load(std::memory_order_relaxed);
  stats.frames_dropped = dropped_.load(std::memory_order_relaxed);
  stats.idr_wait_rejects = idr_wait_rejects_.load(std::memory_order_relaxed);
  stats.input_events = input_events_.load(std::memory_order_relaxed);
  if (transport_ != nullptr) {
    stats.rx_bytes = transport_->rx_bytes();
    stats.tx_bytes = transport_->tx_bytes();
    stats.bulk_errors = transport_->bulk_errors();
    stats.write_timeouts = transport_->write_timeouts();
  }
  return stats;
}

bool SessionLink::has_pending_output() const {
  std::lock_guard lock(session_mutex_);
  return session_ != nullptr && !session_->is_terminal() &&
         session_->has_pending_output();
}

bool SessionLink::is_terminal() const {
  std::lock_guard lock(session_mutex_);
  return session_ == nullptr || session_->is_terminal();
}

std::string SessionLink::failure_reason() const {
  std::lock_guard lock(session_mutex_);
  if (session_ == nullptr || !session_->failure().has_value()) {
    return {};
  }
  return std::string(
      deskmate::protocol::session_failure_name(*session_->failure()));
}

std::string SessionLink::transport_last_error() const {
  return transport_ != nullptr ? transport_->last_error() : std::string{};
}

std::uint64_t SessionLink::rx_bytes() const {
  return transport_ != nullptr ? transport_->rx_bytes() : 0;
}

std::uint64_t SessionLink::tx_bytes() const {
  return transport_ != nullptr ? transport_->tx_bytes() : 0;
}

bool SessionLink::transport_alive() const {
  return transport_ != nullptr && transport_->alive();
}

bool SessionLink::consume_write_blocked() {
  return transport_ != nullptr && transport_->consume_write_blocked();
}

void SessionLink::set_rx_callback(std::function<void()> callback) {
  if (transport_ != nullptr) {
    transport_->set_rx_callback(std::move(callback));
  }
}

void SessionLink::set_accept_device_resolution(bool enabled) {
  accept_device_resolution_.store(enabled, std::memory_order_release);
}

void SessionLink::restart_session() {
  {
    std::lock_guard lock(video_mutex_);
    video_queue_.clear();
  }
  if (transport_ == nullptr || !transport_->alive()) {
    return;
  }
  reconnects_.fetch_add(1, std::memory_order_relaxed);
  start_new_session(random_nonzero_u32(),
                    (random_nonzero_u32() & 0x7FFFFFFFU) | 1U);
}

deskmate::protocol::SessionCommandStatus SessionLink::handle_host_event(
    deskmate::protocol::HostEvent event) {
  std::lock_guard lock(session_mutex_);
  if (session_ == nullptr) {
    return deskmate::protocol::SessionCommandStatus::InvalidState;
  }
  return session_->handle_host_event(event);
}

void SessionLink::submit_video(std::vector<std::uint8_t> annex_b,
                               std::uint64_t pts_us, bool keyframe) {
  if (annex_b.empty()) {
    return;
  }
  submitted_.fetch_add(1, std::memory_order_relaxed);
  std::lock_guard lock(video_mutex_);
  if (video_queue_.size() >= kMaxPendingVideoFrames) {
    if (keyframe) {
      // A keyframe supersedes the whole backlog: nothing older can be decoded.
      dropped_.fetch_add(video_queue_.size(), std::memory_order_relaxed);
      video_queue_.clear();
    } else {
      // Keep the queued GOP intact. Removing an older P-frame while sending
      // its successors guarantees a discontinuity at the device decoder.
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return;
    }
  }
  video_queue_.push_back(
      PendingVideo{std::move(annex_b), pts_us, keyframe});
}

std::vector<deskmate::protocol::InputEvent> SessionLink::take_input() {
  std::lock_guard lock(session_mutex_);
  if (session_ == nullptr) {
    return {};
  }
  auto events = session_->take_input_events();
  input_events_.fetch_add(events.size(), std::memory_order_relaxed);
  return events;
}

std::uint32_t SessionLink::take_keyframe_request() {
  std::lock_guard lock(session_mutex_);
  if (session_ == nullptr) {
    requested_idr_.store(0, std::memory_order_release);
    return 0U;
  }
  const auto& request = session_->pending_idr_request();
  if (!request.has_value()) {
    // The core drops the request once the recovery keyframe starts sending;
    // re-arm so the next request is reported even if its id repeats.
    requested_idr_.store(0, std::memory_order_release);
    return 0U;
  }
  // Report each recovery id once so the capture side is not forced to
  // re-keyframe on every poll.
  std::uint32_t expected = 0;
  if (!requested_idr_.compare_exchange_strong(expected, request->recovery_id,
                                              std::memory_order_acq_rel)) {
    return 0U;
  }
  return request->recovery_id;
}

void SessionLink::pump() {
  if (transport_ == nullptr) {
    return;
  }
  const bool transport_ok = transport_->alive();
  // Drain queued frames into the scheduler in one batch (just like Bridge.cpp)
  // so high-refresh-rate capture never falls behind a single-frame-per-wake cap.
  std::deque<PendingVideo> batch;
  if (transport_ok) {
    std::lock_guard lock(video_mutex_);
    batch.swap(video_queue_);
  }
  std::lock_guard lock(session_mutex_);
  if (session_ == nullptr) {
    return;
  }
  for (const auto& frame : batch) {
    const auto status =
        session_->send_video_frame(frame.bytes, frame.pts_us, frame.keyframe);
    if (status == deskmate::protocol::VideoSendStatus::Sent) {
      sent_.fetch_add(1, std::memory_order_relaxed);
    } else if (status == deskmate::protocol::VideoSendStatus::IdrRequired) {
      idr_wait_rejects_.fetch_add(1, std::memory_order_relaxed);
    } else if (status != deskmate::protocol::VideoSendStatus::NotStreaming &&
               status != deskmate::protocol::VideoSendStatus::Paused) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
    }
  }
  session_->pump();
}

void SessionLink::stop() {
  {
    std::lock_guard lock(video_mutex_);
    video_queue_.clear();
  }
  std::lock_guard lock(session_mutex_);
  if (session_ != nullptr) {
    session_->stop();
  }
}

void SessionLink::close_transport() {
  if (transport_ != nullptr) {
    transport_->set_rx_callback({});
    transport_->cancel();
  }
  std::lock_guard lock(session_mutex_);
  session_.reset();
  transport_.reset();
}

LinkManager::LinkManager(LinkFactory& factory) : factory_(factory) {}

std::uint64_t LinkManager::add(std::unique_ptr<LinkTransport> transport,
                               deskmate::platform::AoaIdentity identity) {
  std::lock_guard lock(mutex_);
  const std::uint64_t id = next_id_++;
  auto link = factory_.attach(std::move(transport), std::move(identity), id);
  if (link == nullptr) {
    return 0;
  }
  links_.emplace(id, std::move(link));
  return id;
}

void LinkManager::pump_all() {
  std::vector<std::shared_ptr<LinkSession>> links;
  {
    std::lock_guard lock(mutex_);
    for (auto& [id, link] : links_) {
      (void)id;
      links.push_back(link);
    }
  }
  for (auto& link : links) {
    link->pump();
  }
}

void LinkManager::submit_video_to_all(std::vector<std::uint8_t> annex_b,
                                      std::uint64_t pts_us, bool keyframe) {
  std::vector<std::shared_ptr<LinkSession>> links;
  {
    std::lock_guard lock(mutex_);
    for (auto& [id, link] : links_) {
      (void)id;
      links.push_back(link);
    }
  }
  for (auto& link : links) {
    link->submit_video(annex_b, pts_us, keyframe);
  }
}

std::vector<std::pair<std::uint64_t, deskmate::protocol::InputEvent>>
LinkManager::drain_input() {
  std::vector<std::shared_ptr<LinkSession>> links;
  {
    std::lock_guard lock(mutex_);
    for (auto& [id, link] : links_) {
      (void)id;
      links.push_back(link);
    }
  }
  std::vector<std::pair<std::uint64_t, deskmate::protocol::InputEvent>> out;
  for (auto& link : links) {
    for (const auto& event : link->take_input()) {
      out.emplace_back(link->id(), event);
    }
  }
  return out;
}

void LinkManager::disconnect(std::uint64_t link_id) {
  std::shared_ptr<LinkSession> link;
  {
    std::lock_guard lock(mutex_);
    const auto found = links_.find(link_id);
    if (found == links_.end()) {
      return;
    }
    link = found->second;
    links_.erase(found);
  }
  link->stop();
}

void LinkManager::stop_all() {
  std::vector<std::shared_ptr<LinkSession>> links;
  {
    std::lock_guard lock(mutex_);
    for (auto& [id, link] : links_) {
      (void)id;
      links.push_back(link);
    }
    links_.clear();
  }
  for (auto& link : links) {
    link->stop();
  }
}

std::vector<std::uint64_t> LinkManager::live_ids() const {
  std::lock_guard lock(mutex_);
  std::vector<std::uint64_t> ids;
  ids.reserve(links_.size());
  for (const auto& [id, link] : links_) {
    if (link->state() != deskmate::protocol::SessionState::Failed &&
        link->state() != deskmate::protocol::SessionState::Stopped) {
      ids.push_back(id);
    }
  }
  return ids;
}

std::size_t LinkManager::size() const {
  std::lock_guard lock(mutex_);
  return links_.size();
}

std::vector<std::shared_ptr<LinkSession>> LinkManager::snapshot() const {
  std::lock_guard lock(mutex_);
  std::vector<std::shared_ptr<LinkSession>> links;
  links.reserve(links_.size());
  for (const auto& [id, link] : links_) {
    (void)id;
    links.push_back(link);
  }
  return links;
}

std::shared_ptr<LinkSession> LinkManager::find(std::uint64_t link_id) const {
  std::lock_guard lock(mutex_);
  const auto found = links_.find(link_id);
  return found == links_.end() ? nullptr : found->second;
}

}  // namespace deskmate::windows
