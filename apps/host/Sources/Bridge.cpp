#include "DMBridge.h"

#include "deskmate/clock.hpp"
#include "deskmate/protocol.hpp"
#include "deskmate/session.hpp"

#include <libusb-1.0/libusb.h>

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
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
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
constexpr int kMaxHandshakeAttempts = 5;
/* In-place session rebuilds are right while the tablet has not opened the
   accessory yet. Once it answers and then stalls mid-handshake, the device side
   is holding a stale session, and only a link hangup makes it rebuild - so
   after this many in-place retries the link is dropped and re-claimed. */
constexpr int kInPlaceRetriesBeforeRelink = 2;
/* 12 KiB payload + 32 B header stays under the tablet's 16 KiB accessory read
   buffer; larger packets are truncated by the driver. 64 fragments per frame
   (the protocol maximum) still covers a 768 KiB keyframe. */
constexpr std::size_t kVideoFragmentPayload = 12U * 1024U;
constexpr std::size_t kMaxPendingVideoFrames = 16;
/* The tablet shows a USB-accessory consent dialog the first time an accessory
   claims the interface. The protocol default of 2 s expires before anyone can
   tap it, so the host allows a full 20 s for the first ClientHello. */
constexpr std::uint64_t kHandshakeStageTimeoutNs = 20'000'000'000ULL;

bool is_accessory(std::uint16_t vendor_id, std::uint16_t product_id) {
  return vendor_id == kAccessoryVendorId &&
         product_id >= kAccessoryProductIdMin &&
         product_id <= kAccessoryProductIdMax;
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

libusb_device *find_device(libusb_context *context, std::uint16_t vendor_id,
                           std::uint16_t product_id, bool want_accessory) {
  libusb_device **devices = nullptr;
  const ssize_t count = libusb_get_device_list(context, &devices);
  if (count < 0) {
    return nullptr;
  }
  libusb_device *found = nullptr;
  for (ssize_t index = 0; index < count; ++index) {
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(devices[index], &descriptor) !=
        LIBUSB_SUCCESS) {
      continue;
    }
    const bool matches = want_accessory
                             ? is_accessory(descriptor.idVendor,
                                            descriptor.idProduct)
                             : (descriptor.idVendor == vendor_id &&
                                descriptor.idProduct == product_id);
    if (matches) {
      found = devices[index];
      libusb_ref_device(found);
      break;
    }
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

bool switch_to_accessory(libusb_context *context, std::uint16_t vendor_id,
                         std::uint16_t product_id, std::string &error) {
  libusb_device *device = find_device(context, vendor_id, product_id, false);
  if (device == nullptr) {
    if (find_device(context, 0, 0, true) != nullptr) {
      error = "";
      return true; /* already an accessory */
    }
    error = "no device with the configured HDC VID/PID";
    return false;
  }

  libusb_device_handle *handle = nullptr;
  const int open_rc = libusb_open(device, &handle);
  libusb_unref_device(device);
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
                            "Wired extended display for HarmonyOS tablets",
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
  return true;
}

}  // namespace

class HostBoundaryImpl;

struct DMBridge {
  DMBridgeConfig config{};
  libusb_context *context = nullptr;
  libusb_device_handle *handle = nullptr;
  int interface_number = -1;
  bool interface_claimed = false;

  struct PendingVideo {
    std::vector<std::uint8_t> bytes;
    std::uint64_t pts_us{};
    bool keyframe{};
  };

  HostBoundaryImpl *boundary = nullptr;
  std::unique_ptr<UsbByteIo> io;
  std::unique_ptr<deskmate::protocol::SteadyClock> clock;
  std::unique_ptr<deskmate::protocol::HostSession> session;

  std::mutex session_mutex;
  std::mutex video_mutex;
  std::condition_variable wake_cv;
  bool rx_wakeup = false;
  std::deque<PendingVideo> video_queue;
  std::atomic<std::uint64_t> frames_dropped_backlog{0};

  std::thread worker;
  std::atomic<bool> stop_requested{false};
  std::atomic<bool> running{false};
  std::atomic<bool> needs_keyframe{false};
  std::atomic<std::uint32_t> keyframe_request_id{0};
  std::atomic<std::int32_t> negotiated_refresh_hz{0};

  DMLogCallback log_callback = nullptr;
  void *log_context = nullptr;
  DMIntentCallback intent_callback = nullptr;
  void *intent_context = nullptr;

  std::atomic<std::uint64_t> frames_submitted{0};
  std::atomic<std::uint64_t> frames_sent{0};
  std::atomic<std::uint64_t> frames_rejected{0};
  std::atomic<std::uint64_t> input_events{0};
  std::atomic<std::uint64_t> reconnects{0};
  std::atomic<std::uint64_t> rx_bytes{0};
  std::atomic<std::uint64_t> tx_bytes{0};
  std::atomic<std::uint64_t> bulk_errors{0};
  const bool perf_trace = std::getenv("DESKMATE_PERF_TRACE") != nullptr;
  std::uint64_t perf_idr_wait = 0;
  std::uint64_t perf_invalid = 0;
  std::uint64_t perf_other_rejected = 0;
  std::uint64_t perf_inactive = 0;
  std::uint64_t perf_write_timeouts = 0;
  std::uint64_t perf_write_timeout_bytes = 0;

  std::mutex error_mutex;
  std::string last_error;
  std::string last_state{"idle"};
  std::string negotiated{"none"};

  void log(const std::string &message) {
    if (log_callback != nullptr) {
      log_callback(message.c_str(), log_context);
    }
  }

  void set_error(const std::string &message) {
    {
      std::lock_guard lock(error_mutex);
      last_error = message;
    }
    log("error: " + message);
  }

  void clear_error() {
    std::lock_guard lock(error_mutex);
    last_error.clear();
  }
};

class HostBoundaryImpl final : public deskmate::protocol::HostBoundary {
 public:
  explicit HostBoundaryImpl(DMBridge *bridge) : bridge_(bridge) {}

  void set_capture_encoding_intent(bool enabled) override {
    if (bridge_->intent_callback != nullptr) {
      bridge_->intent_callback(enabled ? 1 : 0, -1, bridge_->intent_context);
    }
  }
  void set_input_injection_intent(bool enabled) override {
    if (bridge_->intent_callback != nullptr) {
      bridge_->intent_callback(-1, enabled ? 1 : 0, bridge_->intent_context);
    }
  }

 private:
  DMBridge *bridge_;
};

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

}  // namespace

