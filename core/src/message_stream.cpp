#include "deskmate/message_stream.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace deskmate::protocol {
namespace {

constexpr std::size_t kFeedChunkSize = 16 * 1024;

std::uint32_t load_u32_le(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
  std::uint32_t value = 0;
  for (std::size_t index = 0; index < 4; ++index) {
    value |= static_cast<std::uint32_t>(bytes[offset + index]) << (index * 8U);
  }
  return value;
}

bool sequence_is_newer(std::uint32_t candidate, std::uint32_t reference) {
  const std::uint32_t delta = candidate - reference;
  return delta != 0 && delta < 0x80000000U;
}

bool same_frame_identity(const Header& left, const Header& right) {
  return left.session_nonce == right.session_nonce &&
         left.video_stream_id == right.video_stream_id && left.seq == right.seq &&
         left.flags == right.flags && left.fragment_total == right.fragment_total &&
         left.timestamp_us == right.timestamp_us;
}

StreamEvent make_event(StreamEventKind kind, Header header) {
  StreamEvent event;
  event.kind = kind;
  event.header = header;
  return event;
}

}  // namespace

MessageStreamDecoder::MessageStreamDecoder(StreamDecoderConfig config) : config_(config) {}

std::vector<StreamEvent> MessageStreamDecoder::feed(std::span<const std::uint8_t> bytes) {
  std::vector<StreamEvent> events;
  if (failed_ || bytes.empty()) {
    return events;
  }

  std::size_t offset = 0;
  while (offset < bytes.size() && !failed_) {
    const std::size_t amount = std::min(kFeedChunkSize, bytes.size() - offset);
    pending_.insert(pending_.end(), bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                    bytes.begin() + static_cast<std::ptrdiff_t>(offset + amount));
    offset += amount;
    drain(events);
    compact_pending();
  }
  return events;
}

void MessageStreamDecoder::reset(StreamDecoderConfig config) {
  config_ = config;
  pending_.clear();
  read_offset_ = 0;
  failed_ = false;
  active_header_.reset();
  discard_remaining_ = 0;
  resyncing_ = false;
  video_header_.reset();
  for (auto& fragment : video_fragments_) {
    fragment.reset();
  }
  video_fragments_received_ = 0;
  video_payload_bytes_ = 0;
  last_completed_video_seq_.reset();
  ignored_video_seq_.reset();
}

void MessageStreamDecoder::retarget_video_stream(
    std::uint32_t video_stream_id) {
  config_.expected_video_stream_id = video_stream_id;
  if (active_header_.has_value()) {
    // The remaining bytes of the abandoned candidate message are still in the
    // buffer. Drop exactly that payload so the next message starts on a header
    // boundary.
    discard_remaining_ = active_header_->payload_len;
  } else if (discard_remaining_ == 0 &&
             pending_.size() - read_offset_ > 0) {
    // Only a partial header (1..31 bytes) of the abandoned stream arrived. Its
    // payload length is unknown, so resynchronize on the next structurally
    // valid header instead of splicing the leftover bytes onto the next
    // stream's header.
    resyncing_ = true;
  }
  active_header_.reset();
  video_header_.reset();
  for (auto& fragment : video_fragments_) {
    fragment.reset();
  }
  video_fragments_received_ = 0;
  video_payload_bytes_ = 0;
  last_completed_video_seq_.reset();
  ignored_video_seq_.reset();
}

void MessageStreamDecoder::drain(std::vector<StreamEvent>& events) {
  while (!failed_) {
    const std::size_t available = pending_.size() - read_offset_;

    if (resyncing_) {
      if (!resynchronize(events)) {
        return;
      }
      continue;
    }

    if (discard_remaining_ != 0) {
      if (available == 0) {
        return;
      }
      const std::size_t amount =
          std::min(available, static_cast<std::size_t>(discard_remaining_));
      read_offset_ += amount;
      discard_remaining_ -= static_cast<std::uint32_t>(amount);
      continue;
    }

    if (active_header_.has_value()) {
      const std::size_t payload_len = active_header_->payload_len;
      if (available < payload_len) {
        return;
      }
      std::vector<std::uint8_t> payload(
          pending_.begin() + static_cast<std::ptrdiff_t>(read_offset_),
          pending_.begin() + static_cast<std::ptrdiff_t>(read_offset_ + payload_len));
      read_offset_ += payload_len;
      Header header = *active_header_;
      active_header_.reset();
      handle_payload(std::move(header), std::move(payload), events);
      continue;
    }

    if (available < kWireHeaderSize) {
      return;
    }

    const std::span<const std::uint8_t> header_bytes(
        pending_.data() + read_offset_, kWireHeaderSize);
    auto decoded = decode_header(header_bytes);
    if (!decoded) {
      fail(decoded.error(), events);
      return;
    }
    read_offset_ += kWireHeaderSize;
    const Header header = decoded.value();

    if (!is_supported_message_type(header.type)) {
      reject(header,
             ProtocolError{ErrorCode::UnsupportedMessageType, ErrorAction::DiscardMessage,
                           "message type is not supported by protocol revision 1"},
             events);
      discard_remaining_ = header.payload_len;
      continue;
    }
    if (config_.expected_session_nonce != 0 &&
        header.session_nonce != config_.expected_session_nonce) {
      reject(header,
             ProtocolError{ErrorCode::SessionMismatch, ErrorAction::DiscardMessage,
                           "packet belongs to a different session"},
             events);
      discard_remaining_ = header.payload_len;
      continue;
    }
    const bool carries_stream =
        header.type == static_cast<std::uint8_t>(MessageType::Video) ||
        header.type == static_cast<std::uint8_t>(MessageType::Input);
    if (carries_stream && config_.expected_video_stream_id != 0 &&
        header.video_stream_id != config_.expected_video_stream_id) {
      reject(header,
             ProtocolError{ErrorCode::VideoStreamMismatch, ErrorAction::DiscardMessage,
                           "packet belongs to a different video stream generation"},
             events);
      discard_remaining_ = header.payload_len;
      continue;
    }

    active_header_ = header;
  }
}

