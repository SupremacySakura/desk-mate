#include "DMBridge.h"

#include "deskmate/clock.hpp"
#include "deskmate/protocol.hpp"
#include "deskmate/session.hpp"

#include <libusb-1.0/libusb.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr std::uint16_t kHdcVendorId = 0x12D1;
constexpr std::uint16_t kHdcProductId = 0x5000;
constexpr std::uint16_t kAccessoryVendorId = 0x18D1;
constexpr std::uint16_t kAccessoryProductIdMin = 0x2D00;
constexpr std::uint16_t kAccessoryProductIdMax = 0x2D05;
constexpr unsigned kControlTimeoutMs = 3000;
constexpr unsigned kReadTimeoutMs = 1;
constexpr unsigned kReadThreadTimeoutMs = 25;
constexpr unsigned kWriteTimeoutMs = 20;
constexpr std::size_t kMaxRxBufferBytes = 256U * 1024U;
constexpr std::size_t kHostSchedulerMaxVideoUrbs = 8;
constexpr std::size_t kHostSchedulerMaxPendingVideoBytes = 2U * 1024U * 1024U;
constexpr std::uint64_t kHostSchedulerMaxUnsentFrameAgeNs = 100'000'000ULL;
/* 12 KiB payload + 32 B header stays under the device's 16 KiB accessory read
   buffer; larger packets are truncated by the driver. 64 fragments per frame
   (the protocol maximum) still covers a 768 KiB keyframe. */
constexpr std::size_t kVideoFragmentPayload = 12U * 1024U;
constexpr std::size_t kMaxPendingVideoFrames = 16;
/* The device shows a USB-accessory consent dialog the first time an accessory
   claims the interface. The protocol default of 2 s expires before anyone can
   tap it, so the host allows a full 20 s for the first ClientHello. */
constexpr std::uint64_t kHandshakeStageTimeoutNs = 20'000'000'000ULL;
constexpr auto kAoaSwitchCooldown = std::chrono::seconds(5);
constexpr auto kExpectedIdentityTtl = std::chrono::seconds(20);

bool is_accessory(std::uint16_t vendor_id, std::uint16_t product_id) {
  return vendor_id == kAccessoryVendorId &&
         product_id >= kAccessoryProductIdMin &&
         product_id <= kAccessoryProductIdMax;
}

bool is_hdc_device(std::uint16_t vendor_id, std::uint16_t product_id) {
  return vendor_id == kHdcVendorId && product_id == kHdcProductId;
}

std::string libusb_text(int rc) { return libusb_error_name(rc); }

class UsbByteIo final : public deskmate::protocol::ByteIo {
 public:
  UsbByteIo(libusb_device_handle *handle, std::uint8_t endpoint_in,
            std::uint8_t endpoint_out, std::atomic<std::uint64_t> *rx_bytes,
            std::atomic<std::uint64_t> *tx_bytes,
            std::atomic<std::uint64_t> *bulk_errors,
            std::uint64_t *write_timeouts,
            std::uint64_t *write_timeout_bytes,
            std::function<void()> on_rx_ready = nullptr)
      : handle_(handle),
        endpoint_in_(endpoint_in),
        endpoint_out_(endpoint_out),
        rx_bytes_(rx_bytes),
        tx_bytes_(tx_bytes),
        bulk_errors_(bulk_errors),
        write_timeouts_(write_timeouts),
        write_timeout_bytes_(write_timeout_bytes),
        on_rx_ready_(std::move(on_rx_ready)) {
    if (handle_ != nullptr) {
      rx_thread_ = std::thread([this] { rx_loop(); });
    }
  }

  ~UsbByteIo() override { stop_rx(); }

  deskmate::protocol::IoResult read(
      std::span<std::uint8_t> destination) override {
    if (handle_ != nullptr) {
      std::lock_guard lock(rx_mutex_);
      if (!rx_buffer_.empty()) {
        const std::size_t take = std::min(destination.size(), rx_buffer_.size());
        std::copy_n(rx_buffer_.begin(), take, destination.data());
        rx_buffer_.erase(rx_buffer_.begin(),
                         rx_buffer_.begin() + static_cast<std::ptrdiff_t>(take));
        return {deskmate::protocol::IoStatus::Ok, take};
      }
      if (rx_closed_) {
        return {deskmate::protocol::IoStatus::Closed, 0};
      }
      if (rx_error_) {
        return {deskmate::protocol::IoStatus::Error, 0};
      }
      return {deskmate::protocol::IoStatus::WouldBlock, 0};
    }

    int transferred = 0;
    const int rc = libusb_bulk_transfer(
        handle_, endpoint_in_, destination.data(),
        static_cast<int>(destination.size()), &transferred, kReadTimeoutMs);
    if (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_TIMEOUT) {
      if (transferred == 0) {
        return {deskmate::protocol::IoStatus::WouldBlock, 0};
      }
      rx_bytes_->fetch_add(static_cast<std::uint64_t>(transferred));
      return {deskmate::protocol::IoStatus::Ok,
              static_cast<std::size_t>(transferred)};
    }
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
      return {deskmate::protocol::IoStatus::Closed, 0};
    }
    bulk_errors_->fetch_add(1);
    return {deskmate::protocol::IoStatus::Error, 0};
  }

  deskmate::protocol::IoResult write(
      std::span<const std::uint8_t> source) override {
    int transferred = 0;
    const int rc = libusb_bulk_transfer(
        handle_, endpoint_out_,
        const_cast<unsigned char *>(source.data()),
        static_cast<int>(source.size()), &transferred, kWriteTimeoutMs);
    if (rc == LIBUSB_ERROR_TIMEOUT && write_timeouts_ != nullptr) {
      ++*write_timeouts_;
      if (*write_timeouts_ <= 30) {
        std::fprintf(stderr, "[DEBUG-dm-out] len=%zu transferred=%d\n",
                     source.size(), transferred);
      }
      if (transferred > 0) {
        *write_timeout_bytes_ += static_cast<std::uint64_t>(transferred);
      }
    }
    // A timed-out bulk write may have sent a prefix; retry only its suffix.
    if (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_TIMEOUT) {
      if (transferred == 0) {
        write_blocked_.store(true, std::memory_order_relaxed);
        return {deskmate::protocol::IoStatus::WouldBlock, 0};
      }
      tx_bytes_->fetch_add(static_cast<std::uint64_t>(transferred));
      return {deskmate::protocol::IoStatus::Ok,
              static_cast<std::size_t>(transferred)};
    }
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
      return {deskmate::protocol::IoStatus::Closed, 0};
    }
    bulk_errors_->fetch_add(1);
    return {deskmate::protocol::IoStatus::Error, 0};
  }

  bool consume_write_blocked() noexcept {
    return write_blocked_.exchange(false, std::memory_order_relaxed);
  }

  void cancel() override {
    if (on_rx_ready_) {
      on_rx_ready_();
    }
  }

 private:
  void stop_rx() {
    rx_stop_.store(true, std::memory_order_release);
    if (rx_thread_.joinable()) {
      rx_thread_.join();
    }
  }

  void rx_loop() {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
    std::array<std::uint8_t, 4096> chunk{};
    while (!rx_stop_.load(std::memory_order_acquire)) {
      int transferred = 0;
      const int rc = libusb_bulk_transfer(
          handle_, endpoint_in_, chunk.data(), static_cast<int>(chunk.size()),
          &transferred, kReadThreadTimeoutMs);
      if (rc == LIBUSB_SUCCESS || rc == LIBUSB_ERROR_TIMEOUT) {
        if (transferred > 0) {
          rx_bytes_->fetch_add(static_cast<std::uint64_t>(transferred));
          {
            std::lock_guard lock(rx_mutex_);
            const std::size_t count = static_cast<std::size_t>(transferred);
            if (rx_buffer_.size() + count > kMaxRxBufferBytes) {
              const std::size_t drop =
                  std::min(rx_buffer_.size(),
                           rx_buffer_.size() + count - kMaxRxBufferBytes);
              rx_buffer_.erase(rx_buffer_.begin(),
                               rx_buffer_.begin() + static_cast<std::ptrdiff_t>(drop));
            }
            rx_buffer_.insert(rx_buffer_.end(), chunk.begin(),
                              chunk.begin() + static_cast<std::ptrdiff_t>(count));
          }
          if (on_rx_ready_) {
            on_rx_ready_();
          }
        }
        continue;
      }
      if (rc == LIBUSB_ERROR_NO_DEVICE) {
        {
          std::lock_guard lock(rx_mutex_);
          rx_closed_ = true;
        }
        if (on_rx_ready_) {
          on_rx_ready_();
        }
        return;
      }
      if (!rx_stop_.load(std::memory_order_acquire)) {
        bulk_errors_->fetch_add(1);
        {
          std::lock_guard lock(rx_mutex_);
          rx_error_ = true;
        }
        if (on_rx_ready_) {
          on_rx_ready_();
        }
      }
      return;
    }
  }

  libusb_device_handle *handle_;
  std::uint8_t endpoint_in_;
  std::uint8_t endpoint_out_;
  std::atomic<std::uint64_t> *rx_bytes_;
  std::atomic<std::uint64_t> *tx_bytes_;
  std::atomic<std::uint64_t> *bulk_errors_;
  std::uint64_t *write_timeouts_;
  std::uint64_t *write_timeout_bytes_;
  std::function<void()> on_rx_ready_;
  std::atomic<bool> write_blocked_{false};
  std::atomic<bool> rx_stop_{false};
  std::thread rx_thread_;
  std::mutex rx_mutex_;
  std::deque<std::uint8_t> rx_buffer_;
  bool rx_closed_{false};
  bool rx_error_{false};
};

struct BulkEndpoints {
  int interface_number = -1;
  std::uint8_t endpoint_in = 0;
  std::uint8_t endpoint_out = 0;
};

