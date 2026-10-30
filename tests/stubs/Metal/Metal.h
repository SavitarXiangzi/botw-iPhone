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
+ (instancetype)stringWithUTF8String:(const char*)utf8;
@end

@interface NSError : NSObject
- (NSString*)localizedDescription;
@end

typedef NSUInteger MTLCommandBufferStatus;
enum {
    MTLCommandBufferStatusCompleted = 4,
    MTLCommandBufferStatusError = 5,
};

@protocol MTLBuffer;

/* MTLPixelFormat: only the two names nvn_metal.mm mentions.
 *
 * The real SDK declares this as NS_ENUM(NSUInteger, MTLPixelFormat), i.e. a
 * distinct enum type, not a typedef to NSUInteger.  Keeping it an enum here
 * is deliberate: CI caught a real "NSUInteger assigned to MTLPixelFormat"
 * that an integer typedef had hidden. */
typedef enum MTLPixelFormat : NSUInteger {
    MTLPixelFormatRGBA8Unorm = 70,
    MTLPixelFormatBGRA8Unorm = 80,
} MTLPixelFormat;

typedef NSUInteger MTLResourceOptions;
enum { MTLResourceStorageModeShared = 0 };

typedef NSUInteger MTLTextureType;
enum {
    MTLTextureType1D = 0,
    MTLTextureType2D = 2,
    MTLTextureType3D = 3,
};

typedef NSUInteger MTLTextureUsage;
enum {
    MTLTextureUsageShaderRead = 0x01,
    MTLTextureUsageShaderWrite = 0x02,
    MTLTextureUsageRenderTarget = 0x04,
};

typedef NSUInteger MTLStorageMode;
enum {
    MTLStorageModeShared = 0,
    MTLStorageModePrivate = 2,
};

@protocol MTLFunction;
@protocol MTLLibrary
- (id<MTLFunction>)newFunctionWithName:(NSString*)name;
@end

@protocol MTLRenderPipelineState
@end

/* Declared after the pipeline descriptor below; forward-declared here so the
 * property can name it. */
@class MTLVertexDescriptor;

@interface MTLRenderPipelineColorAttachmentDescriptor : NSObject
@property MTLPixelFormat pixelFormat;
@end

@interface MTLRenderPipelineColorAttachmentDescriptorArray : NSObject
- (MTLRenderPipelineColorAttachmentDescriptor*)objectAtIndexedSubscript:(NSUInteger)index;
@end

@interface MTLRenderPipelineDescriptor : NSObject
@property (retain) id<MTLFunction> vertexFunction;
@property (retain) id<MTLFunction> fragmentFunction;
@property (readonly) MTLRenderPipelineColorAttachmentDescriptorArray* colorAttachments;
@property (retain) MTLVertexDescriptor* vertexDescriptor;
@end

/* MTLVertexFormat: every name nvn_metal.mm can select.  The real SDK's
 * numeric values are not needed to type-check, and inventing them here would
 * be worse than useless -- the .mm only ever names them. */
typedef enum MTLVertexFormat : NSUInteger {
    MTLVertexFormatInvalid = 0,
    MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4,
    MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4,
    MTLVertexFormatUCharNormalized, MTLVertexFormatUChar2Normalized,
    MTLVertexFormatUChar3Normalized, MTLVertexFormatUChar4Normalized,
    MTLVertexFormatCharNormalized, MTLVertexFormatChar2Normalized,
    MTLVertexFormatChar3Normalized, MTLVertexFormatChar4Normalized,
    MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4,
    MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4,
    MTLVertexFormatUShortNormalized, MTLVertexFormatUShort2Normalized,
    MTLVertexFormatUShort3Normalized, MTLVertexFormatUShort4Normalized,
    MTLVertexFormatShortNormalized, MTLVertexFormatShort2Normalized,
    MTLVertexFormatShort3Normalized, MTLVertexFormatShort4Normalized,
    MTLVertexFormatHalf, MTLVertexFormatHalf2, MTLVertexFormatHalf3, MTLVertexFormatHalf4,
    MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4,
    MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4,
    MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4,
    MTLVertexFormatInt1010102Normalized, MTLVertexFormatUInt1010102Normalized,
    MTLVertexFormatUChar4Normalized_BGRA,
    MTLVertexFormatFloatRG11B10, MTLVertexFormatFloatRGB9E5,
} MTLVertexFormat;

