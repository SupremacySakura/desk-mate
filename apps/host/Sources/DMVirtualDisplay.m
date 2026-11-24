#import "DMVirtualDisplay.h"

#import <CoreGraphics/CoreGraphics.h>
#import <dispatch/dispatch.h>
#import <stdlib.h>

@protocol DMCGVirtualDisplayDescriptor <NSObject>
- (instancetype)init;
@property (nonatomic, copy) NSString *name;
@property (nonatomic, assign) uint32_t vendorID;
@property (nonatomic, assign) uint32_t productID;
@property (nonatomic, assign) uint32_t serialNum;
@property (nonatomic, assign) CGSize sizeInMillimeters;
@property (nonatomic, assign) uint32_t maxPixelsWide;
@property (nonatomic, assign) uint32_t maxPixelsHigh;
@property (nonatomic, assign) CGPoint redPrimary;
@property (nonatomic, assign) CGPoint greenPrimary;
@property (nonatomic, assign) CGPoint bluePrimary;
@property (nonatomic, assign) CGPoint whitePoint;
@property (nonatomic, strong) dispatch_queue_t queue;
@end

@protocol DMCGVirtualDisplayMode <NSObject>
- (instancetype)initWithWidth:(uint32_t)width
                       height:(uint32_t)height
                  refreshRate:(double)refreshRate;
@end

@protocol DMCGVirtualDisplaySettings <NSObject>
- (instancetype)init;
@property (nonatomic, assign) uint32_t hiDPI;
@property (nonatomic, assign) uint32_t rotation;
@property (nonatomic, copy) NSArray *modes;
@end

@protocol DMCGVirtualDisplay <NSObject>
@property (nonatomic, readonly) uint32_t displayID;
@property (nonatomic, readonly) uint32_t hiDPI;
@property (nonatomic, readonly) NSArray *modes;
- (instancetype)initWithDescriptor:(id)descriptor;
- (BOOL)applySettings:(id)settings;
@end

static NSString * const DMVirtualDisplayErrorDomain = @"dev.deskmate.host.virtual-display";

static NSDictionary<NSString *, id> *DMFailure(NSString *message) {
    return @{
        @"ok": @NO,
        @"error": message,
    };
}

static NSDictionary<NSString *, id> *DMSuccess(void) {
    return @{@"ok": @YES};
}

static NSString *DMModeSummary(CGDisplayModeRef mode) {
    return [NSString stringWithFormat:@"%zux%zu pt / %zux%zu px @ %.2f Hz",
                                      CGDisplayModeGetWidth(mode),
                                      CGDisplayModeGetHeight(mode),
                                      CGDisplayModeGetPixelWidth(mode),
                                      CGDisplayModeGetPixelHeight(mode),
                                      CGDisplayModeGetRefreshRate(mode)];
}

// CGDisplayIsOnline reports a truthy value (-1) for display IDs that no longer
// exist, so a released virtual display looks "online" forever. Membership in
// the online display list is the only reliable liveness check.
static BOOL DMDisplayIsOnlineInList(uint32_t displayID) {
    uint32_t count = 0;
    if (CGGetOnlineDisplayList(0, NULL, &count) != kCGErrorSuccess || count == 0) {
        return NO;
    }
    uint32_t *ids = calloc(count, sizeof(uint32_t));
    if (ids == NULL) {
        return NO;
    }
    BOOL found = NO;
    if (CGGetOnlineDisplayList(count, ids, &count) == kCGErrorSuccess) {
        for (uint32_t index = 0; index < count; index++) {
            if (ids[index] == displayID) {
                found = YES;
                break;
            }
        }
    }
    free(ids);
    return found;
}

static BOOL DMAnyBackingScale2Mode(uint32_t displayID) {
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(displayID, NULL);
    if (modes == NULL) {
        return NO;
    }
    BOOL found = NO;
    for (CFIndex index = 0; index < CFArrayGetCount(modes); index++) {
        CGDisplayModeRef candidate = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, index);
        if (CGDisplayModeGetWidth(candidate) != 0 &&
            CGDisplayModeGetPixelWidth(candidate) == CGDisplayModeGetWidth(candidate) * 2) {
            found = YES;
            break;
        }
    }
    CFRelease(modes);
    return found;
}

