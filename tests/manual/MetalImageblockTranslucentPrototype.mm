// Feasibility test for exact ordered translucent blending in Apple tile memory.
// clang++ -std=c++20 -O3 -fobjc-arc -framework Foundation -framework Metal \
//   tests/manual/MetalImageblockTranslucentPrototype.mm -o /tmp/metal-imageblock-tr

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
constexpr uint32_t Iterations = 120;

struct Constants {
  uint32_t width;
  uint32_t height;
};
}

int main() {
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (device == nil || ![device supportsFamily:MTLGPUFamilyApple4]) {
      std::fprintf(stderr, "Apple4 imageblocks are unavailable\n");
      return 2;
    }

    static const char *source = R"METAL(
#include <metal_stdlib>
using namespace metal;
constant ushort LayerCount = 14;
struct Constants { uint width; uint height; };

inline uint channel(uint color, uint shift) { return (color >> shift) & 255u; }
inline uint fragmentColor(uint2 pixel, uint layer) {
  uint v = (pixel.x * 0x45d9f3bu) ^ (pixel.y * 0x119de1f3u)
         ^ ((layer + 1u) * 0x27d4eb2du);
  v ^= v >> 16;
  const uint alpha = 48u + layer * 11u;
  return (v & 0x00ffffffu) | (alpha << 24);
}
inline uint fragmentDepth(uint2 pixel, uint layer) {
  // Force crossings and ties so that submission order isn't already sorted.
  return ((layer * 9u + pixel.x / 37u + pixel.y / 29u) % LayerCount) * 4u
       + ((layer + pixel.x + pixel.y) & 3u);
}
inline bool precedes(uint depthA, uint sequenceA, uint depthB, uint sequenceB) {
  return depthA < depthB || (depthA == depthB && sequenceA < sequenceB);
}
inline uint alphaBlend(uint source, uint destination) {
  const uint alpha = channel(source, 24);
  const uint inverse = 255u - alpha;
  uint result = 0;
  for (uint shift = 0; shift < 32; shift += 8)
    result |= ((channel(source, shift) * alpha
              + channel(destination, shift) * inverse + 127u) / 255u) << shift;
  return result;
}

kernel void serialResolve(device uint *output [[buffer(0)]],
                          constant Constants& c [[buffer(1)]],
                          uint2 pixel [[thread_position_in_grid]]) {
  if (pixel.x >= c.width || pixel.y >= c.height) return;
  uint colors[LayerCount];
  uint depths[LayerCount];
  uint sequences[LayerCount];
  for (uint i = 0; i < LayerCount; ++i) {
    colors[i] = 0u;
    depths[i] = 0xffffffffu;
    sequences[i] = 0xffffffffu;
  }
  for (uint layer = 0; layer < LayerCount; ++layer) {
    uint color = fragmentColor(pixel, layer);
    uint depth = fragmentDepth(pixel, layer);
    uint sequence = layer;
    for (uint i = 0; i < LayerCount; ++i) {
      if (precedes(depth, sequence, depths[i], sequences[i])) {
        uint displacedColor = colors[i]; colors[i] = color; color = displacedColor;
        uint displacedDepth = depths[i]; depths[i] = depth; depth = displacedDepth;
        uint displacedSequence = sequences[i]; sequences[i] = sequence;
        sequence = displacedSequence;
      }
    }
  }
  uint result = 0xff000000u;
  for (int i = int(LayerCount) - 1; i >= 0; --i)
    result = alphaBlend(colors[i], result);
  output[pixel.y * c.width + pixel.x] = result;
}

struct FragmentValues {
  ushort primitiveIDs [[raster_order_group(0)]] [LayerCount];
};
struct FragmentStore { FragmentValues values [[imageblock_data]]; };

kernel void initializeTile(
    imageblock<FragmentValues, imageblock_layout_explicit> block,
    ushort2 localThreadID [[thread_position_in_threadgroup]]) {
  threadgroup_imageblock FragmentValues *values = block.data(localThreadID);
  for (uint i = 0; i < LayerCount; ++i)
    values->primitiveIDs[i] = 0xffffu;
}

struct RasterOut {
  float4 position [[position]];
  uint layer [[flat]];
};
vertex RasterOut rasterVertex(uint vertexID [[vertex_id]], uint layer [[instance_id]]) {
  const float2 positions[3] = { float2(-1.0f, -1.0f), float2(3.0f, -1.0f),
                                float2(-1.0f, 3.0f) };
  RasterOut out;
  out.position = float4(positions[vertexID], 0.0f, 1.0f);
  out.layer = layer;
  return out;
}

fragment FragmentStore insertFragment(RasterOut in [[stage_in]],
                                      FragmentValues values [[imageblock_data]]) {
  FragmentStore out;
  const uint2 pixel = uint2(in.position.xy);
  uint depth = fragmentDepth(pixel, in.layer);
  uint sequence = in.layer;
  for (uint i = 0; i < LayerCount; ++i) {
    const uint oldSequence = values.primitiveIDs[i];
    const uint oldDepth = oldSequence == 0xffffu
                            ? 0xffffffffu
                            : fragmentDepth(pixel, oldSequence);
    const bool insert = precedes(depth, sequence, oldDepth, oldSequence);
    values.primitiveIDs[i] = ushort(insert ? sequence : oldSequence);
    depth = insert ? oldDepth : depth;
    sequence = insert ? oldSequence : sequence;
  }
  out.values = values;
  return out;
}

struct ResolveOut { uint4 color [[color(0)]]; };
fragment ResolveOut resolveFragments(RasterOut in [[stage_in]],
                                     FragmentValues values [[imageblock_data]]) {
  uint result = 0xff000000u;
  const uint2 pixel = uint2(in.position.xy);
  for (int i = int(LayerCount) - 1; i >= 0; --i) {
    const uint primitiveID = values.primitiveIDs[i];
    result = alphaBlend(fragmentColor(pixel, primitiveID), result);
  }
  ResolveOut out;
  out.color = uint4(channel(result, 0), channel(result, 8),
                    channel(result, 16), channel(result, 24));
  return out;
}
)METAL";

    NSError *error = nil;
    MTLCompileOptions *options = [MTLCompileOptions new];
    if (@available(macOS 15.0, *))
      options.mathMode = MTLMathModeSafe;
    id<MTLLibrary> library = [device
        newLibraryWithSource:[NSString stringWithUTF8String:source]
                     options:options error:&error];
    if (library == nil) {
      std::fprintf(stderr, "shader: %s\n", error.localizedDescription.UTF8String);
      return 2;
    }

    id<MTLComputePipelineState> serial = [device
        newComputePipelineStateWithFunction:[library newFunctionWithName:@"serialResolve"]
        error:&error];
    if (serial == nil) {
      std::fprintf(stderr, "serial pipeline: %s\n", error.localizedDescription.UTF8String);
      return 2;
    }

    auto renderPipeline = [&](NSString *fragmentName, bool writesColor) {
      MTLRenderPipelineDescriptor *desc = [MTLRenderPipelineDescriptor new];
      desc.vertexFunction = [library newFunctionWithName:@"rasterVertex"];
      desc.fragmentFunction = [library newFunctionWithName:fragmentName];
      desc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Uint;
      desc.colorAttachments[0].writeMask = writesColor ? MTLColorWriteMaskAll
                                                       : MTLColorWriteMaskNone;
      return [device newRenderPipelineStateWithDescriptor:desc error:&error];
    };
    id<MTLRenderPipelineState> insert = renderPipeline(@"insertFragment", false);
    id<MTLRenderPipelineState> resolve = renderPipeline(@"resolveFragments", true);
    if (insert == nil || resolve == nil) {
      std::fprintf(stderr, "render pipeline: %s\n", error.localizedDescription.UTF8String);
      return 2;
    }

    MTLTileRenderPipelineDescriptor *tileDesc = [MTLTileRenderPipelineDescriptor new];
    tileDesc.tileFunction = [library newFunctionWithName:@"initializeTile"];
    tileDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Uint;
    tileDesc.threadgroupSizeMatchesTileSize = YES;
    id<MTLRenderPipelineState> initialize = [device
        newRenderPipelineStateWithTileDescriptor:tileDesc options:0 reflection:nil error:&error];
    if (initialize == nil) {
      std::fprintf(stderr, "tile pipeline: %s\n", error.localizedDescription.UTF8String);
      return 2;
    }

    const size_t pixels = size_t(Width) * Height;
    id<MTLBuffer> serialOutput = [device newBufferWithLength:pixels * 4
        options:MTLResourceStorageModeShared];
    MTLTextureDescriptor *textureDesc = [MTLTextureDescriptor
        texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Uint
        width:Width height:Height mipmapped:NO];
    textureDesc.storageMode = MTLStorageModeShared;
    textureDesc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> imageblockOutput = [device newTextureWithDescriptor:textureDesc];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    const Constants constants{Width, Height};
    const MTLSize group = MTLSizeMake(8, 8, 1);

    auto runSerial = [&] {
      const auto start = std::chrono::steady_clock::now();
      for (uint32_t iteration = 0; iteration < Iterations; ++iteration) {
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
        [encoder setComputePipelineState:serial];
        [encoder setBuffer:serialOutput offset:0 atIndex:0];
        [encoder setBytes:&constants length:sizeof(constants) atIndex:1];
        [encoder dispatchThreads:MTLSizeMake(Width, Height, 1)
             threadsPerThreadgroup:group];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
      }
      return std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start).count() / Iterations;
    };

    auto runImageblock = [&] {
      const auto start = std::chrono::steady_clock::now();
      for (uint32_t iteration = 0; iteration < Iterations; ++iteration) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = imageblockOutput;
        pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.tileWidth = 32;
        pass.tileHeight = 16;
        pass.imageblockSampleLength = insert.imageblockSampleLength;
        id<MTLCommandBuffer> command = [queue commandBuffer];
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        [encoder setRenderPipelineState:initialize];
        [encoder dispatchThreadsPerTile:MTLSizeMake(32, 16, 1)];
        [encoder setRenderPipelineState:insert];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3
                     instanceCount:Layers];
        [encoder setRenderPipelineState:resolve];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [encoder endEncoding];
        [command commit];
        [command waitUntilCompleted];
        if (command.status == MTLCommandBufferStatusError) {
          std::fprintf(stderr, "command: %s\n", command.error.localizedDescription.UTF8String);
          return -1.0;
        }
      }
      return std::chrono::duration<double, std::milli>(
          std::chrono::steady_clock::now() - start).count() / Iterations;
    };

    const double serialMs = runSerial();
    const double imageblockMs = runImageblock();
    std::vector<uint32_t> imageblockPixels(pixels);
    [imageblockOutput getBytes:imageblockPixels.data() bytesPerRow:Width * 4
                    fromRegion:MTLRegionMake2D(0, 0, Width, Height)
                   mipmapLevel:0];
    const uint32_t *reference = static_cast<const uint32_t *>(serialOutput.contents);
    size_t mismatches = 0;
    for (size_t i = 0; i < pixels; ++i)
      mismatches += reference[i] != imageblockPixels[i];
    std::printf("device=%s pixels=%zu layers=%u imageblock_bytes=%lu "
                "serial_ms=%.3f imageblock_ms=%.3f speedup=%.2fx mismatches=%zu\n",
                device.name.UTF8String, pixels, Layers,
                static_cast<unsigned long>(insert.imageblockSampleLength),
                serialMs, imageblockMs, serialMs / imageblockMs, mismatches);
    return imageblockMs >= 0.0 && mismatches == 0 ? 0 : 1;
  }
}
