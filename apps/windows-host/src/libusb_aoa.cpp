#include "deskmate/windows/libusb_aoa.hpp"

#include "deskmate/platform/aoa.hpp"

#ifndef DESKMATE_HAVE_LIBUSB
#define DESKMATE_HAVE_LIBUSB 0
#endif

#if DESKMATE_HAVE_LIBUSB

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wzero-length-array"
#pragma clang diagnostic ignored "-Wc99-extensions"
#pragma clang diagnostic ignored "-Wdeprecated-enum-enum-conversion"
#endif
#include <libusb-1.0/libusb.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cfgmgr32.h>
#include <setupapi.h>
#endif

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <sstream>
#include <thread>

namespace deskmate::windows {
namespace {

constexpr unsigned kControlTimeoutMs = 3000;
constexpr std::size_t kMaxRxBufferBytes = 256U * 1024U;
constexpr auto kAoaSwitchCooldown = std::chrono::seconds(5);
constexpr auto kExpectedIdentityTtl = std::chrono::seconds(20);
constexpr auto kPnpSettleGrace = std::chrono::seconds(3);

constexpr std::uint8_t kAoaOutRequest =
    static_cast<std::uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR) |
    static_cast<std::uint8_t>(LIBUSB_RECIPIENT_DEVICE) |
    static_cast<std::uint8_t>(LIBUSB_ENDPOINT_OUT);
constexpr std::uint8_t kAoaInRequest =
    static_cast<std::uint8_t>(LIBUSB_REQUEST_TYPE_VENDOR) |
    static_cast<std::uint8_t>(LIBUSB_RECIPIENT_DEVICE) |
    static_cast<std::uint8_t>(LIBUSB_ENDPOINT_IN);

std::string libusb_text(int rc) {
  const char* name = libusb_error_name(rc);
  return name != nullptr ? std::string(name) : ("err:" + std::to_string(rc));
}

std::string trim_ascii(std::string value) {
  std::string cleaned;
  cleaned.reserve(value.size());
  for (unsigned char ch : value) {
    if (ch >= 0x20 && ch != 0x7F) {
      cleaned.push_back(static_cast<char>(ch));
    }
  }
  const auto start = cleaned.find_first_not_of(" \t");
  if (start == std::string::npos) {
    return {};
  }
  const auto end = cleaned.find_last_not_of(" \t");
  return cleaned.substr(start, end - start + 1);
}

std::string read_string_descriptor(libusb_device_handle* handle,
                                   std::uint8_t index) {
  if (handle == nullptr || index == 0) {
    return {};
  }
  std::array<unsigned char, 256> buffer{};
  const int length = libusb_get_string_descriptor_ascii(
      handle, index, buffer.data(), static_cast<int>(buffer.size() - 1));
  if (length <= 0) {
    return {};
  }
  return trim_ascii(std::string(reinterpret_cast<char*>(buffer.data()),
                                static_cast<std::size_t>(length)));
}

std::string port_path_for(libusb_device* device, std::uint8_t bus) {
  if (device == nullptr) {
    return {};
  }
  std::array<std::uint8_t, 8> path{};
  const int count = libusb_get_port_numbers(device, path.data(),
                                            static_cast<int>(path.size()));
  if (count <= 0) {
    return {};
  }
  std::ostringstream out;
  out << static_cast<unsigned>(bus) << ':';
  for (int index = 0; index < count; ++index) {
    if (index > 0) {
      out << '.';
    }
    out << static_cast<unsigned>(path[static_cast<std::size_t>(index)]);
  }
  return out.str();
}

bool is_generic_name(const std::string& name) {
  const std::string t = trim_ascii(name);
  return t.empty() || t == "Android" || t == "Accessory" || t == "AOA" ||
         t == "DeskMate" || t == "DeskMate Extended Display" ||
         t == "DeskMate accessory" || t == "DeskMate device" ||
         t == "HarmonyOS 设备" || t == "DeskMate 设备";
}

bool is_known_mobile_vendor(std::uint16_t vid) noexcept {
  switch (vid) {
    case 0x12D1:  // Huawei / Honor (HDC / HiSuite / MTP)
    case 0x18D1:  // Google / Android
    case 0x04E8:  // Samsung
    case 0x2717:  // Xiaomi
    case 0x22D9:  // OPPO / Realme
    case 0x2A70:  // OnePlus
    case 0x2D95:  // vivo
    case 0x3511:  // Honor
    case 0x22B8:  // Motorola
    case 0x0FCE:  // Sony
    case 0x2970:  // ZUK / Lenovo mobile
    case 0x2A45:  // Meizu
    case 0x17EF:  // Lenovo
    case 0x0B05:  // ASUS
    case 0x19D2:  // ZTE
    case 0x0E8D:  // MediaTek Android
    case 0x05C6:  // Qualcomm Android
      return true;
    default:
      return false;
  }
}

bool has_adb_or_hdc_interface(libusb_device* device) {
  libusb_config_descriptor* config = nullptr;
  int rc = libusb_get_active_config_descriptor(device, &config);
  if (rc != LIBUSB_SUCCESS) {
    rc = libusb_get_config_descriptor(device, 0, &config);
  }
  if (rc != LIBUSB_SUCCESS || config == nullptr) {
    return false;
  }
  bool matched = false;
  for (std::uint8_t i = 0; i < config->bNumInterfaces && !matched; ++i) {
    const auto& iface = config->interface[i];
    for (int alt = 0; alt < iface.num_altsetting; ++alt) {
      const auto& d = iface.altsetting[alt];
      // ADB: FF/42/01, HDC: FF/50/01, Fastboot: FF/42/03
      if (d.bInterfaceClass == 0xFF &&
          ((d.bInterfaceSubClass == 0x42 &&
            (d.bInterfaceProtocol == 0x01 || d.bInterfaceProtocol == 0x03)) ||
           (d.bInterfaceSubClass == 0x50 && d.bInterfaceProtocol == 0x01))) {
        matched = true;
        break;
      }
    }
  }
  libusb_free_config_descriptor(config);
  return matched;
}

bool is_hid_or_hub_only(libusb_device* device,
                        const libusb_device_descriptor& desc) {
  if (desc.bDeviceClass == LIBUSB_CLASS_HUB) {
    return true;
  }
  libusb_config_descriptor* config = nullptr;
  int rc = libusb_get_active_config_descriptor(device, &config);
  if (rc != LIBUSB_SUCCESS) {
    rc = libusb_get_config_descriptor(device, 0, &config);
  }
  if (rc != LIBUSB_SUCCESS || config == nullptr) {
    return false;
  }
  bool any_non_hid = false;
  for (std::uint8_t i = 0; i < config->bNumInterfaces; ++i) {
    const auto& iface = config->interface[i];
    for (int alt = 0; alt < iface.num_altsetting; ++alt) {
      const auto cls = iface.altsetting[alt].bInterfaceClass;
      if (cls != LIBUSB_CLASS_HID && cls != LIBUSB_CLASS_HUB) {
        any_non_hid = true;
        break;
      }
    }
  }
  libusb_free_config_descriptor(config);
  return !any_non_hid;
}

bool has_aoa_bulk_interface(libusb_device* device) {
  libusb_config_descriptor* config = nullptr;
  int rc = libusb_get_active_config_descriptor(device, &config);
  if (rc != LIBUSB_SUCCESS) {
    rc = libusb_get_config_descriptor(device, 0, &config);
  }
  if (rc != LIBUSB_SUCCESS || config == nullptr) {
    return false;
  }
  bool matched = false;
  for (std::uint8_t i = 0; i < config->bNumInterfaces && !matched; ++i) {
    const auto& iface = config->interface[i];
    for (int alt = 0; alt < iface.num_altsetting; ++alt) {
      const auto& d = iface.altsetting[alt];
      if (d.bInterfaceClass != 0xFF || d.bInterfaceSubClass != 0xFF ||
          d.bInterfaceProtocol != 0x00) {
        continue;
      }
      bool has_in = false;
      bool has_out = false;
      for (std::uint8_t ep = 0; ep < d.bNumEndpoints; ++ep) {
        const auto& e = d.endpoint[ep];
        if ((e.bmAttributes & 0x03U) == 0x02U) {
          if ((e.bEndpointAddress & 0x80U) != 0) {
            has_in = true;
          } else {
            has_out = true;
          }
        }
      }
      if (has_in && has_out) {
        matched = true;
        break;
      }
    }
  }
  libusb_free_config_descriptor(config);
  return matched;
}

std::string describe_interfaces(libusb_device* device) {
  libusb_config_descriptor* config = nullptr;
  int rc = libusb_get_active_config_descriptor(device, &config);
  if (rc != LIBUSB_SUCCESS) {
    rc = libusb_get_config_descriptor(device, 0, &config);
  }
  if (rc != LIBUSB_SUCCESS || config == nullptr) {
    return {};
  }
  std::ostringstream out;
  bool first = true;
  for (std::uint8_t i = 0; i < config->bNumInterfaces; ++i) {
    const auto& iface = config->interface[i];
    if (iface.num_altsetting <= 0) {
      continue;
    }
    const auto& d = iface.altsetting[0];
    if (!first) {
      out << ',';
    }
    first = false;
    char buf[40]{};
    std::snprintf(buf, sizeof(buf), "if%u=%02x/%02x/%02x",
                  static_cast<unsigned>(d.bInterfaceNumber),
                  static_cast<unsigned>(d.bInterfaceClass),
                  static_cast<unsigned>(d.bInterfaceSubClass),
                  static_cast<unsigned>(d.bInterfaceProtocol));
    out << buf;
  }
  libusb_free_config_descriptor(config);
  return out.str();
}

bool select_aoa_bulk_endpoints(libusb_device_handle* handle,
                               deskmate::platform::EndpointPair& out) {
  libusb_device* dev = libusb_get_device(handle);
  libusb_config_descriptor* config = nullptr;
  int rc = libusb_get_active_config_descriptor(dev, &config);
  if (rc != LIBUSB_SUCCESS) {
    rc = libusb_get_config_descriptor(dev, 0, &config);
  }
  if (rc != LIBUSB_SUCCESS || config == nullptr) {
    return false;
  }
  bool found_preferred = false;
  std::vector<deskmate::platform::EndpointDescriptorView> all_views;
  for (std::uint8_t i = 0; i < config->bNumInterfaces; ++i) {
    const auto& iface = config->interface[i];
    for (int alt = 0; alt < iface.num_altsetting; ++alt) {
      const auto& d = iface.altsetting[alt];
      int pref_in = -1;
      int pref_out = -1;
      const bool is_aoa_class =
          (d.bInterfaceClass == 0xFF && d.bInterfaceSubClass == 0xFF &&
           d.bInterfaceProtocol == 0x00);
      for (std::uint8_t ep_idx = 0; ep_idx < d.bNumEndpoints; ++ep_idx) {
        const auto& ep = d.endpoint[ep_idx];
        all_views.push_back(deskmate::platform::EndpointDescriptorView{
            d.bInterfaceNumber, ep.bmAttributes, ep.bEndpointAddress});
        if (is_aoa_class && (ep.bmAttributes & 0x03U) == 0x02U) {
          if ((ep.bEndpointAddress & 0x80U) != 0) {
            if (pref_in < 0) pref_in = ep.bEndpointAddress;
          } else if (pref_out < 0) {
            pref_out = ep.bEndpointAddress;
          }
        }
      }
      if (!found_preferred && is_aoa_class && pref_in >= 0 && pref_out >= 0) {
        out.interface_number = d.bInterfaceNumber;
        out.endpoint_in = static_cast<std::uint8_t>(pref_in);
        out.endpoint_out = static_cast<std::uint8_t>(pref_out);
        found_preferred = true;
      }
    }
  }
  libusb_free_config_descriptor(config);
  if (found_preferred) {
    return true;
  }
  return deskmate::platform::select_bulk_endpoints(all_views, out);
}

bool is_aoa_switch_candidate(libusb_device* device,
                             const libusb_device_descriptor& desc) {
  if (deskmate::platform::is_aoa_accessory(desc.idVendor, desc.idProduct)) {
    return false;
  }
  if (desc.idVendor == deskmate::platform::kHdcVendorId &&
      has_aoa_bulk_interface(device)) {
    return false;
  }
  if (deskmate::platform::is_aoa_hdc_candidate(desc.idVendor, desc.idProduct)) {
    return true;
  }
  if (is_hid_or_hub_only(device, desc)) {
    return false;
  }
  if (has_adb_or_hdc_interface(device)) {
    return true;
  }
  return is_known_mobile_vendor(desc.idVendor);
}

bool send_aoa_string(libusb_device_handle* handle, std::uint16_t index,
                     std::string_view text, std::string& error) {
  const auto bytes = deskmate::platform::encode_aoa_string(text);
  const int rc = libusb_control_transfer(
      handle, kAoaOutRequest, deskmate::platform::kAoaSendString, 0, index,
      const_cast<unsigned char*>(bytes.data()),
      static_cast<std::uint16_t>(bytes.size()), kControlTimeoutMs);
  if (rc < 0) {
    error = "AOA SEND_STRING(" + std::to_string(index) +
            ") failed: " + libusb_text(rc);
    return false;
  }
  return true;
}

// On Windows composite USB devices (WinUSB / UsbDk), EP0 vendor control
// transfers sometimes require claiming an interface first (or fall back
// cleanly if the composite parent handles EP0 directly).
int claim_any_interface_for_control(libusb_device_handle* handle) {
  libusb_device* dev = libusb_get_device(handle);
  libusb_config_descriptor* config = nullptr;
  int rc = libusb_get_active_config_descriptor(dev, &config);
  if (rc != LIBUSB_SUCCESS) {
    rc = libusb_get_config_descriptor(dev, 0, &config);
  }
  if (rc != LIBUSB_SUCCESS || config == nullptr) {
    return -1;
  }
  int claimed = -1;
  for (std::uint8_t i = 0; i < config->bNumInterfaces; ++i) {
    if (config->interface[i].num_altsetting == 0) {
      continue;
    }
    const int ifnum = config->interface[i].altsetting[0].bInterfaceNumber;
    if (libusb_claim_interface(handle, ifnum) == LIBUSB_SUCCESS) {
      claimed = ifnum;
      break;
    }
  }
  libusb_free_config_descriptor(config);
  return claimed;
}

bool switch_to_aoa_control(libusb_device_handle* handle, std::string& error,
                           bool& unsupported_out) {
  unsupported_out = false;
  std::uint8_t protocol[2]{};
  int claimed_if = -1;
  int rc = libusb_control_transfer(
      handle, kAoaInRequest, deskmate::platform::kAoaGetProtocol, 0, 0,
      protocol, static_cast<std::uint16_t>(sizeof(protocol)), kControlTimeoutMs);
  if (rc == LIBUSB_ERROR_ACCESS || rc == LIBUSB_ERROR_NOT_FOUND ||
      rc == LIBUSB_ERROR_NOT_SUPPORTED) {
    claimed_if = claim_any_interface_for_control(handle);
    if (claimed_if >= 0) {
      rc = libusb_control_transfer(
          handle, kAoaInRequest, deskmate::platform::kAoaGetProtocol, 0, 0,
          protocol, static_cast<std::uint16_t>(sizeof(protocol)),
          kControlTimeoutMs);
    }
  }

  auto release_claimed = [&] {
    if (claimed_if >= 0) {
      libusb_release_interface(handle, claimed_if);
      claimed_if = -1;
    }
  };

  if (rc < 0) {
    if (rc == LIBUSB_ERROR_NOT_SUPPORTED || rc == LIBUSB_ERROR_ACCESS) {
      unsupported_out = true;
    }
    error = "AOA GET_PROTOCOL failed: " + libusb_text(rc);
    release_claimed();
    return false;
  }
  const std::uint16_t version =
      static_cast<std::uint16_t>(protocol[0]) |
      (static_cast<std::uint16_t>(protocol[1]) << 8U);
  if (version < 1) {
    error = "AOA GET_PROTOCOL returned version 0";
    release_claimed();
    return false;
  }

  // Send standard AOA identification strings matching accessory_filter.xml and
  // HarmonyOS AccessoryLink expectations (null-terminated UTF-8).
  const std::array<std::string, 4> strings{
      "DeskMate", "DeskMate Extended Display",
      "Wired extended display for HarmonyOS devices", "1.0"};
  for (std::size_t index = 0; index < strings.size(); ++index) {
    if (!send_aoa_string(handle, static_cast<std::uint16_t>(index),
                         strings[index], error)) {
      release_claimed();
      return false;
    }
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  std::uint8_t no_data = 0;
  rc = libusb_control_transfer(
      handle, kAoaOutRequest, deskmate::platform::kAoaStart, 0, 0, &no_data, 0,
      kControlTimeoutMs);
  release_claimed();
  // On some Windows USB stacks, the device immediately drops the bus upon
  // receiving AOA START, causing LIBUSB_ERROR_NO_DEVICE, LIBUSB_ERROR_IO, or
  // LIBUSB_ERROR_PIPE on the status stage even though the switch succeeded.
  if (rc < 0 && rc != LIBUSB_ERROR_NO_DEVICE && rc != LIBUSB_ERROR_IO &&
      rc != LIBUSB_ERROR_PIPE) {
    error = "AOA START failed: " + libusb_text(rc);
    return false;
  }
  return true;
}

std::string cooldown_key_for(const ScannedUsbDevice& candidate) {
  if (!candidate.identity.port_path.empty()) {
    return "port:" + candidate.identity.port_path;
  }
  return deskmate::platform::aoa_device_id(candidate.identity);
}

#ifdef _WIN32
std::string wide_to_utf8_usb(const wchar_t* text) {
  if (text == nullptr || text[0] == L'\0') {
    return {};
  }
  const int needed =
      WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
  if (needed <= 1) {
    return {};
  }
  std::string out(static_cast<std::size_t>(needed - 1), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), needed, nullptr,
                      nullptr);
  return out;
}

std::string ascii_lower(std::string s) {
  for (char& ch : s) {
    ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
  }
  return s;
}

bool is_supported_winusb_service(const std::string& lower_service) {
  return lower_service == "winusb" || lower_service == "libusbk" ||
         lower_service == "libusb0" || lower_service == "usbdk";
}

void probe_windows_pnp_aoa(std::string& summary_out, bool& driver_missing_out,
                           std::string& driver_hint_out) {
  summary_out.clear();
  driver_missing_out = false;
  driver_hint_out.clear();

  HDEVINFO dev_info_set =
      SetupDiGetClassDevsW(nullptr, L"USB", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
  if (dev_info_set == INVALID_HANDLE_VALUE) {
    return;
  }

  std::ostringstream summary;
  bool first = true;
  SP_DEVINFO_DATA dev_info{};
  dev_info.cbSize = sizeof(dev_info);
  for (DWORD idx = 0; SetupDiEnumDeviceInfo(dev_info_set, idx, &dev_info);
       ++idx) {
    wchar_t instance_id[512]{};
    if (!SetupDiGetDeviceInstanceIdW(
            dev_info_set, &dev_info, instance_id,
            static_cast<DWORD>(std::size(instance_id)), nullptr)) {
      continue;
    }
    const std::string id_utf8 = wide_to_utf8_usb(instance_id);
    const std::string id_low = ascii_lower(id_utf8);
    const bool is_aoa_18d1 =
        (id_low.find("vid_18d1&pid_2d0") != std::string::npos);
    const bool is_hdc_12d1 = (id_low.find("vid_12d1") != std::string::npos);
    if (!is_aoa_18d1 && !is_hdc_12d1) {
      continue;
    }

    wchar_t service_buf[128]{};
    DWORD reg_type = 0;
    std::string service;
    if (SetupDiGetDeviceRegistryPropertyW(
            dev_info_set, &dev_info, SPDRP_SERVICE, &reg_type,
            reinterpret_cast<BYTE*>(service_buf), sizeof(service_buf),
            nullptr)) {
      service = wide_to_utf8_usb(service_buf);
    }
    const std::string service_low = ascii_lower(service);

    ULONG dev_status = 0;
    ULONG problem_code = 0;
    const CONFIGRET cr =
        CM_Get_DevNode_Status(&dev_status, &problem_code, dev_info.DevInst, 0);
    if (cr != CR_SUCCESS) {
      problem_code = 0;
    }

    if (!first) {
      summary << "; ";
    }
    first = false;
    summary << id_utf8 << " [svc=" << (service.empty() ? "none" : service)
            << ",prob=" << problem_code << "]";

    if (is_aoa_18d1) {
      const bool has_mi = (id_low.find("&mi_") != std::string::npos);
      const bool is_mi_00 = (id_low.find("&mi_00") != std::string::npos);
      if (has_mi) {
        if (is_mi_00 &&
            (!is_supported_winusb_service(service_low) || problem_code != 0)) {
          driver_missing_out = true;
        }
      } else {
        // Standalone 18D1:2D0x, or composite parent (service=usbccgp is normal).
        if (service_low != "usbccgp" &&
            (!is_supported_winusb_service(service_low) || problem_code != 0)) {
          driver_missing_out = true;
        }
      }
      if (driver_missing_out && driver_hint_out.empty()) {
        driver_hint_out =
            "检测到 Windows USB 配件节点 " + id_utf8 + "（当前驱动：" +
            (service.empty() ? "未安装" : service) + "，错误码 " +
            std::to_string(problem_code) +
            "），libusb 无法直接读写。请用 Zadig 为 Android Accessory (18D1:2D0x / Interface 0) 安装 WinUSB 驱动，或安装 UsbDk 后点击重试。";
      }
    }
  }
  SetupDiDestroyDeviceInfoList(dev_info_set);
  summary_out = summary.str();
}
#endif  // _WIN32

}  // namespace

LibusbBulkTransport::LibusbBulkTransport(libusb_device_handle* handle,
                                         std::uint8_t endpoint_in,
                                         std::uint8_t endpoint_out,
                                         std::string device_id,
                                         int interface_number,
                                         libusb_context* context)
    : handle_(handle),
      endpoint_in_(endpoint_in),
      endpoint_out_(endpoint_out),
      interface_number_(interface_number),
      context_(context),
      device_id_(std::move(device_id)) {
  for (std::size_t i = 0; i < kInTransferCount; ++i) {
    in_transfers_[i] = libusb_alloc_transfer(0);
  }
  for (auto& slot : out_slots_) {
    slot.transfer = libusb_alloc_transfer(0);
    slot.buffer.resize(kMaxOutTransferBytes);
  }
  events_ = std::thread([this] { event_loop(); });
  // Pre-post every IN URB so the device's replies never wait for a new
  // submit() round trip; the completion handler re-arms them immediately.
  for (std::size_t i = 0; i < kInTransferCount; ++i) {
    submit_in_transfer(i);
  }
}

LibusbBulkTransport::~LibusbBulkTransport() {
  cancel();
  if (events_.joinable()) {
    events_.join();
  }
  free_transfers();
  if (handle_ != nullptr) {
    if (interface_number_ >= 0) {
      libusb_release_interface(handle_, interface_number_);
      interface_number_ = -1;
    }
    libusb_close(handle_);
    handle_ = nullptr;
  }
}

void LibusbBulkTransport::submit_in_transfer(std::size_t index) {
  libusb_transfer* transfer = in_transfers_[index];
  if (transfer == nullptr || handle_ == nullptr ||
      stop_.load(std::memory_order_acquire)) {
    return;
  }
  libusb_fill_bulk_transfer(
      transfer, handle_, endpoint_in_, in_buffers_[index].data(),
      static_cast<int>(kInTransferBytes), &LibusbBulkTransport::on_in_complete,
      this, 0U);
  // Count before submitting: libusb may complete the URB on the event thread
  // before submit_transfer() returns to us.
  active_transfers_.fetch_add(1, std::memory_order_acq_rel);
  const int rc = libusb_submit_transfer(transfer);
  if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_BUSY) {
    active_transfers_.fetch_sub(1, std::memory_order_acq_rel);
    report_error("bulk IN submit failed: " + libusb_text(rc));
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
      closed_.store(true, std::memory_order_release);
      cv_.notify_all();
      notify_rx();
    }
  }
}