typedef enum MTLVertexStepFunction : NSUInteger {
    MTLVertexStepFunctionConstant = 0,
    MTLVertexStepFunctionPerVertex = 1,
    MTLVertexStepFunctionPerInstance = 2,
} MTLVertexStepFunction;

@interface MTLVertexAttributeDescriptor : NSObject
@property MTLVertexFormat format;
@property NSUInteger offset;
@property NSUInteger bufferIndex;
@end

@interface MTLVertexAttributeDescriptorArray : NSObject
- (MTLVertexAttributeDescriptor*)objectAtIndexedSubscript:(NSUInteger)index;
@end

@interface MTLVertexBufferLayoutDescriptor : NSObject
@property NSUInteger stride;
@property MTLVertexStepFunction stepFunction;
@property NSUInteger stepRate;
@end

@interface MTLVertexBufferLayoutDescriptorArray : NSObject
- (MTLVertexBufferLayoutDescriptor*)objectAtIndexedSubscript:(NSUInteger)index;
@end

@interface MTLVertexDescriptor : NSObject
+ (MTLVertexDescriptor*)vertexDescriptor;
@property (readonly) MTLVertexAttributeDescriptorArray* attributes;
@property (readonly) MTLVertexBufferLayoutDescriptorArray* layouts;
@end

typedef enum MTLIndexType : NSUInteger {
    MTLIndexTypeUInt16 = 0,
    MTLIndexTypeUInt32 = 1,
} MTLIndexType;

typedef enum MTLPrimitiveType : NSUInteger {
    MTLPrimitiveTypePoint = 0,
    MTLPrimitiveTypeLine = 1,
    MTLPrimitiveTypeTriangle = 3,
} MTLPrimitiveType;

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
- (void)copyFromTexture:(id<MTLTexture>)src
            sourceSlice:(NSUInteger)slice
            sourceLevel:(NSUInteger)level
           sourceOrigin:(MTLOrigin)origin
             sourceSize:(MTLSize)size
               toBuffer:(id<MTLBuffer>)dst
      destinationOffset:(NSUInteger)dstOffset
 destinationBytesPerRow:(NSUInteger)bpr
destinationBytesPerImage:(NSUInteger)bpi;
@end

@protocol MTLRenderCommandEncoder
- (void)endEncoding;
- (void)drawPrimitives:(MTLPrimitiveType)type
           vertexStart:(NSUInteger)start
           vertexCount:(NSUInteger)count;
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type
                   indexCount:(NSUInteger)count
                    indexType:(MTLIndexType)type
                  indexBuffer:(id<MTLBuffer>)buffer
            indexBufferOffset:(NSUInteger)offset;
- (void)setViewport:(MTLViewport)viewport;
- (void)setScissorRect:(MTLScissorRect)rect;
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state;
- (void)setVertexBuffer:(id<MTLBuffer>)buffer
                 offset:(NSUInteger)offset
                atIndex:(NSUInteger)index;
@end

@class MTLRenderPassDescriptor;

@protocol MTLCommandBuffer
- (id<MTLBlitCommandEncoder>)blitCommandEncoder;
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor*)descriptor;
- (void)presentDrawable:(id)drawable;
- (void)commit;
- (void)waitUntilCompleted;
@property (readonly) MTLCommandBufferStatus status;
@property (readonly) NSError* error;
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
- (void*)contents;
@end

@interface MTLTextureDescriptor : NSObject
@property MTLPixelFormat pixelFormat;
@property NSUInteger width;
@property NSUInteger height;
@property NSUInteger depth;
@property NSUInteger mipmapLevelCount;
@property NSUInteger sampleCount;
@property MTLTextureType textureType;
@property MTLTextureUsage usage;
@property MTLStorageMode storageMode;
@end

@protocol MTLDevice
@property (readonly) NSString* name;
- (id<MTLCommandQueue>)newCommandQueue;
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)descriptor;
- (id<MTLBuffer>)newBufferWithBytesNoCopy:(void*)pointer
                                   length:(NSUInteger)length
                                  options:(MTLResourceOptions)options
                              deallocator:(MTLDeallocator)deallocator;
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)length
                            options:(MTLResourceOptions)options;
- (id<MTLLibrary>)newLibraryWithSource:(NSString*)source
                               options:(id)options
                                 error:(NSError**)error;
- (id<MTLRenderPipelineState>)newRenderPipelineStateWithDescriptor:(MTLRenderPipelineDescriptor*)d
                                                             error:(NSError**)error;
@end

id<MTLDevice> MTLCreateSystemDefaultDevice(void);

#endif /* BOTW_STUB_METAL_H */