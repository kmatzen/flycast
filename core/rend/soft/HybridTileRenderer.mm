#include "HybridTileRenderer.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>

namespace rend::soft::hybrid {
namespace {

constexpr u32 TileSize = 32;

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
};

static_assert(sizeof(Constants) == 40);

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
};

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
        const float top = float(channel(samples[0], shift))
                + (float(channel(samples[1], shift)) - float(channel(samples[0], shift)))
                        * fraction.x;
        const float bottom = float(channel(samples[2], shift))
                + (float(channel(samples[3], shift)) - float(channel(samples[2], shift)))
                        * fraction.x;
        result[component] = top + (bottom - top) * fraction.y;
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
        uint2 pixel [[thread_position_in_grid]])
{
    if (pixel.x >= constants.width || pixel.y >= constants.height)
        return;
    const uint offset = pixel.y * constants.width + pixel.x;
    const uint tile = (pixel.y / 32) * constants.tilesX + pixel.x / 32;
    uint color = constants.useInitialColor != 0 ? colors[offset] : 0xff000000u;
    uint secondary = constants.useInitialSecondary != 0
            ? secondaryColors[offset] : 0u;
    float depth = depthClearMask[offset] != 0 ? 0.0f
            : (constants.useInitialDepth != 0 ? depths[offset] : 0.0f);
    constexpr uint MaxSortedFragments = 64;
    uint sortedColors[MaxSortedFragments];
    float sortedDepths[MaxSortedFragments];
    uint sortedIndices[MaxSortedFragments];
    uint sortedCount = 0;
    for (uint i = tileOffsets[tile]; i < tileOffsets[tile + 1]; ++i) {
        const uint triangleIndex = tileReferences[i];
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
        if (!depthTest(t.state, depthZ, depth))
            continue;
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
            const float fog = float(channel(offsetColor, 24)) / 255.0f;
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
        if ((t.state & (1u << 22)) != 0) {
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
        if ((t.state & (1u << 3)) != 0)
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
    colors[offset] = color;
    secondaryColors[offset] = secondary;
    depths[offset] = depth;
}
)METAL";

class MetalState {
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
    if (library == nil)
      return;
    id<MTLFunction> function = [library newFunctionWithName:@"rasterTiles"];
    pipeline = [device newComputePipelineStateWithFunction:function
                                                     error:&error];
    if (pipeline != nil)
      queue = [device newCommandQueue];
  }

  bool valid() const { return queue != nil && pipeline != nil; }

  id<MTLDevice> device = nil;
  id<MTLCommandQueue> queue = nil;
  id<MTLComputePipelineState> pipeline = nil;
};

template <typename T>
id<MTLBuffer> makeBuffer(id<MTLDevice> device, const std::vector<T> &data) {
  if (data.empty())
    return nil;
  return [device newBufferWithBytes:data.data()
                             length:data.size() * sizeof(T)
                            options:MTLResourceStorageModeShared];
}

} // namespace

