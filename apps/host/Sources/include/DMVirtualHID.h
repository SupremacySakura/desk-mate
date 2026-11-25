#pragma once

#import <Foundation/Foundation.h>
#include <stdint.h>

NS_ASSUME_NONNULL_BEGIN

typedef NS_ENUM(int32_t, DMVirtualHIDMode) {
    DMVirtualHIDModeTouchScreen = 1,
    DMVirtualHIDModeTrackpad = 2,
};

typedef struct DMVirtualHIDContact {
    uint8_t contactID;
    uint8_t active;
    uint16_t x;
    uint16_t y;
} DMVirtualHIDContact;

@interface DMVirtualHIDSession : NSObject

+ (NSDictionary<NSString *, id> *)probeCapability;

+ (nullable instancetype)createWithMode:(DMVirtualHIDMode)mode
                                  error:(NSError * _Nullable * _Nullable)error NS_SWIFT_NAME(create(mode:));

- (BOOL)sendContacts:(const DMVirtualHIDContact * _Nullable)contacts
               count:(NSInteger)count
       primaryButton:(BOOL)primaryButton;

- (void)invalidate;

@end

NS_ASSUME_NONNULL_END
