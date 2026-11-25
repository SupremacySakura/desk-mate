#pragma once

#include "deskmate/windows/aoa_links.hpp"

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

struct libusb_context;
struct libusb_device_handle;

namespace deskmate::windows {

struct ScannedUsbDevice {
  deskmate::platform::AoaIdentity identity;
  std::uint16_t vendor_id{};
  std::uint16_t product_id{};
  bool in_accessory_mode{};
  std::string interfaces_summary;
};

// Bulk transport over a claimed AOA interface.
//
// The reader runs on its own thread, mirroring the macOS host: libusb bulk IN
// polling must never share a loop with the video write pump. Writes report the
// exact prefix accepted by libusb so the core can retry only the suffix after a
// timeout or a short transfer.
class LibusbBulkTransport final : public LinkTransport {
 public:
  LibusbBulkTransport(libusb_device_handle* handle, std::uint8_t endpoint_in,
                      std::uint8_t endpoint_out, std::string device_id,
                      int interface_number = -1);
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
  [[nodiscard]] std::string last_error() const override {
    std::lock_guard lock(mutex_);
    return last_error_;
  }
  void set_rx_callback(std::function<void()> callback) override;
  [[nodiscard]] bool consume_write_blocked() noexcept override;

 private:
  void reader_loop();
  void wake_reader();
  void notify_rx();

  // Unused in the no-libusb stub build, where the transport is inert.
  [[maybe_unused]] libusb_device_handle* handle_;
  [[maybe_unused]] std::uint8_t endpoint_in_;
  [[maybe_unused]] std::uint8_t endpoint_out_;
  [[maybe_unused]] int interface_number_;
  std::string device_id_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> closed_{false};
  std::atomic<bool> failed_{false};
  std::atomic<bool> write_blocked_{false};
  std::atomic<std::uint64_t> rx_bytes_{0};
  std::atomic<std::uint64_t> tx_bytes_{0};
  std::atomic<std::uint64_t> bulk_errors_{0};
  [[maybe_unused]] std::atomic<int> consecutive_write_errors_{0};
  std::thread reader_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::uint8_t> rx_buffer_;
  std::string last_error_;
  std::mutex rx_cb_mutex_;
  std::function<void()> rx_callback_;
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