void LIBUSB_CALL LibusbBulkTransport::on_in_complete(
    libusb_transfer* transfer) {
  if (transfer == nullptr || transfer->user_data == nullptr) {
    return;
  }
  static_cast<LibusbBulkTransport*>(transfer->user_data)
      ->handle_in_complete(transfer);
}

void LIBUSB_CALL LibusbBulkTransport::on_out_complete(
    libusb_transfer* transfer) {
  if (transfer == nullptr || transfer->user_data == nullptr) {
    return;
  }
  static_cast<LibusbBulkTransport*>(transfer->user_data)
      ->handle_out_complete(transfer);
}

void LibusbBulkTransport::handle_in_complete(libusb_transfer* transfer) {
  // Decremented at scope exit, never at entry: the destructor must not free a
  // transfer whose callback is still running.
  struct ActiveGuard {
    std::atomic<int>& counter;
    ~ActiveGuard() { counter.fetch_sub(1, std::memory_order_acq_rel); }
  } guard{active_transfers_};
  const auto found =
      std::find(in_transfers_.begin(), in_transfers_.end(), transfer);
  if (found == in_transfers_.end()) {
    return;
  }
  const std::size_t index =
      static_cast<std::size_t>(found - in_transfers_.begin());
  if (stop_.load(std::memory_order_acquire)) {
    return;
  }
  if (transfer->status == LIBUSB_TRANSFER_COMPLETED) {
    consecutive_in_errors_.store(0, std::memory_order_relaxed);
    const int transferred = transfer->actual_length;
    if (transferred > 0) {
      rx_bytes_.fetch_add(static_cast<std::uint64_t>(transferred),
                          std::memory_order_relaxed);
      {
        std::lock_guard lock(mutex_);
        if (rx_buffer_.size() + static_cast<std::size_t>(transferred) >
            kMaxRxBufferBytes) {
          const std::size_t drop =
              rx_buffer_.size() + static_cast<std::size_t>(transferred) -
              kMaxRxBufferBytes;
          rx_buffer_.erase(
              rx_buffer_.begin(),
              rx_buffer_.begin() + static_cast<std::ptrdiff_t>(
                                       std::min(drop, rx_buffer_.size())));
        }
        rx_buffer_.insert(rx_buffer_.end(),
                          transfer->buffer,
                          transfer->buffer + transferred);
      }
      cv_.notify_all();
      notify_rx();
    }
    submit_in_transfer(index);
    return;
  }
  if (transfer->status == LIBUSB_TRANSFER_CANCELLED ||
      transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
    if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE) {
      closed_.store(true, std::memory_order_release);
      cv_.notify_all();
      notify_rx();
    } else if (!stop_.load(std::memory_order_acquire)) {
      // WinUSB aborts every queued URB on an endpoint when one transfer is
      // cancelled, so a cancellation that did not come from cancel() must
      // re-arm or the read side silently loses a slot each time.
      submit_in_transfer(index);
    }
    return;
  }
  // STALL / ERROR: fail the transport and let the host reconnect cleanly.
  // libusb_clear_halt() is a synchronous EP0 control transfer and must never
  // run from this callback: it re-enters the libusb event handler and resets
  // the device's DATA0/DATA1 toggle, which makes the Android AOA driver drop
  // the pipe. Re-arming a stalled endpoint would only spin here.
  bulk_errors_.fetch_add(1, std::memory_order_relaxed);
  const bool transient_error = transfer->status == LIBUSB_TRANSFER_ERROR &&
                               consecutive_in_errors_.fetch_add(
                                   1, std::memory_order_relaxed) + 1 < 3;
  if (transient_error && !stop_.load(std::memory_order_acquire)) {
    // IN URBs use an infinite timeout, so a stray error is usually a one-off
    // glitch; re-arm and let the next completion decide.
    submit_in_transfer(index);
    return;
  }
  report_error("bulk IN failed: " +
               std::string(libusb_error_name(transfer->status)));
  failed_.store(true, std::memory_order_release);
  notify_rx();
}

