#pragma once

#include "types.h"

#include <vector>

namespace rend::soft::hybrid {

// This interface has a no-op portable implementation, but the accelerated
// backend is currently implemented only for Apple platforms with Metal. Its
// direct presentation path additionally depends on macOS IOSurface/OpenGL
// interoperability. Keep the CPU SoftwareRasterizer as the portable oracle;
// adding Vulkan, D3D, or other GPU backends requires a separate implementation
// of this contract and its differential tests.

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
  LookupFog = 1 << 23,
  Trilinear = 1 << 24,
  TrilinearPassB = 1 << 25,
  IntensityShadow = 1 << 26,
  Area1Only = 1 << 27,
  ListTypeShift = 28,
  TextureSuperSample = 1u << 30,
};

enum EffectBits : u32 {
  UseOffsetColor = 1 << 0,
  BumpMap = 1 << 1,
  VertexFog = 1 << 2,
  SourceSelect = 1 << 3,
  DestinationSelect = 1 << 4,
  ColorClamp = 1 << 5,
  Dither = 1 << 6,
  LookupFogAlpha = 1 << 7,
  ExcludeClip = 1 << 8,
  ModifierArea1Only = 1 << 9,
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
  u32 shadeScale;
  u32 area1Index;
  s32 superSampleOffsetX;
  s32 superSampleOffsetY;
  u32 offsetColor[3];
  u32 effects;
  u32 clampMin;
  u32 clampMax;
  u32 ditherDivisors;
  s32 fpuSampleOffset;
  s32 tspSampleOffset;
  s32 excludeX0;
  s32 excludeY0;
  s32 excludeX1;
  s32 excludeY1;
};

static_assert(sizeof(Triangle) == 228, "Metal command layout changed");

// A borrowed native surface containing the packed color output. On macOS the
// handle is an IOSurfaceRef and can also seed a later GPU frame directly.
struct PresentationSurface {
  void *handle = nullptr;
  u32 width = 0;
  u32 height = 0;
};

// A raw accumulation surface is display-ready only when no later PVR or CPU
// stage can change its pixels. Keep this policy shared and exhaustively tested:
// direct presentation is an optimization, never a substitute for correctness.
constexpr bool canUseDirectPresentation(bool renderToTexture,
                                        bool emulateFramebuffer,
                                        bool cpuCompletionRequired) {
  return !renderToTexture && !emulateFramebuffer && !cpuCompletionRequired;
}

struct Frame {
  u32 width = 0;
  u32 height = 0;
  const std::vector<Triangle> *triangles = nullptr;
  // Concatenated RGBA8 texture levels. Triangle offsets index u32 texels.
  const std::vector<u32> *texture = nullptr;
  // Optional starting accumulation buffer. Null starts at opaque black.
  const std::vector<u32> *initialColors = nullptr;
  const std::vector<float> *initialDepths = nullptr;
  const u32 *fogTable = nullptr;
  u32 fogColor = 0;
  u32 fogVertexColor = 0;
  float fogDensity = 0.f;
  const std::vector<u8> *modifierMask = nullptr;
  const std::vector<u8> *depthClearMask = nullptr;
  const std::vector<u8> *opaqueMask = nullptr;
  const std::vector<u8> *punchThroughMask = nullptr;
  const std::vector<u8> *translucentMask = nullptr;
  // Pixels cleared here retain initialColors/initialPresentation in the
  // externally visible frame while internal depth/secondary work may proceed.
  const std::vector<u8> *commitMask = nullptr;
  const std::vector<u32> *initialSecondary = nullptr;
  const PresentationSurface *initialPresentation = nullptr;
};

struct Timing {
  double encodeMilliseconds = 0.;
  // CPU wall time from command submission through completion. This includes
  // queueing and the forced synchronization required by the current caller.
  double gpuMilliseconds = 0.;
  // Device timestamps for actual GPU execution, excluding CPU-side queueing.
  double gpuExecutionMilliseconds = 0.;
  double readbackMilliseconds = 0.;
  u32 hardwareVisibilityTiles = 0;
  u32 hardwareVisibilityDraws = 0;
  u32 totalTiles = 0;
  u32 orderedAutosortFrames = 0;
  u32 orderedAutosortTiles = 0;
  u32 globalAutosortFrames = 0;
  u32 simpleTranslucentFrames = 0;
  u32 orderedAutosortRejectReason = 0;
  u32 opaqueTriangles = 0;
  u32 punchThroughTriangles = 0;
  u32 translucentTriangles = 0;
};

// Renders the currently supported exact subset into packed RGBA8 and IEEE
// depth buffers. False means that Metal is unavailable or the frame violates
// a checked invariant; callers must use the CPU oracle without partial output.
bool render(const Frame &frame, std::vector<u32> &colors,
            std::vector<float> &depths, Timing *timing = nullptr,
            std::vector<u32> *secondaryOutput = nullptr,
            PresentationSurface *presentation = nullptr);

// Queues an exact frame without waiting for GPU completion. This succeeds
// only while a protected frame-resource slot is available. Completion can be
// consumed with takeLatestPresentation; callers retain their previous frame
// until then.
bool renderAsync(const Frame &frame, PresentationSurface &presentation);
bool takeLatestPresentation(PresentationSurface &presentation);
bool copyLatestPresentation(std::vector<u32> &colors,
                            PresentationSurface *presentation = nullptr);

} // namespace rend::soft::hybrid