bool MessageStreamDecoder::resynchronize(std::vector<StreamEvent>&) {
  const std::size_t available = pending_.size() - read_offset_;
  for (std::size_t offset = 0; offset + kWireHeaderSize <= available;
       ++offset) {
    const std::span<const std::uint8_t> candidate(
        pending_.data() + read_offset_ + offset, kWireHeaderSize);
    const auto decoded = decode_header(candidate);
    if (!decoded) {
      continue;
    }
    const Header& header = decoded.value();
    if (!is_supported_message_type(header.type)) {
      continue;
    }
    if (config_.expected_session_nonce != 0 &&
        header.session_nonce != config_.expected_session_nonce) {
      continue;
    }
    const bool carries_stream =
        header.type == static_cast<std::uint8_t>(MessageType::Video) ||
        header.type == static_cast<std::uint8_t>(MessageType::Input);
    if (carries_stream && config_.expected_video_stream_id != 0 &&
        header.video_stream_id != config_.expected_video_stream_id) {
      continue;
    }
    read_offset_ += offset;
    resyncing_ = false;
    return true;
  }
  // No header start found yet. Keep a tail that could be a partial header and
  // discard the rest.
  const std::size_t keep = std::min(available, kWireHeaderSize - 1);
  read_offset_ = pending_.size() - keep;
  return false;
}

void MessageStreamDecoder::compact_pending() {
  if (read_offset_ == 0) {
    return;
  }
  pending_.erase(pending_.begin(),
                 pending_.begin() + static_cast<std::ptrdiff_t>(read_offset_));
  read_offset_ = 0;
}

void MessageStreamDecoder::reject(const Header& header, ProtocolError error,
                                  std::vector<StreamEvent>& events) {
  StreamEvent event = make_event(StreamEventKind::RejectedMessage, header);
  event.error = error;
  events.push_back(std::move(event));
}

void MessageStreamDecoder::fail(ProtocolError error, std::vector<StreamEvent>& events) {
  failed_ = true;
  StreamEvent event;
  event.kind = StreamEventKind::StreamError;
  event.error = error;
  events.push_back(std::move(event));

  pending_.clear();
  read_offset_ = 0;
  active_header_.reset();
  discard_remaining_ = 0;
  video_header_.reset();
  for (auto& fragment : video_fragments_) {
    fragment.reset();
  }
  video_fragments_received_ = 0;
  video_payload_bytes_ = 0;
  last_completed_video_seq_.reset();
  ignored_video_seq_.reset();
}

void MessageStreamDecoder::handle_payload(Header header, std::vector<std::uint8_t> payload,
                                          std::vector<StreamEvent>& events) {
  if (header.type == static_cast<std::uint8_t>(MessageType::Video)) {
    handle_video_fragment(header, std::move(payload), events);
    return;
  }

  StreamEvent event = make_event(StreamEventKind::Message, header);
  event.payload = std::move(payload);
  events.push_back(std::move(event));
}

