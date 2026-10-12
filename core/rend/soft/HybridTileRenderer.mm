#include "HybridTileRenderer.h"

#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>

#include <array>
#include <queue>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>

namespace rend::soft::hybrid {
namespace {

// Internal acceleration bins, not PVR region tiles. Region semantics arrive
// through the per-pixel masks; smaller bins reduce unrelated edge tests.
constexpr u32 TileSize = 16;

struct Constants {
  u32 width;
  u32 height;
  u32 tilesX;
  u32 useInitialColor;
  u32 fogColor;
  u32 fogVertexColor;
  float fogDensity;
  u32 useModifierMask;
  u32 useInitialDepth;
  u32 useInitialSecondary;
  u32 deferOpaqueShading;
  u32 useHardwareVisibility;
  float visibilityBias;
  u32 useCommitMask;
};

static_assert(sizeof(Constants) == 56);

struct VisibilityVertex {
  s32 x;
  s32 y;
  float z;
  u32 padding;
};

static_assert(sizeof(VisibilityVertex) == 16);

const char *KernelSource = R"METAL(
#include <metal_stdlib>
using namespace metal;

struct Triangle {
    int x[3];
    int y[3];
    float z[3];
    float u[3];
    float v[3];
    uint color[3];
    uint textureOffset;
    uint textureWidth;
    uint textureHeight;
    uint state;
    uint alphaReference;
    int minX;
    int minY;
    int maxX;
    int maxY;
    uint mipOffsets[11];
    uint mipLevelCount;
    float mipNumerator;
    uint shadeScale;
    uint area1Index;
    int superSampleOffsetX;
    int superSampleOffsetY;
    uint offsetColor[3];
    uint effects;
    uint clampMin;
    uint clampMax;
    uint ditherDivisors;
    int fpuSampleOffset;
    int tspSampleOffset;
    int excludeX0;
    int excludeY0;
    int excludeX1;
    int excludeY1;
};

struct Constants {
    uint width;
    uint height;
    uint tilesX;
    uint useInitialColor;
    uint fogColor;
    uint fogVertexColor;
    float fogDensity;
    uint useModifierMask;
    uint useInitialDepth;
    uint useInitialSecondary;
    uint deferOpaqueShading;
    uint useHardwareVisibility;
    float visibilityBias;
    uint useCommitMask;
};

struct VisibilityVertex { int2 fixedPosition; float z; uint padding; };
struct VisibilityVarying { float4 position [[position]]; };

vertex VisibilityVarying visibilityVertex(
        const device VisibilityVertex *vertices [[buffer(0)]],
        constant Constants& constants [[buffer(1)]], uint id [[vertex_id]])
{
    const float2 screen = float2(vertices[id].fixedPosition) / 16.0f
            + constants.visibilityBias;
    VisibilityVarying result;
    result.position = float4(screen.x / float(constants.width) * 2.0f - 1.0f,
            1.0f - screen.y / float(constants.height) * 2.0f, 0.5f, 1.0f);
    return result;
}

fragment uint visibilityFragment(
        VisibilityVarying input [[stage_in]],
        const device uint *primitiveIndices [[buffer(0)]],
        const device Triangle *triangles [[buffer(1)]],
        uint primitive [[primitive_id]])
{
    const uint index = primitiveIndices[primitive];
    const device Triangle& triangle = triangles[index];
    const int2 pixel = int2(input.position.xy);
    // Binning/scissors only enforce coarse tile bounds. Reject clipped
    // fragments here, before they can overwrite an earlier valid winner.
    if (pixel.x < triangle.minX || pixel.x > triangle.maxX ||
        pixel.y < triangle.minY || pixel.y > triangle.maxY)
        discard_fragment();
    return index + 1;
}

inline long edge(int ax, int ay, int bx, int by, int x, int y)
{
    return long(bx - ax) * long(y - ay) - long(by - ay) * long(x - ax);
}

inline bool topLeft(int ax, int ay, int bx, int by)
{
    const long dx = long(bx) - ax;
    const long dy = long(by) - ay;
    return dy < 0 || (dy == 0 && dx > 0);
}

inline uint channel(uint color, uint shift)
{
    return (color >> shift) & 0xff;
}

inline uint packColor(float4 value)
{
    const uint4 bytes = uint4(clamp(floor(value + .5f), 0.0f, 255.0f));
    return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}

inline bool depthTest(uint mode, float source, float destination)
{
    switch (mode & 7) {
    case 0: return false;
    case 1: return source < destination;
    case 2: return source == destination;
    case 3: return source <= destination;
    case 4: return source > destination;
    case 5: return source != destination;
    case 6: return source >= destination;
    default: return true;
    }
}

inline uint factor(uint mode, uint source, uint destination,
        bool sourceTerm, uint shift)
{
    const uint other = sourceTerm ? channel(destination, shift) : channel(source, shift);
    switch (mode & 7) {
    case 0: return 0;
    case 1: return 255;
    case 2: return other;
    case 3: return 255 - other;
    case 4: return channel(source, 24);
    case 5: return 255 - channel(source, 24);
    case 6: return channel(destination, 24);
    default: return 255 - channel(destination, 24);
    }
}

inline uint blend(uint source, uint destination, uint sourceFactor, uint destinationFactor)
{
    uint result = 0;
    for (uint shift = 0; shift < 32; shift += 8) {
        const uint sf = factor(sourceFactor, source, destination, true, shift);
        const uint df = factor(destinationFactor, source, destination, false, shift);
        const uint blended = min(255u, (channel(source, shift) * sf
                + channel(destination, shift) * df + 127) / 255);
        result |= blended << shift;
    }
    return result;
}

inline uint sourceAlphaBlend(uint source, uint destination)
{
    const uint alpha = channel(source, 24);
    const uint inverseAlpha = 255 - alpha;
    uint result = 0;
    for (uint shift = 0; shift < 32; shift += 8) {
        const uint blended = (channel(source, shift) * alpha
                + channel(destination, shift) * inverseAlpha + 127) / 255;
        result |= blended << shift;
    }
    return result;
}

inline float wrapCoordinate(float coordinate, bool clampAxis, bool flipAxis)
{
    if (clampAxis)
        return clamp(coordinate, 0.0f, 1.0f);
    const float period = floor(coordinate);
    float fraction = coordinate - period;
    if (flipAxis && (long(period) & 1) != 0)
        fraction = 1.0f - fraction;
    return fraction;
}

inline uint wrapTexel(int coordinate, uint size, bool clampAxis, bool flipAxis)
{
    if (size <= 1)
        return 0;
    if (clampAxis)
        return uint(clamp(coordinate, 0, int(size) - 1));
    if (!flipAxis)
        return uint(coordinate) & (size - 1);
    const uint periodMask = size * 2 - 1;
    const uint wrapped = uint(coordinate) & periodMask;
    return wrapped < size ? wrapped : periodMask - wrapped;
}

inline uint sampleTexture(const device uint *texture, const device Triangle& t, float2 uv,
        float pvrD)
{
    const bool clampU = (t.state & (1u << 6)) != 0;
    const bool clampV = (t.state & (1u << 7)) != 0;
    const bool flipU = (t.state & (1u << 8)) != 0;
    const bool flipV = (t.state & (1u << 9)) != 0;
    uint width = t.textureWidth;
    uint height = t.textureHeight;
    uint textureOffset = t.textureOffset;
    if (t.mipLevelCount > 1) {
        float selected = max(0.0f, floor(max(pvrD, 1.0f)) - 1.0f);
        if ((t.state & (1u << 30)) != 0)
            selected = max(0.0f, selected - 1.0f);
        const uint passOffset = (t.state & (1u << 25)) != 0 ? 1u : 0u;
        selected = min(selected, float(t.mipLevelCount - 1));
        const uint level = min(t.mipLevelCount - 1, uint(selected) + passOffset);
        width = max(1u, width >> level);
        height = max(1u, height >> level);
        textureOffset = t.mipOffsets[level];
    }
    uv.x = wrapCoordinate(uv.x, clampU, flipU);
    uv.y = wrapCoordinate(uv.y, clampV, flipV);
    const float adjustment = (t.state & (1u << 10)) != 0 ? .5f : 0.0f;
    const float2 texel = uv * float2(width, height) - adjustment;
    const int2 base = int2(floor(texel));
    if ((t.state & (1u << 5)) == 0) {
        const int2 nearest = int2(floor(texel + .5f));
        const uint x = wrapTexel(nearest.x, width, clampU, flipU);
        const uint y = wrapTexel(nearest.y, height, clampV, flipV);
        return texture[textureOffset + y * width + x];
    }
    const uint x0 = wrapTexel(base.x, width, clampU, flipU);
    const uint x1 = wrapTexel(base.x + 1, width, clampU, flipU);
    const uint y0 = wrapTexel(base.y, height, clampV, flipV);
    const uint y1 = wrapTexel(base.y + 1, height, clampV, flipV);
    const uint samples[4] = {
        texture[textureOffset + y0 * width + x0],
        texture[textureOffset + y0 * width + x1],
        texture[textureOffset + y1 * width + x0],
        texture[textureOffset + y1 * width + x1],
    };
    const float2 fraction = texel - float2(base);
    float4 result;
    for (uint component = 0; component < 4; ++component) {
        const uint shift = component * 8;
        // Keep the reference's fused interpolation explicit at rounding boundaries.
        const float a = float(channel(samples[0], shift));
        const float b = float(channel(samples[1], shift));
        const float c = float(channel(samples[2], shift));
        const float d = float(channel(samples[3], shift));
        const float top = fma(b - a, fraction.x, a);
        const float bottom = fma(d - c, fraction.x, c);
        result[component] = fma(bottom - top, fraction.y, top);
    }
    return packColor(result);
}

inline uint applyTexture(uint base, uint texel, uint instruction)
{
    if (instruction == 0)
        return texel;
    float4 result;
    if (instruction == 2) {
        const float alpha = float(channel(texel, 24)) / 255.0f;
        for (uint component = 0; component < 3; ++component) {
            const uint shift = component * 8;
            result[component] = float(channel(base, shift))
                    + (float(channel(texel, shift)) - float(channel(base, shift))) * alpha;
        }
        result.w = float(channel(base, 24));
        return packColor(result);
    }
    for (uint component = 0; component < 4; ++component) {
        const uint shift = component * 8;
        result[component] = component == 3 && instruction == 1
                ? float(channel(texel, shift))
                : float(channel(base, shift) * channel(texel, shift)) / 255.0f;
    }
    return packColor(result);
}

inline float fogCoefficient(float inverseDepth, float density,
        const device uint *table)
{
    const float fogZ = clamp(density * inverseDepth, 1.0f, 255.9999f);
    const uint bits = as_type<uint>(fogZ);
    const uint exponent = ((bits >> 23) & 0xff) - 127;
    const uint mantissa = bits & 0x7fffff;
    const uint index = min(127u, (exponent << 4) | (mantissa >> 19));
    const float fraction = float(mantissa & 0x7ffff) / 524288.0f;
    const uint entry = table[index];
    const float first = float(entry & 0xff);
    const float second = float((entry >> 8) & 0xff);
    return (second + (first - second) * fraction) / 255.0f;
}

inline float2 projectedUV(const device Triangle& t, int sx, int sy)
{
    const long w0 = edge(t.x[1], t.y[1], t.x[2], t.y[2], sx, sy);
    const long w1 = edge(t.x[2], t.y[2], t.x[0], t.y[0], sx, sy);
    const long w2 = edge(t.x[0], t.y[0], t.x[1], t.y[1], sx, sy);
    const long area = edge(t.x[0], t.y[0], t.x[1], t.y[1], t.x[2], t.y[2]);
    const float s0 = float(w0) / float(area);
    const float s1 = float(w1) / float(area);
    const float s2 = float(w2) / float(area);
    const float q = t.z[0] * s0 + t.z[1] * s1 + t.z[2] * s2;
    const float inverseQ = q == 0.0f ? 0.0f : 1.0f / q;
    return float2((t.u[0] * t.z[0] * s0 + t.u[1] * t.z[1] * s1
            + t.u[2] * t.z[2] * s2) * inverseQ,
            (t.v[0] * t.z[0] * s0 + t.v[1] * t.z[1] * s1
            + t.v[2] * t.z[2] * s2) * inverseQ);
}

template <uint MaxSortedFragments, bool OrderedAutosort,
        bool SimpleTranslucent = false>
kernel void rasterTiles(const device Triangle *triangles [[buffer(0)]],
        const device uint *tileOffsets [[buffer(1)]],
        const device uint *tileReferences [[buffer(2)]],
        device uint *colors [[buffer(3)]], device float *depths [[buffer(4)]],
        const device uint *texture [[buffer(5)]],
        constant Constants& constants [[buffer(6)]],
        device atomic_uint *overflow [[buffer(7)]],
        const device uint *fogTable [[buffer(8)]],
        const device uchar *modifierMask [[buffer(9)]],
        const device uchar *depthClearMask [[buffer(10)]],
        const device uchar *opaqueMask [[buffer(11)]],
        const device uchar *punchThroughMask [[buffer(12)]],
        const device uchar *translucentMask [[buffer(13)]],
        device uint *secondaryColors [[buffer(14)]],
        const device uchar *hardwareVisibilityTiles [[buffer(15)]],
        const device uchar *commitMask [[buffer(16)]],
        const device uint *opaquePrefixEnds [[buffer(17)]],
        const device uchar *reverseOpaqueBins [[buffer(18)]],
        texture2d<uint, access::read> visibility [[texture(0)]],
        uint2 pixel [[thread_position_in_grid]])
{
    if (pixel.x >= constants.width || pixel.y >= constants.height)
        return;
    const uint offset = pixel.y * constants.width + pixel.x;
    const uint tile = (pixel.y / 16) * constants.tilesX + pixel.x / 16;
    uint color = constants.useInitialColor != 0 ? colors[offset] : 0xff000000u;
    uint secondary = constants.useInitialSecondary != 0
            ? secondaryColors[offset] : 0u;
    float depth = depthClearMask[offset] != 0 ? 0.0f
            : (constants.useInitialDepth != 0 ? depths[offset] : 0.0f);
    // Opaque PVR rendering has a separate ISP visibility phase: expensive TSP
    // texturing and fog are evaluated only for the final visible parameter.
    // Resolve that winner with the same fixed edge/depth arithmetic first, then
    // let the main loop shade it once. Punch-through and translucent parameters
    // retain their ordered paths because acceptance/blending depends on shading.
    uint opaqueWinnerTriangle = 0xffffffffu;
    float opaqueFinalDepth = depth;
    if (constants.deferOpaqueShading != 0 && opaqueMask[offset] != 0) {
        const bool hardwareTile = constants.useHardwareVisibility != 0
                && hardwareVisibilityTiles[tile] != 0;
        if (hardwareTile) {
            const uint candidate = visibility.read(pixel).x;
            if (candidate != 0) {
                const uint candidateIndex = candidate - 1;
                const device Triangle& candidateTriangle = triangles[candidateIndex];
                const int sx = int(pixel.x) * 16 + candidateTriangle.fpuSampleOffset;
                const int sy = int(pixel.y) * 16 + candidateTriangle.fpuSampleOffset;
                const long area = edge(candidateTriangle.x[0], candidateTriangle.y[0],
                        candidateTriangle.x[1], candidateTriangle.y[1],
                        candidateTriangle.x[2], candidateTriangle.y[2]);
                const float s0 = float(edge(candidateTriangle.x[1], candidateTriangle.y[1],
                        candidateTriangle.x[2], candidateTriangle.y[2], sx, sy)) / float(area);
                const float s1 = float(edge(candidateTriangle.x[2], candidateTriangle.y[2],
                        candidateTriangle.x[0], candidateTriangle.y[0], sx, sy)) / float(area);
                const float s2 = float(edge(candidateTriangle.x[0], candidateTriangle.y[0],
                        candidateTriangle.x[1], candidateTriangle.y[1], sx, sy)) / float(area);
                const float candidateDepth = candidateTriangle.z[0] * s0
                        + candidateTriangle.z[1] * s1 + candidateTriangle.z[2] * s2;
                if ((candidateTriangle.state & 7) == 7 || candidateDepth >= depth) {
                    opaqueWinnerTriangle = candidateIndex;
                    opaqueFinalDepth = candidateDepth;
                }
            }
        }
        const bool reverseOpaque = reverseOpaqueBins[tile] != 0;
        const uint opaqueEnd = reverseOpaque ? opaquePrefixEnds[tile] : tileOffsets[tile + 1];
        if (!hardwareTile)
        for (uint position = tileOffsets[tile]; position < opaqueEnd; ++position) {
            const uint i = reverseOpaque ? opaqueEnd - 1 - (position - tileOffsets[tile]) : position;
            const uint triangleIndex = tileReferences[i];
            const device Triangle& base = triangles[triangleIndex];
            if (((base.state >> 28) & 3) != 0)
                continue;
            if ((base.effects & 512u) != 0
                    && (constants.useModifierMask == 0 || modifierMask[offset] == 0))
                continue;
            const bool selectArea1 = constants.useModifierMask != 0
                    && modifierMask[offset] != 0 && base.area1Index != 0xffffffffu;
            const device Triangle& t = triangles[selectArea1
                    ? base.area1Index : triangleIndex];
            if (int(pixel.x) < t.minX || int(pixel.x) > t.maxX
                    || int(pixel.y) < t.minY || int(pixel.y) > t.maxY)
                continue;
            if ((t.effects & 256u) != 0 && int(pixel.x) >= t.excludeX0
                    && int(pixel.x) < t.excludeX1 && int(pixel.y) >= t.excludeY0
                    && int(pixel.y) < t.excludeY1)
                continue;
            if (reverseOpaque) {
                // Covered barycentric weights are nonnegative and sum to one.
                // Inflate the vertex maximum for float edge conversion,
                // division, products and sums; do not reject near-depth ties.
                const float maximum = max(t.z[0], max(t.z[1], t.z[2]));
                const float upper = maximum + max(maximum, 1e-30f) * (32.0f * 0x1p-23f);
                if (upper < opaqueFinalDepth)
                    continue;
            }
            const int sx = int(pixel.x) * 16 + t.fpuSampleOffset;
            const int sy = int(pixel.y) * 16 + t.fpuSampleOffset;
            const long w0 = edge(t.x[1], t.y[1], t.x[2], t.y[2], sx, sy);
            const long w1 = edge(t.x[2], t.y[2], t.x[0], t.y[0], sx, sy);
            const long w2 = edge(t.x[0], t.y[0], t.x[1], t.y[1], sx, sy);
            if (w0 < 0 || (w0 == 0 && !topLeft(t.x[1], t.y[1], t.x[2], t.y[2]))
                    || w1 < 0 || (w1 == 0 && !topLeft(t.x[2], t.y[2], t.x[0], t.y[0]))
                    || w2 < 0 || (w2 == 0 && !topLeft(t.x[0], t.y[0], t.x[1], t.y[1])))
                continue;
            const long area = edge(t.x[0], t.y[0], t.x[1], t.y[1],
                    t.x[2], t.y[2]);
            const float s0 = float(w0) / float(area);
            const float s1 = float(w1) / float(area);
            const float s2 = float(w2) / float(area);
            const float depthZ = t.z[0] * s0 + t.z[1] * s1 + t.z[2] * s2;
            if (reverseOpaque && opaqueWinnerTriangle != 0xffffffffu
                    ? !(depthZ > opaqueFinalDepth)
                    : !depthTest(t.state, depthZ, opaqueFinalDepth))
                continue;
            opaqueWinnerTriangle = triangleIndex;
            if ((t.state & (1u << 3)) != 0)
                opaqueFinalDepth = depthZ;
        }
    }
    uint sortedColors[MaxSortedFragments];
    float sortedDepths[MaxSortedFragments];
    uint sortedIndices[MaxSortedFragments];
    uint sortedCount = 0;
    for (uint i = tileOffsets[tile]; i < tileOffsets[tile + 1]; ++i) {
        uint triangleIndex = tileReferences[i];
        if (constants.deferOpaqueShading != 0 && i == tileOffsets[tile]
                && opaquePrefixEnds[tile] > i) {
            // Visibility already selected the only opaque primitive to shade.
            // CPU proof guarantees all opaque references form this prefix.
            i = opaquePrefixEnds[tile] - 1;
            if (opaqueWinnerTriangle == 0xffffffffu)
                continue;
            triangleIndex = opaqueWinnerTriangle;
        }
        const device Triangle& base = triangles[triangleIndex];
        if ((base.effects & 512u) != 0
                && (constants.useModifierMask == 0 || modifierMask[offset] == 0))
            continue;
        const bool selectArea1 = constants.useModifierMask != 0
                && modifierMask[offset] != 0 && base.area1Index != 0xffffffffu;
        const uint selectedIndex = selectArea1 ? base.area1Index : triangleIndex;
        const device Triangle& t = triangles[selectedIndex];
        const uint listType = (base.state >> 28) & 3;
        if ((listType == 0 && opaqueMask[offset] == 0)
                || (listType == 1 && punchThroughMask[offset] == 0)
                || (listType == 2 && translucentMask[offset] == 0))
            continue;
        const bool deferredOpaque = constants.deferOpaqueShading != 0
                && listType == 0;
        if (deferredOpaque && triangleIndex != opaqueWinnerTriangle)
            continue;
        if (int(pixel.x) < t.minX || int(pixel.x) > t.maxX
                || int(pixel.y) < t.minY || int(pixel.y) > t.maxY)
            continue;
        if ((t.effects & 256u) != 0 && int(pixel.x) >= t.excludeX0
                && int(pixel.x) < t.excludeX1 && int(pixel.y) >= t.excludeY0
                && int(pixel.y) < t.excludeY1)
            continue;
        const int sx = int(pixel.x) * 16 + t.fpuSampleOffset;
        const int sy = int(pixel.y) * 16 + t.fpuSampleOffset;
        const long w0 = edge(t.x[1], t.y[1], t.x[2], t.y[2], sx, sy);
        const long w1 = edge(t.x[2], t.y[2], t.x[0], t.y[0], sx, sy);
        const long w2 = edge(t.x[0], t.y[0], t.x[1], t.y[1], sx, sy);
        if (w0 < 0 || (w0 == 0 && !topLeft(t.x[1], t.y[1], t.x[2], t.y[2]))
                || w1 < 0 || (w1 == 0 && !topLeft(t.x[2], t.y[2], t.x[0], t.y[0]))
                || w2 < 0 || (w2 == 0 && !topLeft(t.x[0], t.y[0], t.x[1], t.y[1])))
            continue;
        const long area = edge(t.x[0], t.y[0], t.x[1], t.y[1], t.x[2], t.y[2]);
        const float s0 = float(w0) / float(area);
        const float s1 = float(w1) / float(area);
        const float s2 = float(w2) / float(area);
        const float depthZ = t.z[0] * s0 + t.z[1] * s1 + t.z[2] * s2;
        if (!deferredOpaque && !depthTest(t.state, depthZ, depth))
            continue;
        if (SimpleTranslucent && listType == 2) {
            const int shadeX = int(pixel.x) * 16 + t.tspSampleOffset;
            const int shadeY = int(pixel.y) * 16 + t.tspSampleOffset;
            const float shadeS0 = float(edge(t.x[1], t.y[1], t.x[2], t.y[2],
                    shadeX, shadeY)) / float(area);
            const float shadeS1 = float(edge(t.x[2], t.y[2], t.x[0], t.y[0],
                    shadeX, shadeY)) / float(area);
            const float shadeS2 = float(edge(t.x[0], t.y[0], t.x[1], t.y[1],
                    shadeX, shadeY)) / float(area);
            const float shadeZ = t.z[0] * shadeS0 + t.z[1] * shadeS1
                    + t.z[2] * shadeS2;
            const float inverseZ = shadeZ == 0.0f ? 0.0f : 1.0f / shadeZ;
            float4 interpolated;
            for (uint component = 0; component < 4; ++component) {
                const uint shift = component * 8;
                interpolated[component] =
                        (float(channel(t.color[0], shift)) * t.z[0] * shadeS0
                        + float(channel(t.color[1], shift)) * t.z[1] * shadeS1
                        + float(channel(t.color[2], shift)) * t.z[2] * shadeS2)
                        * inverseZ;
            }
            uint source = packColor(interpolated);
            const float u = (t.u[0] * t.z[0] * shadeS0
                    + t.u[1] * t.z[1] * shadeS1
                    + t.u[2] * t.z[2] * shadeS2) * inverseZ;
            const float v = (t.v[0] * t.z[0] * shadeS0
                    + t.v[1] * t.z[1] * shadeS1
                    + t.v[2] * t.z[2] * shadeS2) * inverseZ;
            const float pvrD = t.mipLevelCount > 1
                    ? max(0.0f, shadeZ == 0.0f ? 0.0f
                            : t.mipNumerator / (shadeZ * shadeZ)) : 1.0f;
            uint texel = sampleTexture(texture, t, float2(u, v), pvrD);
            if ((t.state & (1u << 12)) != 0)
                texel |= 0xff000000u;
            source = applyTexture(source, texel, 3);
            const float fog = fogCoefficient(shadeZ, constants.fogDensity,
                    fogTable);
            float4 fogged;
            for (uint component = 0; component < 3; ++component) {
                const uint shift = component * 8;
                const float value = float(channel(source, shift));
                fogged[component] = value
                        + (float(channel(constants.fogColor, shift)) - value)
                                * fog;
            }
            fogged.w = float(channel(source, 24));
            source = packColor(fogged);
            color = sourceAlphaBlend(source, color);
            continue;
        }
        const int shadeX = int(pixel.x) * 16 + t.tspSampleOffset;
        const int shadeY = int(pixel.y) * 16 + t.tspSampleOffset;
        const float shadeS0 = float(edge(t.x[1], t.y[1], t.x[2], t.y[2],
                shadeX, shadeY)) / float(area);
        const float shadeS1 = float(edge(t.x[2], t.y[2], t.x[0], t.y[0],
                shadeX, shadeY)) / float(area);
        const float shadeS2 = float(edge(t.x[0], t.y[0], t.x[1], t.y[1],
                shadeX, shadeY)) / float(area);
        const float shadeZ = t.z[0] * shadeS0 + t.z[1] * shadeS1 + t.z[2] * shadeS2;
        const float inverseZ = shadeZ == 0.0f ? 0.0f : 1.0f / shadeZ;
        float4 interpolated;
        for (uint component = 0; component < 4; ++component) {
            const uint shift = component * 8;
            interpolated[component] =
                    (float(channel(t.color[0], shift)) * t.z[0] * shadeS0
                    + float(channel(t.color[1], shift)) * t.z[1] * shadeS1
                    + float(channel(t.color[2], shift)) * t.z[2] * shadeS2) * inverseZ;
        }
        uint source = packColor(interpolated);
        float4 offsetInterpolated;
        for (uint component = 0; component < 4; ++component) {
            const uint shift = component * 8;
            offsetInterpolated[component] =
                    (float(channel(t.offsetColor[0], shift)) * t.z[0] * shadeS0
                    + float(channel(t.offsetColor[1], shift)) * t.z[1] * shadeS1
                    + float(channel(t.offsetColor[2], shift)) * t.z[2] * shadeS2) * inverseZ;
        }
        uint offsetColor = packColor(offsetInterpolated);
        if ((t.state & (1u << 11)) == 0)
            source |= 0xff000000u;
        const bool intensityShadow = (t.state & (1u << 26)) != 0
                && constants.useModifierMask != 0 && modifierMask[offset] != 0;
        if (intensityShadow) {
            uint scaled = source & 0xff000000u;
            for (uint component = 0; component < 3; ++component) {
                const uint shift = component * 8;
                const uint value = min(255u,
                        (channel(source, shift) * t.shadeScale + 128) / 256);
                scaled |= value << shift;
            }
            source = scaled;
        }
        if ((t.effects & 128u) != 0) {
            const float fog = fogCoefficient(shadeZ, constants.fogDensity, fogTable);
            source = packColor(float4(float(channel(constants.fogColor, 0)),
                    float(channel(constants.fogColor, 8)),
                    float(channel(constants.fogColor, 16)), fog * 255.0f));
        }
        float pvrD = 1.0f;
        if ((t.state & (1u << 4)) != 0) {
            const float u = (t.u[0] * t.z[0] * shadeS0 + t.u[1] * t.z[1] * shadeS1
                    + t.u[2] * t.z[2] * shadeS2) * inverseZ;
            const float v = (t.v[0] * t.z[0] * shadeS0 + t.v[1] * t.z[1] * shadeS1
                    + t.v[2] * t.z[2] * shadeS2) * inverseZ;
            pvrD = t.mipLevelCount > 1
                    ? max(0.0f, shadeZ == 0.0f ? 0.0f
                            : t.mipNumerator / (shadeZ * shadeZ)) : 1.0f;
            uint texel = sampleTexture(texture, t, float2(u, v), pvrD);
            if ((t.state & (1u << 30)) != 0) {
                const float2 uv10 = projectedUV(t, shadeX + t.superSampleOffsetX, shadeY);
                const float2 uv01 = projectedUV(t, shadeX, shadeY + t.superSampleOffsetY);
                const float2 uv11 = projectedUV(t, shadeX + t.superSampleOffsetX,
                        shadeY + t.superSampleOffsetY);
                const uint samples[3] = {
                    sampleTexture(texture, t, uv10, pvrD),
                    sampleTexture(texture, t, uv01, pvrD),
                    sampleTexture(texture, t, uv11, pvrD),
                };
                uint averaged = 0;
                for (uint component = 0; component < 4; ++component) {
                    const uint shift = component * 8;
                    const uint value = (channel(texel, shift)
                            + channel(samples[0], shift) + channel(samples[1], shift)
                            + channel(samples[2], shift) + 2) / 4;
                    averaged |= value << shift;
                }
                texel = averaged;
            }
            if ((t.effects & 2u) != 0) {
                constexpr float pi = 3.14159265358979323846f;
                auto angleFraction = [](float parameter) {
                    return parameter >= 255.0f ? 1.0f : parameter / 256.0f;
                };
                const float sParameter = (float(channel(texel, 24)) * 240.0f
                        + float(channel(texel, 0)) * 15.0f) / 255.0f;
                const float rParameter = (float(channel(texel, 8)) * 240.0f
                        + float(channel(texel, 16)) * 15.0f) / 255.0f;
                const float s = pi * 0.5f * angleFraction(sParameter);
                const float r = 2.0f * pi * angleFraction(rParameter);
                const float q = 2.0f * pi * angleFraction(float(channel(offsetColor, 16)));
                const float alpha = clamp(float(channel(offsetColor, 24)) / 255.0f
                        + float(channel(offsetColor, 0)) / 255.0f * sin(s)
                        + float(channel(offsetColor, 8)) / 255.0f * cos(s) * cos(r - q),
                        0.0f, 1.0f) * 255.0f;
                texel = packColor(float4(255.0f, 255.0f, 255.0f, alpha));
            } else if ((t.state & (1u << 12)) != 0)
                texel |= 0xff000000u;
            source = applyTexture(source, texel, (t.state >> 14) & 3);
            if ((t.effects & 1u) != 0 && (t.effects & 2u) == 0) {
                uint added = source & 0xff000000u;
                for (uint component = 0; component < 3; ++component) {
                    const uint shift = component * 8;
                    uint offsetValue = channel(offsetColor, shift);
                    if (intensityShadow)
                        offsetValue = min(255u,
                                (offsetValue * t.shadeScale + 128) / 256);
                    added |= min(255u, channel(source, shift) + offsetValue) << shift;
                }
                source = added;
            }
        }
        if ((t.effects & 32u) != 0) {
            uint clamped = 0;
            for (uint component = 0; component < 4; ++component) {
                const uint shift = component * 8;
                const uint value = clamp(channel(source, shift),
                        channel(t.clampMin, shift), channel(t.clampMax, shift));
                clamped |= value << shift;
            }
            source = clamped;
        }
        if ((t.state & (1u << 23)) != 0) {
            const float fog = fogCoefficient(shadeZ, constants.fogDensity, fogTable);
            float4 fogged;
            for (uint component = 0; component < 3; ++component) {
                const uint shift = component * 8;
                const float value = float(channel(source, shift));
                fogged[component] = value
                        + (float(channel(constants.fogColor, shift)) - value) * fog;
            }
            fogged.w = float(channel(source, 24));
            source = packColor(fogged);
        } else if ((t.effects & 4u) != 0) {
            // The PVR consumes the interpolated offset alpha directly for
            // vertex fog. Quantizing it through the packed offset color first
            // changes the fog result by one or more color levels when other
            // pixel operations are combined with vertex fog.
            const float fog = offsetInterpolated.w / 255.0f;
            float4 fogged;
            for (uint component = 0; component < 3; ++component) {
                const uint shift = component * 8;
                const float value = float(channel(source, shift));
                fogged[component] = value
                        + (float(channel(constants.fogVertexColor, shift)) - value) * fog;
            }
            fogged.w = float(channel(source, 24));
            source = packColor(fogged);
        }
        if ((t.state & (1u << 24)) != 0) {
            const float fraction = pvrD - floor(pvrD);
            const float weight = (t.state & (1u << 25)) != 0
                    ? fraction : 1.0f - fraction;
            float4 weighted;
            for (uint component = 0; component < 4; ++component)
                weighted[component] = float(channel(source, component * 8)) * weight;
            source = packColor(weighted);
        }
        if ((t.state & (1u << 13)) != 0 && channel(source, 24) < t.alphaReference)
            continue;
        if ((t.state & (1u << 13)) != 0)
            source |= 0xff000000u;
        if ((t.effects & 64u) != 0) {
            constexpr uchar ditherTable[16] = {
                5, 13, 7, 15, 9, 1, 11, 3, 6, 14, 4, 12, 10, 2, 8, 0
            };
            const uint dither = ditherTable[(pixel.y & 3) * 4 + (pixel.x & 3)];
            uint dithered = 0;
            for (uint component = 0; component < 4; ++component) {
                const uint shift = component * 8;
                const uint divisor = max(1u, channel(t.ditherDivisors, shift));
                const uint addition = component == 3 ? 1u / divisor : dither / divisor;
                dithered |= min(255u, channel(source, shift) + addition) << shift;
            }
            source = dithered;
        }
        if ((t.state & (1u << 22)) != 0 && !OrderedAutosort) {
            if (sortedCount == MaxSortedFragments) {
                atomic_store_explicit(overflow, 1u, memory_order_relaxed);
                continue;
            }
            uint position = sortedCount++;
            while (position != 0 && (depthZ < sortedDepths[position - 1]
                    || (depthZ == sortedDepths[position - 1]
                            && selectedIndex < sortedIndices[position - 1]))) {
                sortedColors[position] = sortedColors[position - 1];
                sortedDepths[position] = sortedDepths[position - 1];
                sortedIndices[position] = sortedIndices[position - 1];
                --position;
            }
            sortedColors[position] = source;
            sortedDepths[position] = depthZ;
            sortedIndices[position] = selectedIndex;
            continue;
        }
        if (deferredOpaque)
            depth = opaqueFinalDepth;
        else if ((t.state & (1u << 3)) != 0)
            depth = depthZ;
        const uint sourceFactor = (t.state >> 16) & 7;
        const uint destinationFactor = (t.state >> 19) & 7;
        const uint blendSource = (t.effects & 8u) != 0 ? secondary : source;
        const uint blendDestination = (t.effects & 16u) != 0 ? secondary : color;
        uint result;
        if (sourceFactor == 1 && destinationFactor == 0)
            result = blendSource;
        else if (sourceFactor == 4 && destinationFactor == 5)
            result = sourceAlphaBlend(blendSource, blendDestination);
        else
            result = blend(blendSource, blendDestination, sourceFactor, destinationFactor);
        if ((t.effects & 16u) != 0)
            secondary = result;
        else
            color = result;
    }
    for (uint i = 0; i < sortedCount; ++i) {
        const device Triangle& sortedTriangle = triangles[sortedIndices[i]];
        const uint sourceFactor = (sortedTriangle.state >> 16) & 7;
        const uint destinationFactor = (sortedTriangle.state >> 19) & 7;
        const uint blendSource = (sortedTriangle.effects & 8u) != 0
                ? secondary : sortedColors[i];
        const uint blendDestination = (sortedTriangle.effects & 16u) != 0
                ? secondary : color;
        uint result;
        if (sourceFactor == 1 && destinationFactor == 0)
            result = blendSource;
        else if (sourceFactor == 4 && destinationFactor == 5)
            result = sourceAlphaBlend(blendSource, blendDestination);
        else
            result = blend(blendSource, blendDestination, sourceFactor, destinationFactor);
        if ((sortedTriangle.effects & 16u) != 0)
            secondary = result;
        else
            color = result;
    }
    if (constants.useCommitMask == 0 || commitMask[offset] != 0)
        colors[offset] = color;
    secondaryColors[offset] = secondary;
    depths[offset] = depth;
}

template [[host_name("rasterTiles")]] kernel
    decltype(rasterTiles<64, false>) rasterTiles<64, false>;
template [[host_name("rasterTilesOrderedAutosort")]] kernel
    decltype(rasterTiles<1, true>) rasterTiles<1, true>;
template [[host_name("rasterTilesSimpleTranslucent")]] kernel
    decltype(rasterTiles<1, true, true>) rasterTiles<1, true, true>;
)METAL";

class MetalState {
public:
  enum BufferSlot : u32 {
    Triangles,
    TileOffsets,
    TileReferences,
    Texture,
    Fog,
    Modifier,
    DepthClear,
    OpaqueMask,
    PunchMask,
    TranslucentMask,
    HardwareVisibilityTiles,
    Color,
    Depth,
    Overflow,
    Secondary,
    VisibilityVertices,
    VisibilityPrimitives,
    CommitMask,
    OpaquePrefixEnds,
    ReverseOpaqueBins,
    BufferSlotCount,
  };

private:
  struct ReusableBuffer {
    id<MTLBuffer> value = nil;
    size_t capacity = 0;
  };
  struct FrameResources {
    std::array<ReusableBuffer, BufferSlotCount> buffers;
    id<MTLTexture> visibility = nil;
    u32 visibilityWidth = 0;
    u32 visibilityHeight = 0;
    std::atomic<bool> available { true };
    std::atomic<bool> successful { false };
    u64 serial = 0;
    PresentationSurface presentation;
    id<MTLCommandBuffer> command = nil;
  };
  struct PresentationAllocation {
    IOSurfaceRef surface = nullptr;
    id<MTLBuffer> buffer = nil;
    u32 width = 0;
    u32 height = 0;
  };

public:

