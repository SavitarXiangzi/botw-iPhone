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
- (const char*)fileSystemRepresentation;
- (NSString*)stringByAppendingPathComponent:(NSString*)component;
+ (instancetype)stringWithUTF8String:(const char*)utf8;
@end

/* Only the two calls nvn_metal.mm makes to find <app>/shaders. */
@interface NSBundle : NSObject
+ (NSBundle*)mainBundle;
- (NSString*)resourcePath;
@end

@interface NSError : NSObject
- (NSString*)localizedDescription;
@end

/* Metal validation throws instead of returning nil, and the self-test turns
 * that into a log line.  Only `reason` is needed. */
@interface NSException : NSObject
- (NSString*)reason;
@end

/* iOS 16+: resource and sampler-table entries in an argument buffer are
 * 64-bit MTLResourceID values.  The public struct's only field is the
 * implementation handle, which is exactly what the table stores. */
typedef struct { uint64_t _impl; } MTLResourceID;
typedef NSUInteger MTLResourceUsage;
enum {
    MTLResourceUsageRead = 1u << 0,
    MTLResourceUsageWrite = 1u << 1,
    MTLResourceUsageSample = 1u << 2,
};
typedef NSUInteger MTLRenderStages;
enum {
    MTLRenderStageVertex = 1u << 0,
    MTLRenderStageFragment = 1u << 1,
    MTLRenderStageTile = 1u << 2,
};

/* Device capability queries used by the self-test diagnostics.  The numeric
 * values are Metal's public values; only Apple7 and Metal3 are named here. */
typedef NSUInteger MTLGPUFamily;
enum {
    MTLGPUFamilyApple7 = 1007,
    MTLGPUFamilyMetal3 = 5001,
};
@protocol MTLResource
@end

typedef NSUInteger MTLCommandBufferStatus;
enum {
    MTLCommandBufferStatusCompleted = 4,
    MTLCommandBufferStatusError = 5,
};

@protocol MTLBuffer;

/* MTLPixelFormat: the subset used by nvn_metal.mm and its simulator selftest.
 *
 * The real SDK declares this as NS_ENUM(NSUInteger, MTLPixelFormat), i.e. a
 * distinct enum type, not a typedef to NSUInteger.  Keeping it an enum here
 * is deliberate: CI caught a real "NSUInteger assigned to MTLPixelFormat"
 * that an integer typedef had hidden. */
typedef enum MTLPixelFormat : NSUInteger {
    MTLPixelFormatR32Uint = 53,
    MTLPixelFormatR32Sint = 54,
    MTLPixelFormatR32Float = 55,
    MTLPixelFormatRGBA8Unorm = 70,
    MTLPixelFormatRGBA8Unorm_sRGB = 71,
    MTLPixelFormatRGBA8Uint = 73,
    MTLPixelFormatBGRA8Unorm = 80,
    /* Depth and stencil formats.  The numeric values are Metal's; the
     * texture-format table in nvn_texture.c already uses them. */
    MTLPixelFormatDepth16Unorm = 250,
    MTLPixelFormatDepth32Float = 252,
    MTLPixelFormatStencil8 = 253,
    MTLPixelFormatDepth32Float_Stencil8 = 260,
} MTLPixelFormat;

typedef NSUInteger MTLResourceOptions;
enum { MTLResourceStorageModeShared = 0 };

