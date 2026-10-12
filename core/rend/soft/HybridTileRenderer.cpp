#include "HybridTileRenderer.h"

#ifndef TARGET_MAC

namespace rend::soft::hybrid {

bool render(const Frame &, std::vector<u32> &, std::vector<float> &, Timing *,
            std::vector<u32> *) {
  return false;
}

} // namespace rend::soft::hybrid

#endif
