#include "MetalPresentationFrame.h"
#include <cmath>

namespace rend::soft {
id<MTLRenderPipelineState> createMetalPresentationPipeline(id<MTLDevice> device,
    MTLPixelFormat outputFormat, NSError **error)
{
    if (device == nil) return nil;
    NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 p [[position]]; float2 uv; };\n"
        "vertex V vert(uint i [[vertex_id]]) {\n"
        "float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
        "V v; v.p=float4(p[i],0,1); v.uv=float2((p[i].x+1)*.5,(1-p[i].y)*.5); return v; }\n"
        "fragment float4 frag(V v [[stage_in]], texture2d<float> t [[texture(0)]], constant uint& linear [[buffer(0)]]) {\n"
        "constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::nearest);\n"
        "constexpr sampler l(coord::normalized, address::clamp_to_edge, filter::linear);\n"
        // Match the GL presentation texture's GL_TEXTURE_SWIZZLE_A=GL_ONE:
        // framebuffer alpha is data, not window-compositor transparency.
        "return float4((linear ? t.sample(l,v.uv) : t.sample(s,v.uv)).rgb,1.0); }";
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:error];
    if (library == nil) return nil;
    MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
    descriptor.vertexFunction = [library newFunctionWithName:@"vert"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"frag"];
    descriptor.colorAttachments[0].pixelFormat = outputFormat;
    return [device newRenderPipelineStateWithDescriptor:descriptor error:error];
}

bool encodeMetalPresentation(id<MTLRenderCommandEncoder> encoder,
    id<MTLRenderPipelineState> pipeline, id<MTLTexture> texture, MTLViewport viewport, bool linearFiltering)
{
    if (encoder == nil || pipeline == nil || texture == nil
        || !std::isfinite(viewport.originX) || !std::isfinite(viewport.originY)
        || !std::isfinite(viewport.width) || !std::isfinite(viewport.height)
        || viewport.width <= 0 || viewport.height <= 0)
        return false;
    [encoder setViewport:viewport];
    [encoder setRenderPipelineState:pipeline];
    [encoder setFragmentTexture:texture atIndex:0];
    const unsigned linear = linearFiltering ? 1 : 0;
    [encoder setFragmentBytes:&linear length:sizeof(linear) atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    return true;
}

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