  MetalState() {
    device = MTLCreateSystemDefaultDevice();
    if (device == nil)
      return;
    NSError *error = nil;
    MTLCompileOptions *options = [[MTLCompileOptions alloc] init];
    if (@available(macOS 15.0, *))
      options.mathMode = MTLMathModeSafe;
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    NSString *source = [NSString stringWithUTF8String:KernelSource];
    id<MTLLibrary> library = [device newLibraryWithSource:source
                                                  options:options
                                                    error:&error];
    if (library == nil) {
      std::fprintf(stderr, "Hybrid Metal library: %s\n",
                   error.localizedDescription.UTF8String);
      return;
    }
    id<MTLFunction> function = [library newFunctionWithName:@"rasterTiles"];
    pipeline = [device newComputePipelineStateWithFunction:function
                                                     error:&error];
    id<MTLFunction> orderedAutosortFunction =
        [library newFunctionWithName:@"rasterTilesOrderedAutosort"];
    orderedAutosortPipeline = [device
        newComputePipelineStateWithFunction:orderedAutosortFunction error:&error];
    id<MTLFunction> simpleTranslucentFunction =
        [library newFunctionWithName:@"rasterTilesSimpleTranslucent"];
    simpleTranslucentPipeline = [device
        newComputePipelineStateWithFunction:simpleTranslucentFunction
                                      error:&error];
    if (pipeline == nil || orderedAutosortPipeline == nil ||
        simpleTranslucentPipeline == nil)
      std::fprintf(stderr, "Hybrid Metal compute pipeline: %s\n",
                   error.localizedDescription.UTF8String);
    MTLRenderPipelineDescriptor *visibilityDescriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    visibilityDescriptor.vertexFunction =
        [library newFunctionWithName:@"visibilityVertex"];
    visibilityDescriptor.fragmentFunction =
        [library newFunctionWithName:@"visibilityFragment"];
    visibilityDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
    visibilityPipeline = [device
        newRenderPipelineStateWithDescriptor:visibilityDescriptor error:&error];
    if (pipeline != nil && orderedAutosortPipeline != nil &&
        simpleTranslucentPipeline != nil &&
        visibilityPipeline != nil)
      queue = [device newCommandQueue];
  }

