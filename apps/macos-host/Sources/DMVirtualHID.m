#import "DMVirtualHID.h"

#import <IOKit/hid/IOHIDKeys.h>
#import <IOKit/hidsystem/IOHIDUserDevice.h>
#import <dispatch/dispatch.h>
#import <mach/mach_time.h>
#import <string.h>

static NSString * const DMVirtualHIDErrorDomain = @"dev.deskmate.host.virtual-hid";

static const NSInteger kMaxContacts = 10;
static const uint8_t kReportIDContacts = 1;
static const uint8_t kReportIDMaxCount = 2;

static NSData *DMBuildTouchScreenDescriptor(void) {
    static const uint8_t kHeader[] = {
        0x05, 0x0D, // Usage Page (Digitizers)
        0x09, 0x04, // Usage (Touch Screen)
        0xA1, 0x01, // Collection (Application)
        0x85, kReportIDContacts, // Report ID (1)
    };
    static const uint8_t kFinger[] = {
        0x05, 0x0D, // Usage Page (Digitizers)
        0x09, 0x22, // Usage (Finger)
        0xA1, 0x02, // Collection (Logical)
        0x09, 0x42, //   Usage (Tip Switch)
        0x09, 0x32, //   Usage (In Range)
        0x15, 0x00, //   Logical Minimum (0)
        0x25, 0x01, //   Logical Maximum (1)
        0x75, 0x01, //   Report Size (1)
        0x95, 0x02, //   Report Count (2)
        0x81, 0x02, //   Input (Data, Var, Abs)
        0x95, 0x06, //   Report Count (6)
        0x81, 0x03, //   Input (Const, Ary, Abs)
        0x09, 0x51, //   Usage (Contact Identifier)
        0x15, 0x00, //   Logical Minimum (0)
        0x25, 0x7F, //   Logical Maximum (127)
        0x75, 0x08, //   Report Size (8)
        0x95, 0x01, //   Report Count (1)
        0x81, 0x02, //   Input (Data, Var, Abs)
        0x05, 0x01, //   Usage Page (Generic Desktop)
        0x09, 0x30, //   Usage (X)
        0x09, 0x31, //   Usage (Y)
        0x15, 0x00, //   Logical Minimum (0)
        0x26, 0xFF, 0x7F, // Logical Maximum (32767)
        0x35, 0x00, //   Physical Minimum (0)
        0x46, 0xFF, 0x7F, // Physical Maximum (32767)
        0x75, 0x10, //   Report Size (16)
        0x95, 0x02, //   Report Count (2)
        0x81, 0x02, //   Input (Data, Var, Abs)
        0xC0,       // End Collection
    };
    static const uint8_t kFooter[] = {
        0x05, 0x0D, // Usage Page (Digitizers)
        0x09, 0x56, // Usage (Scan Time)
        0x15, 0x00, // Logical Minimum (0)
        0x27, 0xFF, 0xFF, 0x00, 0x00, // Logical Maximum (65535)
        0x75, 0x10, // Report Size (16)
        0x95, 0x01, // Report Count (1)
        0x81, 0x02, // Input (Data, Var, Abs)
        0x09, 0x54, // Usage (Contact Count)
        0x15, 0x00, // Logical Minimum (0)
        0x25, 0x0A, // Logical Maximum (10)
        0x75, 0x08, // Report Size (8)
        0x95, 0x01, // Report Count (1)
        0x81, 0x02, // Input (Data, Var, Abs)
        0x85, kReportIDMaxCount, // Report ID (2)
        0x09, 0x55, // Usage (Contact Count Maximum)
        0x15, 0x00, // Logical Minimum (0)
        0x25, 0x0A, // Logical Maximum (10)
        0x75, 0x08, // Report Size (8)
        0x95, 0x01, // Report Count (1)
        0xB1, 0x02, // Feature (Data, Var, Abs)
        0xC0,       // End Collection
    };

    NSMutableData *data = [NSMutableData dataWithBytes:kHeader length:sizeof(kHeader)];
    for (NSInteger i = 0; i < kMaxContacts; i++) {
        [data appendBytes:kFinger length:sizeof(kFinger)];
    }
    [data appendBytes:kFooter length:sizeof(kFooter)];
    return data;
}