bool find_bulk_endpoints(libusb_device *device, BulkEndpoints &out,
                         std::string &error) {
  libusb_config_descriptor *config = nullptr;
  int rc = libusb_get_active_config_descriptor(device, &config);
  if (rc != LIBUSB_SUCCESS) {
    rc = libusb_get_config_descriptor(device, 0, &config);
  }
  if (rc != LIBUSB_SUCCESS || config == nullptr) {
    error = "cannot read accessory configuration: " + libusb_text(rc);
    return false;
  }
  bool found = false;
  for (uint8_t index = 0; index < config->bNumInterfaces && !found; ++index) {
    const libusb_interface &iface = config->interface[index];
    for (int alt = 0; alt < iface.num_altsetting && !found; ++alt) {
      const libusb_interface_descriptor &descriptor = iface.altsetting[alt];
      int candidate_in = -1;
      int candidate_out = -1;
      for (uint8_t endpoint_index = 0;
           endpoint_index < descriptor.bNumEndpoints; ++endpoint_index) {
        const libusb_endpoint_descriptor &endpoint =
            descriptor.endpoint[endpoint_index];
        if ((endpoint.bmAttributes & 0x03U) != LIBUSB_TRANSFER_TYPE_BULK) {
          continue;
        }
        if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) != 0) {
          if (candidate_in < 0) {
            candidate_in = endpoint.bEndpointAddress;
          }
        } else if (candidate_out < 0) {
          candidate_out = endpoint.bEndpointAddress;
        }
      }
      if (candidate_in >= 0 && candidate_out >= 0) {
        out.interface_number = descriptor.bInterfaceNumber;
        out.endpoint_in = static_cast<std::uint8_t>(candidate_in);
        out.endpoint_out = static_cast<std::uint8_t>(candidate_out);
        found = true;
      }
    }
  }
  libusb_free_config_descriptor(config);
  if (!found) {
    error = "accessory interface has no paired bulk IN/OUT endpoints";
  }
  return found;
}

bool is_accessory_present_at(libusb_context *context, std::uint8_t bus,
                               std::uint8_t address) {
  if (context == nullptr || bus == 0 || address == 0) {
    return false;
  }
  libusb_device **devices = nullptr;
  const ssize_t count = libusb_get_device_list(context, &devices);
  if (count < 0) {
    return false;
  }
  bool found = false;
  for (ssize_t index = 0; index < count; ++index) {
    if (libusb_get_bus_number(devices[index]) != bus ||
        libusb_get_device_address(devices[index]) != address) {
      continue;
    }
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(devices[index], &descriptor) ==
            LIBUSB_SUCCESS &&
        is_accessory(descriptor.idVendor, descriptor.idProduct)) {
      found = true;
    }
    break;
  }
  libusb_free_device_list(devices, 1);
  return found;
}

bool send_aoa_string(libusb_device_handle *handle, std::uint16_t index,
                     const std::string &value, std::string &error) {
  std::vector<std::uint8_t> bytes(value.begin(), value.end());
  bytes.push_back(0);
  if (bytes.size() > 256) {
    error = "AOA string exceeds 256 bytes";
    return false;
  }
  const int rc = libusb_control_transfer(
      handle,
      LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
          LIBUSB_ENDPOINT_OUT,
      52, 0, index, bytes.data(), static_cast<std::uint16_t>(bytes.size()),
      kControlTimeoutMs);
  if (rc < 0) {
    error = "AOA SEND_STRING failed: " + libusb_text(rc);
    return false;
  }
  return true;
}

struct UsbDeviceIdentity {
  std::string id;
  std::string name;
  std::string manufacturer;
  std::string serial;
  std::string port_path;
  std::uint16_t vendor_id = 0;
  std::uint16_t product_id = 0;
  std::uint8_t bus = 0;
  std::uint8_t address = 0;
  bool in_accessory_mode = false;
};

std::string trim_ascii(std::string value) {
  auto keep = [](unsigned char ch) { return ch >= 0x20 && ch != 0x7F; };
  std::string cleaned;
  cleaned.reserve(value.size());
  for (unsigned char ch : value) {
    if (keep(ch)) {
      cleaned.push_back(static_cast<char>(ch));
    }
  }
  const auto start = cleaned.find_first_not_of(" \t");
  if (start == std::string::npos) {
    return "";
  }
  const auto end = cleaned.find_last_not_of(" \t");
  return cleaned.substr(start, end - start + 1);
}

std::string read_usb_string(libusb_device_handle *handle, std::uint8_t index) {
  if (handle == nullptr || index == 0) {
    return "";
  }
  unsigned char buffer[256] = {};
  const int rc = libusb_get_string_descriptor_ascii(
      handle, index, buffer, static_cast<int>(sizeof(buffer) - 1));
  if (rc <= 0) {
    return "";
  }
  return trim_ascii(std::string(reinterpret_cast<const char *>(buffer),
                                static_cast<std::size_t>(rc)));
}

bool is_generic_device_name(const std::string &name) {
  return name.empty() || name == "Android" || name == "Accessory" ||
         name == "AOA" || name == "DeskMate" ||
         name == "DeskMate Extended Display" ||
         name == "HarmonyOS 设备" || name == "DeskMate 设备";
}

std::string format_usb_fallback_id(std::uint16_t vendor_id,
                                   std::uint16_t product_id) {
  char buf[32] = {};
  std::snprintf(buf, sizeof(buf), "usb:%04x:%04x", vendor_id, product_id);
  return std::string(buf);
}

std::string format_port_path(libusb_device *device, std::uint8_t bus) {
  if (device == nullptr) {
    return "";
  }
  std::array<std::uint8_t, 8> ports{};
  const int count = libusb_get_port_numbers(
      device, ports.data(), static_cast<int>(ports.size()));
  if (count <= 0) {
    return "";
  }
  std::ostringstream out;
  out << static_cast<unsigned>(bus) << ":";
  for (int index = 0; index < count; ++index) {
    if (index > 0) {
      out << ".";
    }
    out << static_cast<unsigned>(ports[static_cast<std::size_t>(index)]);
  }
  return out.str();
}

}  // namespace

struct DeviceLink;
class HostBoundaryImpl;

struct DeviceLink : public std::enable_shared_from_this<DeviceLink> {
  struct PendingVideo {
    std::vector<std::uint8_t> bytes;
    std::uint64_t pts_us{};
    bool keyframe{};
  };

  DMBridge *bridge = nullptr;
  std::uint64_t link_id = 0;

  libusb_device_handle *handle = nullptr;
  int interface_number = -1;
  bool interface_claimed = false;

  std::unique_ptr<HostBoundaryImpl> boundary;
  std::unique_ptr<UsbByteIo> io;
  std::unique_ptr<deskmate::protocol::SteadyClock> clock;
  std::unique_ptr<deskmate::protocol::HostSession> session;

  std::mutex session_mutex;
  std::mutex video_mutex;
  std::condition_variable wake_cv;
  bool rx_wakeup = false;
  std::deque<PendingVideo> video_queue;
  std::atomic<std::uint64_t> frames_dropped_backlog{0};

  std::mutex input_mutex;
  std::deque<deskmate::protocol::InputEvent> pending_inputs;

  std::mutex snapshot_mutex;
  std::string cached_state{"idle"};
  std::string cached_negotiated{"none"};
  bool cached_input_enabled{false};
  UsbDeviceIdentity active_device{};
  bool active_device_valid{false};
  std::uint8_t active_bus{0};
  std::uint8_t active_address{0};

  std::thread worker;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> disconnect_requested{false};
  std::atomic<bool> running{false};
  std::atomic<bool> needs_keyframe{false};
  std::atomic<std::uint32_t> keyframe_request_id{0};
  std::atomic<std::int32_t> negotiated_width{0};
  std::atomic<std::int32_t> negotiated_height{0};
  std::atomic<std::int32_t> negotiated_refresh_hz{0};
  std::atomic<std::int32_t> negotiated_codec{0};

  std::atomic<int32_t> last_dispatched_capture_intent{0};
  int32_t pending_capture_intent = -1;
  int32_t pending_input_intent = -1;

  std::atomic<std::uint64_t> frames_submitted{0};
  std::atomic<std::uint64_t> frames_sent{0};
  std::atomic<std::uint64_t> frames_rejected{0};
  std::atomic<std::uint64_t> input_events{0};
  std::atomic<std::uint64_t> reconnects{0};
  std::atomic<std::uint64_t> rx_bytes{0};
  std::atomic<std::uint64_t> tx_bytes{0};
  std::atomic<std::uint64_t> bulk_errors{0};

  std::uint64_t perf_idr_wait = 0;
  std::uint64_t perf_invalid = 0;
  std::uint64_t perf_other_rejected = 0;
  std::uint64_t perf_inactive = 0;
  std::uint64_t perf_write_timeouts = 0;
  std::uint64_t perf_write_timeout_bytes = 0;

  std::mutex error_mutex;
  std::string last_error;

  void log(const std::string &message);
  void set_error(const std::string &message) {
    {
      std::lock_guard lock(error_mutex);
      last_error = message;
    }
    log("[link " + std::to_string(link_id) + "] error: " + message);
  }
  void clear_error() {
    std::lock_guard lock(error_mutex);
    last_error.clear();
  }
};

class HostBoundaryImpl final : public deskmate::protocol::HostBoundary {
 public:
  explicit HostBoundaryImpl(DeviceLink *link) : link_(link) {}

  void set_capture_encoding_intent(bool enabled) override {
    link_->pending_capture_intent = enabled ? 1 : 0;
  }
  void set_input_injection_intent(bool enabled) override {
    link_->pending_input_intent = enabled ? 1 : 0;
  }

 private:
  DeviceLink *link_;
};

struct DMBridge {
  struct TimedIdentity {
    UsbDeviceIdentity identity;
    std::chrono::steady_clock::time_point expires_at;
  };

  DMBridgeConfig config{};
  libusb_context *context = nullptr;

  std::mutex links_mutex;
  std::unordered_map<std::uint64_t, std::shared_ptr<DeviceLink>> links;
  std::vector<std::shared_ptr<DeviceLink>> finished_links;
  std::uint64_t next_link_id{1};

  std::thread discovery_worker;
  std::mutex discovery_mutex;
  std::condition_variable discovery_cv;
  bool discovery_wakeup{false};
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> auto_match_resolution{false};

  std::mutex device_policy_mutex;
  std::unordered_set<std::string> blocked_device_ids;
  std::unordered_map<std::string, UsbDeviceIdentity> hdc_identity_cache;
  std::unordered_map<std::string, UsbDeviceIdentity> hdc_identity_by_port;
  UsbDeviceIdentity last_hdc_identity{};
  bool has_last_hdc_identity{false};
  std::chrono::steady_clock::time_point last_hdc_seen_at{};
  std::unordered_map<std::string, std::chrono::steady_clock::time_point>
      aoa_switch_cooldown;
  std::unordered_map<std::string, TimedIdentity> expected_identity_by_port;

