#include "gate1/aoa.hpp"

#include <chrono>
#include <cstring>
#include <sstream>
#include <thread>

namespace gate1 {
namespace {

constexpr std::uint16_t kGoogleVendorId = 0x18D1;
constexpr std::uint16_t kAccessoryProductIdMin = 0x2D00;
constexpr std::uint16_t kAccessoryProductIdMax = 0x2D05;
constexpr unsigned kControlTimeoutMs = 3000;
constexpr unsigned kPollIntervalMs = 200;

struct FoundDevice {
    libusb_device* device = nullptr;
    libusb_device_descriptor descriptor{};
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
};

bool is_accessory_id(std::uint16_t vendor_id, std::uint16_t product_id)
{
    return vendor_id == kGoogleVendorId &&
           product_id >= kAccessoryProductIdMin &&
           product_id <= kAccessoryProductIdMax;
}

bool device_matches(
    const libusb_device_descriptor& descriptor,
    std::uint8_t bus,
    std::uint8_t address,
    const DeviceSelector& selector)
{
    if (selector.bus >= 0 && bus != static_cast<std::uint8_t>(selector.bus)) {
        return false;
    }
    if (selector.address >= 0 &&
        address != static_cast<std::uint8_t>(selector.address)) {
        return false;
    }
    if (selector.vendor_id >= 0 &&
        descriptor.idVendor != static_cast<std::uint16_t>(selector.vendor_id)) {
        return false;
    }
    if (selector.product_id >= 0 &&
        descriptor.idProduct != static_cast<std::uint16_t>(selector.product_id)) {
        return false;
    }
    return true;
}

bool find_one_selector_match(
    libusb_context* context,
    const DeviceSelector& selector,
    FoundDevice& found,
    std::string& error)
{
    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        error = std::string("libusb_get_device_list failed: ") +
                libusb_error_name(static_cast<int>(count));
        return false;
    }

    int matches = 0;
    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor{};
        const int rc = libusb_get_device_descriptor(devices[i], &descriptor);
        if (rc != LIBUSB_SUCCESS) {
            continue;
        }
        const std::uint8_t bus = libusb_get_bus_number(devices[i]);
        const std::uint8_t address = libusb_get_device_address(devices[i]);
        if (!device_matches(descriptor, bus, address, selector)) {
            continue;
        }
        ++matches;
        if (matches == 1) {
            found.device = devices[i];
            found.descriptor = descriptor;
            found.bus = bus;
            found.address = address;
        }
    }

    if (matches == 0) {
        error = "no USB device matched --bus/--address/--vid/--pid";
        libusb_free_device_list(devices, 1);
        return false;
    }
    if (matches > 1) {
        error = "multiple USB devices matched; use --bus and --address";
        libusb_free_device_list(devices, 1);
        return false;
    }

    libusb_ref_device(found.device);
    libusb_free_device_list(devices, 1);
    return true;
}

bool append_aoa_string(
    const std::string& input,
    std::vector<std::uint8_t>& output,
    std::string& error)
{
    for (std::size_t i = 0; i < input.size();) {
        const std::uint8_t lead = static_cast<std::uint8_t>(input[i]);
        std::size_t extra = 0;
        if (lead < 0x80) {
            extra = 0;
        } else if ((lead & 0xE0) == 0xC0) {
            extra = 1;
        } else if ((lead & 0xF0) == 0xE0) {
            extra = 2;
        } else if ((lead & 0xF8) == 0xF0) {
            extra = 3;
        } else {
            error = "accessory string is not valid UTF-8";
            return false;
        }
        if (i + extra >= input.size()) {
            error = "truncated UTF-8 in accessory string";
            return false;
        }
        for (std::size_t j = 1; j <= extra; ++j) {
            const std::uint8_t continuation = static_cast<std::uint8_t>(input[i + j]);
            if ((continuation & 0xC0) != 0x80) {
                error = "invalid UTF-8 continuation byte";
                return false;
            }
        }
        i += extra + 1;
    }
    output.assign(input.begin(), input.end());
    output.push_back(0);
    return true;
}

bool send_aoa_string(
    libusb_device_handle* handle,
    std::uint16_t index,
    const std::string& value,
    std::string& error)
{
    std::vector<std::uint8_t> utf8;
    if (!append_aoa_string(value, utf8, error)) {
        return false;
    }
    if (utf8.size() > 256) {
        error = "AOA string exceeds 256 bytes";
        return false;
    }
    const int rc = libusb_control_transfer(
        handle,
        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT,
        52,
        0,
        index,
        utf8.data(),
        static_cast<std::uint16_t>(utf8.size()),
        kControlTimeoutMs);
    if (rc < 0) {
        error = "AOA SEND_STRING(request 52, index " + std::to_string(index) +
                ") failed: " + libusb_error_name(rc);
        return false;
    }
    return true;
}

