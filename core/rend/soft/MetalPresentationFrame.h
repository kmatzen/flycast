#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>
#include "PresentationFrame.h"

namespace rend::soft {
// UI-thread upload of an already converted display image. Each returned
// texture is independent: later uploads cannot mutate an in-flight image.
// The caller retains it until its rendering command buffer completes.
id<MTLTexture> uploadMetalPresentationFrame(id<MTLDevice> device, const PresentationFrame& frame);
id<MTLRenderPipelineState> createMetalPresentationPipeline(id<MTLDevice> device,
    MTLPixelFormat outputFormat, NSError **error);
// Encodes an unrotated display image into an existing pass. Filtering affects
// only window scaling, never the uploaded framebuffer bytes.
// The caller owns pass clearing, viewport/layout, overlays, submission, and
// resource lifetime. This function neither ends the pass nor presents it.
bool encodeMetalPresentation(id<MTLRenderCommandEncoder> encoder,
    id<MTLRenderPipelineState> pipeline, id<MTLTexture> texture, MTLViewport viewport,
    bool linearFiltering = false);
}
#endif
