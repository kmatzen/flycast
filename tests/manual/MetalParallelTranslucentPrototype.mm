// Feasibility test for parallel PVR translucent shading followed by an exact
// per-pixel ordered resolve. It compares a global intermediate buffer with an
// on-chip threadgroup tile.
// clang++ -std=c++20 -O3 -fobjc-arc -framework Foundation -framework Metal \
//   tests/manual/MetalParallelTranslucentPrototype.mm -o /tmp/metal-parallel-tr

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t Width = 640;
constexpr uint32_t Height = 480;
constexpr uint32_t Layers = 14;
constexpr uint32_t TextureSize = 64;
constexpr uint32_t Iterations = 60;

struct Constants {
  uint32_t width;
  uint32_t height;
  uint32_t layers;
  uint32_t textureSize;
};
}

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil)
      return 2;
    static const char *source = R"METAL(
#include <metal_stdlib>
using namespace metal;
struct Constants { uint width; uint height; uint layers; uint textureSize; };

inline uint channel(uint color, uint shift) { return (color >> shift) & 255; }
inline uint pack(float4 value) {
  const uint4 bytes = uint4(clamp(floor(value + .5f), 0.0f, 255.0f));
  return bytes.x | bytes.y << 8 | bytes.z << 16 | bytes.w << 24;
}
inline uint shadeFragment(const device uint *texture, uint2 pixel, uint layer,
                          constant Constants& c) {
  const float inverseW = 1.0f / float(c.width);
  const float inverseH = 1.0f / float(c.height);
  const float q0 = .25f + float(layer) * .017f;
  const float q1 = .91f - float(layer) * .011f;
  const float q2 = .43f + float(layer) * .007f;
  const float sx = (float(pixel.x) + .5f) * inverseW;
  const float sy = (float(pixel.y) + .5f) * inverseH;
  const float s0 = 1.0f - sx;
  const float s1 = sx * (1.0f - sy);
  const float s2 = sx * sy;
  const float q = q0 * s0 + q1 * s1 + q2 * s2;
  const float inverseQ = 1.0f / q;
  float2 uv = float2((float(layer) * .13f * q0 * s0
                    + 3.7f * q1 * s1 + .2f * q2 * s2) * inverseQ,
                     (.1f * q0 * s0 + float(layer) * .09f * q1 * s1
                    + 3.2f * q2 * s2) * inverseQ);
  uv -= floor(uv);
  const float2 texel = uv * float(c.textureSize) - .5f;
  const int2 base = int2(floor(texel));
  const uint mask = c.textureSize - 1;
  const uint2 p00 = uint2(base) & mask;
  const uint2 p10 = uint2(base + int2(1, 0)) & mask;
  const uint2 p01 = uint2(base + int2(0, 1)) & mask;
  const uint2 p11 = uint2(base + 1) & mask;
  const uint samples[4] = {
    texture[p00.y * c.textureSize + p00.x],
    texture[p10.y * c.textureSize + p10.x],
    texture[p01.y * c.textureSize + p01.x],
    texture[p11.y * c.textureSize + p11.x],
  };
  const float2 fraction = texel - floor(texel);
  float4 value;
  for (uint component = 0; component < 4; ++component) {
    const uint shift = component * 8;
    const float top = float(channel(samples[0], shift))
        + (float(channel(samples[1], shift)) - float(channel(samples[0], shift)))
            * fraction.x;
    const float bottom = float(channel(samples[2], shift))
        + (float(channel(samples[3], shift)) - float(channel(samples[2], shift)))
            * fraction.x;
    value[component] = top + (bottom - top) * fraction.y;
  }
  const float alpha = (48.0f + float(layer) * 11.0f) / 255.0f;
  value.xyz *= float3(.55f + .3f * sx, .45f + .4f * sy, .75f - .2f * sx);
  value.w = alpha * 255.0f;
  return pack(value);
}
inline uint alphaBlend(uint source, uint destination) {
  const uint alpha = channel(source, 24);
  const uint inverse = 255 - alpha;
  uint result = 0;
  for (uint shift = 0; shift < 32; shift += 8)
    result |= ((channel(source, shift) * alpha
              + channel(destination, shift) * inverse + 127) / 255) << shift;
  return result;
}