bool render(const Frame &frame, std::vector<u32> &colors,
            std::vector<float> &depths, Timing *timing,
            std::vector<u32> *secondaryOutput) {
  static MetalState metal;
  if (!metal.valid() || frame.width == 0 || frame.height == 0 ||
      frame.triangles == nullptr || frame.triangles->empty() ||
      frame.texture == nullptr || frame.texture->empty() ||
      frame.texture->size() > std::numeric_limits<u32>::max())
    return false;

  const auto encodeStart = std::chrono::steady_clock::now();
  const u32 tilesX = (frame.width + TileSize - 1) / TileSize;
  const u32 tilesY = (frame.height + TileSize - 1) / TileSize;
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
    for (s32 y = triangle.minY / static_cast<s32>(TileSize);
         y <= triangle.maxY / static_cast<s32>(TileSize); ++y)
      for (s32 x = triangle.minX / static_cast<s32>(TileSize);
           x <= triangle.maxX / static_cast<s32>(TileSize); ++x)
        bins[static_cast<size_t>(y) * tilesX + x].push_back(index);
  }
  std::vector<u32> tileOffsets(1, 0);
  std::vector<u32> tileReferences;
  for (const auto &bin : bins) {
    tileReferences.insert(tileReferences.end(), bin.begin(), bin.end());
    tileOffsets.push_back(static_cast<u32>(tileReferences.size()));
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
                                   frame.punchThroughMask, frame.translucentMask};
  for (const std::vector<u8> *mask : masks)
    if (mask != nullptr && mask->size() != pixelCount)
      return false;
  id<MTLBuffer> triangleBuffer = makeBuffer(metal.device, *frame.triangles);
  id<MTLBuffer> offsetBuffer = makeBuffer(metal.device, tileOffsets);
  id<MTLBuffer> referenceBuffer = makeBuffer(metal.device, tileReferences);
  id<MTLBuffer> textureBuffer = makeBuffer(metal.device, *frame.texture);
  const std::array<u32, 128> emptyFog {};
  id<MTLBuffer> fogBuffer = [metal.device
      newBufferWithBytes:frame.fogTable != nullptr ? frame.fogTable : emptyFog.data()
                length:emptyFog.size() * sizeof(u32)
               options:MTLResourceStorageModeShared];
  const std::vector<u8> emptyModifier(pixelCount);
  id<MTLBuffer> modifierBuffer = makeBuffer(metal.device,
      frame.modifierMask != nullptr ? *frame.modifierMask : emptyModifier);
  const std::vector<u8> emptyMask(pixelCount);
  const std::vector<u8> fullMask(pixelCount, 1);
  id<MTLBuffer> depthClearBuffer = makeBuffer(metal.device,
      frame.depthClearMask != nullptr ? *frame.depthClearMask : emptyMask);
  id<MTLBuffer> opaqueMaskBuffer = makeBuffer(metal.device,
      frame.opaqueMask != nullptr ? *frame.opaqueMask : fullMask);
  id<MTLBuffer> punchMaskBuffer = makeBuffer(metal.device,
      frame.punchThroughMask != nullptr ? *frame.punchThroughMask : fullMask);
  id<MTLBuffer> translucentMaskBuffer = makeBuffer(metal.device,
      frame.translucentMask != nullptr ? *frame.translucentMask : fullMask);
  id<MTLBuffer> colorBuffer =
      [metal.device newBufferWithLength:pixelCount * sizeof(u32)
                                options:MTLResourceStorageModeShared];
  id<MTLBuffer> depthBuffer =
      [metal.device newBufferWithLength:pixelCount * sizeof(float)
                                options:MTLResourceStorageModeShared];
  id<MTLBuffer> overflowBuffer =
      [metal.device newBufferWithLength:sizeof(u32)
                                options:MTLResourceStorageModeShared];
  id<MTLBuffer> secondaryBuffer =
      [metal.device newBufferWithLength:pixelCount * sizeof(u32)
                                options:MTLResourceStorageModeShared];
  if (triangleBuffer == nil || offsetBuffer == nil || referenceBuffer == nil ||
      textureBuffer == nil || colorBuffer == nil || depthBuffer == nil ||
      overflowBuffer == nil || fogBuffer == nil || modifierBuffer == nil ||
      depthClearBuffer == nil || opaqueMaskBuffer == nil ||
      punchMaskBuffer == nil || translucentMaskBuffer == nil ||
      secondaryBuffer == nil)
    return false;
  *static_cast<u32 *>(overflowBuffer.contents) = 0;
  if (frame.initialColors != nullptr)
    std::memcpy(colorBuffer.contents, frame.initialColors->data(),
                pixelCount * sizeof(u32));
  if (frame.initialDepths != nullptr)
    std::memcpy(depthBuffer.contents, frame.initialDepths->data(),
                pixelCount * sizeof(float));
  if (frame.initialSecondary != nullptr)
    std::memcpy(secondaryBuffer.contents, frame.initialSecondary->data(),
                pixelCount * sizeof(u32));
  const Constants constants{frame.width, frame.height, tilesX,
                            frame.initialColors != nullptr ? 1u : 0u,
                            frame.fogColor, frame.fogVertexColor, frame.fogDensity,
                            frame.modifierMask != nullptr ? 1u : 0u,
                            frame.initialDepths != nullptr ? 1u : 0u,
                            frame.initialSecondary != nullptr ? 1u : 0u};
  const auto encodeEnd = std::chrono::steady_clock::now();

  id<MTLCommandBuffer> command = [metal.queue commandBuffer];
  id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
  [encoder setComputePipelineState:metal.pipeline];
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
  [encoder dispatchThreads:MTLSizeMake(frame.width, frame.height, 1)
      threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
  [encoder endEncoding];
  const auto gpuStart = std::chrono::steady_clock::now();
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
    timing->readbackMilliseconds =
        std::chrono::duration<double, std::milli>(readbackEnd - readbackStart)
            .count();
  }
  return true;
}

} // namespace rend::soft::hybrid
