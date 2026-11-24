#include "deskmate/scheduler.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

namespace deskmate::protocol {
namespace {

void trace_recovery(const char* trigger, std::uint64_t now_ns,
                    std::size_t pending_bytes, std::size_t incoming_bytes,
                    std::uint64_t age_ns, std::uint32_t peer_id) {
  static const bool enabled = std::getenv("DESKMATE_PERF_TRACE") != nullptr;
  if (!enabled) {
    return;
  }
  std::fprintf(stderr,
               "[DEBUG-dm-fps-recovery] mono_ns=%llu trigger=%s "
               "pending=%zu incoming=%zu age_ns=%llu peer_id=%u\n",
               static_cast<unsigned long long>(now_ns), trigger,
               pending_bytes, incoming_bytes,
               static_cast<unsigned long long>(age_ns),
               static_cast<unsigned>(peer_id));
}

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

bool mark_recovery_fragments(ScheduledVideoFrame& frame,
                             std::uint32_t recovery_id) {
  if (frame.fragments.empty()) {
    return false;
  }

  bool prefix_already_present = false;
  for (std::size_t index = 0; index < frame.fragments.size(); ++index) {
    auto& fragment = frame.fragments[index];
    const auto decoded = decode_header(fragment);
    if (!decoded) {
      return false;
    }
    Header header = decoded.value();
    if (header.type != static_cast<std::uint8_t>(MessageType::Video) ||
        header.fragment_index != index ||
        header.fragment_total != frame.fragments.size() ||
        fragment.size() != kWireHeaderSize + header.payload_len) {
      return false;
    }
    if (index == 0) {
      if ((header.flags & kFlagRecoveryIdr) != 0) {
        if (header.payload_len < 4) {
          return false;
        }
        std::uint32_t encoded_id = 0;
        for (std::size_t byte = 0; byte < 4; ++byte) {
          encoded_id |=
              static_cast<std::uint32_t>(fragment[kWireHeaderSize + byte])
              << (byte * 8U);
        }
        if (encoded_id != recovery_id) {
          return false;
        }
        prefix_already_present = true;
      }
      if (!prefix_already_present) {
        if (header.payload_len > kMaxPacketPayload - 4U) {
          return false;
        }
        header.payload_len += 4;
      }
    } else if ((header.flags & kFlagRecoveryIdr) == 0) {
      if (prefix_already_present) {
        return false;
      }
    } else if (!prefix_already_present) {
      return false;
    }
    header.flags |= kFlagRecoveryIdr;
    const auto encoded_header = encode_header(header);
    std::copy(encoded_header.begin(), encoded_header.end(), fragment.begin());
  }

  if (!prefix_already_present) {
    auto& first = frame.fragments.front();
    const std::array<std::uint8_t, 4> prefix{
        static_cast<std::uint8_t>(recovery_id & 0xFFU),
        static_cast<std::uint8_t>((recovery_id >> 8U) & 0xFFU),
        static_cast<std::uint8_t>((recovery_id >> 16U) & 0xFFU),
        static_cast<std::uint8_t>((recovery_id >> 24U) & 0xFFU)};
    first.insert(first.begin() + static_cast<std::ptrdiff_t>(kWireHeaderSize),
                 prefix.begin(), prefix.end());
  }
  return true;
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

  if (idr_request_.has_value() && frame.keyframe) {
    if (frame.recovery_id.has_value() &&
        *frame.recovery_id != idr_request_->recovery_id) {
      return SchedulerQueueStatus::InvalidArgument;
    }
    frame.recovery_id = idr_request_->recovery_id;
    if (!mark_recovery_fragments(frame, idr_request_->recovery_id)) {
      return SchedulerQueueStatus::InvalidArgument;
    }
  }

  const std::size_t incoming_bytes = fragment_bytes(frame.fragments, 0);
  const SchedulerQueueStatus admitted =
      admit_video_frame(frame.keyframe, incoming_bytes);
  if (admitted != SchedulerQueueStatus::Queued) {
    return admitted;
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
  if (keyframe && idr_request_.has_value() && frame.recovery_id.has_value() &&
      *frame.recovery_id != idr_request_->recovery_id) {
    return SchedulerQueueStatus::InvalidArgument;
  }
  const bool carries_recovery =
      keyframe && (idr_request_.has_value() || frame.recovery_id.has_value());
  const std::size_t recovery_prefix = carries_recovery ? 4U : 0U;
  const SchedulerQueueStatus admitted = admit_video_frame(
      keyframe, encoded_video_bytes(frame.payload.size() + recovery_prefix,
                                    fragment_payload));
  if (admitted != SchedulerQueueStatus::Queued) {
    return admitted;
  }

  if (idr_request_.has_value() && keyframe) {
    if (frame.recovery_id.has_value() &&
        *frame.recovery_id != idr_request_->recovery_id) {
      return SchedulerQueueStatus::InvalidArgument;
    }
    frame.flags |= kFlagRecoveryIdr;
    frame.recovery_id = idr_request_->recovery_id;
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
  scheduled.video_stream_id = frame.video_stream_id;
  queue_video_frame(std::move(scheduled));
  return SchedulerQueueStatus::Queued;
}

SchedulerQueueStatus HostSendScheduler::admit_video_frame(
    bool keyframe, std::size_t incoming_bytes) {
  if (idr_request_.has_value() && !keyframe) {
    return SchedulerQueueStatus::IdrRequired;
  }
  const auto pending_bytes = pending_video_bytes();
  if (pending_bytes + incoming_bytes > config_.max_pending_video_bytes) {
    if (!idr_request_.has_value()) {
      trace_recovery("queue_bytes", clock_.now_ns(), pending_bytes,
                     incoming_bytes, 0, 0);
    }
    drop_unstarted_video_frames();
    request_recovery(IdrRequestReason::Congestion);
    if (!keyframe) {
      return SchedulerQueueStatus::IdrRequired;
    }
  }
  return SchedulerQueueStatus::Queued;
}

void HostSendScheduler::queue_video_frame(ScheduledVideoFrame frame) {
  video_.push_back(VideoFrame{std::move(frame.fragments), 0, clock_.now_ns(),
                              frame.keyframe, frame.recovery_id,
                              frame.video_stream_id});
}

void HostSendScheduler::request_idr_from_peer(std::uint32_t recovery_id) {
  if (stopping_ || recovery_id == 0) {
    return;
  }
  if ((idr_request_.has_value() &&
       idr_request_->reason == IdrRequestReason::PeerRequest &&
       idr_request_->recovery_id == recovery_id) ||
      active_recovery_id_ == recovery_id ||
      std::any_of(video_.begin(), video_.end(), [recovery_id](const auto& frame) {
        return frame.next_fragment > 0 &&
               frame.next_fragment < frame.fragments.size() &&
               frame.recovery_id == recovery_id;
      })) {
    return;
  }
  const auto now = clock_.now_ns();
  trace_recovery("peer_request", now, 0, 0, 0, recovery_id);
  last_recovery_id_ = std::max(last_recovery_id_, recovery_id);
  idr_request_ = SchedulerIdrRequest{
      recovery_id, IdrRequestReason::PeerRequest, now};
  drop_unstarted_video_frames();
  mark_queued_recovery_keyframe();
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

void HostSendScheduler::discard_pending_video_frames() noexcept {
  video_.clear();
}

void HostSendScheduler::discard_unstarted_video_frames() noexcept {
  auto frame = video_.begin();
  while (frame != video_.end()) {
    if (frame->next_fragment == 0) {
      frame = video_.erase(frame);
    } else {
      ++frame;
    }
  }
}

void HostSendScheduler::clear_started_video_streams() noexcept {
  started_video_streams_.clear();
}

void HostSendScheduler::begin_stopping() noexcept {
  stopping_ = true;
  priority_.clear();
  video_.clear();
  active_.reset();
  active_recovery_id_.reset();
  active_offset_ = 0;
  idr_request_.reset();
  started_video_streams_.clear();
}

bool HostSendScheduler::has_started_video_stream(
    std::uint32_t video_stream_id) const noexcept {
  return std::find(started_video_streams_.begin(),
                   started_video_streams_.end(),
                   video_stream_id) != started_video_streams_.end();
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
  std::optional<std::size_t> keep_index;
  for (std::size_t index = video_.size(); index > 0; --index) {
    const VideoFrame& frame = video_[index - 1];
    if (frame.next_fragment == 0 && frame.keyframe) {
      keep_index = index - 1;
      break;
    }
  }

  auto frame = video_.begin();
  std::size_t index = 0;
  while (frame != video_.end()) {
    const bool keep = frame->next_fragment != 0 ||
                      (keep_index.has_value() && index == *keep_index);
    if (keep) {
      ++frame;
      ++index;
      continue;
    }
    frame = video_.erase(frame);
    if (keep_index.has_value() && index < *keep_index) {
      --*keep_index;
    }
  }
}

void HostSendScheduler::mark_queued_recovery_keyframe() {
  if (!idr_request_.has_value()) {
    return;
  }

  auto keyframe = video_.end();
  for (auto frame = video_.begin(); frame != video_.end(); ++frame) {
    if (frame->next_fragment == 0 && frame->keyframe) {
      keyframe = frame;
    }
  }
  if (keyframe == video_.end()) {
    return;
  }

  const std::uint32_t recovery_id = idr_request_->recovery_id;
  if ((keyframe->recovery_id.has_value() &&
       *keyframe->recovery_id != recovery_id)) {
    video_.erase(keyframe);
    return;
  }

  ScheduledVideoFrame marked{
      .fragments = keyframe->fragments,
      .keyframe = true,
      .recovery_id = recovery_id,
  };
  if (!mark_recovery_fragments(marked, recovery_id)) {
    video_.erase(keyframe);
    return;
  }
  keyframe->fragments = std::move(marked.fragments);
  keyframe->recovery_id = marked.recovery_id;
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
  if (!idr_request_.has_value()) {
    trace_recovery("queue_age", now, 0, 0, now - oldest->enqueue_ns, 0);
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
  drop_unstarted_video_frames();
  mark_queued_recovery_keyframe();
}

bool HostSendScheduler::has_pending_output() const noexcept {
  return !priority_.empty() || !video_.empty() || active_.has_value();
}

std::optional<std::vector<std::uint8_t>>
HostSendScheduler::next_video_fragment() {
  while (!video_.empty()) {
    VideoFrame& frame = video_.front();
    if (frame.next_fragment < frame.fragments.size()) {
      if (frame.next_fragment == 0) {
        if (std::find(started_video_streams_.begin(),
                      started_video_streams_.end(),
                      frame.video_stream_id) ==
            started_video_streams_.end()) {
          started_video_streams_.push_back(frame.video_stream_id);
        }
      }
      if (frame.next_fragment == 0 && frame.keyframe &&
          frame.recovery_id.has_value() && idr_request_.has_value() &&
          idr_request_->recovery_id == *frame.recovery_id) {
        idr_request_.reset();
      }
      active_recovery_id_ = frame.recovery_id;
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
      active_recovery_id_.reset();
      active_offset_ = 0;
      return;
    }
  }
}

}  // namespace deskmate::protocol
