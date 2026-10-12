#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "rend/soft/MetalPresentationFrame.h"
#include <atomic>
#include <cstdio>
#include <memory>
#include <cstring>

// Manual host-window probe, not a Dreamcast conformance or gameplay benchmark.
static int runProbe(bool paced, bool acknowledged) API_AVAILABLE(macos(10.15.4));
static int runProbe(bool paced, bool acknowledged)
{
    @autoreleasepool {
        SDL_SetMainReady();
        std::fprintf(stderr, "METAL_WINDOW_BEGIN mode=%s frames=120\n", acknowledged ? "acknowledged" : paced ? "minimum-duration" : "immediate");
        if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
        SDL_Window *window = SDL_CreateWindow("Flycast native Metal presentation probe",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
            SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
        if (!window) { SDL_Quit(); return 2; }
        SDL_MetalView view = SDL_Metal_CreateView(window);
        if (!view) { SDL_DestroyWindow(window); SDL_Quit(); return 3; }
        CAMetalLayer *layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        layer.device = device;
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.displaySyncEnabled = YES;
        layer.maximumDrawableCount = 3;
        id<MTLCommandQueue> queue = [device newCommandQueue];
        NSError *error = nil;
        NSString *source = @"#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vert(uint i [[vertex_id]]) {\n"
            "float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
            "V v; v.p=float4(p[i],0,1); v.uv=float2((p[i].x+1)*.5,(1-p[i].y)*.5); return v; }\n"
            "fragment float4 frag(V v [[stage_in]], texture2d<float> t [[texture(0)]]) {\n"
            "constexpr sampler s(coord::normalized, address::clamp_to_edge, filter::nearest);\n"
            "return t.sample(s,v.uv); }";
        id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
        MTLRenderPipelineDescriptor *descriptor = [MTLRenderPipelineDescriptor new];
        descriptor.vertexFunction = [library newFunctionWithName:@"vert"];
        descriptor.fragmentFunction = [library newFunctionWithName:@"frag"];
        descriptor.colorAttachments[0].pixelFormat = layer.pixelFormat;
        id<MTLRenderPipelineState> pipeline = library ? [device newRenderPipelineStateWithDescriptor:descriptor error:&error] : nil;
        if (!pipeline || !queue) {
            std::fprintf(stderr, "METAL_WINDOW init_failed %s\n", error.description.UTF8String);
            SDL_Metal_DestroyView(view); SDL_DestroyWindow(window); SDL_Quit(); return 4;
        }
        auto presented = std::make_shared<std::atomic<unsigned>>(0);
        auto skipped = std::make_shared<std::atomic<unsigned>>(0);
        bool failed = false;
        unsigned submitted = 0;
        unsigned retries = 0;
        for (unsigned frameId = 0; frameId < 120;) {
            @autoreleasepool {
                SDL_Event event;
                while (SDL_PollEvent(&event)) if (event.type == SDL_QUIT) failed = true;
                if (failed) break;
                int width, height;
                SDL_Metal_GetDrawableSize(window, &width, &height);
                if (width <= 0 || height <= 0) { failed = true; break; }
                layer.drawableSize = CGSizeMake(width, height);
                auto frame = rend::soft::PresentationFrame::create(640, 480, 4.f / 3.f, {},
                    std::vector<rend::soft::Color>(640 * 480, {u8((frameId & 31) << 3), u8((frameId >> 5) << 2), 128, 255}));
                id<MTLTexture> texture = rend::soft::uploadMetalPresentationFrame(device, *frame);
                id<CAMetalDrawable> drawable = [layer nextDrawable];
                if (!texture || !drawable) { failed = true; break; }
                auto ack = std::make_shared<std::atomic<int>>(0);
                [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
                    std::fprintf(stderr, "METAL_WINDOW_PRESENT id=%u time=%.9f\n", frameId, shown.presentedTime);
                    if (shown.presentedTime <= 0) skipped->fetch_add(1);
                    presented->fetch_add(1);
                    ack->store(shown.presentedTime > 0 ? 1 : -1);
                }];
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = drawable.texture;
                pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLCommandBuffer> command = [queue commandBuffer];
                id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
                [encoder setRenderPipelineState:pipeline];
                [encoder setFragmentTexture:texture atIndex:0];
                [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
                [encoder endEncoding];
                if (paced)
                    [command presentDrawable:drawable afterMinimumDuration:1.0 / 60.0];
                else
                    [command presentDrawable:drawable];
                [command commit];
                [command waitUntilCompleted];
                if (command.status != MTLCommandBufferStatusCompleted) { failed = true; break; }
                ++submitted;
                if (acknowledged) {
                    for (unsigned wait = 0; wait < 200 && ack->load() == 0; ++wait) {
                        SDL_PumpEvents();
                        SDL_Delay(10);
                    }
                    if (ack->load() == 0) { failed = true; break; }
                    if (ack->load() < 0) {
                        if (++retries > 8) { failed = true; break; }
                        continue; // Preserve this image ID until actually presented.
                    }
                }
                ++frameId;
            }
        }
        for (unsigned i = 0; i < 200 && presented->load() < submitted; ++i) {
            SDL_PumpEvents();
            SDL_Delay(10);
        }
        std::fprintf(stderr, "METAL_WINDOW_END submitted=%u callbacks=%u skipped=%u failed=%u\n",
            submitted, presented->load(), skipped->load(), failed);
        const bool complete = !failed && presented->load() == submitted
            && (acknowledged ? submitted - skipped->load() == 120 : submitted == 120 && skipped->load() == 0);
        SDL_Metal_DestroyView(view);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return complete ? 0 : 5;
    }
}

int main(int argc, char **argv)
{
    if (argc > 2 || (argc == 2 && std::strcmp(argv[1], "--paced") != 0 && std::strcmp(argv[1], "--acknowledged") != 0)) {
        std::fprintf(stderr, "Usage: flycast-metal-window-probe [--paced|--acknowledged]\n");
        return 7;
    }
    if (@available(macOS 10.15.4, *)) return runProbe(argc == 2, argc == 2 && std::strcmp(argv[1], "--acknowledged") == 0);
    std::fprintf(stderr, "METAL_WINDOW requires macOS 10.15.4 presentation callbacks\n");
    return 6;
}
