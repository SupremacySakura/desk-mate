#pragma once

#include <libusb-1.0/libusb.h>

#include <cstdint>
#include <string>
#include <vector>

namespace gate1 {

struct DeviceSelector {
    int bus = -1;
    int address = -1;
    int vendor_id = -1;
    int product_id = -1;
};

struct AccessoryIdentity {
    std::string manufacturer = "DeskMate";
    std::string model = "DeskMate Gate 1 Probe";
    std::string description = "AOA wired duplex probe";
    std::string version = "1.0";
    std::string uri = "https://example.invalid/deskmate/gate1";
    std::string serial;
};

struct DeviceSummary {
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
    int speed = 0;
    bool accessory = false;
};

struct OpenedAccessory {
    libusb_device_handle* handle = nullptr;
    int interface_number = -1;
    std::uint8_t endpoint_in = 0;
    std::uint8_t endpoint_out = 0;
    std::uint16_t vendor_id = 0;
    std::uint16_t product_id = 0;
    std::uint8_t bus = 0;
    std::uint8_t address = 0;
    int speed = 0;
};

std::vector<DeviceSummary> list_devices(libusb_context* context, std::string& error);
std::string speed_name(int speed);
bool selector_has_filter(const DeviceSelector& selector);
bool switch_to_accessory(
    libusb_context* context,
    const DeviceSelector& selector,
    const AccessoryIdentity& identity,
    unsigned timeout_ms,
    std::string& error);
bool wait_for_accessory(
    libusb_context* context,
    unsigned timeout_ms,
    OpenedAccessory& accessory,
    std::string& error);
void close_accessory(OpenedAccessory& accessory);

} // namespace gate1
