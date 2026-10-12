#include "HybridTileRenderer.h"

#ifndef TARGET_MAC

namespace rend::soft::hybrid {

bool render(const Frame &, std::vector<u32> &, std::vector<float> &, Timing *,
            std::vector<u32> *, PresentationSurface *) {
  return false;
}

bool renderAsync(const Frame &, PresentationSurface &) { return false; }
bool takeLatestPresentation(PresentationSurface &) { return false; }
bool copyLatestPresentation(std::vector<u32> &, PresentationSurface *) {
  return false;
}

} // namespace rend::soft::hybrid

#endif
