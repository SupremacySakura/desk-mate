#pragma once

#include <libusb-1.0/libusb.h>

#include <cstdint>
#include <iosfwd>
#include <string>
#include <vector>

namespace gate1 {

constexpr std::size_t kDeviceStatsFieldCount = 16;

struct DeviceStats {
    std::uint64_t zero_reads = 0;
    std::uint64_t poll_wakeups = 0;
    std::uint64_t bytes_read = 0;
    std::uint64_t frames_read = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t frames_written = 0;
    std::uint64_t write_errors = 0;
    std::uint64_t eventfd_wakeups = 0;
    std::uint64_t signal_wakeups = 0;
    std::uint64_t owner_close_wakeups = 0;
    std::uint64_t lifecycle_background = 0;
    std::uint64_t lifecycle_foreground = 0;
    std::uint64_t poll_timeouts = 0;
    std::uint64_t read_errors = 0;
    std::uint64_t zero_writes = 0;
    std::uint64_t parse_errors = 0;
};

struct ProbeOptions {
    int run_seconds = 10;
    int out_transfer_bytes = 16384;
    int heartbeat_ms = 1000;
    int stats_ms = 2000;
    int zlp_every_frames = 128;
    int lost_after_ms = 3000;
    int cancel_timeout_ms = 5000;
    int handshake_timeout_ms = 30000;
    int handshake_retry_ms = 1000;
};

struct ProbeResult {
    bool handshake_ok = false;
    bool echo_ok = false;
    bool cancellation_requested = false;
    bool cancellation_ok = false;
    bool cleanup_safe = false;
    bool device_stats_seen = false;

    std::uint64_t out_bytes = 0;
    std::uint64_t in_bytes = 0;
    std::uint64_t data_frames_sent = 0;
    std::uint64_t data_echoes_received = 0;
    std::uint64_t echo_mismatches = 0;
    std::uint64_t unmatched_echoes = 0;
    std::uint64_t pending_echoes_at_stop = 0;
    std::uint64_t zero_length_out_scheduled = 0;
    std::uint64_t zero_length_out_completed = 0;
    std::uint64_t zero_length_in_completed = 0;
    std::uint64_t heartbeats_sent = 0;
    std::uint64_t heartbeat_acks = 0;
    std::uint64_t heartbeat_last_rtt_us = 0;
    std::uint64_t heartbeat_max_rtt_us = 0;
    std::uint64_t heartbeat_loss_events = 0;

    int in_cancel_rc = 0;
    int out_cancel_rc = 0;
    int in_callback_status = -1;
    int out_callback_status = -1;
    std::uint64_t in_callback_after_cancel_us = 0;
    std::uint64_t out_callback_after_cancel_us = 0;
    std::uint64_t cancel_issue_gap_us = 0;
    std::uint64_t cancel_wait_ms = 0;
    std::size_t active_on_release = 0;

    DeviceStats device_stats;
    std::string error;
    std::vector<std::string> notes;
};

ProbeResult run_duplex_probe(
    libusb_context* context,
    libusb_device_handle* handle,
    int interface_number,
    std::uint8_t endpoint_in,
    std::uint8_t endpoint_out,
    const ProbeOptions& options,
    std::ostream& log);

} // namespace gate1