bool find_bulk_endpoints(
    libusb_device* device,
    int preferred_interface,
    int& interface_number,
    int& alternate_setting,
    int& interface_endpoints,
    std::uint8_t& endpoint_in,
    std::uint8_t& endpoint_out,
    std::uint16_t& endpoint_in_max_packet,
    std::uint16_t& endpoint_out_max_packet,
    std::uint8_t& endpoint_in_attributes,
    std::uint8_t& endpoint_out_attributes,
    std::string& descriptor_dump,
    std::string& error)
{
    libusb_config_descriptor* config = nullptr;
    int rc = libusb_get_active_config_descriptor(device, &config);
    if (rc != LIBUSB_SUCCESS) {
        rc = libusb_get_config_descriptor(device, 0, &config);
    }
    if (rc != LIBUSB_SUCCESS || config == nullptr) {
        error = std::string("cannot read accessory configuration: ") + libusb_error_name(rc);
        return false;
    }

    bool found = false;
    std::ostringstream dump;
    dump << "configs=" << static_cast<int>(config->bNumInterfaces)
         << " configuration=" << static_cast<int>(config->bConfigurationValue);
    for (uint8_t i = 0; i < config->bNumInterfaces && !found; ++i) {
        const libusb_interface& iface = config->interface[i];
        for (int alt = 0; alt < iface.num_altsetting && !found; ++alt) {
            const libusb_interface_descriptor& descriptor = iface.altsetting[alt];
            dump << " | if=" << static_cast<int>(descriptor.bInterfaceNumber)
                 << " alt=" << static_cast<int>(descriptor.bAlternateSetting)
                 << " class=" << static_cast<int>(descriptor.bInterfaceClass)
                 << " sub=" << static_cast<int>(descriptor.bInterfaceSubClass)
                 << " proto=" << static_cast<int>(descriptor.bInterfaceProtocol)
                 << " eps=" << static_cast<int>(descriptor.bNumEndpoints);
            for (uint8_t ep = 0; ep < descriptor.bNumEndpoints; ++ep) {
                const libusb_endpoint_descriptor& endpoint = descriptor.endpoint[ep];
                dump << " [ep=0x" << std::hex
                     << static_cast<int>(endpoint.bEndpointAddress)
                     << " attr=0x" << static_cast<int>(endpoint.bmAttributes)
                     << " max=" << std::dec << endpoint.wMaxPacketSize
                     << " interval=" << static_cast<int>(endpoint.bInterval) << "]";
            }
            if (preferred_interface >= 0 &&
                descriptor.bInterfaceNumber != preferred_interface) {
                continue;
            }
            int candidate_in = -1;
            int candidate_out = -1;
            int candidate_alt = alt;
            int candidate_count = static_cast<int>(descriptor.bNumEndpoints);
            std::uint16_t in_max_packet = 0;
            std::uint16_t out_max_packet = 0;
            std::uint8_t in_attributes = 0;
            std::uint8_t out_attributes = 0;
            for (uint8_t ep = 0; ep < descriptor.bNumEndpoints; ++ep) {
                const libusb_endpoint_descriptor& endpoint = descriptor.endpoint[ep];
                if ((endpoint.bmAttributes & 0x03U) != LIBUSB_TRANSFER_TYPE_BULK) {
                    continue;
                }
                if ((endpoint.bEndpointAddress & LIBUSB_ENDPOINT_DIR_MASK) != 0) {
                    if (candidate_in < 0) {
                        candidate_in = endpoint.bEndpointAddress;
                        in_max_packet = endpoint.wMaxPacketSize;
                        in_attributes = endpoint.bmAttributes;
                    }
                } else if (candidate_out < 0) {
                    candidate_out = endpoint.bEndpointAddress;
                    out_max_packet = endpoint.wMaxPacketSize;
                    out_attributes = endpoint.bmAttributes;
                }
            }
            if (candidate_in >= 0 && candidate_out >= 0) {
                interface_number = descriptor.bInterfaceNumber;
                alternate_setting = candidate_alt;
                interface_endpoints = candidate_count;
                endpoint_in = static_cast<std::uint8_t>(candidate_in);
                endpoint_out = static_cast<std::uint8_t>(candidate_out);
                endpoint_in_max_packet = in_max_packet;
                endpoint_out_max_packet = out_max_packet;
                endpoint_in_attributes = in_attributes;
                endpoint_out_attributes = out_attributes;
                found = true;
            }
        }
    }
    libusb_free_config_descriptor(config);

    if (!found) {
        error = "accessory interface has no paired bulk IN/OUT endpoints";
        return false;
    }
    descriptor_dump = dump.str();
    return true;
}

} // namespace

