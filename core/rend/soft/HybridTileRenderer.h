#pragma once

#include "types.h"

#include <vector>

namespace rend::soft::hybrid {

// GPU-facing representation of one already transformed PVR triangle. The
// layout is deliberately plain and has a matching Metal definition.
struct Triangle {
  s32 x[3];
  s32 y[3];
  float z[3];
  float u[3];
  float v[3];
  u32 color[3];
  // Bit 0: source-alpha blend. Bit 1: depth write.
  u32 flags;
  s32 minX;
  s32 minY;
  s32 maxX;
  s32 maxY;
};

static_assert(sizeof(Triangle) == 92, "Metal command layout changed");

struct Frame {
  u32 width = 0;
  u32 height = 0;
  u32 textureWidth = 0;
  u32 textureHeight = 0;
  const std::vector<Triangle> *triangles = nullptr;
  const std::vector<u32> *texture = nullptr;
};

struct Timing {
  double encodeMilliseconds = 0.;
  double gpuMilliseconds = 0.;
  double readbackMilliseconds = 0.;
};

// Renders the currently supported exact subset into packed RGBA8 and IEEE
// depth buffers. False means that Metal is unavailable or the frame violates
// a checked invariant; callers must use the CPU oracle without partial output.
bool render(const Frame &frame, std::vector<u32> &colors,
            std::vector<float> &depths, Timing *timing = nullptr);

} // namespace rend::soft::hybrid
