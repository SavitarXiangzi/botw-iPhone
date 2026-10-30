/* Minimal Metal.h stub -- NOT Apple's header.
 *
 * Purpose: let `clang++ -fsyntax-only -x objective-c++` type-check
 * ios/nvn/src/nvn_metal.mm on a machine with no Xcode.  Only the symbols that
 * file actually uses are declared, and the method signatures copy Apple's
 * naming so a mismatch in the .mm shows up here as a type error.
 *
 * It is checked in so CI (and anyone on Windows/Linux with clang) can run
 * scripts/syntax_check_metal.py.  It is never compiled into the app.
 */
#ifndef BOTW_STUB_METAL_H
#define BOTW_STUB_METAL_H

#include <stddef.h>
#include <stdint.h>

typedef unsigned long NSUInteger;
typedef long NSInteger;
typedef signed char BOOL;
#define YES ((BOOL)1)
#define NO ((BOOL)0)
#define nil ((id)0)

__attribute__((objc_root_class))
@interface NSObject
+ (instancetype)alloc;
- (instancetype)init;
@end

@interface NSString : NSObject
- (const char*)UTF8String;
@end

@protocol MTLBuffer;

/* MTLPixelFormat: only the two names nvn_metal.mm mentions. */
typedef NSUInteger MTLPixelFormat;
enum {
    MTLPixelFormatRGBA8Unorm = 70,
    MTLPixelFormatBGRA8Unorm = 80,
};

typedef NSUInteger MTLResourceOptions;
enum { MTLResourceStorageModeShared = 0 };

typedef NSUInteger MTLPrimitiveType;
enum {
    MTLPrimitiveTypePoint = 0,
    MTLPrimitiveTypeLine = 1,
    MTLPrimitiveTypeTriangle = 3,
};

typedef struct { double width, height; } CGSize;

typedef struct { double red, green, blue, alpha; } MTLClearColor;
static inline MTLClearColor MTLClearColorMake(double r, double g, double b, double a) {
    MTLClearColor c; c.red = r; c.green = g; c.blue = b; c.alpha = a; return c;
}

typedef NSUInteger MTLLoadAction;
enum { MTLLoadActionDontCare = 0, MTLLoadActionLoad = 1, MTLLoadActionClear = 2 };

typedef NSUInteger MTLStoreAction;
enum { MTLStoreActionDontCare = 0, MTLStoreActionStore = 1 };

typedef struct { double originX, originY, width, height, znear, zfar; } MTLViewport;
typedef struct { NSUInteger x, y, width, height; } MTLScissorRect;
typedef struct { NSUInteger x, y, z; } MTLOrigin;
typedef struct { NSUInteger width, height, depth; } MTLSize;

static inline MTLOrigin MTLOriginMake(NSUInteger x, NSUInteger y, NSUInteger z) {
    MTLOrigin o; o.x = x; o.y = y; o.z = z; return o;
}
static inline MTLSize MTLSizeMake(NSUInteger w, NSUInteger h, NSUInteger d) {
    MTLSize s; s.width = w; s.height = h; s.depth = d; return s;
}

typedef void (^MTLDeallocator)(void* pointer, NSUInteger length);

@protocol MTLTexture
@property (readonly) NSUInteger width;
@property (readonly) NSUInteger height;
@property (readonly) MTLPixelFormat pixelFormat;
@end

@protocol MTLBlitCommandEncoder
- (void)copyFromTexture:(id<MTLTexture>)src
            sourceSlice:(NSUInteger)slice
            sourceLevel:(NSUInteger)level
           sourceOrigin:(MTLOrigin)origin
             sourceSize:(MTLSize)size
              toTexture:(id<MTLTexture>)dst
       destinationSlice:(NSUInteger)dstSlice
       destinationLevel:(NSUInteger)dstLevel
      destinationOrigin:(MTLOrigin)dstOrigin;
- (void)endEncoding;
@end

@protocol MTLRenderCommandEncoder
- (void)endEncoding;
- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count;
- (void)setViewport:(MTLViewport)viewport;
- (void)setScissorRect:(MTLScissorRect)rect;
@end

@class MTLRenderPassDescriptor;

@protocol MTLCommandBuffer
- (id<MTLBlitCommandEncoder>)blitCommandEncoder;
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor*)descriptor;
- (void)presentDrawable:(id)drawable;
- (void)commit;
@end

@interface MTLRenderPassColorAttachmentDescriptor : NSObject
@property (retain) id<MTLTexture> texture;
@property MTLLoadAction loadAction;
@property MTLStoreAction storeAction;
@property MTLClearColor clearColor;
@end

@interface MTLRenderPassColorAttachmentDescriptorArray : NSObject
- (MTLRenderPassColorAttachmentDescriptor*)objectAtIndexedSubscript:(NSUInteger)index;
@end

@interface MTLRenderPassDescriptor : NSObject
+ (MTLRenderPassDescriptor*)renderPassDescriptor;
@property (readonly) MTLRenderPassColorAttachmentDescriptorArray* colorAttachments;
@end

@protocol MTLCommandQueue
- (id<MTLCommandBuffer>)commandBuffer;
@end

@protocol MTLBuffer
@end

@protocol MTLDevice
@property (readonly) NSString* name;
- (id<MTLCommandQueue>)newCommandQueue;
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void*)pointer
                                   length:(NSUInteger)length
                                  options:(MTLResourceOptions)options
                              deallocator:(MTLDeallocator)deallocator;
@end

id<MTLDevice> MTLCreateSystemDefaultDevice(void);

#endif /* BOTW_STUB_METAL_H */