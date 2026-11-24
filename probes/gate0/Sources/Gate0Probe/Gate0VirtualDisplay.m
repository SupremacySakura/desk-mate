#import "Gate0VirtualDisplay.h"

#import <CoreGraphics/CoreGraphics.h>
#import <dispatch/dispatch.h>

@protocol G0CGVirtualDisplayDescriptor <NSObject>
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

@protocol G0CGVirtualDisplayMode <NSObject>
- (instancetype)initWithWidth:(uint32_t)width
                       height:(uint32_t)height
                  refreshRate:(double)refreshRate;
@end

@protocol G0CGVirtualDisplaySettings <NSObject>
- (instancetype)init;
@property (nonatomic, assign) uint32_t hiDPI;
@property (nonatomic, assign) uint32_t rotation;
@property (nonatomic, copy) NSArray *modes;
@end

@protocol G0CGVirtualDisplay <NSObject>
@property (nonatomic, readonly) uint32_t displayID;
@property (nonatomic, readonly) uint32_t hiDPI;
@property (nonatomic, readonly) NSArray *modes;
- (instancetype)initWithDescriptor:(id)descriptor;
- (BOOL)applySettings:(id)settings;
@end

static NSString * const G0VirtualDisplayErrorDomain = @"dev.deskmate.gate0.virtual-display";

static NSDictionary<NSString *, id> *G0Failure(NSString *message) {
    return @{
        @"ok": @NO,
        @"error": message,
    };
}

static NSDictionary<NSString *, id> *G0Success(void) {
    return @{@"ok": @YES};
}

static NSString *G0ModeSummary(CGDisplayModeRef mode) {
    return [NSString stringWithFormat:@"%zux%zu pt / %zux%zu px @ %.2f Hz",
                                      CGDisplayModeGetWidth(mode),
                                      CGDisplayModeGetHeight(mode),
                                      CGDisplayModeGetPixelWidth(mode),
                                      CGDisplayModeGetPixelHeight(mode),
                                      CGDisplayModeGetRefreshRate(mode)];
}

@implementation G0VirtualDisplaySession {
    id<G0CGVirtualDisplay> _display;
    id<G0CGVirtualDisplayDescriptor> _descriptor;
    id<G0CGVirtualDisplaySettings> _settings;
    id<G0CGVirtualDisplayMode> _mode;
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
        return G0Failure(@"logical display dimensions must be positive");
    }
    if (scale != 1 && scale != 2) {
        return G0Failure(@"display scale must be 1 or 2");
    }
    if (refreshRate <= 0) {
        return G0Failure(@"refresh rate must be positive");
    }

    Class descriptorClass = NSClassFromString(@"CGVirtualDisplayDescriptor");
    Class modeClass = NSClassFromString(@"CGVirtualDisplayMode");
    Class settingsClass = NSClassFromString(@"CGVirtualDisplaySettings");
    Class displayClass = NSClassFromString(@"CGVirtualDisplay");
    if (descriptorClass == Nil || modeClass == Nil || settingsClass == Nil || displayClass == Nil) {
        return G0Failure(@"CGVirtualDisplay classes are unavailable");
    }

    G0VirtualDisplaySession *session = [[G0VirtualDisplaySession alloc] init];
    session->_logicalWidth = logicalWidth;
    session->_logicalHeight = logicalHeight;
    session->_scale = scale;
    session->_refreshRate = refreshRate;
    session->_queue = dispatch_queue_create("dev.deskmate.gate0.virtual-display", DISPATCH_QUEUE_SERIAL);

    id<G0CGVirtualDisplayDescriptor> descriptor =
        [(id<G0CGVirtualDisplayDescriptor>)[(id)descriptorClass alloc] init];
    descriptor.name = @"DeskMate Gate 0 Extended Display";
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

    id<G0CGVirtualDisplayMode> mode =
        [(id<G0CGVirtualDisplayMode>)[(id)modeClass alloc] initWithWidth:logicalWidth
                                                                height:logicalHeight
                                                           refreshRate:refreshRate];
    id<G0CGVirtualDisplaySettings> settings =
        [(id<G0CGVirtualDisplaySettings>)[(id)settingsClass alloc] init];
    settings.hiDPI = scale == 2 ? 1 : 0;
    settings.rotation = 0;
    settings.modes = @[mode];

    id<G0CGVirtualDisplay> display =
        [(id<G0CGVirtualDisplay>)[(id)displayClass alloc] initWithDescriptor:descriptor];
    if (display == nil) {
        return G0Failure(@"CGVirtualDisplay initWithDescriptor: returned nil");
    }
    if (![display applySettings:settings]) {
        return G0Failure(@"CGVirtualDisplay applySettings: returned NO");
    }
    if (display.displayID == kCGNullDirectDisplay) {
        return G0Failure(@"CGVirtualDisplay returned a null display ID");
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
        [summaries addObject:G0ModeSummary(mode)];
    }
    CFRelease(modes);
    return summaries;
}

