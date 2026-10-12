#include "MetalPresentationFrame.h"

namespace rend::soft {
id<MTLTexture> uploadMetalPresentationFrame(id<MTLDevice> device, const PresentationFrame& frame)
{
    static_assert(sizeof(Color) == 4, "Presentation pixels must be packed RGBA bytes");
    if (device == nil)
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