  std::mutex snapshot_mutex;
  bool blocked_device_present{false};
  std::string blocked_device_name;
  std::vector<std::string> blocked_device_names;

  std::mutex error_mutex;
  std::string discovery_error;

  DMLogCallback log_callback = nullptr;
  void *log_context = nullptr;
  DMIntentCallback intent_callback = nullptr;
  void *intent_context = nullptr;
  DMInputCallback input_callback = nullptr;
  void *input_context = nullptr;
  DMLinkIntentCallback link_intent_callback = nullptr;
  void *link_intent_context = nullptr;
  DMLinkInputCallback link_input_callback = nullptr;
  void *link_input_context = nullptr;

  const bool perf_trace = std::getenv("DESKMATE_PERF_TRACE") != nullptr;

  void log(const std::string &message) {
    if (log_callback != nullptr) {
      log_callback(message.c_str(), log_context);
    }
  }

  void set_discovery_error(const std::string &message) {
    bool changed = false;
    {
      std::lock_guard lock(error_mutex);
      changed = (discovery_error != message);
      discovery_error = message;
    }
    if (changed && !message.empty()) {
      log("error: " + message);
    }
  }

  void clear_discovery_error() {
    std::lock_guard lock(error_mutex);
    discovery_error.clear();
  }
};

void DeviceLink::log(const std::string &message) {
  if (bridge != nullptr) {
    bridge->log(message);
  }
}

namespace {

std::uint32_t random_nonzero_u32() {
  std::uint32_t value = 0;
  while (value == 0) {
    arc4random_buf(&value, sizeof(value));
  }
  return value;
}

std::string state_name(deskmate::protocol::SessionState state) {
  return std::string(deskmate::protocol::session_state_name(state));
}

std::string json_escape(const std::string &value) {
  std::string out;
  out.reserve(value.size() + 8);
  for (const char character : value) {
    switch (character) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (static_cast<unsigned char>(character) < 0x20) {
          char buffer[8] = {};
          std::snprintf(buffer, sizeof(buffer), "\\u%04x",
                        static_cast<unsigned char>(character));
          out += buffer;
        } else {
          out += character;
        }
        break;
    }
  }
  return out;
}

std::vector<std::shared_ptr<DeviceLink>> snapshot_links(DMBridge *bridge) {
  std::vector<std::shared_ptr<DeviceLink>> out;
  if (bridge == nullptr) {
    return out;
  }
  std::lock_guard lock(bridge->links_mutex);
  out.reserve(bridge->links.size());
  for (const auto &pair : bridge->links) {
    out.push_back(pair.second);
  }
  std::sort(out.begin(), out.end(),
            [](const std::shared_ptr<DeviceLink> &a,
               const std::shared_ptr<DeviceLink> &b) {
              return a->link_id < b->link_id;
            });
  return out;
}

std::shared_ptr<DeviceLink> find_link(DMBridge *bridge, std::uint64_t link_id) {
  if (bridge == nullptr || link_id == 0) {
    return nullptr;
  }
  std::lock_guard lock(bridge->links_mutex);
  auto it = bridge->links.find(link_id);
  return it != bridge->links.end() ? it->second : nullptr;
}

std::shared_ptr<DeviceLink> first_link(DMBridge *bridge) {
  if (bridge == nullptr) {
    return nullptr;
  }
  std::lock_guard lock(bridge->links_mutex);
  std::shared_ptr<DeviceLink> best;
  for (const auto &pair : bridge->links) {
    if (best == nullptr || pair.first < best->link_id) {
      best = pair.second;
    }
  }
  return best;
}

void dispatch_intent(DMBridge *bridge, std::uint64_t link_id,
                     int32_t capture, int32_t input) {
  if (bridge == nullptr) {
    return;
  }
  if (bridge->link_intent_callback != nullptr) {
    bridge->link_intent_callback(link_id, capture, input,
                                 bridge->link_intent_context);
  } else if (bridge->intent_callback != nullptr) {
    bridge->intent_callback(capture, input, bridge->intent_context);
  }
}

void dispatch_input(DMBridge *bridge, std::uint64_t link_id) {
  if (bridge == nullptr) {
    return;
  }
  if (bridge->link_input_callback != nullptr) {
    bridge->link_input_callback(link_id, bridge->link_input_context);
  } else if (bridge->input_callback != nullptr) {
    bridge->input_callback(bridge->input_context);
  }
}

void resolve_device_identity(DMBridge *bridge, UsbDeviceIdentity &identity) {
  if (!identity.in_accessory_mode) {
    if (!identity.serial.empty()) {
      identity.id = "usb:" + identity.serial;
    } else if (!identity.port_path.empty()) {
      identity.id = "usb:port:" + identity.port_path;
    } else {
      identity.id =
          format_usb_fallback_id(identity.vendor_id, identity.product_id);
    }
    if (is_generic_device_name(identity.name)) {
      identity.name = !identity.manufacturer.empty()
                          ? (identity.manufacturer + " 设备")
                          : "HarmonyOS 设备";
    }
    if (bridge != nullptr) {
      std::lock_guard lock(bridge->device_policy_mutex);
      if (!identity.serial.empty()) {
        bridge->hdc_identity_cache[identity.id] = identity;
      }
      if (!identity.port_path.empty()) {
        bridge->hdc_identity_by_port[identity.port_path] = identity;
      }
      bridge->last_hdc_identity = identity;
      bridge->has_last_hdc_identity = true;
      bridge->last_hdc_seen_at = std::chrono::steady_clock::now();
    }
    return;
  }

  if (bridge != nullptr) {
    std::lock_guard lock(bridge->device_policy_mutex);
    if (!identity.serial.empty()) {
      auto it = bridge->hdc_identity_cache.find("usb:" + identity.serial);
      if (it != bridge->hdc_identity_cache.end()) {
        identity.id = it->second.id;
        if (is_generic_device_name(identity.name)) {
          identity.name = it->second.name;
        }
        if (identity.manufacturer.empty()) {
          identity.manufacturer = it->second.manufacturer;
        }
        return;
      }
    }
    if (!identity.port_path.empty()) {
      auto it = bridge->hdc_identity_by_port.find(identity.port_path);
      if (it != bridge->hdc_identity_by_port.end()) {
        identity.id = it->second.id;
        if (identity.serial.empty()) {
          identity.serial = it->second.serial;
        }
        if (is_generic_device_name(identity.name)) {
          identity.name = it->second.name;
        }
        if (identity.manufacturer.empty()) {
          identity.manufacturer = it->second.manufacturer;
        }
        return;
      }
    }
    if (identity.serial.empty() && bridge->has_last_hdc_identity &&
        (std::chrono::steady_clock::now() - bridge->last_hdc_seen_at) <=
            std::chrono::seconds(20) &&
        (identity.port_path.empty() ||
         bridge->last_hdc_identity.port_path == identity.port_path)) {
      identity.id = bridge->last_hdc_identity.id;
      identity.serial = bridge->last_hdc_identity.serial;
      if (is_generic_device_name(identity.name)) {
        identity.name = bridge->last_hdc_identity.name;
      }
      if (identity.manufacturer.empty()) {
        identity.manufacturer = bridge->last_hdc_identity.manufacturer;
      }
      return;
    }
  }

  if (!identity.serial.empty()) {
    identity.id = "usb:" + identity.serial;
  } else if (!identity.port_path.empty()) {
    identity.id = "usb:port:" + identity.port_path;
  } else {
    identity.id =
        format_usb_fallback_id(identity.vendor_id, identity.product_id);
  }
  if (is_generic_device_name(identity.name)) {
    identity.name = !identity.manufacturer.empty()
                        ? (identity.manufacturer + " 设备")
                        : "DeskMate 设备";
  }
}

void inspect_usb_device(DMBridge *bridge, libusb_device *device,
                        const libusb_device_descriptor &desc,
                        libusb_device_handle *existing_handle, bool try_open,
                        UsbDeviceIdentity &identity) {
  identity = {};
  identity.vendor_id = desc.idVendor;
  identity.product_id = desc.idProduct;
  identity.bus = libusb_get_bus_number(device);
  identity.address = libusb_get_device_address(device);
  identity.port_path = format_port_path(device, identity.bus);
  identity.in_accessory_mode = is_accessory(desc.idVendor, desc.idProduct);

  if (bridge != nullptr) {
    const auto current_links = snapshot_links(bridge);
    for (const auto &link : current_links) {
      std::lock_guard snap_lock(link->snapshot_mutex);
      if (link->active_device_valid && link->active_bus == identity.bus &&
          link->active_address == identity.address) {
        identity = link->active_device;
        return;
      }
    }
  }

  libusb_device_handle *opened = nullptr;
  libusb_device_handle *handle = existing_handle;
  if (handle == nullptr && try_open) {
    if (libusb_open(device, &opened) == LIBUSB_SUCCESS && opened != nullptr) {
      handle = opened;
    }
  }
  if (handle != nullptr) {
    identity.manufacturer = read_usb_string(handle, desc.iManufacturer);
    identity.name = read_usb_string(handle, desc.iProduct);
    identity.serial = read_usb_string(handle, desc.iSerialNumber);
  }
  if (opened != nullptr) {
    libusb_close(opened);
  }
  resolve_device_identity(bridge, identity);
}

bool is_device_blocked(DMBridge *bridge, const std::string &device_id) {
  if (bridge == nullptr || device_id.empty()) {
    return false;
  }
  std::lock_guard lock(bridge->device_policy_mutex);
  return bridge->blocked_device_ids.count(device_id) > 0;
}

void merge_expected_identity(UsbDeviceIdentity &target,
                             const UsbDeviceIdentity &expected) {
  if (expected.id.empty()) {
    return;
  }
  if (target.serial.empty() && !expected.serial.empty()) {
    target.id = expected.id;
    target.serial = expected.serial;
  } else if (target.id.rfind("usb:18d1:", 0) == 0 &&
             expected.id.rfind("usb:port:", 0) == 0) {
    target.id = expected.id;
  }
  if (target.port_path.empty() && !expected.port_path.empty()) {
    target.port_path = expected.port_path;
  }
  if (is_generic_device_name(target.name) && !expected.name.empty()) {
    target.name = expected.name;
  }
  if (target.manufacturer.empty() && !expected.manufacturer.empty()) {
    target.manufacturer = expected.manufacturer;
  }
}