kernel void serialShade(const device uint *texture [[buffer(0)]],
                        device uint *output [[buffer(1)]],
                        constant Constants& c [[buffer(2)]],
                        uint2 pixel [[thread_position_in_grid]]) {
  if (pixel.x >= c.width || pixel.y >= c.height) return;
  uint color = 0xff000000u;
  for (uint layer = 0; layer < c.layers; ++layer)
    color = alphaBlend(shadeFragment(texture, pixel, layer, c), color);
  output[pixel.y * c.width + pixel.x] = color;
}

kernel void parallelShade(const device uint *texture [[buffer(0)]],
                          device uint *fragments [[buffer(1)]],
                          constant Constants& c [[buffer(2)]],
                          uint3 position [[thread_position_in_grid]]) {
  if (position.x >= c.width || position.y >= c.height || position.z >= c.layers)
    return;
  const uint pixel = position.y * c.width + position.x;
  fragments[position.z * c.width * c.height + pixel] =
      shadeFragment(texture, position.xy, position.z, c);
}

kernel void resolve(const device uint *fragments [[buffer(0)]],
                    device uint *output [[buffer(1)]],
                    constant Constants& c [[buffer(2)]],
                    uint2 pixel [[thread_position_in_grid]]) {
  if (pixel.x >= c.width || pixel.y >= c.height) return;
  const uint index = pixel.y * c.width + pixel.x;
  uint color = 0xff000000u;
  for (uint layer = 0; layer < c.layers; ++layer)
    color = alphaBlend(fragments[layer * c.width * c.height + index], color);
  output[index] = color;
}