typedef NSUInteger MTLTextureType;
enum {
    MTLTextureType1D = 0,
    MTLTextureType1DArray = 1,
    MTLTextureType2D = 2,
    MTLTextureType2DArray = 3,
    MTLTextureType2DMultisample = 4,
    MTLTextureTypeCube = 5,
    MTLTextureTypeCubeArray = 6,
    MTLTextureType3D = 7,
    MTLTextureType2DMultisampleArray = 8,
    MTLTextureTypeTextureBuffer = 9,
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

/* Sampler state.  The numbers are Metal's real values; only names used by
 * nvn_metal.mm need to exist here. */
typedef enum MTLSamplerMinMagFilter : NSUInteger {
    MTLSamplerMinMagFilterNearest = 0,
    MTLSamplerMinMagFilterLinear = 1,
} MTLSamplerMinMagFilter;

typedef enum MTLSamplerMipFilter : NSUInteger {
    MTLSamplerMipFilterNotMipmapped = 0,
    MTLSamplerMipFilterNearest = 1,
    MTLSamplerMipFilterLinear = 2,
} MTLSamplerMipFilter;

typedef enum MTLSamplerAddressMode : NSUInteger {
    MTLSamplerAddressModeClampToEdge = 0,
    MTLSamplerAddressModeMirrorClampToEdge = 1,
    MTLSamplerAddressModeRepeat = 2,
    MTLSamplerAddressModeMirrorRepeat = 3,
    MTLSamplerAddressModeClampToZero = 4,
    MTLSamplerAddressModeClampToBorderColor = 5,
} MTLSamplerAddressMode;

typedef enum MTLSamplerBorderColor : NSUInteger {
    MTLSamplerBorderColorTransparentBlack = 0,
    MTLSamplerBorderColorOpaqueBlack = 1,
    MTLSamplerBorderColorOpaqueWhite = 2,
} MTLSamplerBorderColor;

typedef enum MTLCompareFunction : NSUInteger {
    MTLCompareFunctionNever = 0,
    MTLCompareFunctionLess = 1,
    MTLCompareFunctionEqual = 2,
    MTLCompareFunctionLessEqual = 3,
    MTLCompareFunctionGreater = 4,
    MTLCompareFunctionNotEqual = 5,
    MTLCompareFunctionGreaterEqual = 6,
    MTLCompareFunctionAlways = 7,
} MTLCompareFunction;

typedef enum MTLBlendFactor : NSUInteger {
    MTLBlendFactorZero = 0,
    MTLBlendFactorOne = 1,
    MTLBlendFactorSourceColor = 2,
    MTLBlendFactorOneMinusSourceColor = 3,
    MTLBlendFactorSourceAlpha = 4,
    MTLBlendFactorOneMinusSourceAlpha = 5,
    MTLBlendFactorDestinationAlpha = 6,
    MTLBlendFactorOneMinusDestinationAlpha = 7,
    MTLBlendFactorDestinationColor = 8,
    MTLBlendFactorOneMinusDestinationColor = 9,
    MTLBlendFactorSourceAlphaSaturated = 10,
    MTLBlendFactorBlendColor = 11,
    MTLBlendFactorOneMinusBlendColor = 12,
    MTLBlendFactorBlendAlpha = 13,
    MTLBlendFactorOneMinusBlendAlpha = 14,
    MTLBlendFactorSource1Color = 15,
    MTLBlendFactorOneMinusSource1Color = 16,
    MTLBlendFactorSource1Alpha = 17,
    MTLBlendFactorOneMinusSource1Alpha = 18,
} MTLBlendFactor;

typedef enum MTLBlendOperation : NSUInteger {
    MTLBlendOperationAdd = 0,
    MTLBlendOperationSubtract = 1,
    MTLBlendOperationReverseSubtract = 2,
    MTLBlendOperationMin = 3,
    MTLBlendOperationMax = 4,
} MTLBlendOperation;

typedef NSUInteger MTLColorWriteMask;
enum {
    MTLColorWriteMaskNone = 0,
    MTLColorWriteMaskAlpha = 1 << 0,
    MTLColorWriteMaskBlue = 1 << 1,
    MTLColorWriteMaskGreen = 1 << 2,
    MTLColorWriteMaskRed = 1 << 3,
    MTLColorWriteMaskAll = 0xF,
};

typedef NSUInteger MTLStencilOperation;
enum {
    MTLStencilOperationKeep = 0,
    MTLStencilOperationZero = 1,
    MTLStencilOperationReplace = 2,
    MTLStencilOperationIncrementClamp = 3,
    MTLStencilOperationDecrementClamp = 4,
    MTLStencilOperationInvert = 5,
    MTLStencilOperationIncrementWrap = 6,
    MTLStencilOperationDecrementWrap = 7,
};

typedef NSUInteger MTLCullMode;
enum {
    MTLCullModeNone = 0,
    MTLCullModeFront = 1,
    MTLCullModeBack = 2,
};

typedef NSUInteger MTLWinding;
enum {
    MTLWindingClockwise = 0,
    MTLWindingCounterClockwise = 1,
};

typedef NSUInteger MTLTriangleFillMode;
enum {
    MTLTriangleFillModeFill = 0,
    MTLTriangleFillModeLines = 1,
};

typedef NSUInteger MTLDepthClipMode;
enum {
    MTLDepthClipModeClip = 0,
    MTLDepthClipModeClamp = 1,
};

typedef enum MTLSamplerReductionMode : NSUInteger {
    MTLSamplerReductionModeWeightedAverage = 0,
    MTLSamplerReductionModeMinimum = 1,
    MTLSamplerReductionModeMaximum = 2,
} MTLSamplerReductionMode;

@interface MTLSamplerDescriptor : NSObject
@property MTLSamplerMinMagFilter minFilter;
@property MTLSamplerMinMagFilter magFilter;
@property MTLSamplerMipFilter mipFilter;
@property NSUInteger maxAnisotropy;
@property MTLSamplerAddressMode sAddressMode;
@property MTLSamplerAddressMode tAddressMode;
@property MTLSamplerAddressMode rAddressMode;
@property MTLSamplerBorderColor borderColor;
@property BOOL normalizedCoordinates;
@property float lodMinClamp;
@property float lodMaxClamp;
@property MTLCompareFunction compareFunction;
@property MTLSamplerReductionMode reductionMode;
@property BOOL supportArgumentBuffers;
@end

@protocol MTLSamplerState
@property (readonly) MTLResourceID gpuResourceID;
@end

/* Metal argument buffers: the encoder's reflection is unavailable on some
 * simulator runtimes, so nvn_metal.mm writes the 8-byte entry ABI directly. */
@protocol MTLTexture;
@protocol MTLSamplerState;

@protocol MTLArgumentEncoder
- (void)setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset;
- (void)setBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset atIndex:(NSUInteger)index;
- (void)setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (void)setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index;
@property (readonly) NSUInteger encodedLength;
@end

@protocol MTLFunction
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index;
@end

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
@property (getter=isBlendingEnabled) BOOL blendingEnabled;
@property MTLBlendFactor sourceRGBBlendFactor;
@property MTLBlendFactor destinationRGBBlendFactor;
@property MTLBlendOperation rgbBlendOperation;
@property MTLBlendFactor sourceAlphaBlendFactor;
@property MTLBlendFactor destinationAlphaBlendFactor;
@property MTLBlendOperation alphaBlendOperation;
@property MTLColorWriteMask writeMask;
@end

@interface MTLRenderPipelineColorAttachmentDescriptorArray : NSObject
- (MTLRenderPipelineColorAttachmentDescriptor*)objectAtIndexedSubscript:(NSUInteger)index;
@end

@interface MTLRenderPipelineDescriptor : NSObject
@property (retain) id<MTLFunction> vertexFunction;
@property (retain) id<MTLFunction> fragmentFunction;
@property (readonly) MTLRenderPipelineColorAttachmentDescriptorArray* colorAttachments;
@property (retain) MTLVertexDescriptor* vertexDescriptor;
@property MTLPixelFormat depthAttachmentPixelFormat;
@property MTLPixelFormat stencilAttachmentPixelFormat;
@property NSUInteger rasterSampleCount;
@property (getter=isAlphaToCoverageEnabled) BOOL alphaToCoverageEnabled;
@end

/* Depth/stencil objects.  Only the fields nvn_metal.mm sets are declared. */
@interface MTLStencilDescriptor : NSObject
@property MTLCompareFunction stencilCompareFunction;
@property MTLStencilOperation stencilFailureOperation;
@property MTLStencilOperation depthFailureOperation;
@property MTLStencilOperation depthStencilPassOperation;
@property uint32_t readMask;
@property uint32_t writeMask;
@end

@interface MTLDepthStencilDescriptor : NSObject
@property MTLCompareFunction depthCompareFunction;
@property (getter=isDepthWriteEnabled) BOOL depthWriteEnabled;
@property (retain) MTLStencilDescriptor* frontFaceStencil;
@property (retain) MTLStencilDescriptor* backFaceStencil;
@end

@protocol MTLDepthStencilState
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

@protocol MTLTexture <MTLResource>
@property (readonly) NSUInteger width;
@property (readonly) NSUInteger height;
@property (readonly) NSUInteger depth;
@property (readonly) NSUInteger arrayLength;
@property (readonly) NSUInteger mipmapLevelCount;
@property (readonly) NSUInteger sampleCount;
@property (readonly) MTLTextureType textureType;
@property (readonly) MTLPixelFormat pixelFormat;
@property (readonly) MTLResourceID gpuResourceID;
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
- (void)copyFromBuffer:(id<MTLBuffer>)src
          sourceOffset:(NSUInteger)srcOffset
     sourceBytesPerRow:(NSUInteger)bpr
   sourceBytesPerImage:(NSUInteger)bpi
            sourceSize:(MTLSize)size
             toTexture:(id<MTLTexture>)dst
      destinationSlice:(NSUInteger)dstSlice
      destinationLevel:(NSUInteger)dstLevel
     destinationOrigin:(MTLOrigin)dstOrigin;
@end

@protocol MTLComputePipelineState
@end

@protocol MTLComputeCommandEncoder
- (void)endEncoding;
- (void)setComputePipelineState:(id<MTLComputePipelineState>)state;
- (void)setBuffer:(id<MTLBuffer>)buffer
           offset:(NSUInteger)offset
          atIndex:(NSUInteger)index;
- (void)setBytes:(const void*)bytes
          length:(NSUInteger)length
         atIndex:(NSUInteger)index;
- (void)dispatchThreadgroups:(MTLSize)threadgroups
      threadsPerThreadgroup:(MTLSize)threadsPerThreadgroup;
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
- (void)setBlendColorRed:(float)red green:(float)green blue:(float)blue alpha:(float)alpha;
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)state;
- (void)setDepthStencilState:(id<MTLDepthStencilState>)state;
- (void)setStencilFrontReferenceValue:(uint32_t)frontReferenceValue
                   backReferenceValue:(uint32_t)backReferenceValue;