  bool valid() const { return queue != nil && pipeline != nil; }

  bool beginFrame() {
    for (size_t attempt = 0; attempt < frameResources.size(); ++attempt) {
      const size_t candidate = (activeFrame + 1 + attempt) %
                               frameResources.size();
      if (frameResources[candidate].available.load(std::memory_order_acquire)) {
        activeFrame = candidate;
        return true;
      }
    }
    return false;
  }

  void markSubmitted(id<MTLCommandBuffer> command, id<MTLBuffer> overflow,
                     const PresentationSurface &presentation) {
    FrameResources *resources = &frameResources[activeFrame];
    resources->available.store(false, std::memory_order_release);
    resources->successful.store(false, std::memory_order_relaxed);
    resources->serial = ++submissionSerial;
    resources->presentation = presentation;
    resources->command = command;
    [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
      const bool success = completed.status == MTLCommandBufferStatusCompleted &&
          *static_cast<const u32 *>(overflow.contents) == 0;
      resources->successful.store(success, std::memory_order_relaxed);
      resources->available.store(true, std::memory_order_release);
    }];
  }

  bool latestPresentation(PresentationSurface &output) const {
    u64 latestSerial = 0;
    const PresentationSurface *latest = nullptr;
    for (const FrameResources &resources : frameResources)
      if (resources.available.load(std::memory_order_acquire) &&
          resources.successful.load(std::memory_order_relaxed) &&
          resources.serial > latestSerial) {
        latestSerial = resources.serial;
        latest = &resources.presentation;
      }
    if (latest == nullptr || latestSerial <= consumedSerial)
      return false;
    output = *latest;
    consumedSerial = latestSerial;
    return output.handle != nullptr;
  }

