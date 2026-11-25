#include "deskmate/platform/input_frames.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace deskmate::platform {
namespace {

float clamp_unit(float value) {
  if (!std::isfinite(value)) {
    return 0.0F;
  }
  return std::min(1.0F, std::max(0.0F, value));
}

}  // namespace

std::uint16_t input_flags_with_contact_count(std::uint16_t base_flags,
                                             std::size_t active_contacts) {
  const auto count = static_cast<std::uint16_t>(
      std::min<std::size_t>(15, active_contacts)
      << deskmate::protocol::kInputFlagContactCountShift);
  return static_cast<std::uint16_t>(
      base_flags | deskmate::protocol::kInputFlagMultiTouchFrame |
      (count & deskmate::protocol::kInputFlagContactCountMask));
}

TouchTracker::TouchTracker(std::uint8_t max_contacts)
    : max_contacts_(std::clamp<std::uint8_t>(max_contacts, 1, 10)) {}

void TouchTracker::set_viewport(std::uint32_t width, std::uint32_t height) {
  width_ = width;
  height_ = height;
}

std::uint8_t TouchTracker::allocate_slot_locked(std::int32_t pointer_id) {
  if (const auto existing = slots_.find(pointer_id); existing != slots_.end()) {
    return existing->second;
  }
  bool used[10] = {};
  for (const auto& [id, slot] : slots_) {
    (void)id;
    if (slot < max_contacts_) {
      used[slot] = true;
    }
  }
  for (std::uint8_t slot = 0; slot < max_contacts_; ++slot) {
    if (!used[slot]) {
      slots_[pointer_id] = slot;
      return slot;
    }
  }
  return 0xFFU;
}

void TouchTracker::append_committed_flags(
    std::vector<deskmate::protocol::InputEvent>& events,
    std::size_t active_count) const {
  if (events.empty()) {
    return;
  }
  for (std::size_t index = 0; index < events.size(); ++index) {
    std::uint16_t flags = input_flags_with_contact_count(0, active_count);
    const bool is_up =
        events[index].kind == deskmate::protocol::InputKind::TouchUp ||
        events[index].kind == deskmate::protocol::InputKind::TouchCancel;
    if (!is_up && static_cast<std::int32_t>(events[index].pointer_id) ==
                      primary_slot_) {
      flags = static_cast<std::uint16_t>(
          flags | deskmate::protocol::kInputFlagPrimaryButton);
    }
    if (index + 1 == events.size()) {
      flags = static_cast<std::uint16_t>(
          flags | deskmate::protocol::kInputFlagFrameCommit);
    }
    events[index].flags = flags;
  }
}

std::vector<deskmate::protocol::InputEvent> TouchTracker::update(
    std::span<const TouchSample> samples, bool cancel) {
  std::vector<deskmate::protocol::InputEvent> events;
  if (width_ == 0 || height_ == 0) {
    return events;
  }
  if (cancel) {
    return release_all(true);
  }

  std::unordered_set<std::int32_t> present;
  for (const auto& sample : samples) {
    present.insert(sample.pointer_id);
  }

  // Release contacts that disappeared from the snapshot before processing
  // newly pressed pointers, matching the HarmonyOS XComponent implementation.
  for (auto it = slots_.begin(); it != slots_.end();) {
    if (present.count(it->first) != 0) {
      ++it;
      continue;
    }
    const std::uint8_t slot = it->second;
    Position position{0.5F, 0.5F};
    if (const auto found = positions_.find(slot); found != positions_.end()) {
      position = found->second;
    }
    deskmate::protocol::InputEvent event;
    event.kind = deskmate::protocol::InputKind::TouchUp;
    event.pointer_id = slot;
    event.x = position.x;
    event.y = position.y;
    events.push_back(event);
    positions_.erase(slot);
    it = slots_.erase(it);
  }

  for (const auto& sample : samples) {
    if (!sample.pressed) {
      continue;
    }
    const std::uint8_t slot = allocate_slot_locked(sample.pointer_id);
    if (slot == 0xFFU) {
      continue;
    }
    const float x = clamp_unit(sample.x / static_cast<float>(width_));
    const float y = clamp_unit(sample.y / static_cast<float>(height_));
    const auto previous = positions_.find(slot);
    deskmate::protocol::InputEvent event;
    event.pointer_id = slot;
    if (previous == positions_.end()) {
      positions_[slot] = Position{x, y};
      if (primary_slot_ < 0) {
        primary_slot_ = slot;
      }
      event.kind = deskmate::protocol::InputKind::TouchDown;
      event.x = x;
      event.y = y;
    } else {
      const float dx = x - previous->second.x;
      const float dy = y - previous->second.y;
      previous->second = Position{x, y};
      if (dx == 0.0F && dy == 0.0F) {
        continue;
      }
      event.kind = deskmate::protocol::InputKind::TouchMove;
      event.x = x;
      event.y = y;
      event.dx = dx;
      event.dy = dy;
    }
    events.push_back(event);
  }

  if (primary_slot_ >= 0 &&
      positions_.find(static_cast<std::uint8_t>(primary_slot_)) ==
          positions_.end()) {
    primary_slot_ = -1;
    for (const auto& [slot, position] : positions_) {
      (void)position;
      if (primary_slot_ < 0 || slot < primary_slot_) {
        primary_slot_ = slot;
      }
    }
  }

  append_committed_flags(events, positions_.size());
  return events;
}

std::vector<deskmate::protocol::InputEvent> TouchTracker::release_all(
    bool cancel) {
  std::vector<deskmate::protocol::InputEvent> events;
  for (const auto& [pointer_id, slot] : slots_) {
    (void)pointer_id;
    deskmate::protocol::InputEvent event;
    event.kind = cancel ? deskmate::protocol::InputKind::TouchCancel
                        : deskmate::protocol::InputKind::TouchUp;
    event.pointer_id = slot;
    if (const auto found = positions_.find(slot); found != positions_.end()) {
      event.x = found->second.x;
      event.y = found->second.y;
    } else {
      event.x = 0.5F;
      event.y = 0.5F;
    }
    events.push_back(event);
  }
  slots_.clear();
  positions_.clear();
  primary_slot_ = -1;
  append_committed_flags(events, 0);
  return events;
}

void TouchTracker::reset() {
  slots_.clear();
  positions_.clear();
  primary_slot_ = -1;
}

}  // namespace deskmate::platform