bool trigger_aoa_switch(DMBridge *bridge, libusb_device *hdc_device,
                        const UsbDeviceIdentity &identity,
                        std::string &error) {
  libusb_device_handle *handle = nullptr;
  const int open_rc = libusb_open(hdc_device, &handle);
  if (open_rc != LIBUSB_SUCCESS || handle == nullptr) {
    error = "libusb_open failed: " + libusb_text(open_rc);
    return false;
  }

  std::uint8_t protocol_bytes[2] = {0, 0};
  const int protocol_rc = libusb_control_transfer(
      handle,
      LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
          LIBUSB_ENDPOINT_IN,
      51, 0, 0, protocol_bytes, sizeof(protocol_bytes), kControlTimeoutMs);
  if (protocol_rc < 0) {
    error = "AOA GET_PROTOCOL failed: " + libusb_text(protocol_rc) +
            " (device may not implement AOA)";
    libusb_close(handle);
    return false;
  }
  const std::uint16_t protocol = static_cast<std::uint16_t>(protocol_bytes[0]) |
                                 static_cast<std::uint16_t>(protocol_bytes[1])
                                     << 8;
  if (protocol < 1) {
    error = "AOA GET_PROTOCOL returned version 0";
    libusb_close(handle);
    return false;
  }

  const char *strings[4] = {"DeskMate", "DeskMate Extended Display",
                            "Wired extended display for HarmonyOS devices",
                            "1.0"};
  for (std::uint16_t index = 0; index < 4; ++index) {
    if (!send_aoa_string(handle, index, strings[index], error)) {
      libusb_close(handle);
      return false;
    }
  }
  std::uint8_t no_data = 0;
  const int start_rc = libusb_control_transfer(
      handle,
      LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE |
          LIBUSB_ENDPOINT_OUT,
      53, 0, 0, &no_data, 0, kControlTimeoutMs);
  libusb_close(handle);
  if (start_rc < 0) {
    error = "AOA START failed: " + libusb_text(start_rc);
    return false;
  }

  if (bridge != nullptr && !identity.port_path.empty()) {
    std::lock_guard lock(bridge->device_policy_mutex);
    bridge->expected_identity_by_port[identity.port_path] = {
        identity, std::chrono::steady_clock::now() + kExpectedIdentityTtl};
  }
  return true;
}

void start_session(const std::shared_ptr<DeviceLink> &link) {
  DMBridge *bridge = link->bridge;
  deskmate::protocol::HostSessionConfig session_config;
  session_config.video_stream_id = random_nonzero_u32();
  /* A fresh config id per session: reusing 1 makes a stale device session
     answer with the previous attempt's StreamReady/StreamCommitted. */
  session_config.initial_config_id = (random_nonzero_u32() & 0x7FFFFFFFU) | 1U;
  session_config.limits.stage_timeout_ns = kHandshakeStageTimeoutNs;
  session_config.scheduler.max_video_urbs = kHostSchedulerMaxVideoUrbs;
  session_config.scheduler.max_pending_video_bytes =
      kHostSchedulerMaxPendingVideoBytes;
  session_config.scheduler.max_oldest_unsent_frame_age_ns =
      kHostSchedulerMaxUnsentFrameAgeNs;
  if (bridge->perf_trace) {
    const char *frame_age_ms = std::getenv("DESKMATE_PERF_FRAME_AGE_MS");
    if (frame_age_ms != nullptr) {
      const long parsed = std::strtol(frame_age_ms, nullptr, 10);
      if (parsed > 0) {
        session_config.scheduler.max_oldest_unsent_frame_age_ns =
            static_cast<std::uint64_t>(parsed) * 1'000'000ULL;
      }
    }
    std::fprintf(stderr, "[DEBUG-dm-fps] link=%llu frame_age_limit_ns=%llu\n",
                 static_cast<unsigned long long>(link->link_id),
                 static_cast<unsigned long long>(
                     session_config.scheduler.max_oldest_unsent_frame_age_ns));
  }
  session_config.limits.video_fragment_payload = kVideoFragmentPayload;
  if (bridge->config.prefer_hevc != 0) {
    session_config.capabilities.encoders = {
        deskmate::protocol::VideoEncoder::Hevc,
        deskmate::protocol::VideoEncoder::H264};
  } else {
    session_config.capabilities.encoders = {
        deskmate::protocol::VideoEncoder::H264};
  }
  session_config.capabilities.pixel_formats = {
      deskmate::protocol::PixelFormat::Nv12};
  const auto width = static_cast<std::uint16_t>(bridge->config.width);
  const auto height = static_cast<std::uint16_t>(bridge->config.height);
  const auto target_hz = static_cast<std::uint16_t>(bridge->config.refresh_hz);
  session_config.capabilities.resolutions.push_back(
      deskmate::protocol::DisplayResolution{width, height, target_hz});
  if (target_hz > 90) {
    session_config.capabilities.resolutions.push_back(
        deskmate::protocol::DisplayResolution{width, height, 90});
  }
  if (target_hz > 60) {
    session_config.capabilities.resolutions.push_back(
        deskmate::protocol::DisplayResolution{width, height, 60});
  }

  session_config.accept_device_resolution =
      bridge->auto_match_resolution.load();
  auto session = std::make_unique<deskmate::protocol::HostSession>(
      *link->io, *link->clock, session_config, link->boundary.get());
  session->start();
  std::lock_guard lock(link->session_mutex);
  link->negotiated_width.store(0);
  link->negotiated_height.store(0);
  link->negotiated_refresh_hz.store(0);
  link->negotiated_codec.store(0);
  link->keyframe_request_id.store(0);
  link->needs_keyframe.store(false);
  link->pending_capture_intent = -1;
  link->pending_input_intent = -1;
  link->session = std::move(session);
}

void wait_stop_or_link(const std::shared_ptr<DeviceLink> &link,
                       std::chrono::milliseconds duration) {
  std::unique_lock video_lock(link->video_mutex);
  link->wake_cv.wait_for(video_lock, duration, [&link] {
    return link->stop_requested.load() || link->disconnect_requested.load() ||
           link->bridge->stop_requested.load();
  });
}

void teardown_link(const std::shared_ptr<DeviceLink> &link) {
  {
    std::lock_guard lock(link->session_mutex);
    link->needs_keyframe.store(false);
    link->keyframe_request_id.store(0);
    link->negotiated_width.store(0);
    link->negotiated_height.store(0);
    link->negotiated_refresh_hz.store(0);
    link->negotiated_codec.store(0);
    link->pending_capture_intent = -1;
    link->pending_input_intent = -1;
    link->session.reset();
  }
  if (link->last_dispatched_capture_intent.exchange(0) != 0) {
    dispatch_intent(link->bridge, link->link_id, 0, 0);
  }
  {
    std::lock_guard lock(link->input_mutex);
    link->pending_inputs.clear();
  }
  {
    std::lock_guard lock(link->video_mutex);
    link->video_queue.clear();
  }
  {
    std::lock_guard lock(link->snapshot_mutex);
    link->cached_state = "idle";
    link->cached_negotiated = "none";
    link->cached_input_enabled = false;
    link->active_device_valid = false;
    link->active_bus = 0;
    link->active_address = 0;
    link->active_device = {};
  }
  link->io.reset();
  link->clock.reset();
  if (link->handle != nullptr) {
    if (link->interface_claimed) {
      libusb_release_interface(link->handle, link->interface_number);
      link->interface_claimed = false;
    }
    libusb_close(link->handle);
    link->handle = nullptr;
  }
}