void LibusbBulkTransport::handle_out_complete(libusb_transfer* transfer) {
  struct ActiveGuard {
    std::atomic<int>& counter;
    ~ActiveGuard() { counter.fetch_sub(1, std::memory_order_acq_rel); }
  } guard{active_transfers_};
  OutSlot* slot = nullptr;
  for (auto& candidate : out_slots_) {
    if (candidate.transfer == transfer) {
      slot = &candidate;
      break;
    }
  }
  if (slot == nullptr) {
    return;
  }
  switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
      if (transfer->actual_length > 0) {
        tx_bytes_.fetch_add(static_cast<std::uint64_t>(transfer->actual_length),
                            std::memory_order_relaxed);
      }
      if (transfer->actual_length < transfer->length) {
        // Bytes already handed to the URB cannot be rewound, so a short write
        // would leave a hole in the framed stream. Fail the link instead: the
        // session then tears down and re-handshakes from a clean offset.
        bulk_errors_.fetch_add(1, std::memory_order_relaxed);
        write_blocked_.store(true, std::memory_order_relaxed);
        failed_.store(true, std::memory_order_release);
        notify_rx();
        report_error("bulk OUT short write: " +
                     std::to_string(transfer->actual_length) + "/" +
                     std::to_string(transfer->length));
      } else {
        consecutive_write_errors_.store(0, std::memory_order_relaxed);
      }
      break;
    case LIBUSB_TRANSFER_TIMED_OUT:
      // write() already reported the full packet as accepted, so core has
      // popped it. A timed-out URB therefore leaves a hole in the framed
      // stream that no retry can fill: fail the link and re-handshake.
      // With the 2 s timeout this only fires on a genuinely wedged pipe.
      write_timeouts_.fetch_add(1, std::memory_order_relaxed);
      write_blocked_.store(true, std::memory_order_relaxed);
      failed_.store(true, std::memory_order_release);
      notify_rx();
      report_error("bulk OUT timed out after " +
                   std::to_string(write_timeout_ms_.load(
                       std::memory_order_relaxed)) +
                   "ms");
      break;
    case LIBUSB_TRANSFER_NO_DEVICE:
      closed_.store(true, std::memory_order_release);
      cv_.notify_all();
      notify_rx();
      break;
    case LIBUSB_TRANSFER_CANCELLED:
      break;
    default:
      bulk_errors_.fetch_add(1, std::memory_order_relaxed);
      write_blocked_.store(true, std::memory_order_relaxed);
      failed_.store(true, std::memory_order_release);
      notify_rx();
      report_error("bulk OUT failed: " +
                   std::string(libusb_error_name(transfer->status)));
      break;
  }
  slot->busy.store(false, std::memory_order_release);
  // A queued frame was refused for lack of a free URB; now that one retired,
  // wake the link worker instead of letting it poll every 1 ms. Use the
  // dedicated flag: write_blocked_ is consumed by consume_write_blocked() on
  // the worker thread, so testing it here would always see false.
  if (out_wake_needed_.exchange(false, std::memory_order_acq_rel)) {
    notify_rx();
  }
}