static NSData *DMBuildTrackpadDescriptor(void) {
    static const uint8_t kHeader[] = {
        0x05, 0x0D, // Usage Page (Digitizers)
        0x09, 0x05, // Usage (Touch Pad)
        0xA1, 0x01, // Collection (Application)
        0x85, kReportIDContacts, // Report ID (1)
    };
    static const uint8_t kFinger[] = {
        0x05, 0x0D, // Usage Page (Digitizers)
        0x09, 0x22, // Usage (Finger)
        0xA1, 0x02, // Collection (Logical)
        0x09, 0x47, //   Usage (Confidence)
        0x09, 0x42, //   Usage (Tip Switch)
        0x15, 0x00, //   Logical Minimum (0)
        0x25, 0x01, //   Logical Maximum (1)
        0x75, 0x01, //   Report Size (1)
        0x95, 0x02, //   Report Count (2)
        0x81, 0x02, //   Input (Data, Var, Abs)
        0x95, 0x06, //   Report Count (6)
        0x81, 0x03, //   Input (Const, Ary, Abs)
        0x09, 0x51, //   Usage (Contact Identifier)
        0x15, 0x00, //   Logical Minimum (0)
        0x25, 0x7F, //   Logical Maximum (127)
        0x75, 0x08, //   Report Size (8)
        0x95, 0x01, //   Report Count (1)
        0x81, 0x02, //   Input (Data, Var, Abs)
        0x05, 0x01, //   Usage Page (Generic Desktop)
        0x55, 0x0E, //   Unit Exponent (-2)
        0x65, 0x11, //   Unit (SI Linear: cm)
        0x09, 0x30, //   Usage (X)
        0x15, 0x00, //   Logical Minimum (0)
        0x26, 0xFF, 0x7F, // Logical Maximum (32767)
        0x35, 0x00, //   Physical Minimum (0)
        0x46, 0x40, 0x06, // Physical Maximum (1600 -> 16.00 cm)
        0x75, 0x10, //   Report Size (16)
        0x95, 0x01, //   Report Count (1)
        0x81, 0x02, //   Input (Data, Var, Abs)
        0x09, 0x31, //   Usage (Y)
        0x46, 0xE8, 0x03, // Physical Maximum (1000 -> 10.00 cm)
        0x81, 0x02, //   Input (Data, Var, Abs)
        0x55, 0x00, //   Unit Exponent (0)
        0x65, 0x00, //   Unit (None)
        0xC0,       // End Collection
    };
    static const uint8_t kFooter[] = {
        0x05, 0x0D, // Usage Page (Digitizers)
        0x09, 0x56, // Usage (Scan Time)
        0x15, 0x00, // Logical Minimum (0)
        0x27, 0xFF, 0xFF, 0x00, 0x00, // Logical Maximum (65535)
        0x75, 0x10, // Report Size (16)
        0x95, 0x01, // Report Count (1)
        0x81, 0x02, // Input (Data, Var, Abs)
        0x09, 0x54, // Usage (Contact Count)
        0x15, 0x00, // Logical Minimum (0)
        0x25, 0x0A, // Logical Maximum (10)
        0x75, 0x08, // Report Size (8)
        0x95, 0x01, // Report Count (1)
        0x81, 0x02, // Input (Data, Var, Abs)
        0x05, 0x09, // Usage Page (Button)
        0x09, 0x01, // Usage (Button 1)
        0x15, 0x00, // Logical Minimum (0)
        0x25, 0x01, // Logical Maximum (1)
        0x75, 0x01, // Report Size (1)
        0x95, 0x01, // Report Count (1)
        0x81, 0x02, // Input (Data, Var, Abs)
        0x95, 0x07, // Report Count (7)
        0x81, 0x03, // Input (Const, Ary, Abs)
        0x85, kReportIDMaxCount, // Report ID (2)
        0x05, 0x0D, // Usage Page (Digitizers)
        0x09, 0x55, // Usage (Contact Count Maximum)
        0x15, 0x00, // Logical Minimum (0)
        0x25, 0x0A, // Logical Maximum (10)
        0x75, 0x08, // Report Size (8)
        0x95, 0x01, // Report Count (1)
        0xB1, 0x02, // Feature (Data, Var, Abs)
        0xC0,       // End Collection
    };

    NSMutableData *data = [NSMutableData dataWithBytes:kHeader length:sizeof(kHeader)];
    for (NSInteger i = 0; i < kMaxContacts; i++) {
        [data appendBytes:kFinger length:sizeof(kFinger)];
    }
    [data appendBytes:kFooter length:sizeof(kFooter)];
    return data;
}