- (void)setCullMode:(MTLCullMode)mode;
- (void)setFrontFacingWinding:(MTLWinding)winding;
- (void)setTriangleFillMode:(MTLTriangleFillMode)mode;
- (void)setDepthBias:(float)depthBias
          slopeScale:(float)slopeScale
               clamp:(float)clamp;
- (void)setDepthClipMode:(MTLDepthClipMode)depthClipMode;
- (void)setVertexBuffer:(id<MTLBuffer>)buffer
                 offset:(NSUInteger)offset
                atIndex:(NSUInteger)index;
- (void)setFragmentBuffer:(id<MTLBuffer>)buffer
                   offset:(NSUInteger)offset
                  atIndex:(NSUInteger)index;
- (void)setFragmentBytes:(const void*)bytes
                  length:(NSUInteger)length
                 atIndex:(NSUInteger)index;
- (void)setFragmentTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index;
- (void)setFragmentSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index;
- (void)useResource:(id<MTLResource>)resource usage:(MTLResourceUsage)usage stages:(MTLRenderStages)stages;
@end

@class MTLRenderPassDescriptor;

@protocol MTLCommandBuffer
- (id<MTLBlitCommandEncoder>)blitCommandEncoder;
- (id<MTLComputeCommandEncoder>)computeCommandEncoder;
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

