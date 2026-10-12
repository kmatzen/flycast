#pragma once

#include "types.h"

#include <vector>

namespace rend::soft::hybrid {

enum StateBits : u32 {
  DepthModeMask = 7,
  DepthWrite = 1 << 3,
  Textured = 1 << 4,
  Bilinear = 1 << 5,
  ClampU = 1 << 6,
  ClampV = 1 << 7,
  FlipU = 1 << 8,
  FlipV = 1 << 9,
  TexelSampleCenter = 1 << 10,
  UseAlpha = 1 << 11,
  IgnoreTextureAlpha = 1 << 12,
  AlphaTest = 1 << 13,
  ShadingInstructionShift = 14,
  SourceBlendShift = 16,
  DestinationBlendShift = 19,
  Autosort = 1 << 22,
};

constexpr u32 makeState(u32 depthMode, bool depthWrite, bool textured,
                        bool bilinear, bool useAlpha, u32 shadingInstruction,
                        u32 sourceBlend, u32 destinationBlend) {
  return (depthMode & 7) | (depthWrite ? DepthWrite : 0) |
         (textured ? Textured : 0) | (bilinear ? Bilinear : 0) |
         (textured ? TexelSampleCenter : 0) | (useAlpha ? UseAlpha : 0) |
         ((shadingInstruction & 3) << ShadingInstructionShift) |
         ((sourceBlend & 7) << SourceBlendShift) |
         ((destinationBlend & 7) << DestinationBlendShift);
}

// GPU-facing representation of one already transformed PVR triangle. The
// layout is deliberately plain and has a matching Metal definition.
struct Triangle {
  s32 x[3];
  s32 y[3];
  float z[3];
  float u[3];
  float v[3];
  u32 color[3];
  u32 textureOffset;
  u32 textureWidth;
  u32 textureHeight;
  u32 state;
  u32 alphaReference;
  s32 minX;
  s32 minY;
  s32 maxX;
  s32 maxY;
  u32 mipOffsets[11];
  u32 mipLevelCount;
  float mipNumerator;
};

static_assert(sizeof(Triangle) == 160, "Metal command layout changed");

struct Frame {
  u32 width = 0;
  u32 height = 0;
  const std::vector<Triangle> *triangles = nullptr;
  // Concatenated RGBA8 texture levels. Triangle offsets index u32 texels.
  const std::vector<u32> *texture = nullptr;
  // Optional starting accumulation buffer. Null starts at opaque black.
  const std::vector<u32> *initialColors = nullptr;
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
