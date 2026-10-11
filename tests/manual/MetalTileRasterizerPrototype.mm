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

struct Triangle {
  int32_t x[3];
  int32_t y[3];
  float z[3];
  uint32_t color;
  int32_t minX;
  int32_t minY;
  int32_t maxX;
  int32_t maxY;
};

static_assert(sizeof(Triangle) == 56);

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
        const float z = static_cast<float>(
            (triangle.z[0] * w0 + triangle.z[1] * w1 + triangle.z[2] * w2) /
            static_cast<double>(area));
        const size_t offset = static_cast<size_t>(y) * Width + x;
        if (z >= depths[offset]) {
          depths[offset] = z;
          colors[offset] = triangle.color;
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
    uint color;
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

kernel void rasterTiles(const device Triangle *triangles [[buffer(0)]],
        const device uint *tileOffsets [[buffer(1)]],
        const device uint *tileReferences [[buffer(2)]],
        device uint *colors [[buffer(3)]], device float *depths [[buffer(4)]],
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
        const float z = (t.z[0] * float(w0) + t.z[1] * float(w1)
                + t.z[2] * float(w2)) / float(area);
        if (z >= depth) {
            depth = z;
            color = t.color;
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
    std::uniform_int_distribution<int32_t> xPosition(0, Width - 1);
    std::uniform_int_distribution<int32_t> yPosition(0, Height - 1);
    std::uniform_int_distribution<int32_t> extent(8, 96);
    std::uniform_real_distribution<float> depth(0.01f, 1.f);
    std::vector<Triangle> triangles;
    triangles.reserve(2000);
    for (uint32_t i = 0; i < 2000; ++i) {
      const int32_t x = xPosition(random);
      const int32_t y = yPosition(random);
      const int32_t w = extent(random);
      const int32_t h = extent(random);
      Triangle triangle{
          {x * SubpixelScale,
           std::min<int32_t>(Width - 1, x + w) * SubpixelScale,
           x * SubpixelScale},
          {y * SubpixelScale, y * SubpixelScale,
           std::min<int32_t>(Height - 1, y + h) * SubpixelScale},
          {depth(random), depth(random), depth(random)},
          0xff000000u | (random() & 0x00ffffffu),
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
      rasterizeReference(triangles, referenceColors, referenceDepths);
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