void LibusbBulkTransport::event_loop() {
#ifdef _WIN32
  SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
#endif
  auto drain_deadline = std::chrono::steady_clock::time_point{};
  while (true) {
    const bool stopping = stop_.load(std::memory_order_acquire);
    if (stopping) {
      if (active_transfers_.load(std::memory_order_acquire) <= 0) {
        return;
      }
      if (drain_deadline.time_since_epoch().count() == 0) {
        drain_deadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(1);
      } else if (std::chrono::steady_clock::now() > drain_deadline) {
        return;
      }
    }
    timeval tv{};
    tv.tv_sec = 0;
    tv.tv_usec = 50'000;
    const int rc =
        libusb_handle_events_timeout_completed(context_, &tv, nullptr);
    if (rc != LIBUSB_SUCCESS && rc != LIBUSB_ERROR_INTERRUPTED &&
        rc != LIBUSB_ERROR_TIMEOUT) {
      report_error("libusb_handle_events failed: " + libusb_text(rc));
      if (rc == LIBUSB_ERROR_NO_DEVICE || rc == LIBUSB_ERROR_IO) {
        // Keep draining: in-flight URBs still have to complete their
        // callbacks before the transport is destroyed.
        closed_.store(true, std::memory_order_release);
        notify_rx();
        // handle_events() returns immediately once the device is gone; sleep
        // rather than spin until the outstanding callbacks drain.
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
    }
  }
}

void LibusbBulkTransport::free_transfers() {
  for (auto*& transfer : in_transfers_) {
    if (transfer != nullptr) {
      libusb_free_transfer(transfer);
      transfer = nullptr;
    }
  }
  for (auto& slot : out_slots_) {
    if (slot.transfer != nullptr) {
      libusb_free_transfer(slot.transfer);
      slot.transfer = nullptr;
    }
  }
}

void LibusbBulkTransport::cancel_all_transfers() {
  for (auto* transfer : in_transfers_) {
    if (transfer != nullptr) {
      (void)libusb_cancel_transfer(transfer);
    }
  }
  for (auto& slot : out_slots_) {
    if (slot.transfer != nullptr) {
      (void)libusb_cancel_transfer(slot.transfer);
    }
  }
}

void LibusbBulkTransport::report_error(const std::string& message) {
  {
    std::lock_guard lock(mutex_);
    last_error_ = message;
  }
}

void LibusbBulkTransport::set_stream_fps(int fps) {
  // Async URBs do not block the sender, so scaling the timeout by frame rate
  // is counter-productive: at 120 Hz it used to become 25 ms, and every URB
  // in a 6-slot pipeline started counting down at submit time. On WinUSB one
  // expired URB calls WinUsb_AbortPipe() and cancels the whole endpoint, which
  // is exactly what made the link drop mid-stream. Keep a generous bound and
  // let the session heartbeat decide whether the device is gone.
  (void)fps;
  write_timeout_ms_.store(2000, std::memory_order_release);
}

void LibusbBulkTransport::wake_reader() { cv_.notify_all(); }

void LibusbBulkTransport::set_rx_callback(std::function<void()> callback) {
  std::lock_guard lock(rx_cb_mutex_);
  rx_callback_ = std::move(callback);
}

bool LibusbBulkTransport::consume_write_blocked() noexcept {
  return write_blocked_.exchange(false, std::memory_order_relaxed);
}

void LibusbBulkTransport::notify_rx() {
  std::function<void()> cb;
  {
    std::lock_guard lock(rx_cb_mutex_);
    cb = rx_callback_;
  }
  if (cb) {
    cb();
  }
}

deskmate::protocol::IoResult LibusbBulkTransport::read(
    std::span<std::uint8_t> destination) {
  std::lock_guard lock(mutex_);
  if (!rx_buffer_.empty()) {
    const std::size_t take = std::min(destination.size(), rx_buffer_.size());
    std::copy_n(rx_buffer_.begin(), take, destination.begin());
    rx_buffer_.erase(rx_buffer_.begin(),
                     rx_buffer_.begin() + static_cast<std::ptrdiff_t>(take));
    return {deskmate::protocol::IoStatus::Ok, take};
  }
  if (closed_.load(std::memory_order_acquire)) {
    return {deskmate::protocol::IoStatus::Closed, 0};
  }
  if (failed_.load(std::memory_order_acquire)) {
    return {deskmate::protocol::IoStatus::Error, 0};
  }
  return {deskmate::protocol::IoStatus::WouldBlock, 0};
}

deskmate::protocol::IoResult LibusbBulkTransport::write(
    std::span<const std::uint8_t> source) {
  if (handle_ == nullptr || closed_.load(std::memory_order_acquire)) {
    return {deskmate::protocol::IoStatus::Closed, 0};
  }
  if (failed_.load(std::memory_order_acquire)) {
    // An async URB that died mid-packet already left a hole in the framed
    // stream, so no later write can repair it: report a hard error and let the
    // session re-handshake from a clean offset.
    return {deskmate::protocol::IoStatus::Error, 0};
  }
  if (source.empty()) {
    return {deskmate::protocol::IoStatus::Ok, 0};
  }
  // ByteIo allows partial writes, and core already advances its offset by the
  // returned byte count. A large keyframe must therefore be split into
  // kMaxOutTransferBytes chunks instead of failing the whole session.
  const std::size_t chunk = std::min(source.size(), kMaxOutTransferBytes);
  // Find a free slot; the whole point of the pool is that a submit never
  // blocks the pump thread waiting for the previous URB to retire.
  // Arm the wake flag *before* scanning: a completion that races with this
  // write either frees a slot we then take, or sees the flag and notifies.
  out_wake_needed_.store(true, std::memory_order_release);
  OutSlot* slot = nullptr;
  for (auto& candidate : out_slots_) {
    if (candidate.transfer == nullptr) {
      continue;
    }
    bool expected = false;
    if (candidate.busy.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      slot = &candidate;
      break;
    }
  }
  if (slot == nullptr) {
    write_blocked_.store(true, std::memory_order_relaxed);
    return {deskmate::protocol::IoStatus::WouldBlock, 0};
  }
  out_wake_needed_.store(false, std::memory_order_release);
  std::memcpy(slot->buffer.data(), source.data(), chunk);
  libusb_fill_bulk_transfer(
      slot->transfer, handle_, endpoint_out_, slot->buffer.data(),
      static_cast<int>(chunk),
      &LibusbBulkTransport::on_out_complete, this,
      static_cast<unsigned>(write_timeout_ms_.load(std::memory_order_relaxed)));
  active_transfers_.fetch_add(1, std::memory_order_acq_rel);
  const int rc = libusb_submit_transfer(slot->transfer);
  if (rc != LIBUSB_SUCCESS) {
    active_transfers_.fetch_sub(1, std::memory_order_acq_rel);
    slot->busy.store(false, std::memory_order_release);
    out_wake_needed_.store(true, std::memory_order_release);
    report_error("bulk OUT submit failed: " + libusb_text(rc));
    bulk_errors_.fetch_add(1, std::memory_order_relaxed);
    write_blocked_.store(true, std::memory_order_relaxed);
    if (rc == LIBUSB_ERROR_NO_DEVICE) {
      closed_.store(true, std::memory_order_release);
      notify_rx();
      return {deskmate::protocol::IoStatus::Closed, 0};
    }
    return {deskmate::protocol::IoStatus::WouldBlock, 0};
  }
  return {deskmate::protocol::IoStatus::Ok, chunk};
}

void LibusbBulkTransport::cancel() {
  if (stop_.exchange(true, std::memory_order_acq_rel)) {
    return;
  }
  cancel_all_transfers();
  wake_reader();
  notify_rx();
}

bool LibusbBulkTransport::alive() const {
  return handle_ != nullptr && !stop_.load(std::memory_order_acquire) &&
         !closed_.load(std::memory_order_acquire) &&
         !failed_.load(std::memory_order_acquire);
}

LibusbAoaDiscovery::LibusbAoaDiscovery() {
  if (libusb_init(&context_) != LIBUSB_SUCCESS) {
    context_ = nullptr;
    last_error_ = "libusb_init failed";
  }
}

LibusbAoaDiscovery::~LibusbAoaDiscovery() {
  if (context_ != nullptr) {
    libusb_exit(context_);
    context_ = nullptr;
  }
}

void LibusbAoaDiscovery::set_log_callback(
    std::function<void(std::string)> callback) {
  std::lock_guard lock(log_mutex_);
  log_callback_ = std::move(callback);
}

void LibusbAoaDiscovery::emit_log(const std::string& message) const {
  std::function<void(std::string)> cb;
  {
    std::lock_guard lock(log_mutex_);
    cb = log_callback_;
  }
  if (cb) {
    cb(message);
  }
}

void LibusbAoaDiscovery::remember_pre_switch_identity(
    const deskmate::platform::AoaIdentity& identity,
    const std::string& cooldown_key) {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(cache_mutex_);
  if (!identity.serial.empty()) {
    hdc_identity_by_serial_[identity.serial] = identity;
  }
  if (!identity.port_path.empty()) {
    hdc_identity_by_port_[identity.port_path] = identity;
  }
  last_switched_identity_ = {identity, now + kExpectedIdentityTtl};
  has_last_switched_ = true;
  last_aoa_start_at_ = now;
  aoa_switch_cooldown_[cooldown_key] = now + kAoaSwitchCooldown;
}

void LibusbAoaDiscovery::resolve_accessory_identity(
    deskmate::platform::AoaIdentity& identity) const {
  const auto now = std::chrono::steady_clock::now();
  std::lock_guard lock(cache_mutex_);
  if (!identity.serial.empty()) {
    const auto it = hdc_identity_by_serial_.find(identity.serial);
    if (it != hdc_identity_by_serial_.end()) {
      if (is_generic_name(identity.product) &&
          !is_generic_name(it->second.product)) {
        identity.product = it->second.product;
      }
      if (identity.manufacturer.empty()) {
        identity.manufacturer = it->second.manufacturer;
      }
      return;
    }
  }
  if (!identity.port_path.empty()) {
    const auto it = hdc_identity_by_port_.find(identity.port_path);
    if (it != hdc_identity_by_port_.end()) {
      if (identity.serial.empty()) {
        identity.serial = it->second.serial;
      }
      if (is_generic_name(identity.product) &&
          !is_generic_name(it->second.product)) {
        identity.product = it->second.product;
      }
      if (identity.manufacturer.empty()) {
        identity.manufacturer = it->second.manufacturer;
      }
      return;
    }
  }
  if (has_last_switched_ && now <= last_switched_identity_.expires_at) {
    if (identity.serial.empty()) {
      identity.serial = last_switched_identity_.identity.serial;
    }
    if (is_generic_name(identity.product) &&
        !is_generic_name(last_switched_identity_.identity.product)) {
      identity.product = last_switched_identity_.identity.product;
    }
    if (identity.manufacturer.empty()) {
      identity.manufacturer = last_switched_identity_.identity.manufacturer;
    }
  }
}

bool LibusbAoaDiscovery::is_in_switch_cooldown(
    const ScannedUsbDevice& candidate) const {
  const auto now = std::chrono::steady_clock::now();
  const std::string key = cooldown_key_for(candidate);
  std::lock_guard lock(cache_mutex_);
  const auto it = aoa_switch_cooldown_.find(key);
  return it != aoa_switch_cooldown_.end() && now < it->second;
}

std::vector<ScannedUsbDevice> LibusbAoaDiscovery::scan_all_devices(
    const std::unordered_set<std::uint16_t>& owned_bus_addrs) {
  std::vector<ScannedUsbDevice> results;
  if (context_ == nullptr) {
    return results;
  }
  libusb_device** devices = nullptr;
  const ssize_t count = libusb_get_device_list(context_, &devices);
  if (count < 0) {
    return results;
  }
  for (ssize_t index = 0; index < count; ++index) {
    libusb_device* device = devices[index];
    libusb_device_descriptor descriptor{};
    if (libusb_get_device_descriptor(device, &descriptor) != LIBUSB_SUCCESS) {
      continue;
    }
    const bool accessory =
        deskmate::platform::is_aoa_accessory(descriptor.idVendor,
                                             descriptor.idProduct) ||
        (descriptor.idVendor == deskmate::platform::kHdcVendorId &&
         has_aoa_bulk_interface(device));
    const bool candidate =
        !accessory && is_aoa_switch_candidate(device, descriptor);
    if (!accessory && !candidate) {
      continue;
    }
    ScannedUsbDevice entry;
    entry.vendor_id = descriptor.idVendor;
    entry.product_id = descriptor.idProduct;
    entry.in_accessory_mode = accessory;
    entry.identity.bus = libusb_get_bus_number(device);
    entry.identity.address = libusb_get_device_address(device);
    entry.identity.port_path = port_path_for(device, entry.identity.bus);
    entry.interfaces_summary = describe_interfaces(device);

    const std::uint16_t bus_addr = static_cast<std::uint16_t>(
        (static_cast<std::uint16_t>(entry.identity.bus) << 8U) |
        static_cast<std::uint16_t>(entry.identity.address));
    const bool already_owned = owned_bus_addrs.count(bus_addr) != 0;

    if (!already_owned) {
      libusb_device_handle* handle = nullptr;
      if (libusb_open(device, &handle) == LIBUSB_SUCCESS && handle != nullptr) {
        entry.identity.serial =
            read_string_descriptor(handle, descriptor.iSerialNumber);
        entry.identity.manufacturer =
            read_string_descriptor(handle, descriptor.iManufacturer);
        entry.identity.product =
            read_string_descriptor(handle, descriptor.iProduct);
        libusb_close(handle);
      }
    }

    if (accessory) {
      resolve_accessory_identity(entry.identity);
      if (is_generic_name(entry.identity.product)) {
        entry.identity.product =
            !entry.identity.manufacturer.empty()
                ? (entry.identity.manufacturer + " 设备")
                : "HarmonyOS / Android 设备";
      }
    } else {
      if (is_generic_name(entry.identity.product)) {
        entry.identity.product =
            !entry.identity.manufacturer.empty()
                ? (entry.identity.manufacturer + " 设备")
                : "HarmonyOS / Android 设备";
      }
      if (!already_owned) {
        std::lock_guard lock(cache_mutex_);
        if (!entry.identity.serial.empty()) {
          hdc_identity_by_serial_[entry.identity.serial] = entry.identity;
        }
        if (!entry.identity.port_path.empty()) {
          hdc_identity_by_port_[entry.identity.port_path] = entry.identity;
        }
      }
    }
    results.push_back(std::move(entry));
  }
  libusb_free_device_list(devices, 1);
  return results;
}

bool LibusbAoaDiscovery::trigger_aoa_switch(const ScannedUsbDevice& candidate,
                                              std::string& error,
                                              bool& driver_missing_out) {
  driver_missing_out = false;
  if (context_ == nullptr) {
    error = "libusb context is not initialized";
    return false;
  }
  libusb_device** devices = nullptr;
  const ssize_t count = libusb_get_device_list(context_, &devices);
  if (count < 0) {
    error = "libusb_get_device_list failed";
    return false;
  }

  libusb_device* target = nullptr;
  for (ssize_t index = 0; index < count; ++index) {
    libusb_device* device = devices[index];
    const std::uint8_t bus = libusb_get_bus_number(device);
    const std::uint8_t addr = libusb_get_device_address(device);
    const std::string path = port_path_for(device, bus);
    const bool match_addr =
        (bus == candidate.identity.bus && addr == candidate.identity.address);
    const bool match_port =
        (!candidate.identity.port_path.empty() &&
         path == candidate.identity.port_path);
    if (!match_addr && !match_port) {
      continue;
    }
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(device, &desc) == LIBUSB_SUCCESS &&
        desc.idVendor == candidate.vendor_id &&
        desc.idProduct == candidate.product_id) {
      target = device;
      break;
    }
  }
  if (target == nullptr) {
    libusb_free_device_list(devices, 1);
    error = "device disappeared before AOA switch";
    return false;
  }

  libusb_device_handle* handle = nullptr;
  const int open_rc = libusb_open(target, &handle);
  libusb_free_device_list(devices, 1);
  if (open_rc != LIBUSB_SUCCESS || handle == nullptr) {
    if (open_rc == LIBUSB_ERROR_NOT_SUPPORTED ||
        open_rc == LIBUSB_ERROR_ACCESS) {
      driver_missing_out = true;
      error = "无法打开 USB 设备（" + libusb_text(open_rc) +
              "）：Windows 上需为设备绑定 WinUSB 或安装 UsbDk 驱动（如 Zadig）";
    } else {
      error = "libusb_open failed: " + libusb_text(open_rc);
    }
    return false;
  }

  bool unsupported = false;
  const bool ok = switch_to_aoa_control(handle, error, unsupported);
  libusb_close(handle);

  const std::string key = cooldown_key_for(candidate);
  if (ok) {
    remember_pre_switch_identity(candidate.identity, key);
    char vid_pid[32]{};
    std::snprintf(vid_pid, sizeof(vid_pid), "%04x:%04x", candidate.vendor_id,
                  candidate.product_id);
    emit_log("[discovery] sent AOA START to " + candidate.identity.product +
             " (" + vid_pid + ", port=" + candidate.identity.port_path + ")");
    return true;
  }
  if (unsupported) {
    driver_missing_out = true;
  }
  std::lock_guard lock(cache_mutex_);
  aoa_switch_cooldown_[key] =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  return false;
}

