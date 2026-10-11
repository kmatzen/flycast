// Standalone feasibility test for a GPU-executed, tile-ordered software PVR.
// Build on macOS with:
//   clang++ -std=c++20 -O3 -fobjc-arc -framework Foundation -framework Metal \
//     tests/manual/MetalTileRasterizerPrototype.mm -o /tmp/metal-tile-prototype

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

namespace {

constexpr uint32_t Width = 640;
constexpr uint32_t Height = 480;
constexpr uint32_t TileSize = 32;
constexpr uint32_t TilesX = (Width + TileSize - 1) / TileSize;
constexpr uint32_t TilesY = (Height + TileSize - 1) / TileSize;
constexpr int32_t SubpixelScale = 16;
constexpr int32_t SampleOffset = 8;
constexpr uint32_t TextureSize = 64;

struct Triangle {
  int32_t x[3];
  int32_t y[3];
  float z[3];
  float u[3];
  float v[3];
  uint32_t color[3];
  // Bit 0: source-alpha blend. Bit 1: depth write.
  uint32_t flags;
  int32_t minX;
  int32_t minY;
  int32_t maxX;
  int32_t maxY;
};

static_assert(sizeof(Triangle) == 92);

uint32_t channel(uint32_t color, uint32_t shift) {
  return (color >> shift) & 0xff;
}

uint32_t packColor(float r, float g, float b, float a) {
  const auto byte = [](float value) {
    return static_cast<uint32_t>(std::clamp(std::lround(value), 0l, 255l));
  };
  return byte(r) | (byte(g) << 8) | (byte(b) << 16) | (byte(a) << 24);
}

uint32_t sampleBilinear(const std::vector<uint32_t> &texture, float u,
                        float v) {
  u -= std::floor(u);
  v -= std::floor(v);
  const float texelX = u * TextureSize - .5f;
  const float texelY = v * TextureSize - .5f;
  const int32_t x0 = static_cast<int32_t>(std::floor(texelX));
  const int32_t y0 = static_cast<int32_t>(std::floor(texelY));
  const uint32_t x1 = static_cast<uint32_t>(x0 + 1) & (TextureSize - 1);
  const uint32_t y1 = static_cast<uint32_t>(y0 + 1) & (TextureSize - 1);
  const uint32_t wrappedX0 = static_cast<uint32_t>(x0) & (TextureSize - 1);
  const uint32_t wrappedY0 = static_cast<uint32_t>(y0) & (TextureSize - 1);
  const uint32_t c00 = texture[wrappedY0 * TextureSize + wrappedX0];
  const uint32_t c10 = texture[wrappedY0 * TextureSize + x1];
  const uint32_t c01 = texture[y1 * TextureSize + wrappedX0];
  const uint32_t c11 = texture[y1 * TextureSize + x1];
  const float tx = texelX - x0;
  const float ty = texelY - y0;
  float result[4];
  for (uint32_t component = 0; component < 4; ++component) {
    const uint32_t shift = component * 8;
    const float top =
        channel(c00, shift) +
        (static_cast<float>(channel(c10, shift)) - channel(c00, shift)) * tx;
    const float bottom =
        channel(c01, shift) +
        (static_cast<float>(channel(c11, shift)) - channel(c01, shift)) * tx;
    result[component] = top + (bottom - top) * ty;
  }
  return packColor(result[0], result[1], result[2], result[3]);
}

uint32_t modulate(uint32_t base, uint32_t texel) {
  float result[4];
  for (uint32_t component = 0; component < 4; ++component) {
    const uint32_t shift = component * 8;
    result[component] = channel(base, shift) * channel(texel, shift) / 255.f;
  }
  return packColor(result[0], result[1], result[2], result[3]);
}

uint32_t sourceAlphaBlend(uint32_t source, uint32_t destination) {
  const uint32_t alpha = source >> 24;
  const uint32_t inverseAlpha = 255 - alpha;
  uint32_t result = 0;
  for (uint32_t shift = 0; shift < 32; shift += 8) {
    const uint32_t sourceChannel = (source >> shift) & 0xff;
    const uint32_t destinationChannel = (destination >> shift) & 0xff;
    const uint32_t channel =
        std::min(255u, (sourceChannel * alpha +
                        destinationChannel * inverseAlpha + 127) /
                           255);
    result |= channel << shift;
  }
  return result;
}

int64_t edge(int32_t ax, int32_t ay, int32_t bx, int32_t by, int32_t x,
             int32_t y) {
  return static_cast<int64_t>(bx - ax) * (y - ay) -
         static_cast<int64_t>(by - ay) * (x - ax);
}

bool topLeft(int32_t ax, int32_t ay, int32_t bx, int32_t by) {
  const int64_t dx = static_cast<int64_t>(bx) - ax;
  const int64_t dy = static_cast<int64_t>(by) - ay;
  return dy < 0 || (dy == 0 && dx > 0);
}

void rasterizeReference(const std::vector<Triangle> &triangles,
                        const std::vector<uint32_t> &texture,
                        std::vector<uint32_t> &colors,
                        std::vector<float> &depths) {
  std::fill(colors.begin(), colors.end(), 0xff000000u);
  std::fill(depths.begin(), depths.end(), 0.f);
  for (const Triangle &triangle : triangles) {
    const int64_t area = edge(triangle.x[0], triangle.y[0], triangle.x[1],
                              triangle.y[1], triangle.x[2], triangle.y[2]);
    const bool tl0 =
        topLeft(triangle.x[1], triangle.y[1], triangle.x[2], triangle.y[2]);
    const bool tl1 =
        topLeft(triangle.x[2], triangle.y[2], triangle.x[0], triangle.y[0]);
    const bool tl2 =
        topLeft(triangle.x[0], triangle.y[0], triangle.x[1], triangle.y[1]);
    for (int32_t y = triangle.minY; y <= triangle.maxY; ++y)
      for (int32_t x = triangle.minX; x <= triangle.maxX; ++x) {
        const int32_t sx = x * SubpixelScale + SampleOffset;
        const int32_t sy = y * SubpixelScale + SampleOffset;
        const int64_t w0 = edge(triangle.x[1], triangle.y[1], triangle.x[2],
                                triangle.y[2], sx, sy);
        const int64_t w1 = edge(triangle.x[2], triangle.y[2], triangle.x[0],
                                triangle.y[0], sx, sy);
        const int64_t w2 = edge(triangle.x[0], triangle.y[0], triangle.x[1],
                                triangle.y[1], sx, sy);
        if (w0 < 0 || (w0 == 0 && !tl0) || w1 < 0 || (w1 == 0 && !tl1) ||
            w2 < 0 || (w2 == 0 && !tl2))
          continue;
        const float s0 = static_cast<float>(w0) / static_cast<float>(area);
        const float s1 = static_cast<float>(w1) / static_cast<float>(area);
        const float s2 = static_cast<float>(w2) / static_cast<float>(area);
        const float z =
            triangle.z[0] * s0 + triangle.z[1] * s1 + triangle.z[2] * s2;
        const size_t offset = static_cast<size_t>(y) * Width + x;
        if (z >= depths[offset]) {
          const float inverseZ = z == 0.f ? 0.f : 1.f / z;
          auto interpolate = [&](uint32_t shift) {
            return (channel(triangle.color[0], shift) * triangle.z[0] * s0 +
                    channel(triangle.color[1], shift) * triangle.z[1] * s1 +
                    channel(triangle.color[2], shift) * triangle.z[2] * s2) *
                   inverseZ;
          };
          uint32_t source = packColor(interpolate(0), interpolate(8),
                                      interpolate(16), interpolate(24));
          const float u = (triangle.u[0] * triangle.z[0] * s0 +
                           triangle.u[1] * triangle.z[1] * s1 +
                           triangle.u[2] * triangle.z[2] * s2) *
                          inverseZ;
          const float v = (triangle.v[0] * triangle.z[0] * s0 +
                           triangle.v[1] * triangle.z[1] * s1 +
                           triangle.v[2] * triangle.z[2] * s2) *
                          inverseZ;
          source = modulate(source, sampleBilinear(texture, u, v));
          if ((triangle.flags & 2) != 0)
            depths[offset] = z;
          colors[offset] = (triangle.flags & 1) != 0
                               ? sourceAlphaBlend(source, colors[offset])
                               : source;
        }
      }
  }
}

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

inline uint sourceAlphaBlend(uint source, uint destination)
{
    const uint alpha = source >> 24;
    const uint inverseAlpha = 255 - alpha;
    uint result = 0;
    for (uint shift = 0; shift < 32; shift += 8) {
        const uint sourceChannel = (source >> shift) & 0xff;
        const uint destinationChannel = (destination >> shift) & 0xff;
        const uint channel = min(255u, (sourceChannel * alpha
                + destinationChannel * inverseAlpha + 127) / 255);
        result |= channel << shift;
    }
    return result;
}

inline uint colorChannel(uint color, uint shift)
{
    return (color >> shift) & 0xff;
}

inline uint packColor(float4 value)
{
    const uint4 bytes = uint4(clamp(floor(value + .5f), 0.0f, 255.0f));
    return bytes.x | (bytes.y << 8) | (bytes.z << 16) | (bytes.w << 24);
}

inline uint sampleBilinear(const device uint *texture, float2 uv)
{
    constexpr uint textureSize = 64;
    uv -= floor(uv);
    const float2 texel = uv * float(textureSize) - .5f;
    const int2 base = int2(floor(texel));
    const uint2 p0 = uint2(base) & (textureSize - 1);
    const uint2 p1 = uint2(base + 1) & (textureSize - 1);
    const uint samples[4] = {
        texture[p0.y * textureSize + p0.x],
        texture[p0.y * textureSize + p1.x],
        texture[p1.y * textureSize + p0.x],
        texture[p1.y * textureSize + p1.x],
    };
    const float2 fraction = texel - float2(base);
    float4 result;
    for (uint component = 0; component < 4; ++component) {
        const uint shift = component * 8;
        const float top = float(colorChannel(samples[0], shift))
                + (float(colorChannel(samples[1], shift))
                        - float(colorChannel(samples[0], shift))) * fraction.x;
        const float bottom = float(colorChannel(samples[2], shift))
                + (float(colorChannel(samples[3], shift))
                        - float(colorChannel(samples[2], shift))) * fraction.x;
        result[component] = top + (bottom - top) * fraction.y;
    }
    return packColor(result);
}

inline uint modulate(uint base, uint texel)
{
    float4 result;
    for (uint component = 0; component < 4; ++component) {
        const uint shift = component * 8;
        result[component] = float(colorChannel(base, shift)
                * colorChannel(texel, shift)) / 255.0f;
    }
    return packColor(result);
}

kernel void rasterTiles(const device Triangle *triangles [[buffer(0)]],
        const device uint *tileOffsets [[buffer(1)]],
        const device uint *tileReferences [[buffer(2)]],
        device uint *colors [[buffer(3)]], device float *depths [[buffer(4)]],
        const device uint *texture [[buffer(5)]],
        uint2 pixel [[thread_position_in_grid]])
{
    constexpr uint width = 640;
    constexpr uint height = 480;
    constexpr uint tilesX = 20;
    if (pixel.x >= width || pixel.y >= height)
        return;
    const uint offset = pixel.y * width + pixel.x;
    const uint tile = (pixel.y / 32) * tilesX + pixel.x / 32;
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
        if (z >= depth) {
            const float inverseZ = z == 0.0f ? 0.0f : 1.0f / z;
            float4 interpolated;
            for (uint component = 0; component < 4; ++component) {
                const uint shift = component * 8;
                interpolated[component] =
                        (float(colorChannel(t.color[0], shift)) * t.z[0] * s0
                        + float(colorChannel(t.color[1], shift)) * t.z[1] * s1
                        + float(colorChannel(t.color[2], shift)) * t.z[2] * s2)
                        * inverseZ;
            }
            const float u = (t.u[0] * t.z[0] * s0 + t.u[1] * t.z[1] * s1
                    + t.u[2] * t.z[2] * s2) * inverseZ;
            const float v = (t.v[0] * t.z[0] * s0 + t.v[1] * t.z[1] * s1
                    + t.v[2] * t.z[2] * s2) * inverseZ;
            const uint source = modulate(packColor(interpolated),
                    sampleBilinear(texture, float2(u, v)));
            if ((t.flags & 2) != 0)
                depth = z;
            color = (t.flags & 1) != 0
                    ? sourceAlphaBlend(source, color) : source;
        }
    }
    colors[offset] = color;
    depths[offset] = depth;
}
)METAL";