  bool copyLatestPresentation(std::vector<u32> &colors,
                              PresentationSurface *output) {
    FrameResources *latest = nullptr;
    for (FrameResources &resources : frameResources)
      if (resources.serial != 0 &&
          (latest == nullptr || resources.serial > latest->serial))
        latest = &resources;
    if (latest == nullptr || latest->command == nil)
      return false;
    [latest->command waitUntilCompleted];
    if (!latest->available.load(std::memory_order_acquire) ||
        !latest->successful.load(std::memory_order_relaxed) ||
        latest->presentation.handle == nullptr)
      return false;
    const size_t pixelCount = static_cast<size_t>(latest->presentation.width) *
                              latest->presentation.height;
    colors.resize(pixelCount);
    std::memcpy(colors.data(),
                IOSurfaceGetBaseAddress(
                    static_cast<IOSurfaceRef>(latest->presentation.handle)),
                pixelCount * sizeof(u32));
    if (output != nullptr)
      *output = latest->presentation;
    consumedSerial = std::max(consumedSerial, latest->serial);
    return true;
  }

  ~MetalState() {
    for (auto &allocation : presentationAllocations)
      if (allocation.surface != nullptr)
        CFRelease(allocation.surface);
  }

  id<MTLBuffer> buffer(BufferSlot slot, size_t length,
                       const void *contents = nullptr) {
    if (length == 0)
      return nil;
    ReusableBuffer &allocation = frameResources[activeFrame].buffers[slot];
    if (allocation.capacity < length) {
      size_t capacity = std::max<size_t>(4096, allocation.capacity);
      while (capacity < length)
        capacity *= 2;
      allocation.value =
          [device newBufferWithLength:capacity
                              options:MTLResourceStorageModeShared];
      allocation.capacity = allocation.value != nil ? capacity : 0;
    }
    if (allocation.value != nil && contents != nullptr)
      std::memcpy(allocation.value.contents, contents, length);
    return allocation.value;
  }

