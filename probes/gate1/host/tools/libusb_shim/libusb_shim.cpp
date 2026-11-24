#include <libusb-1.0/libusb.h>

#include <cstdio>

namespace {

void unused(const void* pointer)
{
    (void)pointer;
}

} // namespace

extern "C" {

int libusb_init(libusb_context** context)
{
    if (context != nullptr) {
        *context = nullptr;
    }
    std::fprintf(
        stderr,
        "GATE1 LIBUSB SHIM: this binary is compile-check only and performs no USB I/O\n");
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

void libusb_exit(libusb_context* context)
{
    unused(context);
}

ssize_t libusb_get_device_list(libusb_context* context, libusb_device*** list)
{
    unused(context);
    if (list != nullptr) {
        *list = nullptr;
    }
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

void libusb_free_device_list(libusb_device** list, int unref_devices)
{
    unused(list);
    unused(&unref_devices);
}

int libusb_get_device_descriptor(
    libusb_device* device,
    struct libusb_device_descriptor* descriptor)
{
    unused(device);
    unused(descriptor);
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

uint8_t libusb_get_bus_number(libusb_device* device)
{
    unused(device);
    return 0;
}

uint8_t libusb_get_device_address(libusb_device* device)
{
    unused(device);
    return 0;
}

int libusb_get_device_speed(libusb_device* device)
{
    unused(device);
    return LIBUSB_SPEED_UNKNOWN;
}

int libusb_open(libusb_device* device, libusb_device_handle** handle)
{
    unused(device);
    if (handle != nullptr) {
        *handle = nullptr;
    }
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

void libusb_close(libusb_device_handle* handle)
{
    unused(handle);
}

int libusb_get_active_config_descriptor(
    libusb_device* device,
    libusb_config_descriptor** config)
{
    unused(device);
    if (config != nullptr) {
        *config = nullptr;
    }
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int libusb_get_config_descriptor(
    libusb_device* device,
    uint8_t index,
    libusb_config_descriptor** config)
{
    unused(device);
    unused(&index);
    if (config != nullptr) {
        *config = nullptr;
    }
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

void libusb_free_config_descriptor(libusb_config_descriptor* config)
{
    unused(config);
}

int libusb_claim_interface(libusb_device_handle* handle, int interface_number)
{
    unused(handle);
    unused(&interface_number);
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int libusb_release_interface(libusb_device_handle* handle, int interface_number)
{
    unused(handle);
    unused(&interface_number);
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int libusb_control_transfer(
    libusb_device_handle* handle,
    uint8_t request_type,
    uint8_t request,
    uint16_t value,
    uint16_t index,
    unsigned char* data,
    uint16_t length,
    unsigned int timeout)
{
    unused(handle);
    unused(&request_type);
    unused(&request);
    unused(&value);
    unused(&index);
    unused(data);
    unused(&length);
    unused(&timeout);
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int libusb_bulk_transfer(
    libusb_device_handle* handle,
    unsigned char endpoint,
    unsigned char* data,
    int length,
    int* transferred,
    unsigned int timeout)
{
    unused(handle);
    unused(&endpoint);
    unused(data);
    unused(&length);
    unused(&timeout);
    if (transferred != nullptr) {
        *transferred = 0;
    }
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

libusb_transfer* libusb_alloc_transfer(int iso_packets)
{
    unused(&iso_packets);
    return nullptr;
}

void libusb_free_transfer(libusb_transfer* transfer)
{
    unused(transfer);
}

void libusb_fill_bulk_transfer(
    libusb_transfer* transfer,
    libusb_device_handle* handle,
    unsigned char endpoint,
    unsigned char* buffer,
    int length,
    libusb_transfer_cb_fn callback,
    void* user_data,
    unsigned int timeout)
{
    if (transfer == nullptr) {
        return;
    }
    transfer->dev_handle = handle;
    transfer->endpoint = endpoint;
    transfer->type = LIBUSB_TRANSFER_TYPE_BULK;
    transfer->timeout = timeout;
    transfer->status = LIBUSB_TRANSFER_ERROR;
    transfer->length = length;
    transfer->actual_length = 0;
    transfer->callback = callback;
    transfer->user_data = user_data;
    transfer->buffer = buffer;
}

int libusb_submit_transfer(libusb_transfer* transfer)
{
    unused(transfer);
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int libusb_cancel_transfer(libusb_transfer* transfer)
{
    unused(transfer);
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

int libusb_handle_events_timeout(libusb_context* context, struct timeval* timeout)
{
    unused(context);
    unused(timeout);
    return LIBUSB_ERROR_NOT_SUPPORTED;
}

const char* libusb_error_name(int error_code)
{
    switch (error_code) {
        case LIBUSB_SUCCESS:
            return "LIBUSB_SUCCESS";
        case LIBUSB_ERROR_IO:
            return "LIBUSB_ERROR_IO";
        case LIBUSB_ERROR_INVALID_PARAM:
            return "LIBUSB_ERROR_INVALID_PARAM";
        case LIBUSB_ERROR_ACCESS:
            return "LIBUSB_ERROR_ACCESS";
        case LIBUSB_ERROR_NO_DEVICE:
            return "LIBUSB_ERROR_NO_DEVICE";
        case LIBUSB_ERROR_NOT_FOUND:
            return "LIBUSB_ERROR_NOT_FOUND";
        case LIBUSB_ERROR_BUSY:
            return "LIBUSB_ERROR_BUSY";
        case LIBUSB_ERROR_TIMEOUT:
            return "LIBUSB_ERROR_TIMEOUT";
        case LIBUSB_ERROR_OVERFLOW:
            return "LIBUSB_ERROR_OVERFLOW";
        case LIBUSB_ERROR_PIPE:
            return "LIBUSB_ERROR_PIPE";
        case LIBUSB_ERROR_INTERRUPTED:
            return "LIBUSB_ERROR_INTERRUPTED";
        case LIBUSB_ERROR_NO_MEM:
            return "LIBUSB_ERROR_NO_MEM";
        case LIBUSB_ERROR_NOT_SUPPORTED:
            return "LIBUSB_ERROR_NOT_SUPPORTED";
        case LIBUSB_ERROR_OTHER:
            return "LIBUSB_ERROR_OTHER";
        default:
            return "LIBUSB_ERROR_UNKNOWN";
    }
}

void libusb_ref_device(libusb_device* device)
{
    unused(device);
}

void libusb_unref_device(libusb_device* device)
{
    unused(device);
}

} // extern "C"
