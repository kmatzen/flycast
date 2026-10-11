#include "HybridTileRenderer.h"

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstring>

namespace rend::soft::hybrid {
namespace {

constexpr u32 TileSize = 32;

struct Constants {
  u32 width;
  u32 height;
  u32 tilesX;
  u32 textureWidth;
  u32 textureHeight;
  u32 reserved;
};

static_assert(sizeof(Constants) == 24);

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
    uint flags;
    int minX;
    int minY;
    int maxX;
    int maxY;
};

struct Constants {
    uint width;
    uint height;
    uint tilesX;
    uint textureWidth;
    uint textureHeight;
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

inline uint sourceAlphaBlend(uint source, uint destination)
{
    const uint alpha = source >> 24;
    const uint inverseAlpha = 255 - alpha;
    uint result = 0;
    for (uint shift = 0; shift < 32; shift += 8) {
        const uint sourceChannel = (source >> shift) & 0xff;
        const uint destinationChannel = (destination >> shift) & 0xff;
        const uint blended = min(255u, (sourceChannel * alpha
                + destinationChannel * inverseAlpha + 127) / 255);
        result |= blended << shift;
    }
    return result;
}

inline uint sampleBilinear(const device uint *texture, float2 uv,
        constant Constants& constants)
{
    uv -= floor(uv);
    const float2 size = float2(constants.textureWidth, constants.textureHeight);
    const float2 texel = uv * size - .5f;
    const int2 base = int2(floor(texel));
    // The supported subset uses native power-of-two PVR textures.
    const uint2 mask = uint2(constants.textureWidth - 1, constants.textureHeight - 1);
    const uint2 p0 = uint2(base) & mask;
    const uint2 p1 = uint2(base + 1) & mask;
    const uint samples[4] = {
        texture[p0.y * constants.textureWidth + p0.x],
        texture[p0.y * constants.textureWidth + p1.x],
        texture[p1.y * constants.textureWidth + p0.x],
        texture[p1.y * constants.textureWidth + p1.x],
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

inline uint modulate(uint base, uint texel)
{
    float4 result;
    for (uint component = 0; component < 4; ++component) {
        const uint shift = component * 8;
        result[component] = float(channel(base, shift) * channel(texel, shift)) / 255.0f;
    }
    return packColor(result);
}

kernel void rasterTiles(const device Triangle *triangles [[buffer(0)]],
        const device uint *tileOffsets [[buffer(1)]],
        const device uint *tileReferences [[buffer(2)]],
        device uint *colors [[buffer(3)]], device float *depths [[buffer(4)]],
        const device uint *texture [[buffer(5)]],
        constant Constants& constants [[buffer(6)]],
        uint2 pixel [[thread_position_in_grid]])
{
    if (pixel.x >= constants.width || pixel.y >= constants.height)
        return;
    const uint offset = pixel.y * constants.width + pixel.x;
    const uint tile = (pixel.y / 32) * constants.tilesX + pixel.x / 32;
    uint color = 0xff000000u;
    float depth = 0.0f;
    const int sx = int(pixel.x) * 16 + 8;
    const int sy = int(pixel.y) * 16 + 8;
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
        if (z < depth)
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
        const float u = (t.u[0] * t.z[0] * s0 + t.u[1] * t.z[1] * s1
                + t.u[2] * t.z[2] * s2) * inverseZ;
        const float v = (t.v[0] * t.z[0] * s0 + t.v[1] * t.z[1] * s1
                + t.v[2] * t.z[2] * s2) * inverseZ;
        const uint source = modulate(packColor(interpolated),
                sampleBilinear(texture, float2(u, v), constants));
        if ((t.flags & 2) != 0)
            depth = z;
        color = (t.flags & 1) != 0 ? sourceAlphaBlend(source, color) : source;
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
      frame.textureWidth == 0 || frame.textureHeight == 0 ||
      (frame.textureWidth & (frame.textureWidth - 1)) != 0 ||
      (frame.textureHeight & (frame.textureHeight - 1)) != 0 ||
      frame.texture->size() !=
          static_cast<size_t>(frame.textureWidth) * frame.textureHeight)
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
        triangle.maxY >= static_cast<s32>(frame.height) || triangle.flags > 3)
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
  id<MTLBuffer> triangleBuffer = makeBuffer(metal.device, *frame.triangles);
  id<MTLBuffer> offsetBuffer = makeBuffer(metal.device, tileOffsets);
  id<MTLBuffer> referenceBuffer = makeBuffer(metal.device, tileReferences);
  id<MTLBuffer> textureBuffer = makeBuffer(metal.device, *frame.texture);
  id<MTLBuffer> colorBuffer =
      [metal.device newBufferWithLength:pixelCount * sizeof(u32)
                                options:MTLResourceStorageModeShared];
  id<MTLBuffer> depthBuffer =
      [metal.device newBufferWithLength:pixelCount * sizeof(float)
                                options:MTLResourceStorageModeShared];
  if (triangleBuffer == nil || offsetBuffer == nil || referenceBuffer == nil ||
      textureBuffer == nil || colorBuffer == nil || depthBuffer == nil)
    return false;
  const Constants constants{frame.width,        frame.height,        tilesX,
                            frame.textureWidth, frame.textureHeight, 0};
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
  [encoder dispatchThreads:MTLSizeMake(frame.width, frame.height, 1)
      threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
  [encoder endEncoding];
  const auto gpuStart = std::chrono::steady_clock::now();
  [command commit];
  [command waitUntilCompleted];
  const auto gpuEnd = std::chrono::steady_clock::now();
  if (command.status != MTLCommandBufferStatusCompleted)
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
