#include "MetalPresentationFrame.h"

namespace rend::soft {
id<MTLTexture> uploadMetalPresentationFrame(id<MTLDevice> device, const PresentationFrame& frame)
{
    static_assert(sizeof(Color) == 4, "Presentation pixels must be packed RGBA bytes");
    // Desktop presentation baseline: Mac2 and Apple3+ support 16384-pixel
    // 2D dimensions. Reject larger packets before Metal validation can abort.
    // This is a supported-path limit, not a claim about newer device maxima.
    if (device == nil || frame.width > 16384 || frame.height > 16384)
        return nil;
    MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:frame.width height:frame.height mipmapped:NO];
    descriptor.usage = MTLTextureUsageShaderRead;
    descriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    if (texture != nil)
        [texture replaceRegion:MTLRegionMake2D(0, 0, frame.width, frame.height)
            mipmapLevel:0 withBytes:frame.pixels.data() bytesPerRow:size_t(frame.width) * sizeof(Color)];
    return texture;
}
}