template <typename T>
id<MTLBuffer> makeBuffer(id<MTLDevice> device, const std::vector<T> &values) {
  return [device newBufferWithBytes:values.data()
                             length:values.size() * sizeof(T)
                            options:MTLResourceStorageModeShared];
}

} // namespace

int main() {
  @autoreleasepool {
    std::mt19937 random(0x505652u);
    std::uniform_real_distribution<float> depth(0.01f, 1.f);
    std::vector<uint32_t> texture(TextureSize * TextureSize);
    for (uint32_t &texel : texture)
      texel = 0xff000000u | (random() & 0x00ffffffu);
    std::vector<Triangle> triangles;
    // Seaman's title workload consists of a small number of very large
    // polygons: 26 opaque triangles followed by 46 blended translucent ones.
    // Model that fill-bound shape rather than a geometry-heavy desktop scene.
    triangles.reserve(72);
    for (uint32_t i = 0; i < 72; ++i) {
      const int32_t inset = static_cast<int32_t>(i % 5);
      const int32_t x = inset;
      const int32_t y = inset;
      const int32_t w = Width - 1 - inset * 2;
      const int32_t h = Height - 1 - inset * 2;
      const bool translucent = i >= 26;
      const uint32_t alpha = translucent ? 32 + random() % 192 : 255;
      const auto vertexColor = [&] {
        return (alpha << 24) | (random() & 0x00ffffffu);
      };
      Triangle triangle{
          {x * SubpixelScale,
           std::min<int32_t>(Width - 1, x + w) * SubpixelScale,
           x * SubpixelScale},
          {y * SubpixelScale, y * SubpixelScale,
           std::min<int32_t>(Height - 1, y + h) * SubpixelScale},
          {depth(random), depth(random), depth(random)},
          {0.f, 4.f, 0.f},
          {0.f, 0.f, 4.f},
          {vertexColor(), vertexColor(), vertexColor()},
          translucent ? 1u : 2u,
          x,
          y,
          std::min<int32_t>(Width - 1, x + w),
          std::min<int32_t>(Height - 1, y + h),
      };
      if (edge(triangle.x[0], triangle.y[0], triangle.x[1], triangle.y[1],
               triangle.x[2], triangle.y[2]) > 0)
        triangles.push_back(triangle);
    }

    std::array<std::vector<uint32_t>, TilesX * TilesY> bins;
    for (uint32_t i = 0; i < triangles.size(); ++i) {
      const Triangle &t = triangles[i];
      for (int32_t ty = t.minY / TileSize; ty <= t.maxY / TileSize; ++ty)
        for (int32_t tx = t.minX / TileSize; tx <= t.maxX / TileSize; ++tx)
          bins[ty * TilesX + tx].push_back(i);
    }
    std::vector<uint32_t> tileOffsets(1, 0);
    std::vector<uint32_t> tileReferences;
    for (const auto &bin : bins) {
      tileReferences.insert(tileReferences.end(), bin.begin(), bin.end());
      tileOffsets.push_back(static_cast<uint32_t>(tileReferences.size()));
    }

    std::vector<uint32_t> referenceColors(Width * Height);
    std::vector<float> referenceDepths(Width * Height);
    const auto cpuStart = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < 20; ++iteration)
      rasterizeReference(triangles, texture, referenceColors, referenceDepths);
    const auto cpuEnd = std::chrono::steady_clock::now();

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil) {
      std::fprintf(stderr, "No Metal device\n");
      return 2;
    }
    NSError *error = nil;
    NSString *source = [NSString stringWithUTF8String:KernelSource];
    MTLCompileOptions *options = [[MTLCompileOptions alloc] init];
    // Safe math forbids reassociation and reciprocal approximations. The
    // CPU renderer remains the oracle, so matching its operation order is a
    // hard requirement rather than a visual-quality preference.
    if (@available(macOS 15.0, *))
      options.mathMode = MTLMathModeSafe;
    else {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
      options.fastMathEnabled = NO;
#pragma clang diagnostic pop
    }
    id<MTLLibrary> library = [device newLibraryWithSource:source
                                                  options:options
                                                    error:&error];
    if (library == nil) {
      std::fprintf(stderr, "Metal compilation failed: %s\n",
                   [[error localizedDescription] UTF8String]);
      return 2;
    }
    id<MTLFunction> function = [library newFunctionWithName:@"rasterTiles"];
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&error];
    if (pipeline == nil) {
      std::fprintf(stderr, "Pipeline creation failed: %s\n",
                   [[error localizedDescription] UTF8String]);
      return 2;
    }
    id<MTLBuffer> triangleBuffer = makeBuffer(device, triangles);
    id<MTLBuffer> offsetBuffer = makeBuffer(device, tileOffsets);
    id<MTLBuffer> referenceBuffer = makeBuffer(device, tileReferences);
    id<MTLBuffer> textureBuffer = makeBuffer(device, texture);
    id<MTLBuffer> colorBuffer =
        [device newBufferWithLength:Width * Height * sizeof(uint32_t)
                            options:MTLResourceStorageModeShared];
    id<MTLBuffer> depthBuffer =
        [device newBufferWithLength:Width * Height * sizeof(float)
                            options:MTLResourceStorageModeShared];
    id<MTLCommandQueue> queue = [device newCommandQueue];

    auto dispatch = [&] {
      id<MTLCommandBuffer> command = [queue commandBuffer];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      [encoder setComputePipelineState:pipeline];
      [encoder setBuffer:triangleBuffer offset:0 atIndex:0];
      [encoder setBuffer:offsetBuffer offset:0 atIndex:1];
      [encoder setBuffer:referenceBuffer offset:0 atIndex:2];
      [encoder setBuffer:colorBuffer offset:0 atIndex:3];
      [encoder setBuffer:depthBuffer offset:0 atIndex:4];
      [encoder setBuffer:textureBuffer offset:0 atIndex:5];
      [encoder dispatchThreads:MTLSizeMake(Width, Height, 1)
          threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
      [encoder endEncoding];
      [command commit];
      [command waitUntilCompleted];
    };
    dispatch();
    const auto gpuStart = std::chrono::steady_clock::now();
    for (int iteration = 0; iteration < 100; ++iteration)
      dispatch();
    const auto gpuEnd = std::chrono::steady_clock::now();

    const auto *gpuColors = static_cast<const uint32_t *>(colorBuffer.contents);
    const auto *gpuDepths = static_cast<const float *>(depthBuffer.contents);
    size_t colorMismatches = 0;
    size_t depthMismatches = 0;
    for (size_t i = 0; i < referenceColors.size(); ++i) {
      colorMismatches += gpuColors[i] != referenceColors[i];
      depthMismatches +=
          std::memcmp(&gpuDepths[i], &referenceDepths[i], sizeof(float)) != 0;
    }
    const double cpuMs =
        std::chrono::duration<double, std::milli>(cpuEnd - cpuStart).count() /
        20.;
    const double gpuMs =
        std::chrono::duration<double, std::milli>(gpuEnd - gpuStart).count() /
        100.;
    std::printf("device=%s triangles=%zu tile_refs=%zu cpu_ms=%.3f gpu_ms=%.3f "
                "speedup=%.2fx color_mismatches=%zu depth_bit_mismatches=%zu\n",
                [device.name UTF8String], triangles.size(),
                tileReferences.size(), cpuMs, gpuMs, cpuMs / gpuMs,
                colorMismatches, depthMismatches);
    return colorMismatches == 0 ? 0 : 1;
  }
}