std::unique_ptr<LinkTransport> LibusbAoaDiscovery::open_accessory(
    const ScannedUsbDevice& accessory, std::string& error,
    bool& driver_missing_out) {
  driver_missing_out = false;
  if (context_ == nullptr) {
    error = "libusb context is not initialized";
    return nullptr;
  }
  libusb_device** devices = nullptr;
  const ssize_t count = libusb_get_device_list(context_, &devices);
  if (count < 0) {
    error = "libusb_get_device_list failed";
    return nullptr;
  }

  libusb_device* target = nullptr;
  std::uint16_t found_vid = accessory.vendor_id;
  std::uint16_t found_pid = accessory.product_id;
  for (ssize_t index = 0; index < count; ++index) {
    libusb_device* device = devices[index];
    const std::uint8_t bus = libusb_get_bus_number(device);
    const std::uint8_t addr = libusb_get_device_address(device);
    const std::string path = port_path_for(device, bus);
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(device, &desc) != LIBUSB_SUCCESS) {
      continue;
    }
    const bool is_acc =
        deskmate::platform::is_aoa_accessory(desc.idVendor, desc.idProduct) ||
        (desc.idVendor == deskmate::platform::kHdcVendorId &&
         has_aoa_bulk_interface(device));
    if (!is_acc) {
      continue;
    }
    const bool match_addr =
        (bus == accessory.identity.bus && addr == accessory.identity.address);
    const bool match_port =
        (!accessory.identity.port_path.empty() &&
         path == accessory.identity.port_path);
    if (match_addr || match_port) {
      target = device;
      found_vid = desc.idVendor;
      found_pid = desc.idProduct;
      break;
    }
  }
  if (target == nullptr) {
    libusb_free_device_list(devices, 1);
    error = "AOA accessory disappeared before open";
    return nullptr;
  }

  auto in_pnp_grace = [this]() -> bool {
    std::lock_guard lock(cache_mutex_);
    return last_aoa_start_at_.time_since_epoch().count() > 0 &&
           (std::chrono::steady_clock::now() - last_aoa_start_at_) <
               kPnpSettleGrace;
  };
  char vid_pid_buf[32]{};
  std::snprintf(vid_pid_buf, sizeof(vid_pid_buf), "%04x:%04x", found_vid,
                found_pid);
  const std::string vid_pid(vid_pid_buf);

  libusb_device_handle* handle = nullptr;
  const int open_rc = libusb_open(target, &handle);
  libusb_free_device_list(devices, 1);
  if (open_rc != LIBUSB_SUCCESS || handle == nullptr) {
    if (open_rc == LIBUSB_ERROR_NOT_SUPPORTED ||
        open_rc == LIBUSB_ERROR_ACCESS || open_rc == LIBUSB_ERROR_NOT_FOUND) {
      if (!in_pnp_grace()) {
        driver_missing_out = true;
      }
      error = "Windows 尚未为 AOA 配件 (" + vid_pid + ") 绑定 WinUSB 驱动（" +
              libusb_text(open_rc) +
              "）。请使用 Zadig 为 Google/Android Accessory 接口安装 WinUSB，或安装 UsbDk。";
    } else {
      error = "libusb_open accessory (" + vid_pid + ") failed: " +
              libusb_text(open_rc);
    }
    return nullptr;
  }

  deskmate::platform::EndpointPair pair;
  if (!select_aoa_bulk_endpoints(handle, pair)) {
    libusb_close(handle);
    error = "accessory (" + vid_pid + ") has no paired bulk IN/OUT endpoints";
    return nullptr;
  }
  const int claim = libusb_claim_interface(handle, pair.interface_number);
  if (claim != LIBUSB_SUCCESS) {
    libusb_close(handle);
    if (claim == LIBUSB_ERROR_NOT_SUPPORTED || claim == LIBUSB_ERROR_ACCESS ||
        claim == LIBUSB_ERROR_NOT_FOUND) {
      if (!in_pnp_grace()) {
        driver_missing_out = true;
      }
      error = "Windows 无法认领 AOA 接口 (" + vid_pid + " if=" +
              std::to_string(pair.interface_number) + ", " +
              libusb_text(claim) +
              ")：请用 Zadig 为该接口安装 WinUSB 驱动或安装 UsbDk。";
    } else {
      error = "libusb_claim_interface(" + vid_pid + " if=" +
              std::to_string(pair.interface_number) + ") failed: " +
              libusb_text(claim);
    }
    return nullptr;
  }
  (void)libusb_clear_halt(handle, pair.endpoint_in);
  (void)libusb_clear_halt(handle, pair.endpoint_out);
  {
    char msg[256]{};
    std::snprintf(msg, sizeof(msg),
                  "[discovery] opened AOA %s bus=%u addr=%u port=%s if=%u in=0x%02x out=0x%02x",
                  vid_pid.c_str(), static_cast<unsigned>(accessory.identity.bus),
                  static_cast<unsigned>(accessory.identity.address),
                  accessory.identity.port_path.c_str(),
                  static_cast<unsigned>(pair.interface_number),
                  static_cast<unsigned>(pair.endpoint_in),
                  static_cast<unsigned>(pair.endpoint_out));
    emit_log(msg);
  }
  return std::make_unique<LibusbBulkTransport>(
      handle, pair.endpoint_in, pair.endpoint_out,
      deskmate::platform::aoa_device_id(accessory.identity),
      static_cast<int>(pair.interface_number), context_);
}

