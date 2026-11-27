#pragma once

#include "deskmate/windows/aoa_links.hpp"

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// libusb.h must be visible here so the async callback declarations below use
// the same calling convention libusb expects (WINAPI on 32-bit Windows).
#if defined(DESKMATE_HAVE_LIBUSB) && DESKMATE_HAVE_LIBUSB
#include <libusb-1.0/libusb.h>
#else
struct libusb_context;
struct libusb_device_handle;
struct libusb_transfer;
#ifndef LIBUSB_CALL
#define LIBUSB_CALL
#endif
#endif

namespace deskmate::windows {

// Selects the byte count for the next Bulk OUT transfer.
// HarmonyOS/Android f_accessory read() is 16 KiB message-oriented: a transfer
// whose length is an exact multiple of USB wMaxPacketSize (512B High-Speed /
// 1024B SuperSpeed) has no terminating short packet, so the device UDC can
// coalesce the next 12 KiB video fragment into the same 16 KiB read and
// overflow/STALL the pipe. Splitting off 64 bytes ensures both chunks end in a
// short packet (< 512B).
[[nodiscard]] constexpr std::size_t select_bulk_out_chunk_bytes(
    std::size_t remaining_bytes,
    std::size_t max_transfer_bytes = 16384) noexcept {
  if (remaining_bytes == 0 || max_transfer_bytes == 0) {
    return 0;
  }
  std::size_t chunk =
      remaining_bytes < max_transfer_bytes ? remaining_bytes : max_transfer_bytes;
  if (chunk >= 512U && (chunk % 512U) == 0U) {
    chunk -= 64U;
  }
  return chunk;
}

struct ScannedUsbDevice {
  deskmate::platform::AoaIdentity identity;
  std::uint16_t vendor_id{};
  std::uint16_t product_id{};
  bool in_accessory_mode{};
  std::string interfaces_summary;
};

// Bulk transport over a claimed AOA interface.
//
// Both directions use libusb's asynchronous API with a dedicated event thread.
// A synchronous bulk_write blocks the caller until the URB completes, which at
// 90/120 Hz serialises the whole video pipeline behind one transfer at a time;
// writes here only fill a free slot and submit, so several URBs stay in flight.
class LibusbBulkTransport final : public LinkTransport {
 public:
  LibusbBulkTransport(libusb_device_handle* handle, std::uint8_t endpoint_in,
                      std::uint8_t endpoint_out, std::string device_id,
                      int interface_number = -1,
                      libusb_context* context = nullptr);
  ~LibusbBulkTransport() override;

  LibusbBulkTransport(const LibusbBulkTransport&) = delete;
  LibusbBulkTransport& operator=(const LibusbBulkTransport&) = delete;

  [[nodiscard]] deskmate::protocol::IoResult read(
      std::span<std::uint8_t> destination) override;
  [[nodiscard]] deskmate::protocol::IoResult write(
      std::span<const std::uint8_t> source) override;
  void cancel() override;

