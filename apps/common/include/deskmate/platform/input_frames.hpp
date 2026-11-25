#pragma once

#include "deskmate/protocol.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace deskmate::platform {

struct TouchSample {
  std::int32_t pointer_id{};
  float x{};
  float y{};
  bool pressed{};
};

// Converts a complete MotionEvent/XComponent pointer snapshot into the 28-byte
// Basic Input frames used by the existing macOS host. Pointer IDs are remapped
// to stable 0..9 slots, and every emitted packet carries the multi-touch frame,
// active contact count, primary button and frame-commit flags.
class TouchTracker {
 public:
  explicit TouchTracker(std::uint8_t max_contacts = 10);

  void set_viewport(std::uint32_t width, std::uint32_t height);
  [[nodiscard]] std::vector<deskmate::protocol::InputEvent> update(
      std::span<const TouchSample> samples, bool cancel = false);
  [[nodiscard]] std::vector<deskmate::protocol::InputEvent> release_all(
      bool cancel = true);
  void reset();

  [[nodiscard]] std::uint32_t viewport_width() const { return width_; }
  [[nodiscard]] std::uint32_t viewport_height() const { return height_; }
  [[nodiscard]] std::size_t active_contacts() const { return positions_.size(); }

 private:
  struct Position {
    float x{};
    float y{};
  };

  std::uint8_t allocate_slot_locked(std::int32_t pointer_id);
  void append_committed_flags(std::vector<deskmate::protocol::InputEvent>& events,
                              std::size_t active_count) const;

  std::uint8_t max_contacts_;
  std::uint32_t width_{};
  std::uint32_t height_{};
  std::unordered_map<std::int32_t, std::uint8_t> slots_;
  std::unordered_map<std::uint8_t, Position> positions_;
  std::int32_t primary_slot_{-1};
};

[[nodiscard]] std::uint16_t input_flags_with_contact_count(
    std::uint16_t base_flags, std::size_t active_contacts);

}  // namespace deskmate::platform
