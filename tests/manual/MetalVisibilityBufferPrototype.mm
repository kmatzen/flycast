// Feasibility test for a hybrid PVR ISP: Metal rasterization emits the exact
// visible primitive, then compute shades only that primitive.
// clang++ -std=c++20 -O3 -fobjc-arc -framework Foundation -framework Metal \
//   tests/manual/MetalVisibilityBufferPrototype.mm -o /tmp/metal-visibility

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
constexpr uint32_t Width = 32;
constexpr uint32_t Height = 32;
constexpr uint32_t StressWidth = 640;
constexpr uint32_t StressHeight = 480;

struct Vertex {
  int32_t x;
  int32_t y;
  float z;
  uint32_t padding = 0;
};

struct Constants {
  float targetSize[2];
  float bias;
  uint32_t primitiveCount;
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

std::vector<uint32_t> reference(const std::vector<Vertex> &vertices,
                                int32_t sampleOffset) {
  std::vector<uint32_t> ids(Width * Height);
  std::vector<float> depths(Width * Height, 0.f);
  for (size_t primitive = 0; primitive < vertices.size() / 3; ++primitive) {
    const Vertex *v = vertices.data() + primitive * 3;
    const int64_t area = edge(v[0], v[1], v[2].x, v[2].y);
    for (uint32_t y = 0; y < Height; ++y)
      for (uint32_t x = 0; x < Width; ++x) {
        const int32_t sx = int32_t(x) * 16 + sampleOffset;
        const int32_t sy = int32_t(y) * 16 + sampleOffset;
        const int64_t w0 = edge(v[1], v[2], sx, sy);
        const int64_t w1 = edge(v[2], v[0], sx, sy);
        const int64_t w2 = edge(v[0], v[1], sx, sy);
        if (w0 < 0 || (w0 == 0 && !topLeft(v[1], v[2])) ||
            w1 < 0 || (w1 == 0 && !topLeft(v[2], v[0])) ||
            w2 < 0 || (w2 == 0 && !topLeft(v[0], v[1])))
          continue;
        const float s0 = float(w0) / float(area);
        const float s1 = float(w1) / float(area);
        const float s2 = float(w2) / float(area);
        const float z = v[0].z * s0 + v[1].z * s1 + v[2].z * s2;
        const size_t pixel = y * Width + x;
        // The common PVR opaque state is greater-or-equal with depth writes.
        // Later equal-depth primitives therefore win, matching ordered draws.
        if (z >= depths[pixel]) {
          depths[pixel] = z;
          ids[pixel] = uint32_t(primitive + 1);
        }
      }
  }
  return ids;
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
struct Input { int2 fixedPosition; float z; };
struct Constants { float2 targetSize; float bias; uint primitiveCount; };
struct Varying { float4 position [[position]]; };
struct FragmentOutput { uint primitive [[color(0)]]; };
vertex Varying vertexMain(const device Input *vertices [[buffer(0)]],
                          constant Constants& constants [[buffer(1)]],
                          uint id [[vertex_id]]) {
  const float2 screen = float2(vertices[id].fixedPosition) / 16.0f
          + constants.bias;
  Varying result;
  result.position = float4(screen.x / constants.targetSize.x * 2.0f - 1.0f,
                           1.0f - screen.y / constants.targetSize.y * 2.0f,
                           vertices[id].z, 1.0f);
  return result;
}
inline long edge(int2 a, int2 b, int2 p) {
  return long(b.x - a.x) * long(p.y - a.y)
       - long(b.y - a.y) * long(p.x - a.x);
}
fragment FragmentOutput fragmentMain(uint primitive [[primitive_id]]) {
  FragmentOutput result;
  result.primitive = primitive + 1;
  return result;
}
kernel void correctVisibility(
                  texture2d<uint, access::read> visibility [[texture(0)]],
                  const device Input *vertices [[buffer(0)]],
                  device uint *corrected [[buffer(1)]],
                  constant Constants& constants [[buffer(2)]],
                  uint2 pixel [[thread_position_in_grid]]) {
  if (pixel.x >= visibility.get_width() || pixel.y >= visibility.get_height())
    return;
  const uint candidate = visibility.read(pixel).x;
  const uint index = pixel.y * visibility.get_width() + pixel.x;
  if (candidate == 0) {
    corrected[index] = 0;
    return;
  }
  const int2 sample = int2(pixel) * 16
          + int2(constants.bias == 0.0f ? 8 : 0);
  const uint candidatePrimitive = candidate - 1;
  const device Input *winner = vertices + candidatePrimitive * 3;
  const long candidateArea = edge(winner[0].fixedPosition,
          winner[1].fixedPosition, winner[2].fixedPosition);
  const long candidateW0 = edge(winner[1].fixedPosition,
          winner[2].fixedPosition, sample);
  const long candidateW1 = edge(winner[2].fixedPosition,
          winner[0].fixedPosition, sample);
  const long candidateW2 = edge(winner[0].fixedPosition,
          winner[1].fixedPosition, sample);
  const float candidateS0 = float(candidateW0) / float(candidateArea);
  const float candidateS1 = float(candidateW1) / float(candidateArea);
  const float candidateS2 = float(candidateW2) / float(candidateArea);
  float bestDepth = winner[0].z * candidateS0 + winner[1].z * candidateS1
          + winner[2].z * candidateS2;
  uint bestPrimitive = candidatePrimitive;
  for (uint primitive = 0; primitive < constants.primitiveCount; ++primitive) {
    if (primitive == candidatePrimitive)
      continue;
    const device Input *v = vertices + primitive * 3;
    if (max(v[0].z, max(v[1].z, v[2].z)) < bestDepth)
      continue;
    const long w0 = edge(v[1].fixedPosition, v[2].fixedPosition, sample);
    const long w1 = edge(v[2].fixedPosition, v[0].fixedPosition, sample);
    const long w2 = edge(v[0].fixedPosition, v[1].fixedPosition, sample);
    // Fixed-function coverage has already proved the candidate. Potential
    // replacements use the exact PVR 1/16 grid and top-left convention.
    const bool covered = w0 > 0 || (w0 == 0 &&
            (v[2].fixedPosition.y - v[1].fixedPosition.y < 0 ||
             (v[2].fixedPosition.y == v[1].fixedPosition.y &&
              v[2].fixedPosition.x - v[1].fixedPosition.x > 0)));
    const bool covered1 = w1 > 0 || (w1 == 0 &&
            (v[0].fixedPosition.y - v[2].fixedPosition.y < 0 ||
             (v[0].fixedPosition.y == v[2].fixedPosition.y &&
              v[0].fixedPosition.x - v[2].fixedPosition.x > 0)));
    const bool covered2 = w2 > 0 || (w2 == 0 &&
            (v[1].fixedPosition.y - v[0].fixedPosition.y < 0 ||
             (v[1].fixedPosition.y == v[0].fixedPosition.y &&
              v[1].fixedPosition.x - v[0].fixedPosition.x > 0)));
    if (!covered || !covered1 || !covered2)
      continue;
    const long area = edge(v[0].fixedPosition, v[1].fixedPosition,
                           v[2].fixedPosition);
    const float s0 = float(w0) / float(area);
    const float s1 = float(w1) / float(area);
    const float s2 = float(w2) / float(area);
    const float depth = v[0].z * s0 + v[1].z * s1 + v[2].z * s2;
    if (depth > bestDepth || (depth == bestDepth && primitive > bestPrimitive)) {
      bestDepth = depth;
      bestPrimitive = primitive;
    }
  }
  corrected[index] = bestPrimitive + 1;
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
    MTLRenderPipelineDescriptor *renderDescriptor =
        [[MTLRenderPipelineDescriptor alloc] init];
    renderDescriptor.vertexFunction = [library newFunctionWithName:@"vertexMain"];
    renderDescriptor.fragmentFunction = [library newFunctionWithName:@"fragmentMain"];
    renderDescriptor.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
    renderDescriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    id<MTLRenderPipelineState> renderPipeline =
        [device newRenderPipelineStateWithDescriptor:renderDescriptor error:&error];
    id<MTLComputePipelineState> shadePipeline = [device
        newComputePipelineStateWithFunction:
            [library newFunctionWithName:@"correctVisibility"]
        error:&error];
    if (renderPipeline == nil || shadePipeline == nil) {
      std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
      return 2;
    }
    MTLDepthStencilDescriptor *depthDescriptor =
        [[MTLDepthStencilDescriptor alloc] init];
    depthDescriptor.depthCompareFunction = MTLCompareFunctionGreaterEqual;
    depthDescriptor.depthWriteEnabled = YES;
    id<MTLDepthStencilState> depthState =
        [device newDepthStencilStateWithDescriptor:depthDescriptor];
    id<MTLCommandQueue> queue = [device newCommandQueue];

    auto makeTexture = ^id<MTLTexture>(MTLPixelFormat format, uint32_t width,
                                       uint32_t height, MTLTextureUsage usage,
                                       MTLStorageMode storage) {
      MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:format width:width height:height
          mipmapped:NO];
      descriptor.usage = usage;
      descriptor.storageMode = storage;
      return [device newTextureWithDescriptor:descriptor];
    };
    id<MTLTexture> visibility = makeTexture(MTLPixelFormatR32Uint, Width, Height,
        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
        MTLStorageModeShared);
    id<MTLTexture> depth = makeTexture(MTLPixelFormatDepth32Float, Width, Height,
        MTLTextureUsageRenderTarget, MTLStorageModePrivate);

    std::mt19937 random(0x495350u);
    std::uniform_int_distribution<int32_t> coordinate(-64, Width * 16 + 64);
    std::uniform_real_distribution<float> z(.01f, .99f);
    size_t scenes = 0;
    size_t randomVisibilityMismatches = 0;
    size_t nearEqualVisibilityMismatches = 0;
    size_t correctedVisibilityMismatches = 0;
    bool reportedFirstMismatch = false;
    for (int32_t sampleOffset : {0, 8}) {
      for (uint32_t iteration = 0; iteration < 250; ++iteration) {
        std::vector<Vertex> vertices;
        vertices.reserve(48 * 3);
        for (uint32_t primitive = 0; primitive < 48; ++primitive) {
          std::array<Vertex, 3> triangle;
          if ((iteration & 1) != 0 && (primitive & 1) != 0) {
            std::copy_n(vertices.end() - 3, 3, triangle.begin());
            for (Vertex &v : triangle)
              v.z = std::nextafter(v.z, 0.f);
          } else {
            do {
              for (Vertex &v : triangle)
                v = {coordinate(random), coordinate(random), z(random)};
            } while (edge(triangle[0], triangle[1], triangle[2].x,
                          triangle[2].y) == 0);
            if (edge(triangle[0], triangle[1], triangle[2].x,
                     triangle[2].y) < 0)
              std::swap(triangle[1], triangle[2]);
          }
          vertices.insert(vertices.end(), triangle.begin(), triangle.end());
        }
        const std::vector<uint32_t> expected = reference(vertices, sampleOffset);
        id<MTLBuffer> vertexBuffer = [device newBufferWithBytes:vertices.data()
            length:vertices.size() * sizeof(Vertex)
            options:MTLResourceStorageModeShared];
        Constants constants{{float(Width), float(Height)},
                            sampleOffset == 0 ? .5f : 0.f,
                            uint32_t(vertices.size() / 3)};
        id<MTLBuffer> correctedBuffer = [device newBufferWithLength:
            Width * Height * sizeof(uint32_t)
            options:MTLResourceStorageModeShared];
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = visibility;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        pass.depthAttachment.texture = depth;
        pass.depthAttachment.loadAction = MTLLoadActionClear;
        pass.depthAttachment.storeAction = MTLStoreActionDontCare;
        pass.depthAttachment.clearDepth = 0.;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder =
            [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:renderPipeline];
        [encoder setDepthStencilState:depthState];
        [encoder setVertexBuffer:vertexBuffer offset:0 atIndex:0];
        [encoder setVertexBytes:&constants length:sizeof(constants) atIndex:1];
        [encoder setCullMode:MTLCullModeNone];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
                    vertexCount:vertices.size()];
        [encoder endEncoding];
        id<MTLComputeCommandEncoder> correction =
            [command computeCommandEncoder];
        [correction setComputePipelineState:shadePipeline];
        [correction setTexture:visibility atIndex:0];
        [correction setBuffer:vertexBuffer offset:0 atIndex:0];
        [correction setBuffer:correctedBuffer offset:0 atIndex:1];
        [correction setBytes:&constants length:sizeof(constants) atIndex:2];
        [correction dispatchThreads:MTLSizeMake(Width, Height, 1)
                  threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [correction endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status == MTLCommandBufferStatusError) {
          std::fprintf(stderr, "command failed: %s\n",
                       command.error.localizedDescription.UTF8String);
          return 2;
        }
        std::vector<uint32_t> actual(Width * Height);
        const uint32_t *corrected =
            static_cast<const uint32_t *>(correctedBuffer.contents);
        [visibility getBytes:actual.data() bytesPerRow:Width * sizeof(uint32_t)
                  fromRegion:MTLRegionMake2D(0, 0, Width, Height) mipmapLevel:0];
        for (size_t pixel = 0; pixel < actual.size(); ++pixel) {
          if (actual[pixel] != expected[pixel]) {
            size_t &mismatches = (iteration & 1) == 0
                ? randomVisibilityMismatches : nearEqualVisibilityMismatches;
            ++mismatches;
            if (!reportedFirstMismatch) {
              std::fprintf(stderr,
                  "first native-depth mismatch offset=%d scene=%u pixel=%zu "
                  "expected=%u actual=%u\n", sampleOffset, iteration, pixel,
                  expected[pixel], actual[pixel]);
              reportedFirstMismatch = true;
            }
          }
          correctedVisibilityMismatches += corrected[pixel] != expected[pixel];
        }
        ++scenes;
      }
    }

    std::vector<Vertex> stressVertices;
    for (uint32_t primitive = 0; primitive < 72; ++primitive) {
      const int32_t inset = int32_t(primitive % 5) * 16;
      const float depthValue = .1f + float(primitive) / 100.f;
      stressVertices.push_back({inset, inset, depthValue});
      stressVertices.push_back(
          {int32_t(StressWidth - 1) * 16 - inset, inset, depthValue});
      stressVertices.push_back(
          {inset, int32_t(StressHeight - 1) * 16 - inset, depthValue});
    }
    id<MTLBuffer> stressVerticesBuffer = [device
        newBufferWithBytes:stressVertices.data()
        length:stressVertices.size() * sizeof(Vertex)
        options:MTLResourceStorageModeShared];
    id<MTLTexture> stressVisibility = makeTexture(MTLPixelFormatR32Uint,
        StressWidth, StressHeight,
        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
        MTLStorageModePrivate);
    id<MTLTexture> stressDepth = makeTexture(MTLPixelFormatDepth32Float,
        StressWidth, StressHeight, MTLTextureUsageRenderTarget,
        MTLStorageModePrivate);
    id<MTLBuffer> stressColors = [device newBufferWithLength:
        StressWidth * StressHeight * sizeof(uint32_t)
        options:MTLResourceStorageModePrivate];
    Constants constants{{float(StressWidth), float(StressHeight)}, 0.f,
                        uint32_t(stressVertices.size() / 3)};
    constexpr uint32_t Iterations = 100;
    const auto start = std::chrono::steady_clock::now();
    for (uint32_t iteration = 0; iteration < Iterations; ++iteration) {
      MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = stressVisibility;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.depthAttachment.texture = stressDepth;
      pass.depthAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.storeAction = MTLStoreActionDontCare;
      pass.depthAttachment.clearDepth = 0.;
      id<MTLCommandBuffer> command = [queue commandBuffer];
      id<MTLRenderCommandEncoder> render =
          [command renderCommandEncoderWithDescriptor:pass];
      [render setRenderPipelineState:renderPipeline];
      [render setDepthStencilState:depthState];
      [render setVertexBuffer:stressVerticesBuffer offset:0 atIndex:0];
      [render setVertexBytes:&constants length:sizeof(constants) atIndex:1];
      [render setCullMode:MTLCullModeNone];
      [render drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0
                  vertexCount:stressVertices.size()];
      [render endEncoding];
      id<MTLComputeCommandEncoder> compute = [command computeCommandEncoder];
      [compute setComputePipelineState:shadePipeline];
      [compute setTexture:stressVisibility atIndex:0];
      [compute setBuffer:stressVerticesBuffer offset:0 atIndex:0];
      [compute setBuffer:stressColors offset:0 atIndex:1];
      [compute setBytes:&constants length:sizeof(constants) atIndex:2];
      const MTLSize group = MTLSizeMake(16, 16, 1);
      [compute dispatchThreads:MTLSizeMake(StressWidth, StressHeight, 1)
             threadsPerThreadgroup:group];
      [compute endEncoding];
      [command commit];
      [command waitUntilCompleted];
    }
    const double milliseconds = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count() / Iterations;
    (void)stressColors;
    std::printf("visibility_scenes=%zu random_visibility_mismatches=%zu "
                "near_equal_visibility_mismatches=%zu "
                "corrected_visibility_mismatches=%zu stress_triangles=72 "
                "mean_submit_gpu_ms=%.3f\n", scenes,
                randomVisibilityMismatches, nearEqualVisibilityMismatches,
                correctedVisibilityMismatches, milliseconds);
    if (randomVisibilityMismatches != 0 || correctedVisibilityMismatches != 0)
      return 1;
  }
  return 0;
}