std::string speed_name(int speed)
{
    switch (speed) {
        case LIBUSB_SPEED_LOW:
            return "1.5 Mbps";
        case LIBUSB_SPEED_FULL:
            return "12 Mbps";
        case LIBUSB_SPEED_HIGH:
            return "480 Mbps";
        case LIBUSB_SPEED_SUPER:
            return "5 Gbps";
        case LIBUSB_SPEED_SUPER_PLUS:
            return "10 Gbps";
        default:
            return "unknown";
    }
}

std::vector<DeviceSummary> list_devices(libusb_context* context, std::string& error)
{
    std::vector<DeviceSummary> summaries;
    libusb_device** devices = nullptr;
    const ssize_t count = libusb_get_device_list(context, &devices);
    if (count < 0) {
        error = std::string("libusb_get_device_list failed: ") +
                libusb_error_name(static_cast<int>(count));
        return summaries;
    }

    for (ssize_t i = 0; i < count; ++i) {
        libusb_device_descriptor descriptor{};
        if (libusb_get_device_descriptor(devices[i], &descriptor) != LIBUSB_SUCCESS) {
            continue;
        }
        DeviceSummary summary;
        summary.vendor_id = descriptor.idVendor;
        summary.product_id = descriptor.idProduct;
        summary.bus = libusb_get_bus_number(devices[i]);
        summary.address = libusb_get_device_address(devices[i]);
        summary.speed = libusb_get_device_speed(devices[i]);
        summary.accessory = is_accessory_id(summary.vendor_id, summary.product_id);
        summaries.push_back(summary);
    }
    libusb_free_device_list(devices, 1);
    return summaries;
}

bool selector_has_filter(const DeviceSelector& selector)
{
    return selector.bus >= 0 || selector.address >= 0 ||
           selector.vendor_id >= 0 || selector.product_id >= 0;
}

bool switch_to_accessory(
    libusb_context* context,
    const DeviceSelector& selector,
    const AccessoryIdentity& identity,
    unsigned timeout_ms,
    std::string& error)
{
    if (!selector_has_filter(selector)) {
        error = "AOA requires an explicit device selector";
        return false;
    }

    FoundDevice found;
    if (!find_one_selector_match(context, selector, found, error)) {
        return false;
    }
    if (is_accessory_id(found.descriptor.idVendor, found.descriptor.idProduct)) {
        libusb_unref_device(found.device);
        error = "selected device already has an AOA accessory VID/PID";
        return false;
    }

    libusb_device_handle* handle = nullptr;
    const int open_rc = libusb_open(found.device, &handle);
    libusb_unref_device(found.device);
    if (open_rc != LIBUSB_SUCCESS || handle == nullptr) {
        error = std::string("libusb_open failed: ") + libusb_error_name(open_rc);
        return false;
    }

    std::uint8_t protocol_bytes[2] = {0, 0};
    const int protocol_rc = libusb_control_transfer(
        handle,
        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_IN,
        51,
        0,
        0,
        protocol_bytes,
        sizeof(protocol_bytes),
        kControlTimeoutMs);
    if (protocol_rc < 0) {
        error = std::string("AOA GET_PROTOCOL(request 51) failed: ") +
                libusb_error_name(protocol_rc) +
                " (device may not support AOA or may be claimed by another process)";
        libusb_close(handle);
        return false;
    }
    const std::uint16_t protocol = static_cast<std::uint16_t>(protocol_bytes[0]) |
                                   static_cast<std::uint16_t>(protocol_bytes[1]) << 8;
    if (protocol < 1) {
        error = "AOA GET_PROTOCOL returned version 0";
        libusb_close(handle);
        return false;
    }

    const std::string* strings[] = {
        &identity.manufacturer,
        &identity.model,
        &identity.description,
        &identity.version,
        &identity.uri,
        &identity.serial,
    };
    for (std::uint16_t index = 0; index < 6; ++index) {
        if (index < 4 || !strings[index]->empty()) {
            if (!send_aoa_string(handle, index, *strings[index], error)) {
                libusb_close(handle);
                return false;
            }
        }
    }

    std::uint8_t no_data = 0;
    const int start_rc = libusb_control_transfer(
        handle,
        LIBUSB_REQUEST_TYPE_VENDOR | LIBUSB_RECIPIENT_DEVICE | LIBUSB_ENDPOINT_OUT,
        53,
        0,
        0,
        &no_data,
        0,
        kControlTimeoutMs);
    libusb_close(handle);
    if (start_rc < 0) {
        error = std::string("AOA START(request 53) failed: ") +
                libusb_error_name(start_rc);
        return false;
    }

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        libusb_device** devices = nullptr;
        const ssize_t count = libusb_get_device_list(context, &devices);
        if (count < 0) {
            error = std::string("re-enumeration list failed: ") +
                    libusb_error_name(static_cast<int>(count));
            return false;
        }
        bool appeared = false;
        for (ssize_t i = 0; i < count; ++i) {
            libusb_device_descriptor descriptor{};
            if (libusb_get_device_descriptor(devices[i], &descriptor) == LIBUSB_SUCCESS &&
                is_accessory_id(descriptor.idVendor, descriptor.idProduct)) {
                appeared = true;
                break;
            }
        }
        libusb_free_device_list(devices, 1);
        if (appeared) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
    }

    error = "AOA START succeeded but accessory VID/PID did not enumerate before timeout";
    return false;
}