  template <typename T>
  id<MTLBuffer> upload(BufferSlot slot, const std::vector<T> &data) {
    return data.empty()
        ? nil
        : buffer(slot, data.size() * sizeof(T), data.data());
  }

  id<MTLTexture> visibilityTexture(u32 width, u32 height) {
    width = std::max(1u, width);
    height = std::max(1u, height);
    FrameResources &resources = frameResources[activeFrame];
    if (resources.visibility == nil || resources.visibilityWidth != width ||
        resources.visibilityHeight != height) {
      MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint
                                        width:width
                                       height:height
                                    mipmapped:NO];
      descriptor.usage =
          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      descriptor.storageMode = MTLStorageModePrivate;
      resources.visibility = [device newTextureWithDescriptor:descriptor];
      resources.visibilityWidth = resources.visibility != nil ? width : 0;
      resources.visibilityHeight = resources.visibility != nil ? height : 0;
    }
    return resources.visibility;
  }

  id<MTLBuffer> presentationBuffer(u32 width, u32 height,
                                   PresentationSurface *output) {
    if (output == nullptr)
      return buffer(Color, static_cast<size_t>(width) * height * sizeof(u32));
    PresentationAllocation *allocation = nullptr;
    if (output->handle != nullptr)
      for (PresentationAllocation &candidate : presentationAllocations)
        if (candidate.surface == output->handle) {
          allocation = &candidate;
          break;
        }
    if (allocation == nullptr) {
      allocation = &presentationAllocations[activeFrame];
    }
    if (allocation->surface == nullptr || allocation->width != width ||
        allocation->height != height) {
      if (allocation->surface != nullptr)
        CFRelease(allocation->surface);
      *allocation = {};
      const size_t bytesPerRow = static_cast<size_t>(width) * sizeof(u32);
      const size_t byteCount = bytesPerRow * height;
      NSDictionary *properties = @{
        (id)kIOSurfaceWidth : @(width),
        (id)kIOSurfaceHeight : @(height),
        (id)kIOSurfaceBytesPerElement : @4,
        (id)kIOSurfaceBytesPerRow : @(bytesPerRow),
        (id)kIOSurfaceAllocSize : @(byteCount),
        (id)kIOSurfacePixelFormat : @(0x42475241u), // 'BGRA'
      };
      IOSurfaceRef surface =
          IOSurfaceCreate((__bridge CFDictionaryRef)properties);
      if (surface != nullptr && IOSurfaceGetBytesPerRow(surface) == bytesPerRow &&
          IOSurfaceGetAllocSize(surface) >= byteCount) {
        id<MTLBuffer> value = [device
            newBufferWithBytesNoCopy:IOSurfaceGetBaseAddress(surface)
                               length:byteCount
                              options:MTLResourceStorageModeShared
                          deallocator:nil];
        if (value != nil) {
          allocation->surface = surface;
          allocation->buffer = value;
          allocation->width = width;
          allocation->height = height;
        } else {
          CFRelease(surface);
        }
      } else if (surface != nullptr) {
        CFRelease(surface);
      }
    }
    output->handle = allocation->surface;
    output->width = allocation->width;
    output->height = allocation->height;
    return allocation->buffer;
  }

  id<MTLBuffer> presentationBufferFor(const PresentationSurface *surface,
                                      u32 width, u32 height) const {
    if (surface == nullptr || surface->handle == nullptr ||
        surface->width != width || surface->height != height)
      return nil;
    for (const PresentationAllocation &allocation : presentationAllocations)
      if (allocation.surface == surface->handle && allocation.width == width &&
          allocation.height == height)
        return allocation.buffer;
    return nil;
  }

  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLComputePipelineState> pipeline = nil;
  id<MTLComputePipelineState> orderedAutosortPipeline = nil;
  id<MTLComputePipelineState> simpleTranslucentPipeline = nil;
  id<MTLRenderPipelineState> visibilityPipeline = nil;

private:
  std::array<FrameResources, 3> frameResources;
  size_t activeFrame = frameResources.size() - 1;
  std::array<PresentationAllocation, 3> presentationAllocations;
  u64 submissionSerial = 0;
  mutable u64 consumedSerial = 0;
};

MetalState &sharedMetalState() {
  static MetalState metal;
  return metal;
}

} // namespace