kernel void threadgroupShade(const device uint *texture [[buffer(0)]],
                             device uint *output [[buffer(1)]],
                             constant Constants& c [[buffer(2)]],
                             uint3 position [[thread_position_in_grid]],
                             uint3 local [[thread_position_in_threadgroup]]) {
  threadgroup uint colors[8 * 8 * 14];
  const uint localPixel = local.y * 8 + local.x;
  colors[local.z * 64 + localPixel] =
      shadeFragment(texture, position.xy, position.z, c);
  threadgroup_barrier(mem_flags::mem_threadgroup);
  if (local.z == 0) {
    uint color = 0xff000000u;
    for (uint layer = 0; layer < 14; ++layer)
      color = alphaBlend(colors[layer * 64 + localPixel], color);
    output[position.y * c.width + position.x] = color;
  }
}
)METAL";
    __block NSError *error = nil;
    MTLCompileOptions *options = [[MTLCompileOptions alloc] init];
    if (@available(macOS 15.0, *))
      options.mathMode = MTLMathModeSafe;
    id<MTLLibrary> library = [device
        newLibraryWithSource:[NSString stringWithUTF8String:source]
                     options:options error:&error];
    if (library == nil) {
      std::fprintf(stderr, "%s\n", error.localizedDescription.UTF8String);
      return 2;
    }
    auto pipeline = ^id<MTLComputePipelineState>(NSString *name) {
      return [device newComputePipelineStateWithFunction:
          [library newFunctionWithName:name] error:&error];
    };
    id<MTLComputePipelineState> serial = pipeline(@"serialShade");
    id<MTLComputePipelineState> parallel = pipeline(@"parallelShade");
    id<MTLComputePipelineState> resolve = pipeline(@"resolve");
    id<MTLComputePipelineState> threadgroup = pipeline(@"threadgroupShade");
    if (serial == nil || parallel == nil || resolve == nil || threadgroup == nil)
      return 2;
    std::vector<uint32_t> texture(TextureSize * TextureSize);
    uint32_t state = 0x505652u;
    for (uint32_t &texel : texture) {
      state = state * 1664525u + 1013904223u;
      texel = 0x80000000u | (state & 0x00ffffffu);
    }
    id<MTLBuffer> textureBuffer = [device newBufferWithBytes:texture.data()
        length:texture.size() * sizeof(uint32_t)
        options:MTLResourceStorageModeShared];
    const size_t pixels = size_t(Width) * Height;
    id<MTLBuffer> serialOutput = [device newBufferWithLength:pixels * 4
        options:MTLResourceStorageModeShared];
    id<MTLBuffer> parallelOutput = [device newBufferWithLength:pixels * 4
        options:MTLResourceStorageModeShared];
    id<MTLBuffer> threadgroupOutput = [device newBufferWithLength:pixels * 4
        options:MTLResourceStorageModeShared];
    id<MTLBuffer> fragments = [device newBufferWithLength:pixels * Layers * 4
        options:MTLResourceStorageModePrivate];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    const Constants constants{Width, Height, Layers, TextureSize};
    const MTLSize group2D = MTLSizeMake(8, 8, 1);
    auto run = [&](int mode) {
      const auto start = std::chrono::steady_clock::now();
      for (uint32_t iteration = 0; iteration < Iterations; ++iteration) {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        if (mode == 1) {
          id<MTLComputeCommandEncoder> shade = [command computeCommandEncoder];
          [shade setComputePipelineState:parallel];
          [shade setBuffer:textureBuffer offset:0 atIndex:0];
          [shade setBuffer:fragments offset:0 atIndex:1];
          [shade setBytes:&constants length:sizeof(constants) atIndex:2];
          [shade dispatchThreads:MTLSizeMake(Width, Height, Layers)
                 threadsPerThreadgroup:group2D];
          [shade endEncoding];
          id<MTLComputeCommandEncoder> blend = [command computeCommandEncoder];
          [blend setComputePipelineState:resolve];
          [blend setBuffer:fragments offset:0 atIndex:0];
          [blend setBuffer:parallelOutput offset:0 atIndex:1];
          [blend setBytes:&constants length:sizeof(constants) atIndex:2];
          [blend dispatchThreads:MTLSizeMake(Width, Height, 1)
                 threadsPerThreadgroup:group2D];
          [blend endEncoding];
        } else if (mode == 2) {
          id<MTLComputeCommandEncoder> shade = [command computeCommandEncoder];
          [shade setComputePipelineState:threadgroup];
          [shade setBuffer:textureBuffer offset:0 atIndex:0];
          [shade setBuffer:threadgroupOutput offset:0 atIndex:1];
          [shade setBytes:&constants length:sizeof(constants) atIndex:2];
          [shade dispatchThreads:MTLSizeMake(Width, Height, Layers)
                 threadsPerThreadgroup:MTLSizeMake(8, 8, Layers)];
          [shade endEncoding];
        } else {
          id<MTLComputeCommandEncoder> shade = [command computeCommandEncoder];
          [shade setComputePipelineState:serial];
          [shade setBuffer:textureBuffer offset:0 atIndex:0];
          [shade setBuffer:serialOutput offset:0 atIndex:1];
          [shade setBytes:&constants length:sizeof(constants) atIndex:2];
          [shade dispatchThreads:MTLSizeMake(Width, Height, 1)
                 threadsPerThreadgroup:group2D];
          [shade endEncoding];
        }
        [command commit];
        [command waitUntilCompleted];
      }
      return std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start).count() / Iterations;
    };
    const double serialMilliseconds = run(0);
    const double parallelMilliseconds = run(1);
    const double threadgroupMilliseconds = run(2);
    size_t parallelMismatches = 0;
    size_t threadgroupMismatches = 0;
    const uint32_t *a = static_cast<const uint32_t *>(serialOutput.contents);
    const uint32_t *b = static_cast<const uint32_t *>(parallelOutput.contents);
    const uint32_t *c = static_cast<const uint32_t *>(threadgroupOutput.contents);
    for (size_t pixel = 0; pixel < pixels; ++pixel) {
      parallelMismatches += a[pixel] != b[pixel];
      threadgroupMismatches += a[pixel] != c[pixel];
    }
    std::printf("pixels=%zu layers=%u serial_ms=%.3f global_ms=%.3f "
                "tile_ms=%.3f tile_speedup=%.2fx global_mismatches=%zu "
                "tile_mismatches=%zu\n", pixels, Layers, serialMilliseconds,
                parallelMilliseconds, threadgroupMilliseconds,
                serialMilliseconds / threadgroupMilliseconds,
                parallelMismatches, threadgroupMismatches);
    return parallelMismatches == 0 && threadgroupMismatches == 0 ? 0 : 1;
  }
}