void link_worker(std::shared_ptr<DeviceLink> link) {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
  DMBridge *bridge = link->bridge;
  int handshake_failures = 0;
  std::uint64_t link_rx_start = link->rx_bytes.load();
  link->running.store(true);
  link->log("usb link up; waiting for the tablet to allow the USB accessory and send ClientHello");

  while (!link->stop_requested.load() && !bridge->stop_requested.load()) {
    std::string failure;
    bool stopped = false;
    bool disconnected_by_policy = false;
    auto next_status_log = std::chrono::steady_clock::now();
    auto next_perf_log = next_status_log;

    for (;;) {
      if (link->stop_requested.load() || bridge->stop_requested.load()) {
        stopped = true;
        break;
      }
      if (link->disconnect_requested.exchange(false)) {
        disconnected_by_policy = true;
        break;
      }
      bool terminal = false;
      bool has_pending_output = false;
      bool write_blocked = false;
      bool got_new_input = false;
      int32_t dispatch_capture_intent = -1;
      int32_t dispatch_input_intent = -1;
      {
        std::lock_guard lock(link->session_mutex);
        if (link->session == nullptr) {
          terminal = true;
        } else {
          std::deque<DeviceLink::PendingVideo> batch;
          {
            std::lock_guard video_lock(link->video_mutex);
            batch.swap(link->video_queue);
            link->rx_wakeup = false;
          }
          for (const DeviceLink::PendingVideo &frame : batch) {
            const auto status = link->session->send_video_frame(
                std::span<const std::uint8_t>(frame.bytes), frame.pts_us,
                frame.keyframe);
            if (bridge->perf_trace) {
              using Status = deskmate::protocol::VideoSendStatus;
              switch (status) {
                case Status::Sent: break;
                case Status::NotStreaming:
                case Status::Paused: ++link->perf_inactive; break;
                case Status::IdrRequired: ++link->perf_idr_wait; break;
                case Status::InvalidArgument:
                case Status::EmptyPayload: ++link->perf_invalid; break;
                default: ++link->perf_other_rejected; break;
              }
            }
            if (status == deskmate::protocol::VideoSendStatus::Sent) {
              link->frames_sent.fetch_add(1);
            } else if (status !=
                           deskmate::protocol::VideoSendStatus::NotStreaming &&
                       status != deskmate::protocol::VideoSendStatus::Paused) {
              link->frames_rejected.fetch_add(1);
            }
          }
          link->session->pump();
          terminal = link->session->is_terminal();
          has_pending_output =
              !terminal && link->session->has_pending_output();
          if (link->io != nullptr) {
            write_blocked = link->io->consume_write_blocked();
          }
          auto input_batch = link->session->take_input_events();
          if (!input_batch.empty()) {
            std::lock_guard input_lock(link->input_mutex);
            for (const auto &event : input_batch) {
              if (link->pending_inputs.size() >= 512) {
                link->pending_inputs.pop_front();
              }
              link->pending_inputs.push_back(event);
            }
            got_new_input = true;
          }
          if (!terminal) {
            if (const auto &idr = link->session->pending_idr_request();
                idr.has_value()) {
              link->needs_keyframe.store(true);
              link->keyframe_request_id.store(idr->recovery_id);
            } else {
              link->needs_keyframe.store(false);
              link->keyframe_request_id.store(0);
            }
            std::string snap_state = state_name(link->session->state());
            std::string snap_negotiated = "none";
            if (const auto &stream = link->session->negotiated_stream();
                stream.has_value()) {
              link->negotiated_width.store(
                  static_cast<std::int32_t>(stream->profile.resolution.width));
              link->negotiated_height.store(
                  static_cast<std::int32_t>(stream->profile.resolution.height));
              link->negotiated_refresh_hz.store(
                  static_cast<std::int32_t>(stream->profile.resolution.refresh_hz));
              link->negotiated_codec.store(
                  stream->profile.encoder == deskmate::protocol::VideoEncoder::Hevc
                      ? 2
                      : 1);
              std::ostringstream summary;
              summary << (stream->profile.encoder ==
                                  deskmate::protocol::VideoEncoder::Hevc
                              ? "hevc"
                              : "h264")
                      << " " << stream->profile.resolution.width << "x"
                      << stream->profile.resolution.height << "@"
                      << stream->profile.resolution.refresh_hz;
              snap_negotiated = summary.str();
            }
            const bool snap_input = link->session->input_injection_intent();
            {
              std::lock_guard snap_lock(link->snapshot_mutex);
              link->cached_state = std::move(snap_state);
              link->cached_negotiated = std::move(snap_negotiated);
              link->cached_input_enabled = snap_input;
            }
          } else {
            link->needs_keyframe.store(false);
            link->keyframe_request_id.store(0);
          }
          if (terminal && link->session->failure().has_value()) {
            failure = std::string(deskmate::protocol::session_failure_name(
                *link->session->failure()));
          }
          dispatch_capture_intent = link->pending_capture_intent;
          dispatch_input_intent = link->pending_input_intent;
          link->pending_capture_intent = -1;
          link->pending_input_intent = -1;
        }
      }
      if (dispatch_capture_intent >= 0 || dispatch_input_intent >= 0) {
        if (dispatch_capture_intent >= 0) {
          link->last_dispatched_capture_intent.store(dispatch_capture_intent);
        }
        dispatch_intent(bridge, link->link_id, dispatch_capture_intent,
                        dispatch_input_intent);
      }
      if (got_new_input) {
        dispatch_input(bridge, link->link_id);
      }
      const auto now = std::chrono::steady_clock::now();
      if (bridge->perf_trace && (now >= next_perf_log || terminal)) {
        next_perf_log = now + std::chrono::seconds(1);
        const auto timestamp_ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count();
        std::fprintf(
            stderr,
            "[DEBUG-dm-fps] link=%llu wall_ms=%lld submitted=%llu queued=%llu "
            "idr_wait=%llu invalid=%llu other=%llu inactive=%llu backlog=%llu "
            "usb_bytes=%llu usb_timeouts=%llu usb_timeout_bytes=%llu bulk_errors=%llu terminal=%d\n",
            static_cast<unsigned long long>(link->link_id),
            static_cast<long long>(timestamp_ms),
            static_cast<unsigned long long>(link->frames_submitted.load()),
            static_cast<unsigned long long>(link->frames_sent.load()),
            static_cast<unsigned long long>(link->perf_idr_wait),
            static_cast<unsigned long long>(link->perf_invalid),
            static_cast<unsigned long long>(link->perf_other_rejected),
            static_cast<unsigned long long>(link->perf_inactive),
            static_cast<unsigned long long>(link->frames_dropped_backlog.load()),
            static_cast<unsigned long long>(link->tx_bytes.load()),
            static_cast<unsigned long long>(link->perf_write_timeouts),
            static_cast<unsigned long long>(link->perf_write_timeout_bytes),
            static_cast<unsigned long long>(link->bulk_errors.load()), terminal);
      }
      if (terminal) {
        break;
      }

      if (now >= next_status_log) {
        next_status_log = now + std::chrono::seconds(5);
        std::string state;
        std::string negotiated;
        {
          std::lock_guard lock(link->session_mutex);
          if (link->session != nullptr) {
            state = state_name(link->session->state());
            if (const auto &stream = link->session->negotiated_stream();
                stream.has_value()) {
              std::ostringstream summary;
              summary << (stream->profile.encoder ==
                                  deskmate::protocol::VideoEncoder::Hevc
                              ? "hevc"
                              : "h264")
                      << " " << stream->profile.resolution.width << "x"
                      << stream->profile.resolution.height << "@"
                      << stream->profile.resolution.refresh_hz;
              negotiated = summary.str();
            }
          }
        }
        if (state == "streaming") {
          handshake_failures = 0;
          link->clear_error();
        }
        std::string line =
            "[link " + std::to_string(link->link_id) + "] state=" + state +
            " rx=" + std::to_string(link->rx_bytes.load()) +
            "B tx=" + std::to_string(link->tx_bytes.load()) +
            "B frames=" + std::to_string(link->frames_sent.load()) +
            " bulkErrors=" + std::to_string(link->bulk_errors.load());
        if (!negotiated.empty()) {
          line += " profile=" + negotiated;
        }
        if (state == "awaiting_client_hello") {
          line += " (waiting for the device to allow the USB accessory, attempt " +
                  std::to_string(handshake_failures + 1) + ")";
        }
        link->log(line);
      }
      if (!has_pending_output || write_blocked) {
        const auto wait_duration =
            has_pending_output ? std::chrono::milliseconds(1)
                               : std::chrono::milliseconds(5);
        std::unique_lock video_lock(link->video_mutex);
        link->wake_cv.wait_for(video_lock, wait_duration, [&link] {
          return link->stop_requested.load() ||
                 link->disconnect_requested.load() ||
                 link->bridge->stop_requested.load() ||
                 !link->video_queue.empty() || link->rx_wakeup;
        });
      }
    }

    if (stopped) {
      break;
    }
    if (disconnected_by_policy) {
      link->clear_error();
      link->log("[link " + std::to_string(link->link_id) +
                "] disconnected device by host policy");
      break;
    }

    const bool device_answered = link->rx_bytes.load() > link_rx_start;
    const bool handshake_retry =
        (failure == "hello_timeout" || failure == "ready_timeout") &&
        link->handle != nullptr && !device_answered;
    if (handshake_retry) {
      std::uint8_t bus = 0;
      std::uint8_t address = 0;
      {
        std::lock_guard snap_lock(link->snapshot_mutex);
        bus = link->active_bus;
        address = link->active_address;
      }
      if (!is_accessory_present_at(bridge->context, bus, address)) {
        link->clear_error();
        link->log("[link " + std::to_string(link->link_id) +
                  "] accessory detached during handshake");
        break;
      }
      ++handshake_failures;
      link->reconnects.fetch_add(1);
      link->set_error(failure +
                      " (retrying the session, device has not answered)");
      link->log("[link " + std::to_string(link->link_id) +
                "] handshake timed out; retrying the session on the same link");
      start_session(link);
      wait_stop_or_link(link, std::chrono::milliseconds(300));
      continue;
    }

    link->set_error(failure.empty() ? "session terminated" : failure);
    break;
  }

  link->running.store(false);
  teardown_link(link);

  if (!bridge->stop_requested.load()) {
    {
      std::lock_guard lock(bridge->links_mutex);
      bridge->links.erase(link->link_id);
      bridge->finished_links.push_back(link);
    }
    {
      std::lock_guard lock(bridge->discovery_mutex);
      bridge->discovery_wakeup = true;
    }
    bridge->discovery_cv.notify_one();
  }
}

void reap_finished_links(DMBridge *bridge) {
  std::vector<std::shared_ptr<DeviceLink>> finished;
  {
    std::lock_guard lock(bridge->links_mutex);
    if (bridge->finished_links.empty()) {
      return;
    }
    finished.swap(bridge->finished_links);
  }
  for (const auto &link : finished) {
    if (link->worker.joinable() &&
        link->worker.get_id() != std::this_thread::get_id()) {
      link->worker.join();
    }
  }
}

