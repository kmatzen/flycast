// Feasibility test for using Metal's fixed-function rasterizer as the PVR ISP.
// clang++ -std=c++20 -O3 -fobjc-arc -framework Foundation -framework Metal \
//   tests/manual/MetalHardwareRasterizerPrototype.mm -o /tmp/metal-hw-raster

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <array>
#include <chrono>
#include <cstdint>
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

std::vector<uint8_t> reference(std::array<Vertex, 3> triangle,
                               int32_t sampleOffset) {
  if (edge(triangle[0], triangle[1], triangle[2].x, triangle[2].y) < 0)
    std::swap(triangle[1], triangle[2]);
  std::vector<uint8_t> result(Width * Height);
  for (uint32_t y = 0; y < Height; ++y)
    for (uint32_t x = 0; x < Width; ++x) {
      const int32_t sx = int32_t(x) * 16 + sampleOffset;
      const int32_t sy = int32_t(y) * 16 + sampleOffset;
      const int64_t w0 = edge(triangle[1], triangle[2], sx, sy);
      const int64_t w1 = edge(triangle[2], triangle[0], sx, sy);
      const int64_t w2 = edge(triangle[0], triangle[1], sx, sy);
      result[y * Width + x] =
          w0 > 0 || (w0 == 0 && topLeft(triangle[1], triangle[2]))
              ? (w1 > 0 || (w1 == 0 && topLeft(triangle[2], triangle[0]))) &&
                        (w2 > 0 || (w2 == 0 && topLeft(triangle[0], triangle[1])))
              : false;
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
struct Input { int2 fixedPosition; };
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
fragment float4 fragmentMain() { return float4(1.0f); }
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
    descriptor.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor error:&error];
    if (pipeline == nil)
      return 2;
    MTLTextureDescriptor *textureDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                    width:Width height:Height mipmapped:NO];
    textureDescriptor.usage = MTLTextureUsageRenderTarget;
    textureDescriptor.storageMode = MTLStorageModeShared;
    id<MTLTexture> target = [device newTextureWithDescriptor:textureDescriptor];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    std::mt19937 random(0x505652u);
    std::uniform_int_distribution<int32_t> coordinate(-64, Width * 16 + 64);
    size_t tested = 0;
    for (int32_t sampleOffset : {0, 8}) {
      for (uint32_t iteration = 0; iteration < 2000; ++iteration) {
        std::array<Vertex, 3> triangle;
        for (Vertex &vertex : triangle)
          vertex = {coordinate(random), coordinate(random)};
        const int64_t area = edge(triangle[0], triangle[1], triangle[2].x,
                                  triangle[2].y);
        if (area == 0)
          continue;
        if (area < 0)
          std::swap(triangle[1], triangle[2]);
        const std::vector<uint8_t> expected = reference(triangle, sampleOffset);
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
        [encoder setCullMode:MTLCullModeNone];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        std::vector<uint8_t> pixels(Width * Height * 4);
        [target getBytes:pixels.data() bytesPerRow:Width * 4
              fromRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0];
        for (size_t pixel = 0; pixel < expected.size(); ++pixel)
          if ((pixels[pixel * 4] != 0) != (expected[pixel] != 0)) {
            std::fprintf(stderr,
                "coverage mismatch offset=%d iteration=%u pixel=%zu triangle="
                "(%d,%d),(%d,%d),(%d,%d)\n", sampleOffset, iteration, pixel,
                triangle[0].x, triangle[0].y, triangle[1].x, triangle[1].y,
                triangle[2].x, triangle[2].y);
            return 1;
          }
        ++tested;
      }
    }
    std::vector<Vertex> stressVertices;
    stressVertices.reserve(72 * 3);
    for (uint32_t triangle = 0; triangle < 72; ++triangle) {
      const int32_t inset = int32_t(triangle % 5) * 16;
      stressVertices.push_back({inset, inset});
      stressVertices.push_back({int32_t(StressWidth - 1) * 16 - inset, inset});
      stressVertices.push_back({inset, int32_t(StressHeight - 1) * 16 - inset});
    }
    id<MTLBuffer> stressBuffer = [device newBufferWithBytes:stressVertices.data()
        length:stressVertices.size() * sizeof(Vertex)
        options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *stressTextureDescriptor = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
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
      [encoder setCullMode:MTLCullModeNone];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
                  vertexCount:stressVertices.size()];
      [encoder endEncoding];
      [command commit];
      [command waitUntilCompleted];
    }
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count() / Iterations;
    std::printf("hardware_coverage_triangles=%zu mismatches=0 "
                "stress_triangles=72 mean_submit_gpu_ms=%.3f\n",
                tested, milliseconds);
  }
  return 0;
}