- (NSDictionary<NSString *, id> *)applyAndSelectHiDPIMode {
    if (_display == nil || _displayID == kCGNullDirectDisplay) {
        return G0Failure(@"no live virtual display");
    }

    BOOL online = NO;
    for (NSInteger attempt = 0; attempt < 50; attempt++) {
        if (CGDisplayIsOnline(_displayID)) {
            online = YES;
            break;
        }
        usleep(100 * 1000);
    }
    if (!online) {
        return G0Failure(@"virtual display never came online");
    }

    if (CGDisplayIsMain(_displayID)) {
        return G0Failure(@"virtual display became the main display; refusing to continue");
    }
    if (CGDisplayIsInMirrorSet(_displayID) || CGDisplayMirrorsDisplay(_displayID) != kCGNullDirectDisplay) {
        return G0Failure(@"virtual display is in a mirroring set; enable Extended Display mode and retry");
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
        return G0Failure([NSString stringWithFormat:
            @"no %zux%zu pt / %zux%zu px HiDPI mode was published; available: %@",
            expectedPointWidth,
            expectedPointHeight,
            expectedPixelWidth,
            expectedPixelHeight,
            self.modeSummaries]);
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
            NSDictionary<NSString *, id> *failure = G0Failure(
                [NSString stringWithFormat:@"CGDisplaySetDisplayMode failed with CGError %d", setError]
            );
            CGDisplayModeRelease(chosen);
            return failure;
        }
    }
    CGDisplayModeRelease(chosen);

    CGDisplayModeRef applied = CGDisplayCopyDisplayMode(_displayID);
    if (applied == NULL) {
        return G0Failure(@"could not read the applied display mode");
    }
    const size_t pointWidth = CGDisplayModeGetWidth(applied);
    const size_t pointHeight = CGDisplayModeGetHeight(applied);
    const size_t pixelWidth = CGDisplayModeGetPixelWidth(applied);
    const size_t pixelHeight = CGDisplayModeGetPixelHeight(applied);
    const double refreshRate = CGDisplayModeGetRefreshRate(applied);
    NSString *summary = G0ModeSummary(applied);
    CGDisplayModeRelease(applied);

    if (pointWidth != expectedPointWidth || pointHeight != expectedPointHeight) {
        return G0Failure([NSString stringWithFormat:@"logical mode verification failed: %@", summary]);
    }
    if (pixelWidth != expectedPixelWidth || pixelHeight != expectedPixelHeight) {
        return G0Failure([NSString stringWithFormat:@"pixel mode verification failed: %@", summary]);
    }
    if (refreshRate > 0 && fabs(refreshRate - _refreshRate) > 0.5) {
        return G0Failure([NSString stringWithFormat:@"refresh verification failed: %@", summary]);
    }
    if (CGDisplayIsInMirrorSet(_displayID) || CGDisplayMirrorsDisplay(_displayID) != kCGNullDirectDisplay) {
        return G0Failure(@"virtual display entered a mirroring set after mode selection");
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
        if (!CGDisplayIsOnline(oldDisplayID)) {
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
        @"onlineAfter": @(CGDisplayIsOnline(oldDisplayID)),
        @"error": @"released CGVirtualDisplay but the display ID remained online; inspect for a ghost display",
    };
}

@end
