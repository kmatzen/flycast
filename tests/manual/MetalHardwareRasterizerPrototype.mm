// Feasibility test for using Metal's fixed-function rasterizer as the PVR ISP.
// clang++ -std=c++20 -O3 -fobjc-arc -framework Foundation -framework Metal \
//   tests/manual/MetalHardwareRasterizerPrototype.mm -o /tmp/metal-hw-raster

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <random>
#include <vector>

namespace {
constexpr uint32_t Width = 32;
constexpr uint32_t Height = 32;
constexpr uint32_t StressWidth = 640;
constexpr uint32_t StressHeight = 480;

struct Vertex {
  int32_t x;
  int32_t y;
  float z;
  uint32_t color;
};

int64_t edge(const Vertex &a, const Vertex &b, int32_t x, int32_t y) {
  return int64_t(b.x - a.x) * (y - a.y) -
         int64_t(b.y - a.y) * (x - a.x);
}

bool topLeft(const Vertex &a, const Vertex &b) {
  const int64_t dx = int64_t(b.x) - a.x;
  const int64_t dy = int64_t(b.y) - a.y;
  return dy < 0 || (dy == 0 && dx > 0);
}

std::vector<uint32_t> reference(std::array<Vertex, 3> triangle,
                                int32_t sampleOffset) {
  if (edge(triangle[0], triangle[1], triangle[2].x, triangle[2].y) < 0)
    std::swap(triangle[1], triangle[2]);
  std::vector<uint32_t> result(Width * Height);
  for (uint32_t y = 0; y < Height; ++y)
    for (uint32_t x = 0; x < Width; ++x) {
      const int32_t sx = int32_t(x) * 16 + sampleOffset;
      const int32_t sy = int32_t(y) * 16 + sampleOffset;
      const int64_t w0 = edge(triangle[1], triangle[2], sx, sy);
      const int64_t w1 = edge(triangle[2], triangle[0], sx, sy);
      const int64_t w2 = edge(triangle[0], triangle[1], sx, sy);
      const bool covered =
          (w0 > 0 || (w0 == 0 && topLeft(triangle[1], triangle[2]))) &&
          (w1 > 0 || (w1 == 0 && topLeft(triangle[2], triangle[0]))) &&
          (w2 > 0 || (w2 == 0 && topLeft(triangle[0], triangle[1])));
      if (!covered)
        continue;
      const int64_t area = edge(triangle[0], triangle[1], triangle[2].x,
                                triangle[2].y);
      const float s[3]{float(w0) / float(area), float(w1) / float(area),
                       float(w2) / float(area)};
      const float q = triangle[0].z * s[0] + triangle[1].z * s[1] +
                      triangle[2].z * s[2];
      const float inverseQ = 1.f / q;
      uint32_t packed = 0;
      for (uint32_t component = 0; component < 4; ++component) {
        const uint32_t shift = component * 8;
        float value = 0.f;
        for (uint32_t vertex = 0; vertex < 3; ++vertex)
          value += float((triangle[vertex].color >> shift) & 255) *
                   triangle[vertex].z * s[vertex];
        packed |= uint32_t(std::clamp(
                      std::floor(value * inverseQ + .5f), 0.f, 255.f))
                  << shift;
      }
      result[y * Width + x] = packed;
    }
  return result;
}
} // namespace

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil)
      return 2;
    static const char *source = R"METAL(