bool LibusbAoaDiscovery::is_accessory_present(std::uint8_t bus,
                                              std::uint8_t address) const {
  deskmate::platform::AoaIdentity id;
  id.bus = bus;
  id.address = address;
  return is_accessory_present(id);
}

bool LibusbAoaDiscovery::is_accessory_present(
    const deskmate::platform::AoaIdentity& identity) const {
  if (context_ == nullptr ||
      (identity.bus == 0 && identity.address == 0 &&
       identity.port_path.empty())) {
    return false;
  }
  libusb_device** devices = nullptr;
  const ssize_t count = libusb_get_device_list(context_, &devices);
  if (count < 0) {
    return false;
  }
  bool found = false;
  for (ssize_t index = 0; index < count; ++index) {
    libusb_device* dev = devices[index];
    const std::uint8_t bus = libusb_get_bus_number(dev);
    const std::uint8_t addr = libusb_get_device_address(dev);
    const std::string path = port_path_for(dev, bus);
    const bool match_addr = (identity.address != 0 && bus == identity.bus &&
                             addr == identity.address);
    const bool match_port =
        (!identity.port_path.empty() && path == identity.port_path);
    if (!match_addr && !match_port) {
      continue;
    }
    libusb_device_descriptor desc{};
    if (libusb_get_device_descriptor(dev, &desc) == LIBUSB_SUCCESS &&
        (deskmate::platform::is_aoa_accessory(desc.idVendor, desc.idProduct) ||
         (desc.idVendor == deskmate::platform::kHdcVendorId &&
          has_aoa_bulk_interface(dev)))) {
      found = true;
      break;
    }
  }
  libusb_free_device_list(devices, 1);
  return found;
}