@implementation DMVirtualHIDSession {
    DMVirtualHIDMode _mode;
    IOHIDUserDeviceRef _device;
    dispatch_queue_t _queue;
    BOOL _invalidated;
    uint64_t _startMachTime;
    mach_timebase_info_data_t _timebase;
}

+ (NSDictionary<NSString *, id> *)probeCapability {
    static NSDictionary<NSString *, id> *cached = nil;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        NSError *error = nil;
        DMVirtualHIDSession *session = [self createWithMode:DMVirtualHIDModeTrackpad error:&error];
        if (session != nil) {
            [session invalidate];
            cached = @{
                @"available": @YES,
                @"detail": @"已授权虚拟 HID 设备创建",
            };
        } else {
            NSString *message = error.localizedDescription ?: @"系统未授予 virtual HID 签名权限，自动切换兼容手势模式";
            cached = @{
                @"available": @NO,
                @"detail": message,
            };
        }
    });
    return cached;
}

+ (nullable instancetype)createWithMode:(DMVirtualHIDMode)mode
                                  error:(NSError * _Nullable * _Nullable)error {
    NSData *descriptor = (mode == DMVirtualHIDModeTrackpad)
        ? DMBuildTrackpadDescriptor()
        : DMBuildTouchScreenDescriptor();
    NSNumber *productID = (mode == DMVirtualHIDModeTrackpad) ? @0x444E : @0x444D;
    NSString *productName = (mode == DMVirtualHIDModeTrackpad)
        ? @"DeskMate Virtual Trackpad"
        : @"DeskMate Virtual Touch Screen";

    NSDictionary *properties = @{
        @kIOHIDReportDescriptorKey: descriptor,
        @kIOHIDVendorIDKey: @0x1209,
        @kIOHIDProductIDKey: productID,
        @kIOHIDManufacturerKey: @"DeskMate",
        @kIOHIDProductKey: productName,
        @kIOHIDTransportKey: @"Virtual",
    };

    IOHIDUserDeviceRef device = IOHIDUserDeviceCreateWithProperties(
        kCFAllocatorDefault,
        (__bridge CFDictionaryRef)properties,
        0
    );
    if (device == NULL) {
        if (error != NULL) {
            *error = [NSError errorWithDomain:DMVirtualHIDErrorDomain
                                         code:1
                                     userInfo:@{
                NSLocalizedDescriptionKey: @"系统拒绝创建虚拟 HID 设备（缺少 com.apple.developer.hid.virtual.device 签名授权），已自动启用兼容手势映射。"
            }];
        }
        return nil;
    }

    DMVirtualHIDSession *session = [[DMVirtualHIDSession alloc] initWithMode:mode device:device];
    return session;
}

