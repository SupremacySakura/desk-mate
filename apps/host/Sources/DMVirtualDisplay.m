#import "DMVirtualDisplay.h"

#import <CoreGraphics/CoreGraphics.h>
#import <dispatch/dispatch.h>
#import <math.h>
#import <stdlib.h>
#import <unistd.h>

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

static CFDictionaryRef DMCopyAllModesOptions(void) {
    static CFDictionaryRef options = NULL;
    static dispatch_once_t onceToken;
    dispatch_once(&onceToken, ^{
        NSDictionary *dict = @{
            (__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes: @YES,
        };
        options = (__bridge_retained CFDictionaryRef)dict;
    });
    return options;
}

static void DMWaitShort(double seconds) {
    if (seconds <= 0) {
        return;
    }
    if ([NSThread isMainThread]) {
        // SkyLight delivers CGSDisplayNotifyProc updates onto the main run loop;
        // blocking the main thread with usleep() would starve display discovery.
        const CFAbsoluteTime deadline = CFAbsoluteTimeGetCurrent() + seconds;
        for (;;) {
            const CFAbsoluteTime remaining = deadline - CFAbsoluteTimeGetCurrent();
            if (remaining <= 0) {
                break;
            }
            const CFRunLoopRunResult result = CFRunLoopRunInMode(
                kCFRunLoopDefaultMode,
                remaining < 0.01 ? remaining : 0.01,
                true
            );
            if (result != kCFRunLoopRunHandledSource) {
                usleep(2000);
            }
        }
    } else {
        usleep((useconds_t)(seconds * 1000000.0));
    }
}

static BOOL DMDisplayHasPublishedModes(uint32_t displayID) {
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(displayID, DMCopyAllModesOptions());
    if (modes == NULL) {
        return NO;
    }
    const CFIndex count = CFArrayGetCount(modes);
    CFRelease(modes);
    return count > 0;
}

static BOOL DMAnyBackingScale2Mode(uint32_t displayID) {
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(displayID, DMCopyAllModesOptions());
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
    return [self createWithLogicalWidth:logicalWidth
                          logicalHeight:logicalHeight
                                  scale:scale
                            refreshRate:refreshRate
                                   name:@"DeskMate Extended Display"
                              serialNum:0x0001];
}

+ (NSDictionary<NSString *, id> *)createWithLogicalWidth:(uint32_t)logicalWidth
                                           logicalHeight:(uint32_t)logicalHeight
                                                   scale:(uint32_t)scale
                                             refreshRate:(double)refreshRate
                                                    name:(NSString *)name
                                               serialNum:(uint32_t)serialNum {
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

    NSString *resolvedName = (name.length > 0) ? name : @"DeskMate Extended Display";
    const uint32_t resolvedSerial = (serialNum != 0) ? serialNum : 0x0001;
    NSString *queueLabel = [NSString stringWithFormat:@"dev.deskmate.host.virtual-display.%u", resolvedSerial];

    DMVirtualDisplaySession *session = [[DMVirtualDisplaySession alloc] init];
    session->_logicalWidth = logicalWidth;
    session->_logicalHeight = logicalHeight;
    session->_scale = scale;
    session->_refreshRate = refreshRate;
    session->_queue = dispatch_queue_create(queueLabel.UTF8String, DISPATCH_QUEUE_SERIAL);

    id<DMCGVirtualDisplayDescriptor> descriptor =
        [(id<DMCGVirtualDisplayDescriptor>)[(id)descriptorClass alloc] init];
    descriptor.name = resolvedName;
    descriptor.vendorID = 0x0D53;
    descriptor.productID = 0x0005;
    descriptor.serialNum = resolvedSerial;
    const uint32_t pixelWidth = logicalWidth * scale;
    const uint32_t pixelHeight = logicalHeight * scale;
    descriptor.maxPixelsWide = pixelWidth;
    descriptor.maxPixelsHigh = pixelHeight;
    // Use realistic Retina tablet PPI (~250 PPI) so macOS publishes 2x HiDPI modes.
    const double ppi = scale == 2 ? 250.0 : 120.0;
    const double widthMM = ((double)pixelWidth / ppi) * 25.4;
    const double heightMM = ((double)pixelHeight / ppi) * 25.4;
    descriptor.sizeInMillimeters = CGSizeMake(widthMM, heightMM);
    descriptor.redPrimary = CGPointMake(0.68, 0.32);
    descriptor.greenPrimary = CGPointMake(0.265, 0.69);
    descriptor.bluePrimary = CGPointMake(0.15, 0.06);
    descriptor.whitePoint = CGPointMake(0.3127, 0.3290);
    descriptor.queue = session->_queue;

    NSMutableArray *modes = [NSMutableArray array];
    NSMutableArray<NSNumber *> *rates = [NSMutableArray arrayWithObject:@(refreshRate)];
    if (fabs(refreshRate - 60.0) > 1.0) {
        [rates addObject:@(60.0)];
    }
    for (NSNumber *rateNum in rates) {
        const double rate = rateNum.doubleValue;
        if (scale == 2) {
            id<DMCGVirtualDisplayMode> pixelMode =
                [(id<DMCGVirtualDisplayMode>)[(id)modeClass alloc] initWithWidth:pixelWidth
                                                                          height:pixelHeight
                                                                     refreshRate:rate];
            if (pixelMode != nil) {
                [modes addObject:pixelMode];
            }
        }
        id<DMCGVirtualDisplayMode> logicalMode =
            [(id<DMCGVirtualDisplayMode>)[(id)modeClass alloc] initWithWidth:logicalWidth
                                                                      height:logicalHeight
                                                                 refreshRate:rate];
        if (logicalMode != nil) {
            [modes addObject:logicalMode];
        }
    }

    id<DMCGVirtualDisplaySettings> settings =
        [(id<DMCGVirtualDisplaySettings>)[(id)settingsClass alloc] init];
    settings.hiDPI = scale == 2 ? 1 : 0;
    settings.rotation = 0;
    settings.modes = modes;

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
    session->_mode = modes.firstObject;
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
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(_displayID, DMCopyAllModesOptions());
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

    BOOL ready = NO;
    for (NSInteger attempt = 0; attempt < 160; attempt++) {
        if (DMDisplayIsOnlineInList(_displayID) && DMDisplayHasPublishedModes(_displayID)) {
            ready = YES;
            break;
        }
        DMWaitShort(0.025);
    }
    if (!ready) {
        return DMFailure(@"virtual display never came online");
    }

    if (CGDisplayIsMain(_displayID)) {
        return DMFailure(@"virtual display became the main display; refusing to continue");
    }
    if (CGDisplayIsInMirrorSet(_displayID) || CGDisplayMirrorsDisplay(_displayID) != kCGNullDirectDisplay) {
        CGDisplayConfigRef cfg = NULL;
        if (CGBeginDisplayConfiguration(&cfg) == kCGErrorSuccess && cfg != NULL) {
            if (CGConfigureDisplayMirrorOfDisplay(cfg, _displayID, kCGNullDirectDisplay) == kCGErrorSuccess) {
                CGCompleteDisplayConfiguration(cfg, kCGConfigureForSession);
                DMWaitShort(0.1);
            } else {
                CGCancelDisplayConfiguration(cfg);
            }
        }
        if (CGDisplayIsInMirrorSet(_displayID) || CGDisplayMirrorsDisplay(_displayID) != kCGNullDirectDisplay) {
            return DMFailure(@"virtual display is in a mirroring set; enable Extended Display mode and retry");
        }
    }

    const size_t expectedPointWidth = _logicalWidth;
    const size_t expectedPointHeight = _logicalHeight;
    const size_t expectedPixelWidth = _logicalWidth * _scale;
    const size_t expectedPixelHeight = _logicalHeight * _scale;

    CGDisplayModeRef chosen = NULL;
    CGDisplayModeRef fallbackSameSize = NULL;
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(_displayID, DMCopyAllModesOptions());
    if (modes != NULL) {
        for (CFIndex index = 0; index < CFArrayGetCount(modes); index++) {
            CGDisplayModeRef candidate = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, index);
            if (CGDisplayModeGetWidth(candidate) == expectedPointWidth &&
                CGDisplayModeGetHeight(candidate) == expectedPointHeight &&
                CGDisplayModeGetPixelWidth(candidate) == expectedPixelWidth &&
                CGDisplayModeGetPixelHeight(candidate) == expectedPixelHeight) {
                const double rate = CGDisplayModeGetRefreshRate(candidate);
                if (rate <= 0 || fabs(rate - _refreshRate) <= 1.0) {
                    chosen = CGDisplayModeRetain(candidate);
                    break;
                }
                if (fallbackSameSize == NULL) {
                    fallbackSameSize = CGDisplayModeRetain(candidate);
                }
            }
        }
        CFRelease(modes);
    }
    if (chosen == NULL && fallbackSameSize != NULL) {
        chosen = fallbackSameSize;
        fallbackSameSize = NULL;
    }
    if (fallbackSameSize != NULL) {
        CGDisplayModeRelease(fallbackSameSize);
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
    const double currentRate = current != NULL ? CGDisplayModeGetRefreshRate(current) : 0.0;
    const double chosenRate = CGDisplayModeGetRefreshRate(chosen);
    const BOOL alreadySelected =
        current != NULL &&
        CGDisplayModeGetPixelWidth(current) == expectedPixelWidth &&
        CGDisplayModeGetPixelHeight(current) == expectedPixelHeight &&
        CGDisplayModeGetWidth(current) == expectedPointWidth &&
        CGDisplayModeGetHeight(current) == expectedPointHeight &&
        fabs(currentRate - chosenRate) <= 0.5;
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
    if (refreshRate > 0 && chosenRate > 0 && fabs(refreshRate - chosenRate) > 1.5) {
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

    for (NSInteger attempt = 0; attempt < 120; attempt++) {
        if (!DMDisplayIsOnlineInList(oldDisplayID)) {
            return @{
                @"ok": @YES,
                @"destroyed": @YES,
                @"onlineAfter": @NO,
            };
        }
        DMWaitShort(0.025);
    }

    return @{
        @"ok": @NO,
        @"destroyed": @NO,
        @"onlineAfter": @(DMDisplayIsOnlineInList(oldDisplayID)),
        @"error": @"released CGVirtualDisplay but the display ID remained online; inspect for a ghost display",
    };
}

@end