@interface MTLRenderPassDepthAttachmentDescriptor : NSObject
@property (retain) id<MTLTexture> texture;
@property MTLLoadAction loadAction;
@property MTLStoreAction storeAction;
@property double clearDepth;
@end

@interface MTLRenderPassStencilAttachmentDescriptor : NSObject
@property (retain) id<MTLTexture> texture;
@property MTLLoadAction loadAction;
@property MTLStoreAction storeAction;
@property uint32_t clearStencil;
@end

@interface MTLRenderPassDescriptor : NSObject
+ (MTLRenderPassDescriptor*)renderPassDescriptor;
@property (readonly) MTLRenderPassColorAttachmentDescriptorArray* colorAttachments;
@property (retain) MTLRenderPassDepthAttachmentDescriptor* depthAttachment;
@property (retain) MTLRenderPassStencilAttachmentDescriptor* stencilAttachment;
@end

@protocol MTLCommandQueue
- (id<MTLCommandBuffer>)commandBuffer;
@end

@protocol MTLBuffer <MTLResource>
- (void*)contents;
@property (readonly) NSUInteger length;
/* Metal 3 / iOS 16+: the GPU address an argument buffer entry stores. */
@property (readonly) unsigned long long gpuAddress;
@end

@interface MTLTextureDescriptor : NSObject
@property MTLPixelFormat pixelFormat;
@property NSUInteger width;
@property NSUInteger height;
@property NSUInteger depth;
@property NSUInteger arrayLength;
@property NSUInteger mipmapLevelCount;
@property NSUInteger sampleCount;
@property MTLTextureType textureType;
@property MTLTextureUsage usage;
@property MTLStorageMode storageMode;
@end

@protocol MTLDevice
@property (readonly) NSString* name;
@property (readonly) BOOL hasUnifiedMemory;
- (BOOL)supportsFamily:(MTLGPUFamily)family;
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
- (id<MTLComputePipelineState>)newComputePipelineStateWithFunction:
                                  (id<MTLFunction>)function
                                                             error:(NSError**)error;
- (id<MTLSamplerState>)newSamplerStateWithDescriptor:(MTLSamplerDescriptor*)descriptor;
- (id<MTLDepthStencilState>)newDepthStencilStateWithDescriptor:
    (MTLDepthStencilDescriptor*)descriptor;
@end

id<MTLDevice> MTLCreateSystemDefaultDevice(void);

#endif /* BOTW_STUB_METAL_H */