bool wait_for_accessory(
    libusb_context* context,
    unsigned timeout_ms,
    OpenedAccessory& accessory,
    std::string& error)
{
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        libusb_device** devices = nullptr;
        const ssize_t count = libusb_get_device_list(context, &devices);
        if (count < 0) {
            error = std::string("accessory enumeration failed: ") +
                    libusb_error_name(static_cast<int>(count));
            return false;
        }

        libusb_device* candidate = nullptr;
        libusb_device_descriptor descriptor{};
        for (ssize_t i = 0; i < count; ++i) {
            libusb_device_descriptor current{};
            if (libusb_get_device_descriptor(devices[i], &current) != LIBUSB_SUCCESS) {
                continue;
            }
            if (is_accessory_id(current.idVendor, current.idProduct)) {
                candidate = devices[i];
                descriptor = current;
                break;
            }
        }
        if (candidate == nullptr) {
            libusb_free_device_list(devices, 1);
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollIntervalMs));
            continue;
        }

        libusb_device_handle* handle = nullptr;
        const int open_rc = libusb_open(candidate, &handle);
        if (open_rc != LIBUSB_SUCCESS || handle == nullptr) {
            libusb_free_device_list(devices, 1);
            error = std::string("cannot open AOA accessory: ") +
                    libusb_error_name(open_rc);
            return false;
        }

        int interface_number = -1;
        int alternate_setting = -1;
        int interface_endpoints = 0;
        std::uint8_t endpoint_in = 0;
        std::uint8_t endpoint_out = 0;
        std::uint16_t endpoint_in_max_packet = 0;
        std::uint16_t endpoint_out_max_packet = 0;
        std::uint8_t endpoint_in_attributes = 0;
        std::uint8_t endpoint_out_attributes = 0;
        std::string descriptor_dump;
        if (!find_bulk_endpoints(
                candidate,
                -1,
                interface_number,
                alternate_setting,
                interface_endpoints,
                endpoint_in,
                endpoint_out,
                endpoint_in_max_packet,
                endpoint_out_max_packet,
                endpoint_in_attributes,
                endpoint_out_attributes,
                descriptor_dump,
                error)) {
            libusb_close(handle);
            libusb_free_device_list(devices, 1);
            return false;
        }

        accessory.handle = handle;
        accessory.interface_number = interface_number;
        accessory.alternate_setting = alternate_setting;
        accessory.interface_endpoints = interface_endpoints;
        accessory.endpoint_in = endpoint_in;
        accessory.endpoint_out = endpoint_out;
        accessory.endpoint_in_max_packet = endpoint_in_max_packet;
        accessory.endpoint_out_max_packet = endpoint_out_max_packet;
        accessory.endpoint_in_attributes = endpoint_in_attributes;
        accessory.endpoint_out_attributes = endpoint_out_attributes;
        accessory.descriptor_dump = descriptor_dump;
        accessory.vendor_id = descriptor.idVendor;
        accessory.product_id = descriptor.idProduct;
        accessory.bus = libusb_get_bus_number(candidate);
        accessory.address = libusb_get_device_address(candidate);
        accessory.speed = libusb_get_device_speed(candidate);
        libusb_free_device_list(devices, 1);
        return true;
    }

    error = "no AOA accessory (18d1:2d00..2d05) enumerated before timeout";
    return false;
}

void close_accessory(OpenedAccessory& accessory)
{
    if (accessory.handle != nullptr) {
        libusb_close(accessory.handle);
        accessory.handle = nullptr;
    }
}

} // namespace gate1
