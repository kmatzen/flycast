#pragma once

#ifdef __OBJC__
#import <Metal/Metal.h>
#include "PresentationFrame.h"

namespace rend::soft {
// UI-thread upload of an already converted display image. Each returned
// texture is independent: later uploads cannot mutate an in-flight image.
// The caller retains it until its rendering command buffer completes.
id<MTLTexture> uploadMetalPresentationFrame(id<MTLDevice> device, const PresentationFrame& frame);
}
#endif