@implementation DMVirtualDisplaySession {
    id<DMCGVirtualDisplay> _display;
    id<DMCGVirtualDisplayDescriptor> _descriptor;
    id<DMCGVirtualDisplaySettings> _settings;
    id<DMCGVirtualDisplayMode> _mode;
    dispatch_queue_t _queue;
    uint32_t _displayID;
    uint32_t _logicalWidth;
    uint32_t _logicalHeight;
    uint32_t _scale;
    double _refreshRate;
}

+ (NSDictionary<NSString *, id> *)createWithLogicalWidth:(uint32_t)logicalWidth
                                           logicalHeight:(uint32_t)logicalHeight
                                                   scale:(uint32_t)scale
                                             refreshRate:(double)refreshRate {
    if (logicalWidth == 0 || logicalHeight == 0) {
        return DMFailure(@"logical display dimensions must be positive");
    }
    if (scale != 1 && scale != 2) {
        return DMFailure(@"display scale must be 1 or 2");
    }
    if (refreshRate <= 0) {
        return DMFailure(@"refresh rate must be positive");
    }

    Class descriptorClass = NSClassFromString(@"CGVirtualDisplayDescriptor");
    Class modeClass = NSClassFromString(@"CGVirtualDisplayMode");
    Class settingsClass = NSClassFromString(@"CGVirtualDisplaySettings");
    Class displayClass = NSClassFromString(@"CGVirtualDisplay");
    if (descriptorClass == Nil || modeClass == Nil || settingsClass == Nil || displayClass == Nil) {
        return DMFailure(@"CGVirtualDisplay classes are unavailable");
    }

    DMVirtualDisplaySession *session = [[DMVirtualDisplaySession alloc] init];
    session->_logicalWidth = logicalWidth;
    session->_logicalHeight = logicalHeight;
    session->_scale = scale;
    session->_refreshRate = refreshRate;
    session->_queue = dispatch_queue_create("dev.deskmate.host.virtual-display", DISPATCH_QUEUE_SERIAL);

    id<DMCGVirtualDisplayDescriptor> descriptor =
        [(id<DMCGVirtualDisplayDescriptor>)[(id)descriptorClass alloc] init];
    descriptor.name = @"DeskMate Extended Display";
    descriptor.vendorID = 0x0D53;
    descriptor.productID = 0x0005;
    descriptor.serialNum = 0x0001;
    descriptor.maxPixelsWide = logicalWidth * scale;
    descriptor.maxPixelsHigh = logicalHeight * scale;
    const double widthMM = ((double)(logicalWidth * scale) / 110.0) * 25.4;
    const double heightMM = ((double)(logicalHeight * scale) / 110.0) * 25.4;
    descriptor.sizeInMillimeters = CGSizeMake(widthMM, heightMM);
    descriptor.redPrimary = CGPointMake(0.64, 0.33);
    descriptor.greenPrimary = CGPointMake(0.30, 0.60);
    descriptor.bluePrimary = CGPointMake(0.15, 0.06);
    descriptor.whitePoint = CGPointMake(0.3127, 0.3290);
    descriptor.queue = session->_queue;

    id<DMCGVirtualDisplayMode> mode =
        [(id<DMCGVirtualDisplayMode>)[(id)modeClass alloc] initWithWidth:logicalWidth
                                                                height:logicalHeight
                                                           refreshRate:refreshRate];
    id<DMCGVirtualDisplaySettings> settings =
        [(id<DMCGVirtualDisplaySettings>)[(id)settingsClass alloc] init];
    settings.hiDPI = scale == 2 ? 1 : 0;
    settings.rotation = 0;
    settings.modes = @[mode];

    id<DMCGVirtualDisplay> display =
        [(id<DMCGVirtualDisplay>)[(id)displayClass alloc] initWithDescriptor:descriptor];
    if (display == nil) {
        return DMFailure(@"CGVirtualDisplay initWithDescriptor: returned nil");
    }
    if (![display applySettings:settings]) {
        return DMFailure(@"CGVirtualDisplay applySettings: returned NO");
    }
    if (display.displayID == kCGNullDirectDisplay) {
        return DMFailure(@"CGVirtualDisplay returned a null display ID");
    }

    session->_descriptor = descriptor;
    session->_mode = mode;
    session->_settings = settings;
    session->_display = display;
    session->_displayID = display.displayID;

    return @{
        @"ok": @YES,
        @"session": session,
        @"displayID": @(session->_displayID),
        @"modeSummaries": session.modeSummaries,
    };
}

