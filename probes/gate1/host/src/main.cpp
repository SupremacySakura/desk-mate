#include "gate1/aoa.hpp"
#include "gate1/cleanup.hpp"
#include "gate1/probe_session.hpp"

#include <libusb-1.0/libusb.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kDefaultEnumerateTimeoutMs = 15000;

struct CommandLine {
    std::string command;
    gate1::DeviceSelector selector;
    gate1::AccessoryIdentity identity;
    gate1::ProbeOptions probe;
    int enumerate_timeout_ms = kDefaultEnumerateTimeoutMs;
    std::string json_path;
};

std::string json_escape(const std::string& value)
{
    std::ostringstream escaped;
    for (const char character : value) {
        switch (character) {
            case '"':
                escaped << "\\\"";
                break;
            case '\\':
                escaped << "\\\\";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                escaped << character;
                break;
        }
    }
    return escaped.str();
}

std::string iso8601_now()
{
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
#if defined(_WIN32)
    gmtime_s(&utc, &now);
#else
    gmtime_r(&now, &utc);
#endif
    char buffer[32] = {};
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &utc);
    return buffer;
}

bool parse_integer(const std::string& text, int& value)
{
    try {
        std::size_t consumed = 0;
        const long parsed = std::stol(text, &consumed, 0);
        if (consumed != text.size() ||
            parsed < std::numeric_limits<int>::min() ||
            parsed > std::numeric_limits<int>::max()) {
            return false;
        }
        value = static_cast<int>(parsed);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void print_usage()
{
    std::cout
        << "DeskMate Gate 1 AOA duplex probe\n\n"
        << "Usage:\n"
        << "  gate1_host list\n"
        << "  gate1_host aoa   --bus N --address N [string options]\n"
        << "  gate1_host probe [probe options]\n"
        << "  gate1_host run   --bus N --address N [aoa + probe options]\n\n"
        << "Selectors:\n"
        << "  --bus N --address N | --vid 0xNNNN --pid 0xNNNN\n\n"
        << "AOA strings:\n"
        << "  --manufacturer TEXT --model TEXT --description TEXT\n"
        << "  --version TEXT --uri TEXT --serial TEXT\n\n"
        << "Probe options:\n"
        << "  --run-seconds N        default 10\n"
        << "  --out-bytes N          async bulk OUT size, default 16384\n"
        << "  --heartbeat-ms N       default 1000\n"
        << "  --stats-ms N           default 2000\n"
        << "  --zlp-every N          default 128 data frames; 0 disables\n"
        << "  --lost-after-ms N      default 3000\n"
        << "  --cancel-timeout-ms N  default 5000\n"
        << "  --handshake-initial-delay-ms N\n"
        << "                         wait after claim before first HELLO, default 0\n"
        << "  --handshake-timeout-ms N\n"
        << "                         wait for Device HELLO_ACK, default 30000\n"
        << "  --handshake-retry-ms N resend HELLO interval, default 1000\n"
        << "  --json PATH            write machine-readable evidence\n\n"
        << "This tool does not decide whether Gate 1 passed. Hardware evidence\n"
        << "from the recorded Reference Environment remains mandatory.\n";
}

bool parse_command_line(int argc, char** argv, CommandLine& parsed)
{
    if (argc < 2) {
        return false;
    }
    parsed.command = argv[1];
    if (parsed.command == "--help" || parsed.command == "-h" || parsed.command == "help") {
        parsed.command = "help";
        return true;
    }
    if (parsed.command != "list" && parsed.command != "aoa" &&
        parsed.command != "probe" && parsed.command != "run") {
        return false;
    }

    auto require_value = [&](int& index, std::string& value) -> bool {
        if (index + 1 >= argc) {
            return false;
        }
        value = argv[++index];
        return true;
    };
    auto require_int = [&](int& index, int& value) -> bool {
        std::string text;
        return require_value(index, text) && parse_integer(text, value);
    };
    auto require_string = [&](int& index, std::string& value) -> bool {
        return require_value(index, value);
    };

    for (int i = 2; i < argc; ++i) {
        const std::string argument = argv[i];
        bool ok = true;
        if (argument == "--bus") {
            ok = require_int(i, parsed.selector.bus);
        } else if (argument == "--address") {
            ok = require_int(i, parsed.selector.address);
        } else if (argument == "--vid") {
            ok = require_int(i, parsed.selector.vendor_id);
        } else if (argument == "--pid") {
            ok = require_int(i, parsed.selector.product_id);
        } else if (argument == "--manufacturer") {
            ok = require_string(i, parsed.identity.manufacturer);
        } else if (argument == "--model") {
            ok = require_string(i, parsed.identity.model);
        } else if (argument == "--description") {
            ok = require_string(i, parsed.identity.description);
        } else if (argument == "--version") {
            ok = require_string(i, parsed.identity.version);
        } else if (argument == "--uri") {
            ok = require_string(i, parsed.identity.uri);
        } else if (argument == "--serial") {
            ok = require_string(i, parsed.identity.serial);
        } else if (argument == "--enumerate-timeout-ms") {
            ok = require_int(i, parsed.enumerate_timeout_ms);
        } else if (argument == "--run-seconds") {
            ok = require_int(i, parsed.probe.run_seconds);
        } else if (argument == "--out-bytes") {
            ok = require_int(i, parsed.probe.out_transfer_bytes);
        } else if (argument == "--heartbeat-ms") {
            ok = require_int(i, parsed.probe.heartbeat_ms);
        } else if (argument == "--stats-ms") {
            ok = require_int(i, parsed.probe.stats_ms);
        } else if (argument == "--zlp-every") {
            ok = require_int(i, parsed.probe.zlp_every_frames);
        } else if (argument == "--lost-after-ms") {
            ok = require_int(i, parsed.probe.lost_after_ms);
        } else if (argument == "--cancel-timeout-ms") {
            ok = require_int(i, parsed.probe.cancel_timeout_ms);
        } else if (argument == "--handshake-initial-delay-ms") {
            ok = require_int(i, parsed.probe.handshake_initial_delay_ms);
        } else if (argument == "--handshake-timeout-ms") {
            ok = require_int(i, parsed.probe.handshake_timeout_ms);
        } else if (argument == "--handshake-retry-ms") {
            ok = require_int(i, parsed.probe.handshake_retry_ms);
        } else if (argument == "--json") {
            ok = require_string(i, parsed.json_path);
        } else {
            std::cerr << "unknown argument: " << argument << "\n";
            return false;
        }
        if (!ok) {
            std::cerr << "missing or invalid value for " << argument << "\n";
            return false;
        }
    }

    if (parsed.enumerate_timeout_ms <= 0 ||
        parsed.selector.bus < -1 ||
        parsed.selector.address < -1 ||
        parsed.selector.vendor_id < -1 ||
        parsed.selector.product_id < -1) {
        std::cerr << "invalid selector or timeout\n";
        return false;
    }
    return true;
}

void print_summary(const gate1::ProbeResult& result)
{
    std::cout
        << "\n=== Probe summary ===\n"
        << "handshake_ok=" << (result.handshake_ok ? "true" : "false") << "\n"
        << "echo_ok=" << (result.echo_ok ? "true" : "false") << "\n"
        << "out_bytes=" << result.out_bytes << "\n"
        << "in_bytes=" << result.in_bytes << "\n"
        << "data_frames_sent=" << result.data_frames_sent << "\n"
        << "data_echoes_received=" << result.data_echoes_received << "\n"
        << "echo_mismatches=" << result.echo_mismatches << "\n"
        << "unmatched_echoes=" << result.unmatched_echoes << "\n"
        << "pending_echoes_at_stop=" << result.pending_echoes_at_stop << "\n"
        << "zlp_out_scheduled=" << result.zero_length_out_scheduled << "\n"
        << "zlp_out_completed=" << result.zero_length_out_completed << "\n"
        << "zlp_in_completed=" << result.zero_length_in_completed << "\n"
        << "heartbeats_sent=" << result.heartbeats_sent << "\n"
        << "heartbeat_acks=" << result.heartbeat_acks << "\n"
        << "heartbeat_last_rtt_us=" << result.heartbeat_last_rtt_us << "\n"
        << "heartbeat_max_rtt_us=" << result.heartbeat_max_rtt_us << "\n"
        << "heartbeat_loss_events=" << result.heartbeat_loss_events << "\n"
        << "cancellation_requested=" << (result.cancellation_requested ? "true" : "false") << "\n"
        << "cancellation_ok=" << (result.cancellation_ok ? "true" : "false") << "\n"
        << "cancel_wait_ms=" << result.cancel_wait_ms << "\n"
        << "cancel_issue_gap_us=" << result.cancel_issue_gap_us << "\n"
        << "in_callback_after_cancel_us=" << result.in_callback_after_cancel_us << "\n"
        << "out_callback_after_cancel_us=" << result.out_callback_after_cancel_us << "\n"
        << "active_on_release=" << result.active_on_release << "\n"
        << "cleanup_safe=" << (result.cleanup_safe ? "true" : "false") << "\n"
        << "device_stats_seen=" << (result.device_stats_seen ? "true" : "false") << "\n";
    if (result.device_stats_seen) {
        std::cout
            << "device.zero_reads=" << result.device_stats.zero_reads << "\n"
            << "device.poll_wakeups=" << result.device_stats.poll_wakeups << "\n"
            << "device.bytes_read=" << result.device_stats.bytes_read << "\n"
            << "device.bytes_written=" << result.device_stats.bytes_written << "\n"
            << "device.zero_writes=" << result.device_stats.zero_writes << "\n"
            << "device.parse_errors=" << result.device_stats.parse_errors << "\n";
    }
    for (const std::string& note : result.notes) {
        std::cout << "note=" << note << "\n";
    }
    if (!result.error.empty()) {
        std::cout << "error=" << result.error << "\n";
    }
    std::cout
        << "\nGate 1 conclusion: NOT EVALUATED\n"
        << "A transport run is not a Gate 1 pass. Reference Environment and\n"
        << "direct-link topology must be documented and the full matrix repeated.\n";
}

bool write_json(
    const std::string& path,
    const gate1::ProbeResult& result,
    const gate1::ProbeOptions& options,
    const gate1::OpenedAccessory& accessory)
{
    if (path.empty()) {
        return true;
    }
    std::ofstream output(path);
    if (!output) {
        std::cerr << "cannot open JSON output: " << path << "\n";
        return false;
    }
    output << "{\n"
           << "  \"schema\": \"deskmate.gate1.host-probe.v1\",\n"
           << "  \"captured_at_utc\": \"" << iso8601_now() << "\",\n"
           << "  \"gate1_conclusion\": \"not_evaluated\",\n"
           << "  \"reference_environment_attestation\": \"not-captured-by-this-tool\",\n"
           << "  \"reference_environment_note\": "
           << "\"The tool cannot certify Reference Environment or direct-link topology.\",\n"
           << "  \"device\": {\n"
           << "    \"vid\": " << accessory.vendor_id << ",\n"
           << "    \"pid\": " << accessory.product_id << ",\n"
           << "    \"bus\": " << static_cast<int>(accessory.bus) << ",\n"
           << "    \"address\": " << static_cast<int>(accessory.address) << ",\n"
           << "    \"interface\": " << accessory.interface_number << ",\n"
           << "    \"alternate_setting\": " << accessory.alternate_setting << ",\n"
           << "    \"interface_endpoints\": " << accessory.interface_endpoints << ",\n"
           << "    \"endpoint_in\": " << static_cast<int>(accessory.endpoint_in) << ",\n"
           << "    \"endpoint_out\": " << static_cast<int>(accessory.endpoint_out) << ",\n"
           << "    \"endpoint_in_max_packet\": "
           << accessory.endpoint_in_max_packet << ",\n"
           << "    \"endpoint_out_max_packet\": "
           << accessory.endpoint_out_max_packet << ",\n"
           << "    \"endpoint_in_attributes\": "
           << static_cast<int>(accessory.endpoint_in_attributes) << ",\n"
           << "    \"endpoint_out_attributes\": "
           << static_cast<int>(accessory.endpoint_out_attributes) << ",\n"
           << "    \"descriptors\": \"" << accessory.descriptor_dump << "\",\n"
           << "    \"speed\": \"" << gate1::speed_name(accessory.speed) << "\"\n"
           << "  },\n"
           << "  \"options\": {\n"
           << "    \"run_seconds\": " << options.run_seconds << ",\n"
           << "    \"out_transfer_bytes\": " << options.out_transfer_bytes << ",\n"
           << "    \"heartbeat_ms\": " << options.heartbeat_ms << ",\n"
           << "    \"stats_ms\": " << options.stats_ms << ",\n"
           << "    \"zlp_every_frames\": " << options.zlp_every_frames << ",\n"
           << "    \"lost_after_ms\": " << options.lost_after_ms << ",\n"
           << "    \"cancel_timeout_ms\": " << options.cancel_timeout_ms << ",\n"
           << "    \"handshake_initial_delay_ms\": "
           << options.handshake_initial_delay_ms << ",\n"
           << "    \"handshake_timeout_ms\": " << options.handshake_timeout_ms << ",\n"
           << "    \"handshake_retry_ms\": " << options.handshake_retry_ms << "\n"
           << "  },\n"
           << "  \"result\": {\n"
           << "    \"handshake_ok\": " << (result.handshake_ok ? "true" : "false") << ",\n"
           << "    \"echo_ok\": " << (result.echo_ok ? "true" : "false") << ",\n"
           << "    \"cancellation_requested\": "
           << (result.cancellation_requested ? "true" : "false") << ",\n"
           << "    \"cancellation_ok\": "
           << (result.cancellation_ok ? "true" : "false") << ",\n"
           << "    \"cleanup_safe\": " << (result.cleanup_safe ? "true" : "false") << ",\n"
           << "    \"active_on_release\": " << result.active_on_release << ",\n"
           << "    \"cancel_wait_ms\": " << result.cancel_wait_ms << ",\n"
           << "    \"cancel_issue_gap_us\": " << result.cancel_issue_gap_us << ",\n"
           << "    \"in_callback_after_cancel_us\": "
           << result.in_callback_after_cancel_us << ",\n"
           << "    \"out_callback_after_cancel_us\": "
           << result.out_callback_after_cancel_us << ",\n"
           << "    \"out_bytes\": " << result.out_bytes << ",\n"
           << "    \"in_bytes\": " << result.in_bytes << ",\n"
           << "    \"data_frames_sent\": " << result.data_frames_sent << ",\n"
           << "    \"data_echoes_received\": " << result.data_echoes_received << ",\n"
           << "    \"echo_mismatches\": " << result.echo_mismatches << ",\n"
           << "    \"unmatched_echoes\": " << result.unmatched_echoes << ",\n"
           << "    \"pending_echoes_at_stop\": " << result.pending_echoes_at_stop << ",\n"
           << "    \"zero_length_out_scheduled\": "
           << result.zero_length_out_scheduled << ",\n"
           << "    \"zero_length_out_completed\": "
           << result.zero_length_out_completed << ",\n"
           << "    \"zero_length_in_completed\": "
           << result.zero_length_in_completed << ",\n"
           << "    \"heartbeats_sent\": " << result.heartbeats_sent << ",\n"
           << "    \"heartbeat_acks\": " << result.heartbeat_acks << ",\n"
           << "    \"heartbeat_last_rtt_us\": " << result.heartbeat_last_rtt_us << ",\n"
           << "    \"heartbeat_max_rtt_us\": " << result.heartbeat_max_rtt_us << ",\n"
           << "    \"heartbeat_loss_events\": " << result.heartbeat_loss_events << ",\n"
           << "    \"device_stats_seen\": "
           << (result.device_stats_seen ? "true" : "false") << ",\n"
           << "    \"device_stats\": {\n"
           << "      \"zero_reads\": " << result.device_stats.zero_reads << ",\n"
           << "      \"poll_wakeups\": " << result.device_stats.poll_wakeups << ",\n"
           << "      \"bytes_read\": " << result.device_stats.bytes_read << ",\n"
           << "      \"frames_read\": " << result.device_stats.frames_read << ",\n"
           << "      \"bytes_written\": " << result.device_stats.bytes_written << ",\n"
           << "      \"frames_written\": " << result.device_stats.frames_written << ",\n"
           << "      \"write_errors\": " << result.device_stats.write_errors << ",\n"
           << "      \"eventfd_wakeups\": " << result.device_stats.eventfd_wakeups << ",\n"
           << "      \"signal_wakeups\": " << result.device_stats.signal_wakeups << ",\n"
           << "      \"owner_close_wakeups\": "
           << result.device_stats.owner_close_wakeups << ",\n"
           << "      \"lifecycle_background\": "
           << result.device_stats.lifecycle_background << ",\n"
           << "      \"lifecycle_foreground\": "
           << result.device_stats.lifecycle_foreground << ",\n"
           << "      \"poll_timeouts\": " << result.device_stats.poll_timeouts << ",\n"
           << "      \"read_errors\": " << result.device_stats.read_errors << ",\n"
           << "      \"zero_writes\": " << result.device_stats.zero_writes << ",\n"
           << "      \"parse_errors\": " << result.device_stats.parse_errors << "\n"
           << "    },\n"
           << "    \"error\": \"" << json_escape(result.error) << "\",\n"
           << "    \"notes\": [";
    for (std::size_t i = 0; i < result.notes.size(); ++i) {
        if (i != 0) {
            output << ", ";
        }
        output << "\"" << json_escape(result.notes[i]) << "\"";
    }
    output << "]\n"
           << "  }\n"
           << "}\n";
    return static_cast<bool>(output);
}

int list_command(libusb_context* context)
{
    std::string error;
    const std::vector<gate1::DeviceSummary> devices = gate1::list_devices(context, error);
    if (!error.empty()) {
        std::cerr << error << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "bus addr vid:pid      speed        role\n";
    for (const gate1::DeviceSummary& device : devices) {
        std::cout << std::setw(3) << static_cast<int>(device.bus) << " "
                  << std::setw(3) << static_cast<int>(device.address) << " "
                  << std::hex << std::setfill('0') << std::setw(4) << device.vendor_id
                  << ":" << std::setw(4) << device.product_id << std::dec
                  << std::setfill(' ') << "  "
                  << std::left << std::setw(12) << gate1::speed_name(device.speed)
                  << std::right << (device.accessory ? "AOA accessory" : "device")
                  << "\n";
    }
    return EXIT_SUCCESS;
}

int open_accessory(libusb_context* context, int timeout_ms, gate1::OpenedAccessory& accessory)
{
    std::cout << "[open] waiting for AOA accessory enumeration...\n";
    std::string error;
    if (!gate1::wait_for_accessory(
            context,
            static_cast<unsigned>(timeout_ms),
            accessory,
            error)) {
        std::cerr << "[open] " << error << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "[open] accessory "
              << std::hex << accessory.vendor_id << ":" << accessory.product_id
              << std::dec << " bus=" << static_cast<int>(accessory.bus)
              << " address=" << static_cast<int>(accessory.address)
              << " speed=" << gate1::speed_name(accessory.speed)
              << " interface=" << accessory.interface_number
              << " alt=" << accessory.alternate_setting
              << " endpoints=" << accessory.interface_endpoints
              << " ep_in=0x" << std::hex << static_cast<int>(accessory.endpoint_in)
              << "(max=" << std::dec << accessory.endpoint_in_max_packet
              << ",attr=0x" << std::hex
              << static_cast<int>(accessory.endpoint_in_attributes) << ")"
              << " ep_out=0x" << std::hex << static_cast<int>(accessory.endpoint_out)
              << "(max=" << std::dec << accessory.endpoint_out_max_packet
              << ",attr=0x" << std::hex
              << static_cast<int>(accessory.endpoint_out_attributes) << ")"
              << std::dec << "\n";
    return EXIT_SUCCESS;
}

struct ProbeCommandOutcome {
    int exit_code = EXIT_FAILURE;
    bool cleanup_safe = false;
};

ProbeCommandOutcome probe_command(
    libusb_context* context,
    const CommandLine& options,
    gate1::OpenedAccessory& accessory)
{
    const gate1::ProbeResult result = gate1::run_duplex_probe(
        context,
        accessory.handle,
        accessory.interface_number,
        accessory.endpoint_in,
        accessory.endpoint_out,
        options.probe,
        std::cout);
    ProbeCommandOutcome outcome;
    outcome.cleanup_safe = result.cleanup_safe;
    print_summary(result);
    if (!write_json(options.json_path, result, options.probe, accessory)) {
        outcome.exit_code = EXIT_FAILURE;
        return outcome;
    }
    if (!result.cleanup_safe) {
        std::cerr << "cleanup unsafe: leaving handle/context open until process exit\n";
        outcome.exit_code = EXIT_FAILURE;
        return outcome;
    }
    outcome.exit_code = (result.handshake_ok && result.echo_ok && result.cancellation_ok)
        ? EXIT_SUCCESS
        : EXIT_FAILURE;
    return outcome;
}

} // namespace

int main(int argc, char** argv)
{
    CommandLine parsed;
    if (!parse_command_line(argc, argv, parsed)) {
        print_usage();
        return EXIT_FAILURE;
    }
    if (parsed.command == "help") {
        print_usage();
        return EXIT_SUCCESS;
    }

    libusb_context* context = nullptr;
    const int init_rc = libusb_init(&context);
    if (init_rc != LIBUSB_SUCCESS || context == nullptr) {
        std::cerr << "libusb_init failed: " << libusb_error_name(init_rc) << "\n";
        return EXIT_FAILURE;
    }

    int exit_code = EXIT_SUCCESS;
    bool probe_cleanup_safe = true;
    gate1::OpenedAccessory accessory;

    if (parsed.command == "list") {
        exit_code = list_command(context);
        libusb_exit(context);
        return exit_code;
    }

    if (parsed.command == "aoa" || parsed.command == "run") {
        if (!gate1::selector_has_filter(parsed.selector)) {
            std::cerr << "aoa/run requires --bus/--address or --vid/--pid\n";
            exit_code = EXIT_FAILURE;
        } else {
            std::string error;
            std::cout << "[aoa] GET_PROTOCOL(51), SEND_STRING(52), START(53)\n";
            if (!gate1::switch_to_accessory(
                    context,
                    parsed.selector,
                    parsed.identity,
                    static_cast<unsigned>(parsed.enumerate_timeout_ms),
                    error)) {
                std::cerr << "[aoa] " << error << "\n";
                exit_code = EXIT_FAILURE;
            } else {
                std::cout << "[aoa] START accepted; device re-enumerated\n";
            }
        }
    }

    if (exit_code == EXIT_SUCCESS && parsed.command != "aoa") {
        exit_code = open_accessory(context, parsed.enumerate_timeout_ms, accessory);
        if (exit_code == EXIT_SUCCESS) {
            const ProbeCommandOutcome outcome =
                probe_command(context, parsed, accessory);
            exit_code = outcome.exit_code;
            probe_cleanup_safe = outcome.cleanup_safe;
        }
    } else if (exit_code == EXIT_SUCCESS && parsed.command == "aoa") {
        exit_code = open_accessory(context, parsed.enumerate_timeout_ms, accessory);
    }

    const gate1::ResourceRelease release = gate1::resource_release(
        accessory.handle != nullptr,
        probe_cleanup_safe);
    if (accessory.handle != nullptr && release.close_handle) {
        gate1::close_accessory(accessory);
    }
    if (release.exit_context) {
        libusb_exit(context);
    }
    return exit_code;
}