extern "C" {

DMBridge *dm_bridge_create(const DMBridgeConfig *config) {
  auto *bridge = new DMBridge();
  if (config != nullptr) {
    bridge->config = *config;
  }
  if (bridge->config.width <= 0) {
    bridge->config.width = 2800;
  }
  if (bridge->config.height <= 0) {
    bridge->config.height = 1840;
  }
  if (bridge->config.refresh_hz <= 0) {
    bridge->config.refresh_hz = 60;
  }
  bridge->boundary = new HostBoundaryImpl(bridge);
  if (libusb_init(&bridge->context) != LIBUSB_SUCCESS) {
    bridge->context = nullptr;
  }
  return bridge;
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

namespace {

/* Builds a fresh HostSession over an already-claimed accessory link. */
void start_session(DMBridge *bridge) {
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
    std::fprintf(stderr, "[DEBUG-dm-fps] frame_age_limit_ns=%llu\n",
        static_cast<unsigned long long>(
            session_config.scheduler.max_oldest_unsent_frame_age_ns));
  }
  /* The tablet's accessory read() is message-oriented and its buffer tops out
     at 16 KiB (64 KiB requests fail with EINVAL); anything beyond the read
     buffer is discarded by the driver, not returned by a later read. A 32 KiB
     fragment therefore arrives truncated and desynchronises the stream decoder,
     which showed up as streaming for ~2 s and then heartbeat_timeout. Keep every
     packet, header included, comfortably below that 16 KiB ceiling. */
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

  auto session = std::make_unique<deskmate::protocol::HostSession>(
      *bridge->io, *bridge->clock, session_config, bridge->boundary);
  session->start();
  std::lock_guard lock(bridge->session_mutex);
  bridge->negotiated_refresh_hz.store(0);
  bridge->keyframe_request_id.store(0);
  bridge->needs_keyframe.store(false);
  bridge->session = std::move(session);
}

bool connect_once(DMBridge *bridge) {
  if (bridge->context == nullptr) {
    bridge->set_error("libusb_init failed");
    return false;
  }
  std::string error;
  if (!switch_to_accessory(bridge->context, kHdcVendorId, kHdcProductId,
                           error)) {
    if (!error.empty()) {
      bridge->set_error(error);
      return false;
    }
  }

  libusb_device *device = nullptr;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(15);
  while (std::chrono::steady_clock::now() < deadline &&
         !bridge->stop_requested.load()) {
    device = find_device(bridge->context, 0, 0, true);
    if (device != nullptr) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }
  if (device == nullptr) {
    bridge->set_error("accessory 18d1:2d00 did not enumerate");
    return false;
  }

  const int open_rc = libusb_open(device, &bridge->handle);
  if (open_rc != LIBUSB_SUCCESS || bridge->handle == nullptr) {
    libusb_unref_device(device);
    bridge->set_error("cannot open accessory: " + libusb_text(open_rc));
    return false;
  }

  BulkEndpoints endpoints;
  if (!find_bulk_endpoints(device, endpoints, error)) {
    libusb_unref_device(device);
    bridge->set_error(error);
    return false;
  }
  libusb_unref_device(device);

  bridge->interface_number = endpoints.interface_number;
  const int claim_rc =
      libusb_claim_interface(bridge->handle, endpoints.interface_number);
  if (claim_rc != LIBUSB_SUCCESS) {
    bridge->set_error("libusb_claim_interface failed: " + libusb_text(claim_rc));
    return false;
  }
  bridge->interface_claimed = true;

  bridge->io = std::make_unique<UsbByteIo>(
      bridge->handle, endpoints.endpoint_in, endpoints.endpoint_out,
      &bridge->rx_bytes, &bridge->tx_bytes, &bridge->bulk_errors,
      bridge->perf_trace ? &bridge->perf_write_timeouts : nullptr,
      bridge->perf_trace ? &bridge->perf_write_timeout_bytes : nullptr,
      [bridge] {
        {
          std::lock_guard lock(bridge->video_mutex);
          bridge->rx_wakeup = true;
        }
        bridge->wake_cv.notify_one();
      });
  {
    std::ostringstream line;
    line << "claimed interface " << endpoints.interface_number << " in=0x"
         << std::hex << static_cast<int>(endpoints.endpoint_in) << " out=0x"
         << static_cast<int>(endpoints.endpoint_out) << std::dec;
    bridge->log(line.str());
  }
  bridge->clock = std::make_unique<deskmate::protocol::SteadyClock>();
  start_session(bridge);
  return true;
}

void teardown_link(DMBridge *bridge) {
  {
    std::lock_guard lock(bridge->session_mutex);
    bridge->needs_keyframe.store(false);
    bridge->keyframe_request_id.store(0);
    bridge->negotiated_refresh_hz.store(0);
    bridge->session.reset();
  }
  bridge->io.reset();
  bridge->clock.reset();
  if (bridge->handle != nullptr) {
    if (bridge->interface_claimed) {
      libusb_release_interface(bridge->handle, bridge->interface_number);
      bridge->interface_claimed = false;
    }
    libusb_close(bridge->handle);
    bridge->handle = nullptr;
  }
}

void worker_main(DMBridge *bridge) {
  int link_failures = 0;
  int handshake_failures = 0;
  while (!bridge->stop_requested.load()) {
    if (bridge->handle == nullptr) {
      if (!connect_once(bridge)) {
        if (bridge->stop_requested.load()) {
          break;
        }
        ++link_failures;
        if (link_failures >= kMaxHandshakeAttempts) {
          bridge->set_error("giving up after repeated USB link failures");
          break;
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
      link_failures = 0;
      bridge->running.store(true);
      bridge->log("usb link up; waiting for the tablet to send ClientHello");
    }

    /* The link stays claimed for as long as the tablet keeps it. A handshake
       timeout means the tablet has not opened its side yet (consent dialog, app
       in the background), so the session is rebuilt in place: re-claiming the
       interface would fail with LIBUSB_ERROR_BUSY, and dropping the link would
       make the tablet re-authorize, which never converges. */
    std::string failure;
    bool stopped = false;
    auto next_status_log = std::chrono::steady_clock::now();
    auto next_perf_log = next_status_log;
    for (;;) {
      if (bridge->stop_requested.load()) {
        stopped = true;
        break;
      }
      bool terminal = false;
      bool has_pending_output = false;
      bool write_blocked = false;
      {
        std::lock_guard lock(bridge->session_mutex);
        if (bridge->session == nullptr) {
          terminal = true;
        } else {
          /* Drain queued frames before pumping so the write path never blocks
             the capture callback. */
          std::deque<DMBridge::PendingVideo> batch;
          {
            std::lock_guard video_lock(bridge->video_mutex);
            batch.swap(bridge->video_queue);
            bridge->rx_wakeup = false;
          }
          for (const DMBridge::PendingVideo &frame : batch) {
            const auto status = bridge->session->send_video_frame(
                std::span<const std::uint8_t>(frame.bytes), frame.pts_us,
                frame.keyframe);
            if (bridge->perf_trace) {
              using Status = deskmate::protocol::VideoSendStatus;
              switch (status) {
                case Status::Sent: break;
                case Status::NotStreaming:
                case Status::Paused: ++bridge->perf_inactive; break;
                case Status::IdrRequired: ++bridge->perf_idr_wait; break;
                case Status::InvalidArgument:
                case Status::EmptyPayload: ++bridge->perf_invalid; break;
                default: ++bridge->perf_other_rejected; break;
              }
            }
            if (status == deskmate::protocol::VideoSendStatus::Sent) {
              bridge->frames_sent.fetch_add(1);
            } else if (status !=
                           deskmate::protocol::VideoSendStatus::NotStreaming &&
                       status != deskmate::protocol::VideoSendStatus::Paused) {
              bridge->frames_rejected.fetch_add(1);
            }
          }
          bridge->session->pump();
          terminal = bridge->session->is_terminal();
          has_pending_output =
              !terminal && bridge->session->has_pending_output();
          if (bridge->io != nullptr) {
            write_blocked = bridge->io->consume_write_blocked();
          }
          if (!terminal) {
            if (const auto &idr = bridge->session->pending_idr_request();
                idr.has_value()) {
              bridge->needs_keyframe.store(true);
              bridge->keyframe_request_id.store(idr->recovery_id);
            } else {
              bridge->needs_keyframe.store(false);
              bridge->keyframe_request_id.store(0);
            }
            if (const auto &stream = bridge->session->negotiated_stream();
                stream.has_value()) {
              bridge->negotiated_refresh_hz.store(
                  static_cast<std::int32_t>(stream->profile.resolution.refresh_hz));
            }
          } else {
            bridge->needs_keyframe.store(false);
            bridge->keyframe_request_id.store(0);
          }
          if (terminal && bridge->session->failure().has_value()) {
            failure = std::string(deskmate::protocol::session_failure_name(
                *bridge->session->failure()));
          }
        }
      }
      const auto now = std::chrono::steady_clock::now();
      if (bridge->perf_trace && (now >= next_perf_log || terminal)) {
        next_perf_log = now + std::chrono::seconds(1);
        const auto timestamp_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        std::fprintf(stderr,
            "[DEBUG-dm-fps] wall_ms=%lld submitted=%llu queued=%llu "
            "idr_wait=%llu invalid=%llu other=%llu inactive=%llu backlog=%llu "
            "usb_bytes=%llu usb_timeouts=%llu usb_timeout_bytes=%llu bulk_errors=%llu terminal=%d\n",
            static_cast<long long>(timestamp_ms),
            static_cast<unsigned long long>(bridge->frames_submitted.load()),
            static_cast<unsigned long long>(bridge->frames_sent.load()),
            static_cast<unsigned long long>(bridge->perf_idr_wait),
            static_cast<unsigned long long>(bridge->perf_invalid),
            static_cast<unsigned long long>(bridge->perf_other_rejected),
            static_cast<unsigned long long>(bridge->perf_inactive),
            static_cast<unsigned long long>(bridge->frames_dropped_backlog.load()),
            static_cast<unsigned long long>(bridge->tx_bytes.load()),
            static_cast<unsigned long long>(bridge->perf_write_timeouts),
            static_cast<unsigned long long>(bridge->perf_write_timeout_bytes),
            static_cast<unsigned long long>(bridge->bulk_errors.load()), terminal);
      }
      if (terminal) {
        break;
      }

      /* Periodic state line: state, byte counters and frame count make a single
         run diagnosable without a second attempt. */
      if (now >= next_status_log) {
        next_status_log = now + std::chrono::seconds(5);
        std::string state;
        std::string negotiated;
        {
          std::lock_guard lock(bridge->session_mutex);
          if (bridge->session != nullptr) {
            state = state_name(bridge->session->state());
            if (const auto &stream = bridge->session->negotiated_stream();
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
          /* A working link earns a fresh retry budget: a session that drops
             later must not inherit earlier handshake failures. */
          handshake_failures = 0;
          bridge->clear_error();
        }
        std::string line =
            "state=" + state +
            " rx=" + std::to_string(bridge->rx_bytes.load()) +
            "B tx=" + std::to_string(bridge->tx_bytes.load()) +
            "B frames=" + std::to_string(bridge->frames_sent.load()) +
            " bulkErrors=" + std::to_string(bridge->bulk_errors.load());
        if (!negotiated.empty()) {
          line += " profile=" + negotiated;
        }
        if (state == "awaiting_client_hello") {
          line += " (waiting for the tablet to allow the USB accessory, attempt " +
                  std::to_string(handshake_failures + 1) + "/" +
                  std::to_string(kMaxHandshakeAttempts) + ")";
        }
        bridge->log(line);
      }
      if (!has_pending_output || write_blocked) {
        const auto wait_duration =
            has_pending_output ? std::chrono::milliseconds(1)
                               : std::chrono::milliseconds(5);
        std::unique_lock video_lock(bridge->video_mutex);
        bridge->wake_cv.wait_for(video_lock, wait_duration, [bridge] {
          return bridge->stop_requested.load() ||
                 !bridge->video_queue.empty() || bridge->rx_wakeup;
        });
      }
    }

    bridge->running.store(false);
    if (stopped) {
      break;
    }

    const bool handshake_retry =
        (failure == "hello_timeout" || failure == "ready_timeout") &&
        bridge->handle != nullptr;
    if (handshake_retry) {
      ++handshake_failures;
      bridge->reconnects.fetch_add(1);
      if (handshake_failures >= kMaxHandshakeAttempts) {
        bridge->set_error(
            "giving up after " + std::to_string(handshake_failures) +
            " handshake attempts; last failure: " + failure +
            " (keep the tablet app in the foreground across the AOA flip, "
            "then relaunch)");
        break;
      }
      /* Once the tablet has answered at least once, an in-place retry can be
         swallowed by the stale device session. Drop the link so the tablet's
         worker rebuilds; the next attempt re-claims and starts clean. */
      if (bridge->rx_bytes.load() > 0 &&
          handshake_failures >= kInPlaceRetriesBeforeRelink) {
        bridge->set_error(failure + " (device answered but stalled; relinking)");
        bridge->log("handshake stalled after the tablet answered; relinking");
        teardown_link(bridge);
        std::this_thread::sleep_for(std::chrono::seconds(1));
        continue;
      }
      bridge->set_error(failure +
                        " (retrying the session, tablet has not answered)");
      bridge->log("handshake timed out; retrying the session on the same link");
      start_session(bridge);
      std::this_thread::sleep_for(std::chrono::seconds(1));
      continue;
    }

    bridge->set_error(failure.empty() ? "session terminated" : failure);
    teardown_link(bridge);
    ++handshake_failures;
    if (handshake_failures >= kMaxHandshakeAttempts) {
      bridge->set_error("giving up after repeated session failures");
      break;
    }
    bridge->reconnects.fetch_add(1);
    std::this_thread::sleep_for(std::chrono::seconds(1));
  }
  bridge->running.store(false);
}

}  // namespace

int32_t dm_bridge_start(DMBridge *bridge) {
  if (bridge == nullptr || bridge->worker.joinable()) {
    return -1;
  }
  bridge->stop_requested.store(false);
  bridge->worker = std::thread(worker_main, bridge);
  return 0;
}

void dm_bridge_stop(DMBridge *bridge) {
  if (bridge == nullptr) {
    return;
  }
  bridge->stop_requested.store(true);
  bridge->wake_cv.notify_all();
  if (bridge->worker.joinable()) {
    bridge->worker.join();
  }
  teardown_link(bridge);
  /* Deliberately no set_configuration(0)/reset here: both are ineffective at
     leaving accessory mode on this tablet and can strand it in 18d1:2d00.
     teardown_link already released the interface; recovery is a user replug. */
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
  delete bridge->boundary;
  delete bridge;
}

int32_t dm_bridge_submit_video(DMBridge *bridge, const std::uint8_t *data,
                               size_t size, std::uint64_t pts_us,
                               int32_t keyframe) {
  if (bridge == nullptr || data == nullptr || size == 0) {
    return -1;
  }
  bridge->frames_submitted.fetch_add(1);
  {
    std::lock_guard lock(bridge->video_mutex);
    if (bridge->video_queue.size() >= kMaxPendingVideoFrames) {
      if (keyframe == 0) {
        bridge->frames_dropped_backlog.fetch_add(1);
        bridge->frames_rejected.fetch_add(1);
        return -2;
      }
      /* A keyframe supersedes the whole backlog: nothing older can be decoded. */
      bridge->frames_dropped_backlog.fetch_add(bridge->video_queue.size());
      bridge->video_queue.clear();
    }
    DMBridge::PendingVideo pending;
    pending.bytes.assign(data, data + size);
    pending.pts_us = pts_us;
    pending.keyframe = keyframe != 0;
    bridge->video_queue.push_back(std::move(pending));
  }
  bridge->wake_cv.notify_one();
  return 0;
}

int32_t dm_bridge_take_input(DMBridge *bridge, DMInputEvent *out,
                             int32_t capacity) {
  if (bridge == nullptr || out == nullptr || capacity <= 0) {
    return 0;
  }
  std::unique_lock lock(bridge->session_mutex, std::try_to_lock);
  if (!lock.owns_lock() || bridge->session == nullptr) {
    return 0;
  }
  const auto events = bridge->session->take_input_events();
  const int32_t count =
      std::min<int32_t>(capacity, static_cast<int32_t>(events.size()));
  for (int32_t index = 0; index < count; ++index) {
    const auto &event = events[static_cast<std::size_t>(index)];
    out[index].kind = static_cast<int32_t>(event.kind);
    out[index].pointer_id = event.pointer_id;
    out[index].flags = event.flags;
    out[index].x = event.x;
    out[index].y = event.y;
    out[index].dx = event.dx;
    out[index].dy = event.dy;
    out[index].keycode = event.keycode;
  }
  bridge->input_events.fetch_add(static_cast<std::uint64_t>(count));
  return count;
}

int32_t dm_bridge_needs_keyframe(DMBridge *bridge) {
  return bridge != nullptr && bridge->needs_keyframe.load() ? 1 : 0;
}

uint32_t dm_bridge_keyframe_request_id(DMBridge *bridge) {
  return bridge != nullptr ? bridge->keyframe_request_id.load() : 0;
}

int32_t dm_bridge_negotiated_refresh_hz(DMBridge *bridge) {
  if (bridge == nullptr) {
    return 0;
  }
  const std::int32_t cached = bridge->negotiated_refresh_hz.load();
  if (cached > 0) {
    return cached;
  }
  std::unique_lock lock(bridge->session_mutex, std::try_to_lock);
  if (lock.owns_lock() && bridge->session != nullptr) {
    if (const auto &stream = bridge->session->negotiated_stream();
        stream.has_value()) {
      const auto hz = static_cast<std::int32_t>(stream->profile.resolution.refresh_hz);
      bridge->negotiated_refresh_hz.store(hz);
      return hz;
    }
  }
  return 0;
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
  std::lock_guard lock(bridge->session_mutex);
  if (bridge->session == nullptr) {
    return -1;
  }
  const auto status = bridge->session->handle_host_event(mapped);
  return status == deskmate::protocol::SessionCommandStatus::Ok ? 0 : 1;
}

char *dm_bridge_snapshot(DMBridge *bridge) {
  if (bridge == nullptr) {
    return nullptr;
  }
  std::string state = "idle";
  std::string negotiated = "none";
  bool input_enabled = false;
  {
    std::unique_lock lock(bridge->session_mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
      return nullptr;
    }
    if (bridge->session != nullptr) {
      state = state_name(bridge->session->state());
      if (const auto &stream = bridge->session->negotiated_stream();
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
      input_enabled = bridge->session->input_injection_intent();
    }
  }
  std::string error;
  {
    std::lock_guard lock(bridge->error_mutex);
    error = bridge->last_error;
  }
  std::ostringstream json;
  json << "{\"state\":\"" << state << "\""
       << ",\"running\":" << (bridge->running.load() ? "true" : "false")
       << ",\"negotiated\":\"" << negotiated << "\""
       << ",\"inputEnabled\":" << (input_enabled ? "true" : "false")
       << ",\"framesSubmitted\":" << bridge->frames_submitted.load()
       << ",\"framesSent\":" << bridge->frames_sent.load()
       << ",\"framesRejected\":" << bridge->frames_rejected.load()
       << ",\"framesDroppedBacklog\":" << bridge->frames_dropped_backlog.load()
       << ",\"inputEvents\":" << bridge->input_events.load()
       << ",\"reconnects\":" << bridge->reconnects.load()
       << ",\"rxBytes\":" << bridge->rx_bytes.load()
       << ",\"txBytes\":" << bridge->tx_bytes.load()
       << ",\"bulkErrors\":" << bridge->bulk_errors.load()
       << ",\"error\":\"" << json_escape(error) << "\"}";
  const std::string text = json.str();
  auto *buffer = static_cast<char *>(std::malloc(text.size() + 1));
  if (buffer == nullptr) {
    return nullptr;
  }
  std::memcpy(buffer, text.c_str(), text.size() + 1);
  return buffer;
}

}  // extern "C"