void attach_accessory_link(DMBridge *bridge, libusb_device *device,
                           const libusb_device_descriptor &desc,
                           UsbDeviceIdentity identity) {
  {
    std::lock_guard lock(bridge->device_policy_mutex);
    if (!identity.port_path.empty()) {
      auto it = bridge->expected_identity_by_port.find(identity.port_path);
      if (it != bridge->expected_identity_by_port.end()) {
        if (std::chrono::steady_clock::now() <= it->second.expires_at) {
          merge_expected_identity(identity, it->second.identity);
        } else {
          bridge->expected_identity_by_port.erase(it);
        }
      }
    }
  }

  if (is_device_blocked(bridge, identity.id)) {
    return;
  }

  {
    const auto active = snapshot_links(bridge);
    for (const auto &existing : active) {
      std::lock_guard snap_lock(existing->snapshot_mutex);
      if (existing->active_device_valid &&
          ((existing->active_bus == identity.bus &&
            existing->active_address == identity.address) ||
           (!identity.id.empty() && existing->active_device.id == identity.id &&
            identity.id.rfind("usb:18d1:", 0) != 0))) {
        return;
      }
    }
  }

  libusb_device_handle *handle = nullptr;
  const int open_rc = libusb_open(device, &handle);
  if (open_rc != LIBUSB_SUCCESS || handle == nullptr) {
    bridge->set_discovery_error("cannot open accessory: " + libusb_text(open_rc));
    return;
  }

  BulkEndpoints endpoints;
  std::string error;
  if (!find_bulk_endpoints(device, endpoints, error)) {
    libusb_close(handle);
    bridge->set_discovery_error(error);
    return;
  }

  UsbDeviceIdentity refreshed;
  inspect_usb_device(bridge, device, desc, handle, false, refreshed);
  merge_expected_identity(refreshed, identity);
  identity = std::move(refreshed);

  if (is_device_blocked(bridge, identity.id)) {
    libusb_close(handle);
    return;
  }

  const int claim_rc =
      libusb_claim_interface(handle, endpoints.interface_number);
  if (claim_rc != LIBUSB_SUCCESS) {
    libusb_close(handle);
    bridge->set_discovery_error("libusb_claim_interface failed: " +
                                libusb_text(claim_rc));
    return;
  }

  auto link = std::make_shared<DeviceLink>();
  link->bridge = bridge;
  {
    std::lock_guard lock(bridge->links_mutex);
    link->link_id = bridge->next_link_id++;
  }
  link->handle = handle;
  link->interface_number = endpoints.interface_number;
  link->interface_claimed = true;
  link->boundary = std::make_unique<HostBoundaryImpl>(link.get());

  {
    std::lock_guard snap_lock(link->snapshot_mutex);
    link->active_device = identity;
    link->active_device_valid = true;
    link->active_bus = identity.bus;
    link->active_address = identity.address;
    link->cached_state = "starting";
  }

  std::weak_ptr<DeviceLink> weak_link = link;
  link->io = std::make_unique<UsbByteIo>(
      link->handle, endpoints.endpoint_in, endpoints.endpoint_out,
      &link->rx_bytes, &link->tx_bytes, &link->bulk_errors,
      bridge->perf_trace ? &link->perf_write_timeouts : nullptr,
      bridge->perf_trace ? &link->perf_write_timeout_bytes : nullptr,
      [weak_link] {
        if (auto locked = weak_link.lock()) {
          {
            std::lock_guard lock(locked->video_mutex);
            locked->rx_wakeup = true;
          }
          locked->wake_cv.notify_one();
        }
      });

  {
    std::ostringstream line;
    line << "[link " << link->link_id << "] claimed interface "
         << endpoints.interface_number << " in=0x" << std::hex
         << static_cast<int>(endpoints.endpoint_in) << " out=0x"
         << static_cast<int>(endpoints.endpoint_out) << std::dec
         << " device=" << identity.name << " (" << identity.id << ")";
    link->log(line.str());
  }
  bridge->clear_discovery_error();
  link->clock = std::make_unique<deskmate::protocol::SteadyClock>();
  start_session(link);

  {
    std::lock_guard lock(bridge->links_mutex);
    bridge->links.emplace(link->link_id, link);
  }
  link->worker = std::thread(link_worker, link);
}

void discovery_main(DMBridge *bridge) {
#ifdef __APPLE__
  pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
  if (bridge->context == nullptr) {
    bridge->set_discovery_error("libusb_init failed");
    return;
  }

  while (!bridge->stop_requested.load()) {
    reap_finished_links(bridge);

    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(bridge->context, &devices);
    if (count < 0) {
      std::unique_lock lock(bridge->discovery_mutex);
      bridge->discovery_cv.wait_for(lock, std::chrono::milliseconds(400),
                                    [bridge] {
                                      return bridge->stop_requested.load() ||
                                             bridge->discovery_wakeup;
                                    });
      bridge->discovery_wakeup = false;
      continue;
    }

    const auto current_links = snapshot_links(bridge);
    std::unordered_set<std::uint16_t> owned_bus_addr;
    std::unordered_set<std::string> connected_ids;
    for (const auto &link : current_links) {
      std::lock_guard snap_lock(link->snapshot_mutex);
      if (link->active_device_valid) {
        owned_bus_addr.insert(
            static_cast<std::uint16_t>((link->active_bus << 8) |
                                       link->active_address));
        if (!link->active_device.id.empty() &&
            link->active_device.id.rfind("usb:18d1:", 0) != 0) {
          connected_ids.insert(link->active_device.id);
        }
      }
    }

    std::vector<std::string> blocked_names;
    struct Candidate {
      libusb_device *device = nullptr;
      libusb_device_descriptor desc{};
      UsbDeviceIdentity identity;
      bool accessory = false;
    };
    std::vector<Candidate> candidates;

    for (ssize_t index = 0; index < count; ++index) {
      libusb_device *dev = devices[index];
      libusb_device_descriptor desc{};
      if (libusb_get_device_descriptor(dev, &desc) != LIBUSB_SUCCESS) {
        continue;
      }
      const bool hdc = is_hdc_device(desc.idVendor, desc.idProduct);
      const bool accessory = is_accessory(desc.idVendor, desc.idProduct);
      if (!hdc && !accessory) {
        continue;
      }
      const std::uint8_t bus = libusb_get_bus_number(dev);
      const std::uint8_t addr = libusb_get_device_address(dev);
      const std::uint16_t key = static_cast<std::uint16_t>((bus << 8) | addr);
      if (owned_bus_addr.count(key) > 0) {
        continue;
      }

      UsbDeviceIdentity identity;
      inspect_usb_device(bridge, dev, desc, nullptr, true, identity);
      if (is_device_blocked(bridge, identity.id)) {
        blocked_names.push_back(identity.name.empty() ? identity.id
                                                      : identity.name);
        continue;
      }
      if (!identity.id.empty() && connected_ids.count(identity.id) > 0) {
        continue;
      }
      libusb_ref_device(dev);
      candidates.push_back({dev, desc, std::move(identity), accessory});
    }
    libusb_free_device_list(devices, 1);

    {
      std::lock_guard snap_lock(bridge->snapshot_mutex);
      bridge->blocked_device_present = !blocked_names.empty();
      bridge->blocked_device_name =
          blocked_names.empty() ? "" : blocked_names.front();
      bridge->blocked_device_names = std::move(blocked_names);
    }

    const auto now = std::chrono::steady_clock::now();
    for (const auto &candidate : candidates) {
      if (bridge->stop_requested.load()) {
        break;
      }
      if (candidate.accessory) {
        attach_accessory_link(bridge, candidate.device, candidate.desc,
                              candidate.identity);
      } else {
        const std::string cooldown_key =
            !candidate.identity.port_path.empty()
                ? ("port:" + candidate.identity.port_path)
                : candidate.identity.id;
        bool in_cooldown = false;
        {
          std::lock_guard lock(bridge->device_policy_mutex);
          auto it = bridge->aoa_switch_cooldown.find(cooldown_key);
          if (it != bridge->aoa_switch_cooldown.end() && now < it->second) {
            in_cooldown = true;
          }
        }
        if (!in_cooldown) {
          std::string error;
          if (trigger_aoa_switch(bridge, candidate.device, candidate.identity,
                                 error)) {
            std::lock_guard lock(bridge->device_policy_mutex);
            bridge->aoa_switch_cooldown[cooldown_key] = now + kAoaSwitchCooldown;
            bridge->clear_discovery_error();
          } else if (!error.empty()) {
            bridge->set_discovery_error(error);
            std::lock_guard lock(bridge->device_policy_mutex);
            bridge->aoa_switch_cooldown[cooldown_key] =
                now + std::chrono::seconds(2);
          }
        }
      }
    }
    for (auto &candidate : candidates) {
      if (candidate.device != nullptr) {
        libusb_unref_device(candidate.device);
      }
    }

    std::unique_lock lock(bridge->discovery_mutex);
    bridge->discovery_cv.wait_for(lock, std::chrono::milliseconds(400),
                                  [bridge] {
                                    return bridge->stop_requested.load() ||
                                           bridge->discovery_wakeup;
                                  });
    bridge->discovery_wakeup = false;
  }

  reap_finished_links(bridge);
}

}  // namespace

