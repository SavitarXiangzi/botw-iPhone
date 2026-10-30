/* Minimal QuartzCore/CAMetalLayer.h stub -- NOT Apple's header.
 * See tests/stubs/Metal/Metal.h for why this exists. */
#ifndef BOTW_STUB_CAMETALAYER_H
#define BOTW_STUB_CAMETALAYER_H

#import <Metal/Metal.h>

@protocol CAMetalDrawable
@property (readonly) id<MTLTexture> texture;
@end

@interface CAMetalLayer : NSObject
+ (instancetype)layer;
@property (retain) id<MTLDevice> device;
@property MTLPixelFormat pixelFormat;
@property BOOL framebufferOnly;
/* NOTE: displaySyncEnabled is deliberately absent.  It exists on macOS but the
 * iOS 17 SDK does not declare it, and an earlier version of this stub had it --
 * which let a real build error through to CI.  Keep this file no looser than
 * the SDK it stands in for. */
@property NSUInteger maximumDrawableCount;
- (id<CAMetalDrawable>)nextDrawable;
@end

#endif /* BOTW_STUB_CAMETALAYER_H */