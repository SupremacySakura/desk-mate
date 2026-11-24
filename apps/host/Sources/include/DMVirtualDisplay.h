#import <Foundation/Foundation.h>

NS_ASSUME_NONNULL_BEGIN

/// Owns one CGVirtualDisplay instance. All private API calls are dynamic so the
/// executable does not link private Objective-C class symbols.
@interface DMVirtualDisplaySession : NSObject

@property (nonatomic, readonly, nullable) NSNumber *displayIDNumber;
@property (nonatomic, readonly) NSArray<NSString *> *modeSummaries;

+ (NSDictionary<NSString *, id> *)createWithLogicalWidth:(uint32_t)logicalWidth
                                           logicalHeight:(uint32_t)logicalHeight
                                                   scale:(uint32_t)scale
                                             refreshRate:(double)refreshRate;

- (NSDictionary<NSString *, id> *)applyAndSelectHiDPIMode;
- (NSDictionary<NSString *, id> *)destroyAndWait;

@end

NS_ASSUME_NONNULL_END