void LibusbAoaDiscovery::probe_pnp_aoa_status(
    std::string& summary_out, bool& driver_missing_out,
    std::string& driver_hint_out) const {
#ifdef _WIN32
  probe_windows_pnp_aoa(summary_out, driver_missing_out, driver_hint_out);
#else
  summary_out.clear();
  driver_missing_out = false;
  driver_hint_out.clear();
#endif
}

std::vector<deskmate::platform::AoaIdentity>
LibusbAoaDiscovery::list_candidates() {
  std::vector<deskmate::platform::AoaIdentity> out;
  for (auto& dev : scan_all_devices()) {
    out.push_back(std::move(dev.identity));
  }
  return out;
}

std::unique_ptr<LinkTransport> LibusbAoaDiscovery::attach(
    const deskmate::platform::AoaIdentity& identity, std::string& error) {
  auto devices = scan_all_devices();
  const auto matches = [&](const ScannedUsbDevice& d) {
    if (!identity.port_path.empty() && d.identity.port_path == identity.port_path) {
      return true;
    }
    return d.identity.bus == identity.bus &&
           d.identity.address == identity.address;
  };
  for (const auto& d : devices) {
    if (matches(d) && d.in_accessory_mode) {
      bool missing = false;
      return open_accessory(d, error, missing);
    }
  }
  for (const auto& d : devices) {
    if (matches(d) && !d.in_accessory_mode) {
      bool missing = false;
      if (!trigger_aoa_switch(d, error, missing)) {
        return nullptr;
      }
      // Poll up to 10s across ALL ports (including USB 3.0 -> USB 2.0
      // companion port re-enumeration) when called synchronously.
      for (int attempt = 0; attempt < 40; ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        for (const auto& refreshed : scan_all_devices()) {
          if (!refreshed.in_accessory_mode) {
            continue;
          }
          if ((!identity.serial.empty() &&
               refreshed.identity.serial == identity.serial) ||
              refreshed.identity.port_path == identity.port_path ||
              devices.size() == 1) {
            auto transport = open_accessory(refreshed, error, missing);
            if (transport != nullptr) {
              return transport;
            }
          }
        }
      }
      if (error.empty()) {
        error = "accessory did not re-enumerate after AOA START";
      }
      return nullptr;
    }
  }
  error = "device disappeared before attach";
  return nullptr;
}

}  // namespace deskmate::windows