#include <metal_stdlib>
using namespace metal;
struct Input { int2 fixedPosition; float z; uint color; };
struct Constants { float2 targetSize; float bias; };
struct Output { float4 position [[position]]; };
vertex Output vertexMain(const device Input *vertices [[buffer(0)]],
                         constant Constants& constants [[buffer(1)]],
                         uint id [[vertex_id]]) {
  const float2 screen = float2(vertices[id].fixedPosition) / 16.0f
          + constants.bias;
  Output result;
  result.position = float4(screen.x / constants.targetSize.x * 2.0f - 1.0f,
                           1.0f - screen.y / constants.targetSize.y * 2.0f,
                           0.0f, 1.0f);
  return result;
}
inline long edge(int2 a, int2 b, int2 p) {
  return long(b.x - a.x) * long(p.y - a.y)
       - long(b.y - a.y) * long(p.x - a.x);
}
fragment uint4 fragmentMain(Output input [[stage_in]],
                             const device Input *vertices [[buffer(0)]],
                             constant Constants& constants [[buffer(1)]],
                             uint primitive [[primitive_id]]) {
  const device Input *v = vertices + primitive * 3;
  const int2 sample = int2(round((input.position.xy - constants.bias) * 16.0f));
  const long w0 = edge(v[1].fixedPosition, v[2].fixedPosition, sample);
  const long w1 = edge(v[2].fixedPosition, v[0].fixedPosition, sample);
  const long w2 = edge(v[0].fixedPosition, v[1].fixedPosition, sample);
  const long area = edge(v[0].fixedPosition, v[1].fixedPosition,
                         v[2].fixedPosition);
  const float3 s = float3(w0, w1, w2) / float(area);
  const float q = v[0].z * s.x + v[1].z * s.y + v[2].z * s.z;
  const float inverseQ = 1.0f / q;
  float4 color;
  for (uint component = 0; component < 4; ++component) {
    const uint shift = component * 8;
    color[component] = (float((v[0].color >> shift) & 255) * v[0].z * s.x
                      + float((v[1].color >> shift) & 255) * v[1].z * s.y
                      + float((v[2].color >> shift) & 255) * v[2].z * s.z)
                      * inverseQ;
  }
  return uint4(clamp(floor(color + .5f), 0.0f, 255.0f));
}
)METAL";
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
    id<MTLLibrary> library = [device
        newLibraryWithSource:[NSString stringWithUTF8String:source]
                     options:options error:&error];
    if (library == nil) {
      std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
      return 2;
    }
    MTLRenderPipelineDescriptor *descriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = [library newFunctionWithName:@"vertexMain"];
    descriptor.fragmentFunction = [library newFunctionWithName:@"fragmentMain"];
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Uint;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil)
      return 2;
    MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Uint
                                    width:Width height:Height mipmapped:NO];
    textureDescriptor.usage = MTLTextureUsageRenderTarget;
    textureDescriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [device newTextureWithDescriptor:textureDescriptor];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    std::mt19937 random(0x505652u);
    std::uniform_int_distribution<int32_t> coordinate(-64, Width * 16 + 64);
    std::uniform_real_distribution<float> depth(.05f, 1.f);
    size_t tested = 0;
    size_t interpolationMismatches = 0;
    size_t lowerChannels = 0;
    size_t higherChannels = 0;
    for (int32_t sampleOffset : {0, 8}) {
      for (uint32_t iteration = 0; iteration < 2000; ++iteration) {
        std::array<Vertex, 3> triangle;
        for (Vertex &vertex : triangle)
          vertex = {coordinate(random), coordinate(random), depth(random), random()};
        const int64_t area = edge(triangle[0], triangle[1], triangle[2].x,
                                  triangle[2].y);
        if (area == 0)
          continue;
        if (area < 0)
          std::swap(triangle[1], triangle[2]);
        const std::vector<uint32_t> expected = reference(triangle, sampleOffset);
        id<MTLBuffer> vertices = [device newBufferWithBytes:triangle.data()
            length:sizeof(triangle) options:MTLResourceStorageModeShared];
        struct { float size[2]; float bias; } constants{
            {float(Width), float(Height)}, sampleOffset == 0 ? .5f : 0.f};
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder =
            [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:vertices offset:0 atIndex:0];
        [encoder setVertexBytes:&constants length:sizeof(constants) atIndex:1];
        [encoder setFragmentBuffer:vertices offset:0 atIndex:0];
        [encoder setFragmentBytes:&constants length:sizeof(constants) atIndex:1];
        [encoder setCullMode:MTLCullModeNone];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        std::vector<uint8_t> pixels(Width * Height * 4);
        [target getBytes:pixels.data() bytesPerRow:Width * 4
              fromRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0];
        for (size_t pixel = 0; pixel < expected.size(); ++pixel) {
          uint32_t actual;
          std::memcpy(&actual, pixels.data() + pixel * 4, sizeof(actual));
          if ((actual == 0) != (expected[pixel] == 0)) {
            std::fprintf(stderr,
                "coverage mismatch offset=%d iteration=%u pixel=%zu "
                "expected=%08x actual=%08x triangle="
                "(%d,%d),(%d,%d),(%d,%d)\n", sampleOffset, iteration, pixel,
                expected[pixel], actual,
                triangle[0].x, triangle[0].y, triangle[1].x, triangle[1].y,
                triangle[2].x, triangle[2].y);
            return 1;
          }
          if (actual != expected[pixel]) {
            ++interpolationMismatches;
            for (uint32_t shift = 0; shift < 32; shift += 8) {
              const uint32_t expectedChannel = (expected[pixel] >> shift) & 255;
              const uint32_t actualChannel = (actual >> shift) & 255;
              lowerChannels += actualChannel < expectedChannel;
              higherChannels += actualChannel > expectedChannel;
            }
          }
        }
        ++tested;
      }
    }
    std::vector<Vertex> stressVertices;
    stressVertices.reserve(72 * 3);
    for (uint32_t triangle = 0; triangle < 72; ++triangle) {
      const int32_t inset = int32_t(triangle % 5) * 16;
      stressVertices.push_back({inset, inset, 1.f, 0xffffffffu});
      stressVertices.push_back(
          {int32_t(StressWidth - 1) * 16 - inset, inset, 1.f, 0xffffffffu});
      stressVertices.push_back(
          {inset, int32_t(StressHeight - 1) * 16 - inset, 1.f, 0xffffffffu});
    }
    id<MTLBuffer> stressBuffer = [device newBufferWithBytes:stressVertices.data()
        length:stressVertices.size() * sizeof(Vertex)
        options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *stressTextureDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Uint
                                    width:StressWidth height:StressHeight mipmapped:NO];
    stressTextureDescriptor.usage = MTLTextureUsageRenderTarget;
    stressTextureDescriptor.storageMode = MTLStorageModePrivate;
    id<MTLTexture> stressTarget =
        [device newTextureWithDescriptor:stressTextureDescriptor];
    struct { float size[2]; float bias; } stressConstants{
        {float(StressWidth), float(StressHeight)}, 0.f};
    constexpr uint32_t Iterations = 100;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0; iteration < Iterations; ++iteration) {
      MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = stressTarget;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
      id<MTLCommandBuffer> command = [queue commandBuffer];
      id<MTLRenderCommandEncoder> encoder =
          [command renderCommandEncoderWithDescriptor:pass];
      [encoder setRenderPipelineState:pipeline];
      [encoder setVertexBuffer:stressBuffer offset:0 atIndex:0];
      [encoder setVertexBytes:&stressConstants length:sizeof(stressConstants)
                       atIndex:1];
      [encoder setFragmentBuffer:stressBuffer offset:0 atIndex:0];
      [encoder setFragmentBytes:&stressConstants length:sizeof(stressConstants)
                         atIndex:1];
      [encoder setCullMode:MTLCullModeNone];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
                  vertexCount:stressVertices.size()];
      [encoder endEncoding];
      [command commit];
      [command waitUntilCompleted];
    }
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count() / Iterations;
    std::printf("hardware_coverage_triangles=%zu coverage_mismatches=0 "
                "interpolation_mismatches=%zu stress_triangles=72 "
                "lower_channels=%zu higher_channels=%zu mean_submit_gpu_ms=%.3f\n",
                tested, interpolationMismatches, lowerChannels, higherChannels,
                milliseconds);
  }
  return 0;
}