static bool renderInternal(const Frame &frame, std::vector<u32> &colors,
                           std::vector<float> &depths, Timing *timing,
                           std::vector<u32> *secondaryOutput,
                           PresentationSurface *presentation,
                           bool asynchronous) {
  MetalState &metal = sharedMetalState();
  if (!metal.valid() || frame.width == 0 || frame.height == 0 ||
      frame.triangles == nullptr || frame.triangles->empty() ||
      frame.texture == nullptr || frame.texture->empty() ||
      frame.texture->size() > std::numeric_limits<u32>::max())
    return false;

  if (!metal.beginFrame())
    return false;

  const auto encodeStart = std::chrono::steady_clock::now();
  const u32 tilesX = (frame.width + TileSize - 1) / TileSize;
  const u32 tilesY = (frame.height + TileSize - 1) / TileSize;
  bool hasOpaque = false;
  bool deferOpaqueShading = true;
  bool useHardwareVisibility = true;
  s32 visibilitySampleOffset = -1;
  std::vector<std::vector<u32>> bins(static_cast<size_t>(tilesX) * tilesY);
  for (u32 index = 0; index < frame.triangles->size(); ++index) {
    const Triangle &triangle = (*frame.triangles)[index];
    if (triangle.minX < 0 || triangle.minY < 0 ||
        triangle.maxX < triangle.minX || triangle.maxY < triangle.minY ||
        triangle.maxX >= static_cast<s32>(frame.width) ||
        triangle.maxY >= static_cast<s32>(frame.height) ||
        triangle.alphaReference > 255 || (triangle.state >> 31) != 0 ||
        triangle.shadeScale > 256)
      return false;
    if (triangle.area1Index != std::numeric_limits<u32>::max() &&
        (triangle.area1Index >= frame.triangles->size() ||
         ((*frame.triangles)[triangle.area1Index].state & Area1Only) == 0))
      return false;
    const s64 area = (static_cast<s64>(triangle.x[1]) - triangle.x[0]) *
                         (static_cast<s64>(triangle.y[2]) - triangle.y[0]) -
                     (static_cast<s64>(triangle.y[1]) - triangle.y[0]) *
                         (static_cast<s64>(triangle.x[2]) - triangle.x[0]);
    if (area <= 0)
      return false;
    for (u32 vertex = 0; vertex < 3; ++vertex)
      if (!std::isfinite(triangle.z[vertex]) ||
          !std::isfinite(triangle.u[vertex]) ||
          !std::isfinite(triangle.v[vertex]))
        return false;
    if ((triangle.state & Textured) != 0) {
      if (triangle.textureWidth == 0 || triangle.textureHeight == 0 ||
          (triangle.textureWidth & (triangle.textureWidth - 1)) != 0 ||
          (triangle.textureHeight & (triangle.textureHeight - 1)) != 0 ||
          triangle.mipLevelCount == 0 || triangle.mipLevelCount > 11 ||
          !std::isfinite(triangle.mipNumerator) || triangle.mipNumerator < 0.f)
        return false;
      for (u32 level = 0; level < triangle.mipLevelCount; ++level) {
        const size_t width = std::max<size_t>(1, triangle.textureWidth >> level);
        const size_t height = std::max<size_t>(1, triangle.textureHeight >> level);
        const size_t textureEnd =
            static_cast<size_t>(triangle.mipOffsets[level]) + width * height;
        if (textureEnd > frame.texture->size())
          return false;
      }
    }
    if ((triangle.state & Area1Only) != 0 &&
        (triangle.effects & ModifierArea1Only) == 0)
      continue;
    if (((triangle.state >> ListTypeShift) & 3) == 0) {
      hasOpaque = true;
      const u32 sourceBlend = (triangle.state >> SourceBlendShift) & 7;
      const u32 destinationBlend =
          (triangle.state >> DestinationBlendShift) & 7;
      if (sourceBlend != 1 || destinationBlend != 0 ||
          (triangle.state & (AlphaTest | Autosort)) != 0 ||
          (triangle.effects & (SourceSelect | DestinationSelect)) != 0)
        deferOpaqueShading = false;
      const u32 depthMode = triangle.state & DepthModeMask;
      if ((depthMode != 6 && depthMode != 7) ||
          (triangle.state & DepthWrite) == 0 ||
          triangle.area1Index != std::numeric_limits<u32>::max() ||
          (triangle.state & Area1Only) != 0 ||
          (triangle.effects & ExcludeClip) != 0 ||
          (triangle.fpuSampleOffset != 0 && triangle.fpuSampleOffset != 8) ||
          (visibilitySampleOffset != -1 &&
           visibilitySampleOffset != triangle.fpuSampleOffset))
        useHardwareVisibility = false;
      visibilitySampleOffset = triangle.fpuSampleOffset;
    }
    for (s32 y = triangle.minY / static_cast<s32>(TileSize);
         y <= triangle.maxY / static_cast<s32>(TileSize); ++y)
      for (s32 x = triangle.minX / static_cast<s32>(TileSize);
           x <= triangle.maxX / static_cast<s32>(TileSize); ++x)
        bins[static_cast<size_t>(y) * tilesX + x].push_back(index);
  }
  std::vector<u32> tileOffsets(1, 0);
  std::vector<u32> tileReferences;
  // Autosort depth is affine over a covered triangle. Its extrema over a tile's
  // sample rectangle are therefore at the rectangle corners; an outward error
  // margin makes these conservative bounds for the float shader arithmetic.
  // Only triangles with positive-area overlap constrain each other's order:
  // disjoint triangles (including the shared-edge pair making up a quad) can be
  // interleaved arbitrarily. Strictly disjoint depth bounds on overlapping
  // pairs form a partial order that can be resolved once per tile on the CPU.
  bool orderedAutosort = true;
  u32 orderedAutosortTiles = 0;
  u32 orderedAutosortRejectReason = 0;
  std::vector<std::vector<u32>> orderedBins(bins.size());
  constexpr u32 SimpleTranslucentVariable = ClampU | ClampV | FlipU | FlipV |
      TexelSampleCenter | IgnoreTextureAlpha;
  constexpr u32 SimpleTranslucentFixed = 6 | Textured | Bilinear | UseAlpha |
      (3u << ShadingInstructionShift) | (4u << SourceBlendShift) |
      (5u << DestinationBlendShift) | Autosort | LookupFog |
      (2u << ListTypeShift);
  bool simpleCandidate = std::getenv("FLYCAST_HYBRID_DISABLE_SIMPLE_TRANSLUCENT") == nullptr;
  bool hasTranslucent = false;
  for (const Triangle &triangle : *frame.triangles)
    if (((triangle.state >> ListTypeShift) & 3) == 2) {
      hasTranslucent = true;
      if ((triangle.state & ~SimpleTranslucentVariable) != SimpleTranslucentFixed || triangle.effects != 0)
        simpleCandidate = false;
    }
  simpleCandidate &= hasTranslucent;
  // The global proof only gates the specialized translucent kernel. Ordinary
  // rendering still performs the unchanged tile-local ordering proof below.
  const size_t globalSize = simpleCandidate ? frame.triangles->size() : 0;
  std::vector<std::vector<u32>> globalSuccessors(globalSize);
  std::vector<u32> globalIndegrees(globalSize);
  std::vector<u8> globalActive(globalSize);
  for (size_t tile = 0; tile < bins.size(); ++tile) {
    bool tileOrdered = frame.modifierMask == nullptr;
    if (!tileOrdered)
      orderedAutosortRejectReason |= 1;
    struct DeferredRange {
      u32 index;
      double minimum;
      double maximum;
    };
    std::vector<DeferredRange> deferred;
    const u32 tileX = static_cast<u32>(tile) % tilesX;
    const u32 tileY = static_cast<u32>(tile) / tilesX;
    const u32 firstX = tileX * TileSize;
    const u32 firstY = tileY * TileSize;
    const u32 lastX = std::min(frame.width, firstX + TileSize) - 1;
    const u32 lastY = std::min(frame.height, firstY + TileSize) - 1;
    for (u32 index : bins[tile]) {
      const Triangle &triangle = (*frame.triangles)[index];
      if ((triangle.state & Autosort) != 0) {
        if (triangle.area1Index != std::numeric_limits<u32>::max()) {
          tileOrdered = false;
          orderedAutosortRejectReason |= 2;
          break;
        }
        const double area = static_cast<double>(
            (static_cast<s64>(triangle.x[1]) - triangle.x[0]) *
                (static_cast<s64>(triangle.y[2]) - triangle.y[0]) -
            (static_cast<s64>(triangle.y[1]) - triangle.y[0]) *
                (static_cast<s64>(triangle.x[2]) - triangle.x[0]));
        double minimum = std::numeric_limits<double>::infinity();
        double maximum = -std::numeric_limits<double>::infinity();
        for (u32 y : {firstY, lastY})
          for (u32 x : {firstX, lastX}) {
            const s64 sx = static_cast<s64>(x) * 16 + triangle.fpuSampleOffset;
            const s64 sy = static_cast<s64>(y) * 16 + triangle.fpuSampleOffset;
            auto edgeAt = [&](u32 a, u32 b) {
              return static_cast<double>(
                  (static_cast<s64>(triangle.x[b]) - triangle.x[a]) *
                      (sy - triangle.y[a]) -
                  (static_cast<s64>(triangle.y[b]) - triangle.y[a]) *
                      (sx - triangle.x[a]));
            };
            const double weights[3] = {
                edgeAt(1, 2) / area, edgeAt(2, 0) / area,
                edgeAt(0, 1) / area};
            const double terms[3] = {triangle.z[0] * weights[0],
                                     triangle.z[1] * weights[1],
                                     triangle.z[2] * weights[2]};
            const double value = terms[0] + terms[1] + terms[2];
            const double magnitude = std::abs(terms[0]) + std::abs(terms[1]) +
                                     std::abs(terms[2]) + std::abs(value);
            const double error = 32. * std::numeric_limits<float>::epsilon() *
                                 std::max(magnitude, 1e-30);
            minimum = std::min(minimum, value - error);
            maximum = std::max(maximum, value + error);
          }
        deferred.push_back({index, minimum, maximum});
        if (simpleCandidate)
          globalActive[index] = 1;
      } else {
        orderedBins[tile].push_back(index);
      }
    }
    if (tileOrdered) {
      auto hasPositiveAreaOverlap = [&](u32 leftIndex, u32 rightIndex) {
        const Triangle &left = (*frame.triangles)[leftIndex];
        const Triangle &right = (*frame.triangles)[rightIndex];
        if (left.maxX < right.minX || right.maxX < left.minX ||
            left.maxY < right.minY || right.maxY < left.minY)
          return false;
        auto separatedByEdge = [](const Triangle &edgeTriangle,
                                  const Triangle &other) {
          for (u32 edge = 0; edge < 3; ++edge) {
            const u32 next = (edge + 1) % 3;
            bool allOutsideOrOn = true;
            for (u32 vertex = 0; vertex < 3; ++vertex) {
              const s64 value =
                  (static_cast<s64>(edgeTriangle.x[next]) -
                   edgeTriangle.x[edge]) *
                      (static_cast<s64>(other.y[vertex]) -
                       edgeTriangle.y[edge]) -
                  (static_cast<s64>(edgeTriangle.y[next]) -
                   edgeTriangle.y[edge]) *
                      (static_cast<s64>(other.x[vertex]) -
                       edgeTriangle.x[edge]);
              if (value > 0) {
                allOutsideOrOn = false;
                break;
              }
            }
            if (allOutsideOrOn)
              return true;
          }
          return false;
        };
        return !separatedByEdge(left, right) &&
               !separatedByEdge(right, left);
      };
      std::vector<std::vector<u32>> successors(deferred.size());
      std::vector<u32> indegrees(deferred.size());
      for (u32 left = 0; left < deferred.size() && tileOrdered; ++left) {
        for (u32 right = left + 1; right < deferred.size(); ++right) {
          if (!hasPositiveAreaOverlap(deferred[left].index,
                                      deferred[right].index))
            continue;
          u32 before;
          u32 after;
          if (deferred[left].maximum < deferred[right].minimum) {
            before = left;
            after = right;
          } else if (deferred[right].maximum < deferred[left].minimum) {
            before = right;
            after = left;
          } else {
            tileOrdered = false;
            orderedAutosortRejectReason |= 4;
            break;
          }
          successors[before].push_back(after);
          ++indegrees[after];
          if (simpleCandidate) {
            const u32 globalBefore = deferred[before].index;
            const u32 globalAfter = deferred[after].index;
            globalSuccessors[globalBefore].push_back(globalAfter);
            ++globalIndegrees[globalAfter];
          }
        }
      }
      std::vector<u32> resolved;
      resolved.reserve(deferred.size());
      // Bins are populated in input-index order. The smallest ready local
      // index preserves the old lowest-original-index-first ordering.
      std::priority_queue<u32, std::vector<u32>, std::greater<u32>> ready;
      for (u32 candidate = 0; candidate < deferred.size(); ++candidate)
        if (indegrees[candidate] == 0)
          ready.push(candidate);
      while (tileOrdered && resolved.size() != deferred.size()) {
        if (ready.empty()) {
          tileOrdered = false;
          orderedAutosortRejectReason |= 4;
          break;
        }
        const u32 selected = ready.top();
        ready.pop();
        resolved.push_back(selected);
        for (u32 successor : successors[selected])
          if (--indegrees[successor] == 0)
            ready.push(successor);
      }
      if (tileOrdered)
        for (u32 resolvedIndex : resolved)
          orderedBins[tile].push_back(deferred[resolvedIndex].index);
    }
    if (!tileOrdered)
      for (const DeferredRange &range : deferred)
        orderedBins[tile].push_back(range.index);
    orderedAutosortTiles += tileOrdered;
    orderedAutosort &= tileOrdered;
  }
  bool globalAutosort = orderedAutosort && simpleCandidate;
  std::vector<u32> globalOrder;
  const size_t globalActiveCount = static_cast<size_t>(std::count(
      globalActive.begin(), globalActive.end(), u8{1}));
  globalOrder.reserve(globalActiveCount);
  std::priority_queue<u32, std::vector<u32>, std::greater<u32>> globalReady;
  for (u32 candidate = 0; candidate < globalSize; ++candidate)
    if (globalActive[candidate] != 0 && globalIndegrees[candidate] == 0)
      globalReady.push(candidate);
  while (globalAutosort && globalOrder.size() != globalActiveCount) {
    if (globalReady.empty()) {
      globalAutosort = false;
      break;
    }
    const u32 selected = globalReady.top();
    globalReady.pop();
    globalOrder.push_back(selected);
    for (u32 successor : globalSuccessors[selected])
      if (--globalIndegrees[successor] == 0)
        globalReady.push(successor);
  }
  // Async submission returns before it can inspect the overflow flag. Use it
  // only with the proven one-fragment ordered kernel, which cannot overflow.
  if (asynchronous && !orderedAutosort)
    return false;
  const bool simpleTranslucent = globalAutosort && globalActiveCount != 0;
  const auto &referenceBins = orderedAutosort ? orderedBins : bins;
  std::vector<u32> opaquePrefixEnds;
  std::vector<u8> reverseOpaqueBins;
  opaquePrefixEnds.reserve(referenceBins.size());
  const bool skipOpaquePrefix = std::getenv("FLYCAST_HYBRID_DISABLE_OPAQUE_PREFIX_SKIP") == nullptr;
  const bool reverseOpaque = std::getenv("FLYCAST_HYBRID_DISABLE_REVERSE_OPAQUE") == nullptr;
  for (const auto &bin : referenceBins) {
    size_t prefix = 0;
    while (prefix < bin.size() &&
        (((*frame.triangles)[bin[prefix]].state >> ListTypeShift) & 3) == 0)
      ++prefix;
    // Keep the original sequence for mixed/interleaved list types.
    for (size_t i = prefix; i < bin.size(); ++i)
      if ((((*frame.triangles)[bin[i]].state >> ListTypeShift) & 3) == 0) {
        prefix = 0;
        break;
      }
    // Bound integer edge products so the barycentric upper-bound proof cannot
    // depend on signed overflow. Other geometry keeps the original traversal.
    bool reversible = reverseOpaque && skipOpaquePrefix && prefix != 0
        && frame.width <= (1u << 22) && frame.height <= (1u << 22);
    for (size_t i = 0; i < prefix && reversible; ++i) {
      const Triangle &triangle = (*frame.triangles)[bin[i]];
      reversible = (triangle.state & (DepthModeMask | DepthWrite)) == (6 | DepthWrite)
          && triangle.area1Index == std::numeric_limits<u32>::max()
          && triangle.z[0] >= 0.f && triangle.z[1] >= 0.f && triangle.z[2] >= 0.f
          && (triangle.fpuSampleOffset == 0 || triangle.fpuSampleOffset == 8);
      for (u32 vertex = 0; vertex < 3 && reversible; ++vertex)
        reversible = triangle.x[vertex] >= -(1 << 26) && triangle.x[vertex] <= (1 << 26)
            && triangle.y[vertex] >= -(1 << 26) && triangle.y[vertex] <= (1 << 26);
    }
    reverseOpaqueBins.push_back(reversible);
    opaquePrefixEnds.push_back(static_cast<u32>(tileReferences.size() + (skipOpaquePrefix ? prefix : 0)));
    tileReferences.insert(tileReferences.end(), bin.begin(), bin.end());
    tileOffsets.push_back(static_cast<u32>(tileReferences.size()));
  }
  useHardwareVisibility = useHardwareVisibility && hasOpaque &&
      deferOpaqueShading && frame.modifierMask == nullptr &&
      frame.initialDepths == nullptr;
  std::vector<VisibilityVertex> visibilityVertices;
  std::vector<u32> visibilityPrimitiveIndices;
  std::vector<u8> hardwareVisibilityTiles(bins.size());
  std::vector<u32> visibilityDrawStarts(bins.size());
  std::vector<u32> visibilityDrawCounts(bins.size());
  if (useHardwareVisibility) {
    for (u32 tile = 0; tile < bins.size(); ++tile) {
      std::vector<u32> alwaysReferences;
      std::vector<u32> depthReferences;
      bool sawDepthTested = false;
      bool ordered = true;
      for (u32 index : bins[tile])
        if (((*frame.triangles)[index].state >> ListTypeShift & 3) == 0) {
          if (((*frame.triangles)[index].state & DepthModeMask) == 7) {
            if (sawDepthTested) {
              ordered = false;
              break;
            }
            alwaysReferences.push_back(index);
          } else {
            sawDepthTested = true;
            depthReferences.push_back(index);
          }
        }
      std::sort(depthReferences.begin(), depthReferences.end(),
          [&](u32 left, u32 right) {
            const Triangle &a = (*frame.triangles)[left];
            const Triangle &b = (*frame.triangles)[right];
            return std::min({a.z[0], a.z[1], a.z[2]}) <
                std::min({b.z[0], b.z[1], b.z[2]});
          });
      float previousMaximum = -std::numeric_limits<float>::infinity();
      for (u32 index : alwaysReferences) {
        const Triangle &triangle = (*frame.triangles)[index];
        previousMaximum = std::max(previousMaximum,
            std::max({triangle.z[0], triangle.z[1], triangle.z[2]}));
      }
      for (u32 index : depthReferences) {
        const Triangle &triangle = (*frame.triangles)[index];
        const float minimum = std::min({triangle.z[0], triangle.z[1],
                                        triangle.z[2]});
        if (minimum <= previousMaximum) {
          ordered = false;
          break;
        }
        previousMaximum = std::max({triangle.z[0], triangle.z[1],
                                    triangle.z[2]});
      }
      if (!ordered)
        continue;
      hardwareVisibilityTiles[tile] = 1;
      visibilityDrawStarts[tile] = static_cast<u32>(visibilityVertices.size());
      alwaysReferences.insert(alwaysReferences.end(), depthReferences.begin(),
                              depthReferences.end());
      for (u32 index : alwaysReferences) {
        const Triangle &triangle = (*frame.triangles)[index];
        visibilityPrimitiveIndices.push_back(index);
        for (u32 vertex = 0; vertex < 3; ++vertex)
          visibilityVertices.push_back({triangle.x[vertex], triangle.y[vertex],
                                        triangle.z[vertex], 0});
      }
      visibilityDrawCounts[tile] =
          static_cast<u32>(visibilityVertices.size()) - visibilityDrawStarts[tile];
    }
    if (visibilityVertices.empty())
      useHardwareVisibility = false;
  }
  const size_t pixelCount = static_cast<size_t>(frame.width) * frame.height;
  if (frame.initialColors != nullptr &&
      frame.initialColors->size() != pixelCount)
    return false;
  if (frame.modifierMask != nullptr && frame.modifierMask->size() != pixelCount)
    return false;
  if (frame.initialDepths != nullptr && frame.initialDepths->size() != pixelCount)
    return false;
  if (frame.initialSecondary != nullptr &&
      frame.initialSecondary->size() != pixelCount)
    return false;
  const std::vector<u8> *masks[] = {frame.depthClearMask, frame.opaqueMask,
                                   frame.punchThroughMask, frame.translucentMask,
                                   frame.commitMask};
  for (const std::vector<u8> *mask : masks)
    if (mask != nullptr && mask->size() != pixelCount)
      return false;
  id<MTLBuffer> triangleBuffer =
      metal.upload(MetalState::Triangles, *frame.triangles);
  id<MTLBuffer> offsetBuffer =
      metal.upload(MetalState::TileOffsets, tileOffsets);
  id<MTLBuffer> referenceBuffer =
      metal.upload(MetalState::TileReferences, tileReferences);
  id<MTLBuffer> opaquePrefixBuffer =
      metal.upload(MetalState::OpaquePrefixEnds, opaquePrefixEnds);
  id<MTLBuffer> reverseOpaqueBuffer =
      metal.upload(MetalState::ReverseOpaqueBins, reverseOpaqueBins);
  id<MTLBuffer> textureBuffer =
      metal.upload(MetalState::Texture, *frame.texture);
  const std::array<u32, 128> emptyFog {};
  id<MTLBuffer> fogBuffer = metal.buffer(
      MetalState::Fog, emptyFog.size() * sizeof(u32),
      frame.fogTable != nullptr ? frame.fogTable : emptyFog.data());
  const std::vector<u8> emptyModifier(pixelCount);
  id<MTLBuffer> modifierBuffer = metal.upload(MetalState::Modifier,
      frame.modifierMask != nullptr ? *frame.modifierMask : emptyModifier);
  const std::vector<u8> emptyMask(pixelCount);
  const std::vector<u8> fullMask(pixelCount, 1);
  id<MTLBuffer> depthClearBuffer = metal.upload(MetalState::DepthClear,
      frame.depthClearMask != nullptr ? *frame.depthClearMask : emptyMask);
  id<MTLBuffer> opaqueMaskBuffer = metal.upload(MetalState::OpaqueMask,
      frame.opaqueMask != nullptr ? *frame.opaqueMask : fullMask);
  id<MTLBuffer> punchMaskBuffer = metal.upload(MetalState::PunchMask,
      frame.punchThroughMask != nullptr ? *frame.punchThroughMask : fullMask);
  id<MTLBuffer> translucentMaskBuffer = metal.upload(MetalState::TranslucentMask,
      frame.translucentMask != nullptr ? *frame.translucentMask : fullMask);
  id<MTLBuffer> hardwareVisibilityTileBuffer =
      metal.upload(MetalState::HardwareVisibilityTiles,
                   hardwareVisibilityTiles);
  id<MTLBuffer> commitMaskBuffer = metal.upload(MetalState::CommitMask,
      frame.commitMask != nullptr ? *frame.commitMask : fullMask);
  id<MTLBuffer> colorBuffer =
      metal.presentationBuffer(frame.width, frame.height, presentation);
  id<MTLBuffer> initialPresentationBuffer = metal.presentationBufferFor(
      frame.initialPresentation, frame.width, frame.height);
  id<MTLBuffer> depthBuffer =
      metal.buffer(MetalState::Depth, pixelCount * sizeof(float));
  id<MTLBuffer> overflowBuffer =
      metal.buffer(MetalState::Overflow, sizeof(u32));
  id<MTLBuffer> secondaryBuffer =
      metal.buffer(MetalState::Secondary, pixelCount * sizeof(u32));
  id<MTLBuffer> visibilityVertexBuffer = useHardwareVisibility
      ? metal.upload(MetalState::VisibilityVertices, visibilityVertices) : nil;
  id<MTLBuffer> visibilityPrimitiveBuffer = useHardwareVisibility
      ? metal.upload(MetalState::VisibilityPrimitives,
                     visibilityPrimitiveIndices)
      : nil;
  id<MTLTexture> visibilityTexture =
      metal.visibilityTexture(frame.width, frame.height);
  if (triangleBuffer == nil || offsetBuffer == nil || referenceBuffer == nil || opaquePrefixBuffer == nil || reverseOpaqueBuffer == nil ||
      textureBuffer == nil || colorBuffer == nil || depthBuffer == nil ||
      overflowBuffer == nil || fogBuffer == nil || modifierBuffer == nil ||
      depthClearBuffer == nil || opaqueMaskBuffer == nil ||
      punchMaskBuffer == nil || translucentMaskBuffer == nil ||
      hardwareVisibilityTileBuffer == nil || commitMaskBuffer == nil ||
      secondaryBuffer == nil ||
      visibilityTexture == nil ||
      (useHardwareVisibility && (visibilityVertexBuffer == nil ||
          visibilityPrimitiveBuffer == nil)))
    return false;
  *static_cast<u32 *>(overflowBuffer.contents) = 0;
  if (initialPresentationBuffer == nil && frame.initialColors != nullptr)
    std::memcpy(colorBuffer.contents, frame.initialColors->data(),
                pixelCount * sizeof(u32));
  if (frame.initialDepths != nullptr)
    std::memcpy(depthBuffer.contents, frame.initialDepths->data(),
                pixelCount * sizeof(float));
  if (frame.initialSecondary != nullptr)
    std::memcpy(secondaryBuffer.contents, frame.initialSecondary->data(),
                pixelCount * sizeof(u32));
  const Constants constants{frame.width, frame.height, tilesX,
                            (frame.initialColors != nullptr ||
                             initialPresentationBuffer != nil) ? 1u : 0u,
                            frame.fogColor, frame.fogVertexColor, frame.fogDensity,
                            frame.modifierMask != nullptr ? 1u : 0u,
                            frame.initialDepths != nullptr ? 1u : 0u,
                            frame.initialSecondary != nullptr ? 1u : 0u,
                            hasOpaque && deferOpaqueShading ? 1u : 0u,
                            useHardwareVisibility ? 1u : 0u,
                            visibilitySampleOffset == 0 ? .5f : 0.f,
                            frame.commitMask != nullptr ? 1u : 0u};
  const auto encodeEnd = std::chrono::steady_clock::now();

  id<MTLCommandBuffer> command = [metal.queue commandBuffer];
  if (initialPresentationBuffer != nil) {
    id<MTLBlitCommandEncoder> blit = [command blitCommandEncoder];
    [blit copyFromBuffer:initialPresentationBuffer sourceOffset:0
                toBuffer:colorBuffer destinationOffset:0
                    size:pixelCount * sizeof(u32)];
    [blit endEncoding];
  }
  u32 hardwareVisibilityDraws = 0;
  const bool batchVisibility = std::getenv("FLYCAST_HYBRID_DISABLE_VISIBILITY_BATCHING") == nullptr;
  if (useHardwareVisibility) {
    MTLRenderPassDescriptor *pass =
        [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = visibilityTexture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
    id<MTLRenderCommandEncoder> visibilityEncoder =
        [command renderCommandEncoderWithDescriptor:pass];
    [visibilityEncoder setRenderPipelineState:metal.visibilityPipeline];
    [visibilityEncoder setVertexBuffer:visibilityVertexBuffer offset:0 atIndex:0];
    [visibilityEncoder setVertexBytes:&constants length:sizeof(constants) atIndex:1];
    [visibilityEncoder setFragmentBuffer:triangleBuffer offset:0 atIndex:1];
    [visibilityEncoder setCullMode:MTLCullModeNone];
    for (u32 tile = 0; tile < bins.size(); ++tile) {
      if (visibilityDrawCounts[tile] == 0)
        continue;
      const u32 tileX = tile % tilesX;
      const u32 tileY = tile / tilesX;
      // Merge only horizontal neighbors with exactly the same ordered
      // primitives. The wider scissor is their union; no pixel gains a
      // primitive or sees a different order. Keep gaps and row boundaries.
      u32 runLength = 1;
      const auto first = visibilityPrimitiveIndices.begin() + visibilityDrawStarts[tile] / 3;
      const auto last = first + visibilityDrawCounts[tile] / 3;
      while (batchVisibility && tileX + runLength < tilesX &&
             visibilityDrawCounts[tile + runLength] == visibilityDrawCounts[tile] &&
             std::equal(first, last, visibilityPrimitiveIndices.begin() +
                 visibilityDrawStarts[tile + runLength] / 3))
        ++runLength;
      const MTLScissorRect scissor{
          static_cast<NSUInteger>(tileX * TileSize),
          static_cast<NSUInteger>(tileY * TileSize),
          static_cast<NSUInteger>(std::min(TileSize * runLength,
              frame.width - tileX * TileSize)),
          static_cast<NSUInteger>(std::min(TileSize,
              frame.height - tileY * TileSize))};
      [visibilityEncoder setScissorRect:scissor];
      [visibilityEncoder setFragmentBuffer:visibilityPrimitiveBuffer
          offset:(visibilityDrawStarts[tile] / 3) * sizeof(u32) atIndex:0];
      [visibilityEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                            vertexStart:visibilityDrawStarts[tile]
                            vertexCount:visibilityDrawCounts[tile]];
      ++hardwareVisibilityDraws;
      tile += runLength - 1;
    }
    [visibilityEncoder endEncoding];
  }
  id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
  [encoder setComputePipelineState:simpleTranslucent
      ? metal.simpleTranslucentPipeline
      : (orderedAutosort ? metal.orderedAutosortPipeline : metal.pipeline)];
  [encoder setBuffer:triangleBuffer offset:0 atIndex:0];
  [encoder setBuffer:offsetBuffer offset:0 atIndex:1];
  [encoder setBuffer:referenceBuffer offset:0 atIndex:2];
  [encoder setBuffer:colorBuffer offset:0 atIndex:3];
  [encoder setBuffer:depthBuffer offset:0 atIndex:4];
  [encoder setBuffer:textureBuffer offset:0 atIndex:5];
  [encoder setBytes:&constants length:sizeof(constants) atIndex:6];
  [encoder setBuffer:overflowBuffer offset:0 atIndex:7];
  [encoder setBuffer:fogBuffer offset:0 atIndex:8];
  [encoder setBuffer:modifierBuffer offset:0 atIndex:9];
  [encoder setBuffer:depthClearBuffer offset:0 atIndex:10];
  [encoder setBuffer:opaqueMaskBuffer offset:0 atIndex:11];
  [encoder setBuffer:punchMaskBuffer offset:0 atIndex:12];
  [encoder setBuffer:translucentMaskBuffer offset:0 atIndex:13];
  [encoder setBuffer:secondaryBuffer offset:0 atIndex:14];
  [encoder setBuffer:hardwareVisibilityTileBuffer offset:0 atIndex:15];
  [encoder setBuffer:commitMaskBuffer offset:0 atIndex:16];
  [encoder setBuffer:opaquePrefixBuffer offset:0 atIndex:17];
  [encoder setBuffer:reverseOpaqueBuffer offset:0 atIndex:18];
  [encoder setTexture:visibilityTexture atIndex:0];
  [encoder dispatchThreads:MTLSizeMake(frame.width, frame.height, 1)
      threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
  [encoder endEncoding];
  const auto gpuStart = std::chrono::steady_clock::now();
  if (asynchronous) {
    if (presentation == nullptr || presentation->handle == nullptr)
      return false;
    metal.markSubmitted(command, overflowBuffer, *presentation);
    [command commit];
    if (timing != nullptr)
      timing->encodeMilliseconds =
          std::chrono::duration<double, std::milli>(encodeEnd - encodeStart)
              .count();
    return true;
  }
  [command commit];
  [command waitUntilCompleted];
  const auto gpuEnd = std::chrono::steady_clock::now();
  if (command.status != MTLCommandBufferStatusCompleted)
    return false;
  if (*static_cast<const u32 *>(overflowBuffer.contents) != 0)
    return false;

  const auto readbackStart = std::chrono::steady_clock::now();
  colors.resize(pixelCount);
  depths.resize(pixelCount);
  std::memcpy(colors.data(), colorBuffer.contents, pixelCount * sizeof(u32));
  std::memcpy(depths.data(), depthBuffer.contents, pixelCount * sizeof(float));
  if (secondaryOutput != nullptr) {
    secondaryOutput->resize(pixelCount);
    std::memcpy(secondaryOutput->data(), secondaryBuffer.contents,
                pixelCount * sizeof(u32));
  }
  const auto readbackEnd = std::chrono::steady_clock::now();
  if (timing != nullptr) {
    timing->encodeMilliseconds =
        std::chrono::duration<double, std::milli>(encodeEnd - encodeStart)
            .count();
    timing->gpuMilliseconds =
        std::chrono::duration<double, std::milli>(gpuEnd - gpuStart).count();
    timing->gpuExecutionMilliseconds =
        std::max(0., (command.GPUEndTime - command.GPUStartTime) * 1000.);
    timing->readbackMilliseconds =
        std::chrono::duration<double, std::milli>(readbackEnd - readbackStart)
            .count();
    timing->hardwareVisibilityTiles = static_cast<u32>(std::count(
        hardwareVisibilityTiles.begin(), hardwareVisibilityTiles.end(), u8{1}));
    timing->hardwareVisibilityDraws = hardwareVisibilityDraws;
    timing->totalTiles = static_cast<u32>(hardwareVisibilityTiles.size());
    timing->orderedAutosortFrames = orderedAutosort ? 1 : 0;
    timing->orderedAutosortTiles = orderedAutosortTiles;
    timing->globalAutosortFrames = globalAutosort ? 1 : 0;
    timing->simpleTranslucentFrames = simpleTranslucent ? 1 : 0;
    timing->orderedAutosortRejectReason = orderedAutosortRejectReason;
    for (const Triangle &triangle : *frame.triangles) {
      const u32 listType = (triangle.state >> ListTypeShift) & 3;
      timing->opaqueTriangles += listType == 0;
      timing->punchThroughTriangles += listType == 1;
      timing->translucentTriangles += listType == 2;
    }
  }
  return true;
}

bool render(const Frame &frame, std::vector<u32> &colors,
            std::vector<float> &depths, Timing *timing,
            std::vector<u32> *secondaryOutput,
            PresentationSurface *presentation) {
  return renderInternal(frame, colors, depths, timing, secondaryOutput,
                        presentation, false);
}

bool renderAsync(const Frame &frame, PresentationSurface &presentation) {
  std::vector<u32> unusedColors;
  std::vector<float> unusedDepths;
  presentation = {};
  return renderInternal(frame, unusedColors, unusedDepths, nullptr, nullptr,
                        &presentation, true);
}

bool takeLatestPresentation(PresentationSurface &presentation) {
  return sharedMetalState().latestPresentation(presentation);
}

bool copyLatestPresentation(std::vector<u32> &colors,
                            PresentationSurface *presentation) {
  return sharedMetalState().copyLatestPresentation(colors, presentation);
}

} // namespace rend::soft::hybrid
