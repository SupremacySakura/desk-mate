#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gate1 {

enum class Direction {
    kIn,
    kOut,
};

class TransferSet {
public:
    std::uint64_t add(Direction direction);
    bool mark_cancel_requested(Direction direction);
    void complete(std::uint64_t id);

    std::size_t active() const;
    bool ready_to_release() const;
    bool cancel_observed(Direction direction) const;

private:
    struct Entry {
        std::uint64_t id = 0;
        Direction direction = Direction::kIn;
        bool cancel_requested = false;
    };

    std::vector<Entry> entries_;
    std::uint64_t next_id_ = 1;
    std::size_t observed_in_cancellations_ = 0;
    std::size_t observed_out_cancellations_ = 0;
};

} // namespace gate1