extern "C" {

DMBridge *dm_bridge_create(const DMBridgeConfig *config) {
  auto *bridge = new DMBridge();
  if (config != nullptr) {
    bridge->config = *config;
  }
  bridge->auto_match_resolution.store(
      bridge->config.auto_match_device_resolution != 0);
  if (bridge->config.width <= 0) {
    bridge->config.width = 2800;
  }
  if (bridge->config.height <= 0) {
    bridge->config.height = 1840;
  }
  if (bridge->config.refresh_hz <= 0) {
    bridge->config.refresh_hz = 60;
  }
  if (libusb_init(&bridge->context) != LIBUSB_SUCCESS) {
    bridge->context = nullptr;
  }
  return bridge;
}

void dm_bridge_set_auto_match_resolution(DMBridge *bridge, int32_t enabled) {
  if (bridge == nullptr) {
    return;
  }
  bridge->auto_match_resolution.store(enabled != 0);
}

void dm_bridge_set_log(DMBridge *bridge, DMLogCallback callback, void *context) {
  if (bridge == nullptr) {
    return;
  }
  bridge->log_callback = callback;
  bridge->log_context = context;
}

void dm_bridge_set_intent_callback(DMBridge *bridge, DMIntentCallback callback,
                                   void *context) {
  if (bridge == nullptr) {
    return;
  }
  bridge->intent_callback = callback;
  bridge->intent_context = context;
}

void dm_bridge_set_input_callback(DMBridge *bridge, DMInputCallback callback,
                                  void *context) {
  if (bridge == nullptr) {
    return;
  }
  bridge->input_callback = callback;
  bridge->input_context = context;
}

void dm_bridge_set_link_intent_callback(DMBridge *bridge,
                                        DMLinkIntentCallback callback,
                                        void *context) {
  if (bridge == nullptr) {
    return;
  }
  bridge->link_intent_callback = callback;
  bridge->link_intent_context = context;
}

void dm_bridge_set_link_input_callback(DMBridge *bridge,
                                       DMLinkInputCallback callback,
                                       void *context) {
  if (bridge == nullptr) {
    return;
  }
  bridge->link_input_callback = callback;
  bridge->link_input_context = context;
}

int32_t dm_bridge_start(DMBridge *bridge) {
  if (bridge == nullptr || bridge->discovery_worker.joinable()) {
    return -1;
  }
  bridge->stop_requested.store(false);
  bridge->discovery_worker = std::thread(discovery_main, bridge);
  return 0;
}

void dm_bridge_stop(DMBridge *bridge) {
  if (bridge == nullptr) {
    return;
  }
  bridge->stop_requested.store(true);
  {
    std::lock_guard lock(bridge->discovery_mutex);
    bridge->discovery_wakeup = true;
  }
  bridge->discovery_cv.notify_all();

  if (bridge->discovery_worker.joinable()) {
    bridge->discovery_worker.join();
  }

  std::vector<std::shared_ptr<DeviceLink>> all_links;
  {
    std::lock_guard lock(bridge->links_mutex);
    all_links.reserve(bridge->links.size() + bridge->finished_links.size());
    for (const auto &pair : bridge->links) {
      all_links.push_back(pair.second);
    }
    for (const auto &finished : bridge->finished_links) {
      all_links.push_back(finished);
    }
    bridge->links.clear();
    bridge->finished_links.clear();
  }

  for (const auto &link : all_links) {
    link->stop_requested.store(true);
    link->wake_cv.notify_all();
  }
  for (const auto &link : all_links) {
    if (link->worker.joinable()) {
      link->worker.join();
    }
    teardown_link(link);
  }
}

void dm_bridge_destroy(DMBridge *bridge) {
  if (bridge == nullptr) {
    return;
  }
  dm_bridge_stop(bridge);
  if (bridge->context != nullptr) {
    libusb_exit(bridge->context);
    bridge->context = nullptr;
  }
  delete bridge;
}

int32_t dm_bridge_submit_video_for_link(DMBridge *bridge, uint64_t link_id,
                                        const std::uint8_t *data, size_t size,
                                        std::uint64_t pts_us,
                                        int32_t keyframe) {
  if (bridge == nullptr || data == nullptr || size == 0) {
    return -1;
  }
  auto link = find_link(bridge, link_id);
  if (link == nullptr) {
    return -1;
  }
  link->frames_submitted.fetch_add(1);
  {
    std::lock_guard lock(link->video_mutex);
    if (link->video_queue.size() >= kMaxPendingVideoFrames) {
      if (keyframe == 0) {
        link->frames_dropped_backlog.fetch_add(1);
        link->frames_rejected.fetch_add(1);
        return -2;
      }
      /* A keyframe supersedes the whole backlog: nothing older can be decoded. */
      link->frames_dropped_backlog.fetch_add(link->video_queue.size());
      link->video_queue.clear();
    }
    DeviceLink::PendingVideo pending;
    pending.bytes.assign(data, data + size);
    pending.pts_us = pts_us;
    pending.keyframe = keyframe != 0;
    link->video_queue.push_back(std::move(pending));
  }
  link->wake_cv.notify_one();
  return 0;
}

int32_t dm_bridge_submit_video(DMBridge *bridge, const std::uint8_t *data,
                               size_t size, std::uint64_t pts_us,
                               int32_t keyframe) {
  auto link = first_link(bridge);
  if (link == nullptr) {
    return -1;
  }
  return dm_bridge_submit_video_for_link(bridge, link->link_id, data, size,
                                         pts_us, keyframe);
}

int32_t dm_bridge_take_input_for_link(DMBridge *bridge, uint64_t link_id,
                                      DMInputEvent *out, int32_t capacity) {
  if (bridge == nullptr || out == nullptr || capacity <= 0) {
    return 0;
  }
  auto link = find_link(bridge, link_id);
  if (link == nullptr) {
    return 0;
  }
  std::lock_guard lock(link->input_mutex);
  const int32_t count = std::min<int32_t>(
      capacity, static_cast<int32_t>(link->pending_inputs.size()));
  for (int32_t index = 0; index < count; ++index) {
    const auto &event = link->pending_inputs.front();
    out[index].kind = static_cast<int32_t>(event.kind);
    out[index].pointer_id = event.pointer_id;
    out[index].flags = event.flags;
    out[index].x = event.x;
    out[index].y = event.y;
    out[index].dx = event.dx;
    out[index].dy = event.dy;
    out[index].keycode = event.keycode;
    link->pending_inputs.pop_front();
  }
  link->input_events.fetch_add(static_cast<std::uint64_t>(count));
  return count;
}

int32_t dm_bridge_take_input(DMBridge *bridge, DMInputEvent *out,
                             int32_t capacity) {
  auto link = first_link(bridge);
  if (link == nullptr) {
    return 0;
  }
  return dm_bridge_take_input_for_link(bridge, link->link_id, out, capacity);
}

int32_t dm_bridge_needs_keyframe_for_link(DMBridge *bridge, uint64_t link_id) {
  auto link = find_link(bridge, link_id);
  return link != nullptr && link->needs_keyframe.load() ? 1 : 0;
}

int32_t dm_bridge_needs_keyframe(DMBridge *bridge) {
  auto link = first_link(bridge);
  return link != nullptr && link->needs_keyframe.load() ? 1 : 0;
}

uint32_t dm_bridge_keyframe_request_id_for_link(DMBridge *bridge,
                                                uint64_t link_id) {
  auto link = find_link(bridge, link_id);
  return link != nullptr ? link->keyframe_request_id.load() : 0;
}

uint32_t dm_bridge_keyframe_request_id(DMBridge *bridge) {
  auto link = first_link(bridge);
  return link != nullptr ? link->keyframe_request_id.load() : 0;
}

int32_t dm_bridge_negotiated_width_for_link(DMBridge *bridge, uint64_t link_id) {
  auto link = find_link(bridge, link_id);
  return link != nullptr ? link->negotiated_width.load() : 0;
}

int32_t dm_bridge_negotiated_height_for_link(DMBridge *bridge,
                                             uint64_t link_id) {
  auto link = find_link(bridge, link_id);
  return link != nullptr ? link->negotiated_height.load() : 0;
}

int32_t dm_bridge_negotiated_refresh_hz_for_link(DMBridge *bridge,
                                                 uint64_t link_id) {
  auto link = find_link(bridge, link_id);
  return link != nullptr ? link->negotiated_refresh_hz.load() : 0;
}

int32_t dm_bridge_negotiated_codec_for_link(DMBridge *bridge, uint64_t link_id) {
  auto link = find_link(bridge, link_id);
  return link != nullptr ? link->negotiated_codec.load() : 0;
}

int32_t dm_bridge_negotiated_width(DMBridge *bridge) {
  auto link = first_link(bridge);
  return link != nullptr ? link->negotiated_width.load() : 0;
}

int32_t dm_bridge_negotiated_height(DMBridge *bridge) {
  auto link = first_link(bridge);
  return link != nullptr ? link->negotiated_height.load() : 0;
}

int32_t dm_bridge_negotiated_refresh_hz(DMBridge *bridge) {
  auto link = first_link(bridge);
  return link != nullptr ? link->negotiated_refresh_hz.load() : 0;
}

int32_t dm_bridge_negotiated_codec(DMBridge *bridge) {
  auto link = first_link(bridge);
  return link != nullptr ? link->negotiated_codec.load() : 0;
}

int32_t dm_bridge_host_event(DMBridge *bridge, int32_t event) {
  if (bridge == nullptr) {
    return -1;
  }
  deskmate::protocol::HostEvent mapped;
  switch (event) {
    case DM_HOST_EVENT_SCREEN_LOCKED:
      mapped = deskmate::protocol::HostEvent::ScreenLocked;
      break;
    case DM_HOST_EVENT_SCREEN_UNLOCKED:
      mapped = deskmate::protocol::HostEvent::ScreenUnlocked;
      break;
    case DM_HOST_EVENT_WILL_SLEEP:
      mapped = deskmate::protocol::HostEvent::WillSleep;
      break;
    case DM_HOST_EVENT_SCREEN_RECORDING_REVOKED:
      mapped = deskmate::protocol::HostEvent::ScreenRecordingPermissionRevoked;
      break;
    case DM_HOST_EVENT_ACCESSIBILITY_REVOKED:
      mapped = deskmate::protocol::HostEvent::AccessibilityPermissionRevoked;
      break;
    case DM_HOST_EVENT_DISPLAY_ERROR:
      mapped = deskmate::protocol::HostEvent::DisplayError;
      break;
    default:
      return -1;
  }

  const auto current_links = snapshot_links(bridge);
  if (current_links.empty()) {
    return -1;
  }
  int32_t aggregate = 1;
  bool any_session = false;
  for (const auto &link : current_links) {
    deskmate::protocol::SessionCommandStatus status;
    int32_t dispatch_capture = -1;
    int32_t dispatch_input = -1;
    {
      std::lock_guard lock(link->session_mutex);
      if (link->session == nullptr) {
        continue;
      }
      any_session = true;
      status = link->session->handle_host_event(mapped);
      dispatch_capture = link->pending_capture_intent;
      dispatch_input = link->pending_input_intent;
      link->pending_capture_intent = -1;
      link->pending_input_intent = -1;
    }
    if (status == deskmate::protocol::SessionCommandStatus::Ok) {
      aggregate = 0;
    }
    if (dispatch_capture >= 0 || dispatch_input >= 0) {
      if (dispatch_capture >= 0) {
        link->last_dispatched_capture_intent.store(dispatch_capture);
      }
      dispatch_intent(bridge, link->link_id, dispatch_capture, dispatch_input);
    }
  }
  return any_session ? aggregate : -1;
}

char *dm_bridge_snapshot(DMBridge *bridge) {
  if (bridge == nullptr) {
    return nullptr;
  }

  const auto current_links = snapshot_links(bridge);

  bool device_blocked = false;
  std::string blocked_name;
  std::vector<std::string> blocked_names;
  {
    std::lock_guard lock(bridge->snapshot_mutex);
    device_blocked = bridge->blocked_device_present;
    blocked_name = bridge->blocked_device_name;
    blocked_names = bridge->blocked_device_names;
  }
  std::string disc_error;
  {
    std::lock_guard lock(bridge->error_mutex);
    disc_error = bridge->discovery_error;
  }

  std::string primary_state = "idle";
  std::string primary_negotiated = "none";
  bool primary_input_enabled = false;
  UsbDeviceIdentity primary_device;
  bool primary_has_device = false;
  std::string primary_error = disc_error;
  bool any_running = false;
  bool any_input_enabled = false;

  std::uint64_t total_submitted = 0;
  std::uint64_t total_sent = 0;
  std::uint64_t total_rejected = 0;
  std::uint64_t total_dropped_backlog = 0;
  std::uint64_t total_inputs = 0;
  std::uint64_t total_reconnects = 0;
  std::uint64_t total_rx = 0;
  std::uint64_t total_tx = 0;
  std::uint64_t total_bulk_errors = 0;

  std::ostringstream links_json;
  links_json << "[";

  for (std::size_t index = 0; index < current_links.size(); ++index) {
    const auto &link = current_links[index];
    std::string state;
    std::string negotiated;
    bool input_enabled = false;
    UsbDeviceIdentity device;
    bool has_device = false;
    {
      std::lock_guard lock(link->snapshot_mutex);
      state = link->cached_state;
      negotiated = link->cached_negotiated;
      input_enabled = link->cached_input_enabled;
      device = link->active_device;
      has_device = link->active_device_valid;
    }
    std::string error;
    {
      std::lock_guard lock(link->error_mutex);
      error = link->last_error;
    }

    const bool link_running = link->running.load();
    const std::uint64_t submitted = link->frames_submitted.load();
    const std::uint64_t sent = link->frames_sent.load();
    const std::uint64_t rejected = link->frames_rejected.load();
    const std::uint64_t backlog = link->frames_dropped_backlog.load();
    const std::uint64_t inputs = link->input_events.load();
    const std::uint64_t reconn = link->reconnects.load();
    const std::uint64_t rx = link->rx_bytes.load();
    const std::uint64_t tx = link->tx_bytes.load();
    const std::uint64_t bulk_err = link->bulk_errors.load();

    any_running = any_running || link_running;
    any_input_enabled = any_input_enabled || input_enabled;
    total_submitted += submitted;
    total_sent += sent;
    total_rejected += rejected;
    total_dropped_backlog += backlog;
    total_inputs += inputs;
    total_reconnects += reconn;
    total_rx += rx;
    total_tx += tx;
    total_bulk_errors += bulk_err;

    if (index == 0) {
      primary_state = state;
      primary_negotiated = negotiated;
      primary_input_enabled = input_enabled;
      primary_device = device;
      primary_has_device = has_device;
      if (!error.empty()) {
        primary_error = error;
      }
    } else if (primary_state != "streaming" && state == "streaming") {
      primary_state = state;
      primary_negotiated = negotiated;
      primary_device = device;
      primary_has_device = has_device;
    }

    if (index > 0) {
      links_json << ",";
    }
    links_json << "{\"linkId\":" << link->link_id
               << ",\"state\":\"" << json_escape(state) << "\""
               << ",\"running\":" << (link_running ? "true" : "false")
               << ",\"negotiated\":\"" << json_escape(negotiated) << "\""
               << ",\"inputEnabled\":" << (input_enabled ? "true" : "false")
               << ",\"framesSubmitted\":" << submitted
               << ",\"framesSent\":" << sent
               << ",\"framesRejected\":" << rejected
               << ",\"framesDroppedBacklog\":" << backlog
               << ",\"inputEvents\":" << inputs
               << ",\"reconnects\":" << reconn
               << ",\"rxBytes\":" << rx
               << ",\"txBytes\":" << tx
               << ",\"bulkErrors\":" << bulk_err
               << ",\"deviceId\":\"" << json_escape(has_device ? device.id : "") << "\""
               << ",\"deviceName\":\"" << json_escape(has_device ? device.name : "") << "\""
               << ",\"deviceManufacturer\":\""
               << json_escape(has_device ? device.manufacturer : "") << "\""
               << ",\"deviceSerial\":\""
               << json_escape(has_device ? device.serial : "") << "\""
               << ",\"error\":\"" << json_escape(error) << "\"}";
  }
  links_json << "]";

  std::ostringstream blocked_json;
  blocked_json << "[";
  for (std::size_t index = 0; index < blocked_names.size(); ++index) {
    if (index > 0) {
      blocked_json << ",";
    }
    blocked_json << "\"" << json_escape(blocked_names[index]) << "\"";
  }
  blocked_json << "]";

  std::ostringstream json;
  json << "{\"activeLinkCount\":" << current_links.size()
       << ",\"links\":" << links_json.str()
       << ",\"blockedDevices\":" << blocked_json.str()
       << ",\"state\":\"" << json_escape(primary_state) << "\""
       << ",\"running\":" << (any_running ? "true" : "false")
       << ",\"negotiated\":\"" << json_escape(primary_negotiated) << "\""
       << ",\"inputEnabled\":"
       << ((primary_input_enabled || any_input_enabled) ? "true" : "false")
       << ",\"framesSubmitted\":" << total_submitted
       << ",\"framesSent\":" << total_sent
       << ",\"framesRejected\":" << total_rejected
       << ",\"framesDroppedBacklog\":" << total_dropped_backlog
       << ",\"inputEvents\":" << total_inputs
       << ",\"reconnects\":" << total_reconnects
       << ",\"rxBytes\":" << total_rx
       << ",\"txBytes\":" << total_tx
       << ",\"bulkErrors\":" << total_bulk_errors
       << ",\"deviceId\":\""
       << json_escape(primary_has_device ? primary_device.id : "") << "\""
       << ",\"deviceName\":\""
       << json_escape(primary_has_device ? primary_device.name : "") << "\""
       << ",\"deviceManufacturer\":\""
       << json_escape(primary_has_device ? primary_device.manufacturer : "")
       << "\""
       << ",\"deviceSerial\":\""
       << json_escape(primary_has_device ? primary_device.serial : "") << "\""
       << ",\"deviceBlocked\":" << (device_blocked ? "true" : "false")
       << ",\"blockedDeviceName\":\"" << json_escape(blocked_name) << "\""
       << ",\"error\":\"" << json_escape(primary_error) << "\"}";

  const std::string text = json.str();
  auto *buffer = static_cast<char *>(std::malloc(text.size() + 1));
  if (buffer == nullptr) {
    return nullptr;
  }
  std::memcpy(buffer, text.c_str(), text.size() + 1);
  return buffer;
}

void dm_bridge_set_blocked_devices(DMBridge *bridge,
                                   const char *const *device_ids,
                                   int32_t count) {
  if (bridge == nullptr) {
    return;
  }
  std::unordered_set<std::string> next_blocked;
  if (device_ids != nullptr && count > 0) {
    for (int32_t index = 0; index < count; ++index) {
      if (device_ids[index] != nullptr && device_ids[index][0] != '\0') {
        next_blocked.emplace(device_ids[index]);
      }
    }
  }
  {
    std::lock_guard lock(bridge->device_policy_mutex);
    bridge->blocked_device_ids = std::move(next_blocked);
  }

  const auto current_links = snapshot_links(bridge);
  for (const auto &link : current_links) {
    bool should_disconnect = false;
    {
      std::lock_guard snap_lock(link->snapshot_mutex);
      if (link->active_device_valid) {
        should_disconnect =
            is_device_blocked(bridge, link->active_device.id);
      }
    }
    if (should_disconnect) {
      link->disconnect_requested.store(true);
      link->wake_cv.notify_all();
    }
  }
  {
    std::lock_guard lock(bridge->discovery_mutex);
    bridge->discovery_wakeup = true;
  }
  bridge->discovery_cv.notify_one();
}

void dm_bridge_disconnect_device(DMBridge *bridge, const char *device_id) {
  if (bridge == nullptr) {
    return;
  }
  const auto current_links = snapshot_links(bridge);
  for (const auto &link : current_links) {
    bool matches = false;
    {
      std::lock_guard snap_lock(link->snapshot_mutex);
      if (link->active_device_valid) {
        matches = (device_id == nullptr || device_id[0] == '\0' ||
                   link->active_device.id == device_id);
      }
    }
    if (matches) {
      link->disconnect_requested.store(true);
      link->wake_cv.notify_all();
    }
  }
}

void dm_bridge_disconnect_link(DMBridge *bridge, uint64_t link_id) {
  auto link = find_link(bridge, link_id);
  if (link == nullptr) {
    return;
  }
  link->disconnect_requested.store(true);
  link->wake_cv.notify_all();
}

char *dm_bridge_scan_devices(DMBridge *bridge) {
  libusb_context *ctx = bridge != nullptr ? bridge->context : nullptr;
  libusb_context *temp_ctx = nullptr;
  if (ctx == nullptr) {
    if (libusb_init(&temp_ctx) != LIBUSB_SUCCESS) {
      temp_ctx = nullptr;
    }
    ctx = temp_ctx;
  }

  std::vector<UsbDeviceIdentity> found;
  if (ctx != nullptr) {
    libusb_device **devices = nullptr;
    const ssize_t count = libusb_get_device_list(ctx, &devices);
    if (count >= 0) {
      for (ssize_t index = 0; index < count; ++index) {
        libusb_device_descriptor desc{};
        if (libusb_get_device_descriptor(devices[index], &desc) !=
            LIBUSB_SUCCESS) {
          continue;
        }
        if (!is_hdc_device(desc.idVendor, desc.idProduct) &&
            !is_accessory(desc.idVendor, desc.idProduct)) {
          continue;
        }
        UsbDeviceIdentity identity;
        inspect_usb_device(bridge, devices[index], desc, nullptr, true,
                           identity);
        if (!identity.id.empty()) {
          found.push_back(std::move(identity));
        }
      }
      libusb_free_device_list(devices, 1);
    }
  } else if (bridge != nullptr) {
    const auto current_links = snapshot_links(bridge);
    for (const auto &link : current_links) {
      std::lock_guard snap_lock(link->snapshot_mutex);
      if (link->active_device_valid && !link->active_device.id.empty()) {
        found.push_back(link->active_device);
      }
    }
  }

  if (temp_ctx != nullptr) {
    libusb_exit(temp_ctx);
  }

  std::ostringstream json;
  json << "[";
  for (std::size_t index = 0; index < found.size(); ++index) {
    if (index > 0) {
      json << ",";
    }
    const auto &item = found[index];
    json << "{\"id\":\"" << json_escape(item.id) << "\""
         << ",\"name\":\"" << json_escape(item.name) << "\""
         << ",\"manufacturer\":\"" << json_escape(item.manufacturer) << "\""
         << ",\"serial\":\"" << json_escape(item.serial) << "\""
         << ",\"inAccessoryMode\":"
         << (item.in_accessory_mode ? "true" : "false") << "}";
  }
  json << "]";
  const std::string text = json.str();
  auto *buffer = static_cast<char *>(std::malloc(text.size() + 1));
  if (buffer == nullptr) {
    return nullptr;
  }
  std::memcpy(buffer, text.c_str(), text.size() + 1);
  return buffer;
}

}  // extern "C"