  [[nodiscard]] bool alive() const override;
  [[nodiscard]] std::uint64_t rx_bytes() const override {
    return rx_bytes_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t tx_bytes() const override {
    return tx_bytes_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t bulk_errors() const override {
    return bulk_errors_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::uint64_t write_timeouts() const override {
    return write_timeouts_.load(std::memory_order_relaxed);
  }
  [[nodiscard]] std::string last_error() const override {
    std::lock_guard lock(mutex_);
    return last_error_;
  }
  void set_rx_callback(std::function<void()> callback) override;
  void set_stream_fps(int fps) override;
  [[nodiscard]] bool consume_write_blocked() noexcept override;

 private:
  struct OutSlot {
    libusb_transfer* transfer{nullptr};
    std::vector<unsigned char> buffer;
    std::atomic<bool> busy{false};
  };

  static void LIBUSB_CALL on_in_complete(libusb_transfer* transfer);
  static void LIBUSB_CALL on_out_complete(libusb_transfer* transfer);

  void event_loop();
  void submit_in_transfer(std::size_t index);
  void handle_in_complete(libusb_transfer* transfer);
  void handle_out_complete(libusb_transfer* transfer);
  void cancel_all_transfers();
  void free_transfers();
  void report_error(const std::string& message);
  void wake_reader();
  void notify_rx();

  [[maybe_unused]] libusb_device_handle* handle_;
  [[maybe_unused]] std::uint8_t endpoint_in_;
  [[maybe_unused]] std::uint8_t endpoint_out_;
  [[maybe_unused]] int interface_number_;
  [[maybe_unused]] libusb_context* context_;
  std::string device_id_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> closed_{false};
  std::atomic<bool> failed_{false};
  std::atomic<bool> write_blocked_{false};
  // Set when a frame was refused for lack of a free OUT slot. Unlike
  // write_blocked_ (consumed by the link worker) this flag is only cleared by
  // the OUT completion handler, so a slot retiring always wakes the sender.
  std::atomic<bool> out_wake_needed_{false};
  std::atomic<std::uint64_t> rx_bytes_{0};
  std::atomic<std::uint64_t> tx_bytes_{0};
  std::atomic<std::uint64_t> bulk_errors_{0};
  std::atomic<std::uint64_t> write_timeouts_{0};
  std::atomic<int> consecutive_write_errors_{0};
  std::atomic<int> consecutive_in_errors_{0};
  // Bulk OUT URB timeout. Async writes never block the caller, so this only
  // bounds a genuinely wedged pipe; 25 ms (the old 3000/fps value) made every
  // queued URB race the timer and let WinUSB abort the whole pipe.
  std::atomic<int> write_timeout_ms_{2000};
  std::atomic<int> active_transfers_{0};
  std::thread events_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::uint8_t> rx_buffer_;
  std::string last_error_;
  std::mutex rx_cb_mutex_;
  std::function<void()> rx_callback_;

  static constexpr std::size_t kInTransferCount = 4;
  static constexpr std::size_t kInTransferBytes = 16384;
  static constexpr std::size_t kOutSlotCount = 8;
  static constexpr std::size_t kMaxOutTransferBytes = 16384;

  [[maybe_unused]] std::array<libusb_transfer*, kInTransferCount>
      in_transfers_{};
  [[maybe_unused]] std::array<
      std::array<unsigned char, kInTransferBytes>, kInTransferCount>
      in_buffers_{};
  [[maybe_unused]] std::array<OutSlot, kOutSlotCount> out_slots_{};
};

// libusb-backed AOA discovery and attach. libusb's WinUSB backend is used on
// Windows; the same implementation compiles on macOS for host-side contract
// tests when libusb is installed.
class LibusbAoaDiscovery final : public AoaDiscovery {
 public:
  LibusbAoaDiscovery();
  ~LibusbAoaDiscovery() override;

  LibusbAoaDiscovery(const LibusbAoaDiscovery&) = delete;
  LibusbAoaDiscovery& operator=(const LibusbAoaDiscovery&) = delete;

  void set_log_callback(std::function<void(std::string)> callback);

  [[nodiscard]] std::vector<deskmate::platform::AoaIdentity> list_candidates()
      override;
  [[nodiscard]] std::unique_ptr<LinkTransport> attach(
      const deskmate::platform::AoaIdentity& identity,
      std::string& error) override;

  // Non-blocking discovery helpers mirroring macOS Bridge.cpp:
  // 1) scan_all_devices() enumerates all attached HDC/Android/AOA devices
  //    (with cached identity resolution across USB 3.0 <-> USB 2.0 companion
  //    port re-enumeration).
  // 2) trigger_aoa_switch() sends GET_PROTOCOL + UTF-8 SEND_STRING + START
  //    without blocking 10s, recording cooldown & expected identity.
  // 3) open_accessory() attaches directly to an enumerated 18d1:2d00..2d05
  //    device on any port and surfaces missing-WinUSB driver hints.
  [[nodiscard]] std::vector<ScannedUsbDevice> scan_all_devices(
      const std::unordered_set<std::uint16_t>& owned_bus_addrs = {});
  [[nodiscard]] bool trigger_aoa_switch(
      const ScannedUsbDevice& candidate, std::string& error,
      bool& driver_missing_out);
  [[nodiscard]] std::unique_ptr<LinkTransport> open_accessory(
      const ScannedUsbDevice& accessory, std::string& error,
      bool& driver_missing_out);
  [[nodiscard]] bool is_accessory_present(std::uint8_t bus,
                                          std::uint8_t address) const;
  [[nodiscard]] bool is_accessory_present(
      const deskmate::platform::AoaIdentity& identity) const;
  [[nodiscard]] bool is_in_switch_cooldown(
      const ScannedUsbDevice& candidate) const;
  void probe_pnp_aoa_status(std::string& summary_out, bool& driver_missing_out,
                            std::string& driver_hint_out) const;

  [[nodiscard]] bool initialized() const { return context_ != nullptr; }
  [[nodiscard]] const std::string& last_error() const { return last_error_; }

 private:
  struct CachedIdentity {
    deskmate::platform::AoaIdentity identity;
    std::chrono::steady_clock::time_point expires_at;
  };

  void emit_log(const std::string& message) const;
  void remember_pre_switch_identity(
      const deskmate::platform::AoaIdentity& identity,
      const std::string& cooldown_key);
  void resolve_accessory_identity(
      deskmate::platform::AoaIdentity& identity) const;

  libusb_context* context_{};
  std::string last_error_;
  mutable std::mutex log_mutex_;
  std::function<void(std::string)> log_callback_;

  mutable std::mutex cache_mutex_;
  std::unordered_map<std::string, deskmate::platform::AoaIdentity>
      hdc_identity_by_serial_;
  std::unordered_map<std::string, deskmate::platform::AoaIdentity>
      hdc_identity_by_port_;
  CachedIdentity last_switched_identity_{};
  [[maybe_unused]] bool has_last_switched_{false};
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      aoa_switch_cooldown_;
  std::chrono::steady_clock::time_point last_aoa_start_at_{};
};

}  // namespace deskmate::windows