- (instancetype)initWithMode:(DMVirtualHIDMode)mode device:(IOHIDUserDeviceRef)device {
    self = [super init];
    if (self != nil) {
        _mode = mode;
        _device = device;
        _queue = dispatch_queue_create("dev.deskmate.host.virtual-hid", DISPATCH_QUEUE_SERIAL);
        _startMachTime = mach_absolute_time();
        mach_timebase_info(&_timebase);

        IOHIDUserDeviceRegisterGetReportBlock(
            device,
            ^IOReturn(IOHIDReportType type, uint32_t reportID, uint8_t *report, CFIndex *reportLength) {
                if (type == kIOHIDReportTypeFeature && reportID == kReportIDMaxCount &&
                    report != NULL && reportLength != NULL && *reportLength >= 2) {
                    report[0] = kReportIDMaxCount;
                    report[1] = (uint8_t)kMaxContacts;
                    *reportLength = 2;
                    return kIOReturnSuccess;
                }
                return kIOReturnUnsupported;
            }
        );

        IOHIDUserDeviceSetDispatchQueue(device, _queue);
        IOHIDUserDeviceSetCancelHandler(device, ^{
            CFRelease(device);
        });
        IOHIDUserDeviceActivate(device);
    }
    return self;
}

- (void)dealloc {
    [self invalidate];
}

- (BOOL)sendContacts:(const DMVirtualHIDContact * _Nullable)contacts
               count:(NSInteger)count
       primaryButton:(BOOL)primaryButton {
    if (_invalidated || _device == NULL) {
        return NO;
    }

    uint8_t report[65];
    memset(report, 0, sizeof(report));
    report[0] = kReportIDContacts;

    NSInteger boundedCount = (contacts == NULL || count <= 0) ? 0 : (count > kMaxContacts ? kMaxContacts : count);
    uint8_t activeCount = 0;
    for (NSInteger i = 0; i < boundedCount; i++) {
        DMVirtualHIDContact c = contacts[i];
        NSInteger offset = 1 + i * 6;
        uint8_t flags = 0;
        if (c.active != 0) {
            flags = 0x03; // TouchScreen: TipSwitch|InRange; Trackpad: Confidence|TipSwitch
            activeCount++;
        }
        report[offset] = flags;
        report[offset + 1] = c.contactID & 0x7F;
        uint16_t x = c.x > 32767 ? 32767 : c.x;
        uint16_t y = c.y > 32767 ? 32767 : c.y;
        report[offset + 2] = (uint8_t)(x & 0xFF);
        report[offset + 3] = (uint8_t)((x >> 8) & 0xFF);
        report[offset + 4] = (uint8_t)(y & 0xFF);
        report[offset + 5] = (uint8_t)((y >> 8) & 0xFF);
    }

    uint64_t now = mach_absolute_time();
    uint64_t elapsedNs = 0;
    if (now > _startMachTime && _timebase.denom > 0) {
        elapsedNs = ((now - _startMachTime) * _timebase.numer) / _timebase.denom;
    }
    uint16_t scanTime = (uint16_t)((elapsedNs / 100000ULL) & 0xFFFFU); // 100us units
    NSInteger footerOffset = 1 + kMaxContacts * 6; // byte 61
    report[footerOffset] = (uint8_t)(scanTime & 0xFF);
    report[footerOffset + 1] = (uint8_t)((scanTime >> 8) & 0xFF);
    report[footerOffset + 2] = activeCount;

    CFIndex reportSize = 64;
    if (_mode == DMVirtualHIDModeTrackpad) {
        report[footerOffset + 3] = primaryButton ? 0x01 : 0x00;
        reportSize = 65;
    }

    IOReturn kr = IOHIDUserDeviceHandleReportWithTimeStamp(_device, now, report, reportSize);
    return kr == kIOReturnSuccess;
}

- (void)invalidate {
    if (_invalidated) {
        return;
    }
    _invalidated = YES;
    if (_device != NULL) {
        IOHIDUserDeviceCancel(_device);
        _device = NULL;
    }
}

@end
