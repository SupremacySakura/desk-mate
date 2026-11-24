#include "gate1/transfer_set.hpp"

#include <algorithm>

namespace gate1 {

std::uint64_t TransferSet::add(Direction direction)
{
    const std::uint64_t id = next_id_++;
    entries_.push_back(Entry{id, direction, false});
    return id;
}

bool TransferSet::mark_cancel_requested(Direction direction)
{
    bool found = false;
    for (Entry& entry : entries_) {
        if (entry.direction == direction) {
            entry.cancel_requested = true;
            found = true;
        }
    }
    return found;
}

void TransferSet::complete(std::uint64_t id)
{
    const auto it = std::find_if(
        entries_.begin(),
        entries_.end(),
        [id](const Entry& entry) { return entry.id == id; });
    if (it == entries_.end()) {
        return;
    }

    if (it->cancel_requested) {
        if (it->direction == Direction::kIn) {
            ++observed_in_cancellations_;
        } else {
            ++observed_out_cancellations_;
        }
    }
    entries_.erase(it);
}

std::size_t TransferSet::active() const
{
    return entries_.size();
}

bool TransferSet::ready_to_release() const
{
    return entries_.empty();
}

bool TransferSet::cancel_observed(Direction direction) const
{
    return direction == Direction::kIn
        ? observed_in_cancellations_ != 0
        : observed_out_cancellations_ != 0;
}

} // namespace gate1
