#import <Foundation/Foundation.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <OpenGL/OpenGL.h>
#import <OpenGL/gl3.h>

#include <cstdio>
#include <vector>

int main() {
  @autoreleasepool {
    constexpr size_t width = 64;
    constexpr size_t height = 32;
    constexpr size_t bytesPerRow = width * sizeof(uint32_t);
    constexpr size_t byteCount = bytesPerRow * height;
    NSDictionary *properties = @{
      (id)kIOSurfaceWidth : @(width),
      (id)kIOSurfaceHeight : @(height),
      (id)kIOSurfaceBytesPerElement : @4,
      (id)kIOSurfaceBytesPerRow : @(bytesPerRow),
      (id)kIOSurfaceAllocSize : @(byteCount),
      (id)kIOSurfacePixelFormat : @((uint32_t)'BGRA'),
    };
    IOSurfaceRef surface = IOSurfaceCreate((__bridge CFDictionaryRef)properties);
    if (surface == nullptr) {
      std::fprintf(stderr, "IOSurfaceCreate failed\n");
      return 1;
    }
    IOSurfaceLock(surface, 0, nullptr);
    void *base = IOSurfaceGetBaseAddress(surface);
    IOSurfaceUnlock(surface, 0, nullptr);

    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    id<MTLBuffer> buffer = [device newBufferWithBytesNoCopy:base
                                                    length:byteCount
                                                   options:MTLResourceStorageModeShared
                                               deallocator:nil];
    NSError *error = nil;
    id<MTLLibrary> library = [device
        newLibraryWithSource:@"#include <metal_stdlib>\n"
                              "using namespace metal;\n"
                              "kernel void fill(device uint *out [[buffer(0)]], "
                              "uint i [[thread_position_in_grid]]) {\n"
                              "  uint x = i % 64; uint y = i / 64;\n"
                              "  out[i] = 0xff000000u | (y << 16) | (x << 8) | (x ^ y);\n"
                              "}\n"
                     options:nil error:&error];
    id<MTLFunction> function = [library newFunctionWithName:@"fill"];
    id<MTLComputePipelineState> pipeline =
        [device newComputePipelineStateWithFunction:function error:&error];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:buffer offset:0 atIndex:0];
    [encoder dispatchThreads:MTLSizeMake(width * height, 1, 1)
        threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [encoder endEncoding];
    [command commit];
    [command waitUntilCompleted];
    if (command.status != MTLCommandBufferStatusCompleted) {
      std::fprintf(stderr, "Metal command failed: %s\n",
                   command.error.localizedDescription.UTF8String);
      CFRelease(surface);
      return 1;
    }
    IOSurfaceLock(surface, 0, nullptr);
    IOSurfaceUnlock(surface, 0, nullptr);

    CGLPixelFormatAttribute attributes[] = {
      kCGLPFAOpenGLProfile,
      (CGLPixelFormatAttribute)kCGLOGLPVersion_3_2_Core,
      (CGLPixelFormatAttribute)0,
    };
    CGLPixelFormatObj pixelFormat = nullptr;
    GLint pixelFormatCount = 0;
    CGLContextObj context = nullptr;
    if (CGLChoosePixelFormat(attributes, &pixelFormat, &pixelFormatCount) !=
            kCGLNoError ||
        CGLCreateContext(pixelFormat, nullptr, &context) != kCGLNoError ||
        CGLSetCurrentContext(context) != kCGLNoError) {
      std::fprintf(stderr, "OpenGL context creation failed\n");
      if (pixelFormat != nullptr)
        CGLReleasePixelFormat(pixelFormat);
      CFRelease(surface);
      return 1;
    }
    GLuint texture = 0;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_RECTANGLE, texture);
    const CGLError bindError = CGLTexImageIOSurface2D(
        context, GL_TEXTURE_RECTANGLE, GL_RGBA, width, height, GL_BGRA,
        GL_UNSIGNED_INT_8_8_8_8_REV, surface, 0);
    std::vector<uint32_t> pixels(width * height);
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_RECTANGLE, texture, 0);
    if (bindError == kCGLNoError &&
        glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE)
      glReadPixels(0, 0, width, height, GL_BGRA,
                   GL_UNSIGNED_INT_8_8_8_8_REV, pixels.data());
    size_t mismatches = 0;
    for (size_t i = 0; i < pixels.size(); ++i) {
      const uint32_t x = i % width;
      const uint32_t y = i / width;
      const uint32_t expected =
          0xff000000u | (y << 16) | (x << 8) | (x ^ y);
      mismatches += pixels[i] != expected;
    }
    std::printf("iosurface_bind_error=%d mismatches=%zu\n", bindError,
                mismatches);
    glDeleteFramebuffers(1, &framebuffer);
    glDeleteTextures(1, &texture);
    CGLSetCurrentContext(nullptr);
    CGLReleaseContext(context);
    CGLReleasePixelFormat(pixelFormat);
    CFRelease(surface);
    return bindError == kCGLNoError && mismatches == 0 ? 0 : 1;
  }
}
