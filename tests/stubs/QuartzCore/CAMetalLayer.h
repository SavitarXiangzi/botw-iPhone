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
@property BOOL displaySyncEnabled;
@property NSUInteger maximumDrawableCount;
- (id<CAMetalDrawable>)nextDrawable;
@end

#endif /* BOTW_STUB_CAMETALAYER_H */