void MessageStreamDecoder::handle_video_fragment(const Header& header,
                                                 std::vector<std::uint8_t> payload,
                                                 std::vector<StreamEvent>& events) {
  if (ignored_video_seq_.has_value()) {
    if (*ignored_video_seq_ == header.seq) {
      return;
    }
    ignored_video_seq_.reset();
  }

  if (!video_header_.has_value()) {
    if (last_completed_video_seq_.has_value()) {
      if (header.seq == *last_completed_video_seq_ ||
          sequence_is_newer(*last_completed_video_seq_, header.seq)) {
        ignored_video_seq_ = header.seq;
        emit_video_error(header, ErrorCode::OutOfOrderFrame,
                         "video frame arrived after a newer frame", false, events);
        return;
      }
      if (header.seq != *last_completed_video_seq_ + 1U) {
        emit_video_error(header, ErrorCode::SequenceDiscontinuity,
                         "video frame sequence has a gap", false, events);
      }
    }
    start_video_frame(header);
  } else if (header.seq != video_header_->seq) {
    if (sequence_is_newer(header.seq, video_header_->seq)) {
      emit_video_error(*video_header_, ErrorCode::InterleavedFrame,
                       "a new video frame started before the previous frame completed", true,
                       events);
      start_video_frame(header);
    } else {
      ignored_video_seq_ = header.seq;
      emit_video_error(header, ErrorCode::OutOfOrderFrame,
                       "video frame arrived after a newer frame", false, events);
      return;
    }
  }

  if (!same_frame_identity(header, *video_header_)) {
    emit_video_error(header, ErrorCode::FragmentMetadataMismatch,
                     "video fragment metadata does not match its frame", true, events);
    return;
  }

  const std::size_t fragment_index = header.fragment_index;
  if (video_fragments_[fragment_index].has_value()) {
    emit_video_error(header, ErrorCode::DuplicateFragment,
                     "video frame contains a duplicate fragment", true, events);
    return;
  }

  video_payload_bytes_ += payload.size();
  if (video_payload_bytes_ > kMaxVideoFramePayload) {
    emit_video_error(header, ErrorCode::PayloadTooLarge,
                     "reassembled video frame exceeds 4 MiB", true, events);
    return;
  }
  video_fragments_[fragment_index] = std::move(payload);
  ++video_fragments_received_;

  if (video_fragments_received_ == video_header_->fragment_total) {
    complete_video_frame(events);
  }
}

void MessageStreamDecoder::start_video_frame(const Header& header) {
  video_header_ = header;
  for (auto& fragment : video_fragments_) {
    fragment.reset();
  }
  video_fragments_received_ = 0;
  video_payload_bytes_ = 0;
}

void MessageStreamDecoder::emit_video_error(const Header& header, ErrorCode code,
                                            std::string_view detail, bool retire_frame,
                                            std::vector<StreamEvent>& events) {
  StreamEvent event = make_event(StreamEventKind::RejectedMessage, header);
  event.error = ProtocolError{code, ErrorAction::RequestRecovery, detail};
  events.push_back(std::move(event));

  if (retire_frame && video_header_.has_value()) {
    ignored_video_seq_ = video_header_->seq;
    video_header_.reset();
    for (auto& fragment : video_fragments_) {
      fragment.reset();
    }
    video_fragments_received_ = 0;
    video_payload_bytes_ = 0;
  }
}

void MessageStreamDecoder::complete_video_frame(std::vector<StreamEvent>& events) {
  const Header frame_header = *video_header_;
  std::vector<std::uint8_t> payload;
  payload.reserve(video_payload_bytes_);
  for (std::size_t index = 0; index < frame_header.fragment_total; ++index) {
    if (!video_fragments_[index].has_value()) {
      emit_video_error(frame_header, ErrorCode::FragmentMetadataMismatch,
                       "video frame is missing a required fragment", true, events);
      return;
    }
    payload.insert(payload.end(), video_fragments_[index]->begin(),
                   video_fragments_[index]->end());
  }

  if ((frame_header.flags & kFlagRecoveryIdr) != 0) {
    if (payload.size() < 4) {
      emit_video_error(frame_header, ErrorCode::RecoveryIdMissing,
                       "recovery IDR is missing its four-byte recovery_id prefix", true,
                       events);
      return;
    }
    const std::uint32_t recovery_id = load_u32_le(payload, 0);
    payload.erase(payload.begin(), payload.begin() + 4);

    video_header_.reset();
    for (auto& fragment : video_fragments_) {
      fragment.reset();
    }
    video_fragments_received_ = 0;
    video_payload_bytes_ = 0;
    last_completed_video_seq_ = frame_header.seq;

    StreamEvent event = make_event(StreamEventKind::VideoFrame, frame_header);
    event.header.fragment_index = 0;
    event.header.payload_len = static_cast<std::uint32_t>(payload.size());
    event.payload = std::move(payload);
    event.recovery_id = recovery_id;
    events.push_back(std::move(event));
    return;
  }

  video_header_.reset();
  for (auto& fragment : video_fragments_) {
    fragment.reset();
  }
  video_fragments_received_ = 0;
  video_payload_bytes_ = 0;
  last_completed_video_seq_ = frame_header.seq;

  StreamEvent event = make_event(StreamEventKind::VideoFrame, frame_header);
  event.header.fragment_index = 0;
  event.header.payload_len = static_cast<std::uint32_t>(payload.size());
  event.payload = std::move(payload);
  events.push_back(std::move(event));
}

}  // namespace deskmate::protocol
