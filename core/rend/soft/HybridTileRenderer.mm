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
  float fogDensity;
  u32 reserved;
};

static_assert(sizeof(Constants) == 28);

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
};

struct Constants {
    uint width;
    uint height;
    uint tilesX;
    uint useInitialColor;
    uint fogColor;
    float fogDensity;
    uint reserved;
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

inline uint sampleTexture(const device uint *texture, const Triangle t, float2 uv,
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
        const float selected = max(0.0f, floor(max(pvrD, 1.0f)) - 1.0f);
        const uint level = min(t.mipLevelCount - 1, uint(selected));
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

kernel void rasterTiles(const device Triangle *triangles [[buffer(0)]],
        const device uint *tileOffsets [[buffer(1)]],
        const device uint *tileReferences [[buffer(2)]],
        device uint *colors [[buffer(3)]], device float *depths [[buffer(4)]],
        const device uint *texture [[buffer(5)]],
        constant Constants& constants [[buffer(6)]],
        device atomic_uint *overflow [[buffer(7)]],
        const device uint *fogTable [[buffer(8)]],
        uint2 pixel [[thread_position_in_grid]])
{
    if (pixel.x >= constants.width || pixel.y >= constants.height)
        return;
    const uint offset = pixel.y * constants.width + pixel.x;
    const uint tile = (pixel.y / 32) * constants.tilesX + pixel.x / 32;
    uint color = constants.useInitialColor != 0 ? colors[offset] : 0xff000000u;
    float depth = 0.0f;
    const int sx = int(pixel.x) * 16 + 8;
    const int sy = int(pixel.y) * 16 + 8;
    constexpr uint MaxSortedFragments = 64;
    uint sortedColors[MaxSortedFragments];
    float sortedDepths[MaxSortedFragments];
    uint sortedStates[MaxSortedFragments];
    uint sortedSequences[MaxSortedFragments];
    uint sortedCount = 0;
    for (uint i = tileOffsets[tile]; i < tileOffsets[tile + 1]; ++i) {
        const Triangle t = triangles[tileReferences[i]];
        if (int(pixel.x) < t.minX || int(pixel.x) > t.maxX
                || int(pixel.y) < t.minY || int(pixel.y) > t.maxY)
            continue;
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
        const float z = t.z[0] * s0 + t.z[1] * s1 + t.z[2] * s2;
        if (!depthTest(t.state, z, depth))
            continue;
        const float inverseZ = z == 0.0f ? 0.0f : 1.0f / z;
        float4 interpolated;
        for (uint component = 0; component < 4; ++component) {
            const uint shift = component * 8;
            interpolated[component] =
                    (float(channel(t.color[0], shift)) * t.z[0] * s0
                    + float(channel(t.color[1], shift)) * t.z[1] * s1
                    + float(channel(t.color[2], shift)) * t.z[2] * s2) * inverseZ;
        }
        uint source = packColor(interpolated);
        if ((t.state & (1u << 11)) == 0)
            source |= 0xff000000u;
        if ((t.state & (1u << 4)) != 0) {
            const float u = (t.u[0] * t.z[0] * s0 + t.u[1] * t.z[1] * s1
                    + t.u[2] * t.z[2] * s2) * inverseZ;
            const float v = (t.v[0] * t.z[0] * s0 + t.v[1] * t.z[1] * s1
                    + t.v[2] * t.z[2] * s2) * inverseZ;
            const float pvrD = t.mipLevelCount > 1
                    ? max(0.0f, z == 0.0f ? 0.0f : t.mipNumerator / (z * z)) : 1.0f;
            uint texel = sampleTexture(texture, t, float2(u, v), pvrD);
            if ((t.state & (1u << 12)) != 0)
                texel |= 0xff000000u;
            source = applyTexture(source, texel, (t.state >> 14) & 3);
        }
        if ((t.state & (1u << 23)) != 0) {
            const float fog = fogCoefficient(z, constants.fogDensity, fogTable);
            float4 fogged;
            for (uint component = 0; component < 3; ++component) {
                const uint shift = component * 8;
                const float value = float(channel(source, shift));
                fogged[component] = value
                        + (float(channel(constants.fogColor, shift)) - value) * fog;
            }
            fogged.w = float(channel(source, 24));
            source = packColor(fogged);
        }
        if ((t.state & (1u << 13)) != 0 && channel(source, 24) < t.alphaReference)
            continue;
        if ((t.state & (1u << 13)) != 0)
            source |= 0xff000000u;
        if ((t.state & (1u << 22)) != 0) {
            if (sortedCount == MaxSortedFragments) {
                atomic_store_explicit(overflow, 1u, memory_order_relaxed);
                continue;
            }
            uint position = sortedCount++;
            while (position != 0 && (z < sortedDepths[position - 1]
                    || (z == sortedDepths[position - 1]
                            && tileReferences[i] < sortedSequences[position - 1]))) {
                sortedColors[position] = sortedColors[position - 1];
                sortedDepths[position] = sortedDepths[position - 1];
                sortedStates[position] = sortedStates[position - 1];
                sortedSequences[position] = sortedSequences[position - 1];
                --position;
            }
            sortedColors[position] = source;
            sortedDepths[position] = z;
            sortedStates[position] = t.state;
            sortedSequences[position] = tileReferences[i];
            continue;
        }
        if ((t.state & (1u << 3)) != 0)
            depth = z;
        const uint sourceFactor = (t.state >> 16) & 7;
        const uint destinationFactor = (t.state >> 19) & 7;
        if (sourceFactor == 1 && destinationFactor == 0)
            color = source;
        else if (sourceFactor == 4 && destinationFactor == 5)
            color = sourceAlphaBlend(source, color);
        else
            color = blend(source, color, sourceFactor, destinationFactor);
    }
    for (uint i = 0; i < sortedCount; ++i) {
        const uint sourceFactor = (sortedStates[i] >> 16) & 7;
        const uint destinationFactor = (sortedStates[i] >> 19) & 7;
        if (sourceFactor == 1 && destinationFactor == 0)
            color = sortedColors[i];
        else if (sourceFactor == 4 && destinationFactor == 5)
            color = sourceAlphaBlend(sortedColors[i], color);
        else
            color = blend(sortedColors[i], color, sourceFactor, destinationFactor);
    }
    colors[offset] = color;
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
            std::vector<float> &depths, Timing *timing) {
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
        triangle.alphaReference > 255 || (triangle.state >> 24) != 0)
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
  id<MTLBuffer> triangleBuffer = makeBuffer(metal.device, *frame.triangles);
  id<MTLBuffer> offsetBuffer = makeBuffer(metal.device, tileOffsets);
  id<MTLBuffer> referenceBuffer = makeBuffer(metal.device, tileReferences);
  id<MTLBuffer> textureBuffer = makeBuffer(metal.device, *frame.texture);
  const std::array<u32, 128> emptyFog {};
  id<MTLBuffer> fogBuffer = [metal.device
      newBufferWithBytes:frame.fogTable != nullptr ? frame.fogTable : emptyFog.data()
                length:emptyFog.size() * sizeof(u32)
               options:MTLResourceStorageModeShared];
  id<MTLBuffer> colorBuffer =
      [metal.device newBufferWithLength:pixelCount * sizeof(u32)
                                options:MTLResourceStorageModeShared];
  id<MTLBuffer> depthBuffer =
      [metal.device newBufferWithLength:pixelCount * sizeof(float)
                                options:MTLResourceStorageModeShared];
  id<MTLBuffer> overflowBuffer =
      [metal.device newBufferWithLength:sizeof(u32)
                                options:MTLResourceStorageModeShared];
  if (triangleBuffer == nil || offsetBuffer == nil || referenceBuffer == nil ||
      textureBuffer == nil || colorBuffer == nil || depthBuffer == nil ||
      overflowBuffer == nil || fogBuffer == nil)
    return false;
  *static_cast<u32 *>(overflowBuffer.contents) = 0;
  if (frame.initialColors != nullptr)
    std::memcpy(colorBuffer.contents, frame.initialColors->data(),
                pixelCount * sizeof(u32));
  const Constants constants{frame.width, frame.height, tilesX,
                            frame.initialColors != nullptr ? 1u : 0u,
                            frame.fogColor, frame.fogDensity, 0};
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
  [encoder dispatchThreads:MTLSizeMake(frame.width, frame.height, 1)
      threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
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