- (void)dealloc {
    _display = nil;
    _settings = nil;
    _mode = nil;
    _descriptor = nil;
    _queue = nil;
}

- (NSNumber *)displayIDNumber {
    return _displayID == kCGNullDirectDisplay ? nil : @(_displayID);
}

- (NSArray<NSString *> *)modeSummaries {
    if (_displayID == kCGNullDirectDisplay) {
        return @[];
    }
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(_displayID, NULL);
    if (modes == NULL) {
        return @[];
    }
    NSMutableArray<NSString *> *summaries = [NSMutableArray array];
    for (CFIndex index = 0; index < CFArrayGetCount(modes); index++) {
        CGDisplayModeRef mode = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, index);
        [summaries addObject:DMModeSummary(mode)];
    }
    CFRelease(modes);
    return summaries;
}

- (NSDictionary<NSString *, id> *)applyAndSelectHiDPIMode {
    if (_display == nil || _displayID == kCGNullDirectDisplay) {
        return DMFailure(@"no live virtual display");
    }

    BOOL online = NO;
    for (NSInteger attempt = 0; attempt < 50; attempt++) {
        if (DMDisplayIsOnlineInList(_displayID)) {
            online = YES;
            break;
        }
        usleep(100 * 1000);
    }
    if (!online) {
        return DMFailure(@"virtual display never came online");
    }

    if (CGDisplayIsMain(_displayID)) {
        return DMFailure(@"virtual display became the main display; refusing to continue");
    }
    if (CGDisplayIsInMirrorSet(_displayID) || CGDisplayMirrorsDisplay(_displayID) != kCGNullDirectDisplay) {
        return DMFailure(@"virtual display is in a mirroring set; enable Extended Display mode and retry");
    }

    const size_t expectedPointWidth = _logicalWidth;
    const size_t expectedPointHeight = _logicalHeight;
    const size_t expectedPixelWidth = _logicalWidth * _scale;
    const size_t expectedPixelHeight = _logicalHeight * _scale;

    CGDisplayModeRef chosen = NULL;
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(_displayID, NULL);
    if (modes != NULL) {
        for (CFIndex index = 0; index < CFArrayGetCount(modes); index++) {
            CGDisplayModeRef candidate = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, index);
            if (CGDisplayModeGetWidth(candidate) == expectedPointWidth &&
                CGDisplayModeGetHeight(candidate) == expectedPointHeight &&
                CGDisplayModeGetPixelWidth(candidate) == expectedPixelWidth &&
                CGDisplayModeGetPixelHeight(candidate) == expectedPixelHeight) {
                chosen = CGDisplayModeRetain(candidate);
                break;
            }
        }
        CFRelease(modes);
    }

    if (chosen == NULL) {
        const BOOL anyHiDPIMode = DMAnyBackingScale2Mode(_displayID);
        NSString *hint = anyHiDPIMode
            ? @""
            : @" The platform published only 1x modes for this virtual display, so CGVirtualDisplaySettings.hiDPI had no effect on this macOS build.";
        return DMFailure([NSString stringWithFormat:
            @"no %zux%zu pt / %zux%zu px HiDPI mode was published; available: %@%@",
            expectedPointWidth,
            expectedPointHeight,
            expectedPixelWidth,
            expectedPixelHeight,
            self.modeSummaries,
            hint]);
    }

    CGDisplayModeRef current = CGDisplayCopyDisplayMode(_displayID);
    const BOOL alreadySelected =
        current != NULL &&
        CGDisplayModeGetPixelWidth(current) == expectedPixelWidth &&
        CGDisplayModeGetPixelHeight(current) == expectedPixelHeight &&
        CGDisplayModeGetWidth(current) == expectedPointWidth &&
        CGDisplayModeGetHeight(current) == expectedPointHeight;
    if (current != NULL) {
        CGDisplayModeRelease(current);
    }

    if (!alreadySelected) {
        CGError setError = CGDisplaySetDisplayMode(_displayID, chosen, NULL);
        if (setError != kCGErrorSuccess) {
            NSDictionary<NSString *, id> *failure = DMFailure(
                [NSString stringWithFormat:@"CGDisplaySetDisplayMode failed with CGError %d", setError]
            );
            CGDisplayModeRelease(chosen);
            return failure;
        }
    }
    CGDisplayModeRelease(chosen);

    CGDisplayModeRef applied = CGDisplayCopyDisplayMode(_displayID);
    if (applied == NULL) {
        return DMFailure(@"could not read the applied display mode");
    }
    const size_t pointWidth = CGDisplayModeGetWidth(applied);
    const size_t pointHeight = CGDisplayModeGetHeight(applied);
    const size_t pixelWidth = CGDisplayModeGetPixelWidth(applied);
    const size_t pixelHeight = CGDisplayModeGetPixelHeight(applied);
    const double refreshRate = CGDisplayModeGetRefreshRate(applied);
    NSString *summary = DMModeSummary(applied);
    CGDisplayModeRelease(applied);

    if (pointWidth != expectedPointWidth || pointHeight != expectedPointHeight) {
        return DMFailure([NSString stringWithFormat:@"logical mode verification failed: %@", summary]);
    }
    if (pixelWidth != expectedPixelWidth || pixelHeight != expectedPixelHeight) {
        return DMFailure([NSString stringWithFormat:@"pixel mode verification failed: %@", summary]);
    }
    if (refreshRate > 0 && fabs(refreshRate - _refreshRate) > 0.5) {
        return DMFailure([NSString stringWithFormat:@"refresh verification failed: %@", summary]);
    }
    if (CGDisplayIsInMirrorSet(_displayID) || CGDisplayMirrorsDisplay(_displayID) != kCGNullDirectDisplay) {
        return DMFailure(@"virtual display entered a mirroring set after mode selection");
    }

    return @{
        @"ok": @YES,
        @"displayID": @(_displayID),
        @"modeSummary": summary,
        @"pointWidth": @(pointWidth),
        @"pointHeight": @(pointHeight),
        @"pixelWidth": @(pixelWidth),
        @"pixelHeight": @(pixelHeight),
        @"refreshRate": @(refreshRate),
        @"isMain": @(CGDisplayIsMain(_displayID)),
        @"isMirroring": @(CGDisplayIsInMirrorSet(_displayID) ||
                          CGDisplayMirrorsDisplay(_displayID) != kCGNullDirectDisplay),
    };
}

- (NSDictionary<NSString *, id> *)destroyAndWait {
    if (_displayID == kCGNullDirectDisplay) {
        return @{
            @"ok": @YES,
            @"destroyed": @YES,
            @"onlineAfter": @NO,
        };
    }

    const uint32_t oldDisplayID = _displayID;
    _display = nil;
    _settings = nil;
    _mode = nil;
    _descriptor = nil;
    _queue = nil;
    _displayID = kCGNullDirectDisplay;

    for (NSInteger attempt = 0; attempt < 50; attempt++) {
        if (!DMDisplayIsOnlineInList(oldDisplayID)) {
            return @{
                @"ok": @YES,
                @"destroyed": @YES,
                @"onlineAfter": @NO,
            };
        }
        usleep(100 * 1000);
    }

    return @{
        @"ok": @NO,
        @"destroyed": @NO,
        @"onlineAfter": @(DMDisplayIsOnlineInList(oldDisplayID)),
        @"error": @"released CGVirtualDisplay but the display ID remained online; inspect for a ghost display",
    };
}

@end