#else  // !DESKMATE_HAVE_LIBUSB

namespace deskmate::windows {

// Stub build: link/session logic still compiles and is testable, but there is
// no USB backend until libusb is provided (vcpkg or portable build on Windows).
LibusbBulkTransport::LibusbBulkTransport(libusb_device_handle* handle,
                                         std::uint8_t endpoint_in,
                                         std::uint8_t endpoint_out,
                                         std::string device_id,
                                         int interface_number,
                                         libusb_context* context)
    : handle_(handle),
      endpoint_in_(endpoint_in),
      endpoint_out_(endpoint_out),
      interface_number_(interface_number),
      context_(context),
      device_id_(std::move(device_id)) {}

LibusbBulkTransport::~LibusbBulkTransport() = default;

deskmate::protocol::IoResult LibusbBulkTransport::read(
    std::span<std::uint8_t>) {
  return {deskmate::protocol::IoStatus::Closed, 0};
}

deskmate::protocol::IoResult LibusbBulkTransport::write(
    std::span<const std::uint8_t>) {
  return {deskmate::protocol::IoStatus::Closed, 0};
}

void LibusbBulkTransport::cancel() {}
void LibusbBulkTransport::wake_reader() {}
void LibusbBulkTransport::notify_rx() {}
void LibusbBulkTransport::set_rx_callback(std::function<void()>) {}
void LibusbBulkTransport::set_stream_fps(int) {}
bool LibusbBulkTransport::consume_write_blocked() noexcept { return false; }
bool LibusbBulkTransport::alive() const { return false; }
void LIBUSB_CALL LibusbBulkTransport::on_in_complete(libusb_transfer*) {}
void LIBUSB_CALL LibusbBulkTransport::on_out_complete(libusb_transfer*) {}
void LibusbBulkTransport::event_loop() {}
void LibusbBulkTransport::submit_in_transfer(std::size_t) {}
void LibusbBulkTransport::handle_in_complete(libusb_transfer*) {}
void LibusbBulkTransport::handle_out_complete(libusb_transfer*) {}
void LibusbBulkTransport::cancel_all_transfers() {}
void LibusbBulkTransport::free_transfers() {}
void LibusbBulkTransport::report_error(const std::string&) {}

LibusbAoaDiscovery::LibusbAoaDiscovery()
    : last_error_(
          "libusb-1.0 was not found at configure time; install it with vcpkg") {}

LibusbAoaDiscovery::~LibusbAoaDiscovery() = default;

void LibusbAoaDiscovery::set_log_callback(std::function<void(std::string)>) {}

std::vector<deskmate::platform::AoaIdentity>
LibusbAoaDiscovery::list_candidates() {
  return {};
}

std::unique_ptr<LinkTransport> LibusbAoaDiscovery::attach(
    const deskmate::platform::AoaIdentity&, std::string& error) {
  error = last_error_;
  return nullptr;
}

std::vector<ScannedUsbDevice> LibusbAoaDiscovery::scan_all_devices(
    const std::unordered_set<std::uint16_t>&) {
  return {};
}

bool LibusbAoaDiscovery::trigger_aoa_switch(const ScannedUsbDevice&,
                                              std::string& error,
                                              bool& driver_missing_out) {
  driver_missing_out = false;
  error = last_error_;
  return false;
}

std::unique_ptr<LinkTransport> LibusbAoaDiscovery::open_accessory(
    const ScannedUsbDevice&, std::string& error, bool& driver_missing_out) {
  driver_missing_out = false;
  error = last_error_;
  return nullptr;
}

bool LibusbAoaDiscovery::is_accessory_present(std::uint8_t,
                                              std::uint8_t) const {
  return false;
}

bool LibusbAoaDiscovery::is_accessory_present(
    const deskmate::platform::AoaIdentity&) const {
  return false;
}

bool LibusbAoaDiscovery::is_in_switch_cooldown(const ScannedUsbDevice&) const {
  return false;
}

void LibusbAoaDiscovery::probe_pnp_aoa_status(std::string& summary_out,
                                              bool& driver_missing_out,
                                              std::string& driver_hint_out) const {
  summary_out.clear();
  driver_missing_out = false;
  driver_hint_out.clear();
}

void LibusbAoaDiscovery::emit_log(const std::string&) const {}
void LibusbAoaDiscovery::remember_pre_switch_identity(
    const deskmate::platform::AoaIdentity&, const std::string&) {}
void LibusbAoaDiscovery::resolve_accessory_identity(
    deskmate::platform::AoaIdentity&) const {}

}  // namespace deskmate::windows

#endif  // DESKMATE_HAVE_LIBUSB
