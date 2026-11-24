#include "deskmate/scheduler.hpp"

#include <algorithm>
#include <utility>

namespace deskmate::protocol {
namespace {

std::size_t fragment_bytes(
    const std::vector<std::vector<std::uint8_t>>& fragments,
    std::size_t start) {
  std::size_t bytes = 0;
  for (std::size_t index = start; index < fragments.size(); ++index) {
    bytes += fragments[index].size();
  }
  return bytes;
}

std::size_t encoded_video_bytes(std::size_t payload_bytes,
                                std::size_t fragment_payload) {
  const std::size_t fragment_count =
      (payload_bytes + fragment_payload - 1) / fragment_payload;
  return payload_bytes + fragment_count * kWireHeaderSize;
}

}  // namespace

HostSendScheduler::HostSendScheduler(ByteIo& io, MonotonicClock& clock,
                                     SendSchedulerConfig config)
    : io_(io), clock_(clock), config_(config) {}

void HostSendScheduler::enqueue_priority(std::vector<std::uint8_t> packet) {
  if (stopping_ || packet.empty()) {
    return;
  }
  priority_.push_back(std::move(packet));
}

SchedulerQueueStatus HostSendScheduler::enqueue_video_frame(
    std::vector<std::vector<std::uint8_t>> fragments) {
  ScheduledVideoFrame frame;
  frame.fragments = std::move(fragments);
  return enqueue_video_frame(std::move(frame));
}

SchedulerQueueStatus HostSendScheduler::enqueue_video_frame(
    ScheduledVideoFrame frame) {
  if (stopping_) {
    return SchedulerQueueStatus::Stopped;
  }
  if (frame.fragments.empty()) {
    return SchedulerQueueStatus::InvalidArgument;
  }

  const std::size_t incoming_bytes = fragment_bytes(frame.fragments, 0);
  const SchedulerQueueStatus admitted =
      admit_video_frame(frame.keyframe, incoming_bytes);
  if (admitted != SchedulerQueueStatus::Queued) {
    return admitted;
  }

  if (idr_request_.has_value() && frame.keyframe) {
    if (!frame.recovery_id.has_value()) {
      frame.recovery_id = idr_request_->recovery_id;
    }
    idr_request_.reset();
  }
  queue_video_frame(std::move(frame));
  return SchedulerQueueStatus::Queued;
}

SchedulerQueueStatus HostSendScheduler::enqueue_video_frame(
    VideoFrameToSend frame, std::size_t fragment_payload) {
  if (stopping_) {
    return SchedulerQueueStatus::Stopped;
  }
  if (frame.payload.empty() || fragment_payload == 0) {
    return SchedulerQueueStatus::InvalidArgument;
  }
  const bool keyframe = (frame.flags & kFlagKeyFrame) != 0;
  const bool will_recover = keyframe && idr_request_.has_value();
  const std::size_t recovery_prefix = will_recover ? 4U : 0U;
  const SchedulerQueueStatus admitted = admit_video_frame(
      keyframe, encoded_video_bytes(frame.payload.size() + recovery_prefix,
                                    fragment_payload));
  if (admitted != SchedulerQueueStatus::Queued) {
    return admitted;
  }

  std::optional<std::uint32_t> expected_recovery_id;
  if (idr_request_.has_value() && keyframe) {
    expected_recovery_id =
        frame.recovery_id.value_or(idr_request_->recovery_id);
    frame.flags |= kFlagRecoveryIdr;
    frame.recovery_id = expected_recovery_id;
  }
  auto fragments =
      encode_video_fragments(frame, fragment_payload);
  if (!fragments) {
    return SchedulerQueueStatus::InvalidArgument;
  }

  ScheduledVideoFrame scheduled;
  scheduled.fragments = std::move(fragments.value());
  scheduled.keyframe = keyframe;
  scheduled.recovery_id = frame.recovery_id;
  queue_video_frame(std::move(scheduled));
  if (expected_recovery_id.has_value() && idr_request_.has_value() &&
      idr_request_->recovery_id == *expected_recovery_id) {
    idr_request_.reset();
  }
  return SchedulerQueueStatus::Queued;
}

SchedulerQueueStatus HostSendScheduler::admit_video_frame(
    bool keyframe, std::size_t incoming_bytes) {
  if (idr_request_.has_value() && !keyframe) {
    return SchedulerQueueStatus::IdrRequired;
  }
  if (pending_video_bytes() + incoming_bytes >
      config_.max_pending_video_bytes) {
    drop_unstarted_video_frames();
    request_recovery(IdrRequestReason::Congestion);
    if (!keyframe) {
      return SchedulerQueueStatus::IdrRequired;
    }
  }
  return SchedulerQueueStatus::Queued;
}

void HostSendScheduler::queue_video_frame(ScheduledVideoFrame frame) {
  video_.push_back(
      VideoFrame{std::move(frame.fragments), 0, clock_.now_ns()});
}

void HostSendScheduler::request_idr_from_peer(std::uint32_t recovery_id) {
  if (stopping_ || recovery_id == 0) {
    return;
  }
  last_recovery_id_ = std::max(last_recovery_id_, recovery_id);
  idr_request_ = SchedulerIdrRequest{
      recovery_id, IdrRequestReason::PeerRequest, clock_.now_ns()};
}

void HostSendScheduler::pump() {
  pump_priority();
  if (stopping_ || active_.has_value()) {
    return;
  }

  check_oldest_unsent_frame_age();
  std::size_t submitted = 0;
  while (submitted < config_.max_video_urbs) {
    auto fragment = next_video_fragment();
    if (!fragment.has_value()) {
      return;
    }
    active_ = std::move(*fragment);
    active_offset_ = 0;
    ++submitted;
    write_active();
    if (active_.has_value()) {
      return;
    }
  }
}

void HostSendScheduler::pump_priority() {
  if (stopping_) {
    priority_.clear();
    video_.clear();
    active_.reset();
    active_offset_ = 0;
    return;
  }

  if (active_.has_value()) {
    write_active();
    if (active_.has_value()) {
      return;
    }
  }

  while (!priority_.empty()) {
    active_ = std::move(priority_.front());
    priority_.pop_front();
    active_offset_ = 0;
    write_active();
    if (active_.has_value()) {
      return;
    }
  }
}

void HostSendScheduler::begin_stopping() noexcept {
  stopping_ = true;
  priority_.clear();
  video_.clear();
  active_.reset();
  active_offset_ = 0;
  idr_request_.reset();
}

bool HostSendScheduler::idle() const noexcept {
  return priority_.empty() && video_.empty() && !active_.has_value();
}

std::size_t HostSendScheduler::pending_video_bytes() const noexcept {
  std::size_t bytes = 0;
  for (const VideoFrame& frame : video_) {
    bytes += fragment_bytes(frame.fragments, frame.next_fragment);
  }
  if (active_.has_value()) {
    bytes += active_->size() - active_offset_;
  }
  return bytes;
}

void HostSendScheduler::drop_unstarted_video_frames() {
  auto frame = video_.begin();
  while (frame != video_.end()) {
    if (frame->next_fragment != 0) {
      ++frame;
      continue;
    }
    frame = video_.erase(frame);
  }
}

void HostSendScheduler::check_oldest_unsent_frame_age() {
  const std::uint64_t now = clock_.now_ns();
  const auto oldest = std::find_if(
      video_.begin(), video_.end(),
      [](const VideoFrame& frame) { return frame.next_fragment == 0; });
  if (oldest == video_.end()) {
    return;
  }
  if (now < oldest->enqueue_ns ||
      now - oldest->enqueue_ns < config_.max_oldest_unsent_frame_age_ns) {
    return;
  }
  drop_unstarted_video_frames();
  request_recovery(IdrRequestReason::Congestion);
}

void HostSendScheduler::request_recovery(IdrRequestReason reason) {
  if (idr_request_.has_value()) {
    return;
  }
  ++last_recovery_id_;
  if (last_recovery_id_ == 0) {
    ++last_recovery_id_;
  }
  idr_request_ =
      SchedulerIdrRequest{last_recovery_id_, reason, clock_.now_ns()};
}

bool HostSendScheduler::has_pending_output() const noexcept {
  return !priority_.empty() || !video_.empty() || active_.has_value();
}

std::optional<std::vector<std::uint8_t>>
HostSendScheduler::next_video_fragment() {
  while (!video_.empty()) {
    VideoFrame& frame = video_.front();
    if (frame.next_fragment < frame.fragments.size()) {
      return std::move(frame.fragments[frame.next_fragment++]);
    }
    video_.pop_front();
  }
  return std::nullopt;
}

void HostSendScheduler::write_active() {
  while (active_.has_value()) {
    const std::span<const std::uint8_t> remaining(
        active_->data() + active_offset_, active_->size() - active_offset_);
    const IoResult result = io_.write(remaining);
    if (result.status == IoStatus::WouldBlock) {
      return;
    }
    if (result.status != IoStatus::Ok || result.bytes == 0 ||
        result.bytes > remaining.size()) {
      begin_stopping();
      return;
    }
    active_offset_ += result.bytes;
    if (active_offset_ == active_->size()) {
      active_.reset();
      active_offset_ = 0;
      return;
    }
  }
}

}  // namespace deskmate::protocol
