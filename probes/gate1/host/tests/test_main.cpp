#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include "gate1/protocol.hpp"
#include "gate1/transfer_set.hpp"

namespace {

int failures = 0;

void expect(bool condition, const char* expression, const char* test, int line)
{
    if (condition) {
        return;
    }
    std::cerr << "FAIL " << test << ":" << line << ": " << expression << "\n";
    ++failures;
}

#define EXPECT_TRUE(expr) expect((expr), #expr, __func__, __LINE__)
#define EXPECT_EQ(lhs, rhs) expect((lhs) == (rhs), #lhs " == " #rhs, __func__, __LINE__)

void decoder_waits_for_a_frame_split_across_chunks()
{
    const std::vector<std::uint8_t> encoded =
        gate1::encode_frame(gate1::FrameType::kData, 0, 7, {0x10, 0x20, 0x30});
    gate1::FrameDecoder decoder;
    std::vector<gate1::Frame> frames;
    std::string error;

    EXPECT_TRUE(decoder.push(encoded.data(), 5, frames, error));
    EXPECT_TRUE(frames.empty());
    EXPECT_TRUE(decoder.push(encoded.data() + 5, encoded.size() - 5, frames, error));

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(frames.size(), std::size_t{1});
    if (frames.size() == 1) {
        EXPECT_EQ(frames[0].type, gate1::FrameType::kData);
        EXPECT_EQ(frames[0].seq, std::uint32_t{7});
        EXPECT_TRUE(frames[0].payload == std::vector<std::uint8_t>({0x10, 0x20, 0x30}));
    }
}

void decoder_reassembles_coalesced_frames_and_records_zero_length_transfers()
{
    const std::vector<std::uint8_t> first =
        gate1::encode_frame(gate1::FrameType::kHeartbeat, 0, 11, {});
    const std::vector<std::uint8_t> second =
        gate1::encode_frame(gate1::FrameType::kDataEcho, 3, 12, {0xAA});
    std::vector<std::uint8_t> combined = first;
    combined.insert(combined.end(), second.begin(), second.begin() + 9);
    const std::vector<std::uint8_t> tail(second.begin() + 9, second.end());

    gate1::FrameDecoder decoder;
    std::vector<gate1::Frame> frames;
    std::string error;

    EXPECT_TRUE(decoder.push(combined.data(), combined.size(), frames, error));
    EXPECT_EQ(frames.size(), std::size_t{1});
    EXPECT_TRUE(decoder.push(nullptr, 0, frames, error));
    EXPECT_TRUE(decoder.push(tail.data(), tail.size(), frames, error));

    EXPECT_TRUE(error.empty());
    EXPECT_EQ(decoder.zero_length_chunks(), std::size_t{1});
    EXPECT_EQ(frames.size(), std::size_t{2});
    if (frames.size() == 2) {
        EXPECT_EQ(frames[1].type, gate1::FrameType::kDataEcho);
        EXPECT_EQ(frames[1].flags, std::uint16_t{3});
        EXPECT_TRUE(frames[1].payload == std::vector<std::uint8_t>({0xAA}));
    }
}

void transfer_set_releases_only_after_both_cancelled_callbacks_arrive()
{
    gate1::TransferSet transfers;
    const std::uint64_t in_id = transfers.add(gate1::Direction::kIn);
    const std::uint64_t out_id = transfers.add(gate1::Direction::kOut);

    EXPECT_EQ(transfers.active(), std::size_t{2});
    EXPECT_TRUE(transfers.mark_cancel_requested(gate1::Direction::kIn));
    EXPECT_TRUE(transfers.mark_cancel_requested(gate1::Direction::kOut));
    EXPECT_EQ(transfers.active(), std::size_t{2});
    EXPECT_TRUE(!transfers.ready_to_release());

    transfers.complete(in_id);
    EXPECT_EQ(transfers.active(), std::size_t{1});
    EXPECT_TRUE(!transfers.ready_to_release());

    transfers.complete(out_id);
    EXPECT_EQ(transfers.active(), std::size_t{0});
    EXPECT_TRUE(transfers.ready_to_release());
    EXPECT_TRUE(transfers.cancel_observed(gate1::Direction::kIn));
    EXPECT_TRUE(transfers.cancel_observed(gate1::Direction::kOut));
}

void deterministic_payload_has_a_stable_checksum()
{
    const std::vector<std::uint8_t> payload = gate1::make_data_payload(7, 6);
    EXPECT_TRUE(payload == std::vector<std::uint8_t>({0xD9, 0xC8, 0xFB, 0xEA, 0x9D, 0x8C}));
    EXPECT_EQ(gate1::fnv1a64(payload.data(), payload.size()), std::uint64_t{0x6a3511fc2239713cULL});
}

} // namespace

int main()
{
    decoder_waits_for_a_frame_split_across_chunks();
    decoder_reassembles_coalesced_frames_and_records_zero_length_transfers();
    transfer_set_releases_only_after_both_cancelled_callbacks_arrive();
    deterministic_payload_has_a_stable_checksum();

    if (failures != 0) {
        std::cerr << failures << " assertion(s) failed\n";
        return EXIT_FAILURE;
    }
    std::cout << "all tests passed\n";
    return EXIT_SUCCESS;
}
