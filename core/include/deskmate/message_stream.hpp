#pragma once

#include "deskmate/protocol.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace deskmate::protocol {

struct StreamDecoderConfig {
  std::uint32_t expected_session_nonce{};
  std::uint32_t expected_video_stream_id{};
};

enum class StreamEventKind {
  Message,
  VideoFrame,
  RejectedMessage,
  StreamError,
};

struct StreamEvent {
  StreamEventKind kind{};
  Header header{};
  std::vector<std::uint8_t> payload;
  std::optional<std::uint32_t> recovery_id;
  ProtocolError error{};
};

class MessageStreamDecoder {
 public:
  explicit MessageStreamDecoder(StreamDecoderConfig config = {});

  [[nodiscard]] std::vector<StreamEvent> feed(std::span<const std::uint8_t> bytes);
  void reset(StreamDecoderConfig config = {});
  void retarget_video_stream(std::uint32_t video_stream_id);
  [[nodiscard]] bool failed() const { return failed_; }
  [[nodiscard]] const StreamDecoderConfig& config() const { return config_; }

 private:
  void drain(std::vector<StreamEvent>& events);
  bool resynchronize(std::vector<StreamEvent>& events);
  void compact_pending();
  void reject(const Header& header, ProtocolError error, std::vector<StreamEvent>& events);
  void fail(ProtocolError error, std::vector<StreamEvent>& events);
  void handle_payload(Header header, std::vector<std::uint8_t> payload,
                      std::vector<StreamEvent>& events);
  void handle_video_fragment(const Header& header, std::vector<std::uint8_t> payload,
                             std::vector<StreamEvent>& events);
  void start_video_frame(const Header& header);
  void emit_video_error(const Header& header, ErrorCode code, std::string_view detail,
                        bool retire_frame, std::vector<StreamEvent>& events);
  void complete_video_frame(std::vector<StreamEvent>& events);

  StreamDecoderConfig config_{};
  std::vector<std::uint8_t> pending_;
  std::size_t read_offset_{};
  bool failed_{};
  std::optional<Header> active_header_;
  std::uint32_t discard_remaining_{};
  bool resyncing_{};

  std::optional<Header> video_header_;
  std::array<std::optional<std::vector<std::uint8_t>>, kMaxVideoFragments> video_fragments_{};
  std::size_t video_fragments_received_{};
  std::size_t video_payload_bytes_{};
  std::optional<std::uint32_t> last_completed_video_seq_;
  std::optional<std::uint32_t> ignored_video_seq_;
};

}  // namespace deskmate::protocol
