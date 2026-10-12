#define SDL_MAIN_HANDLED
#include <SDL.h>
#include <SDL_metal.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include "rend/soft/MetalUiDriver.h"
#include <cstdio>
#include <cstring>
#include <thread>
#include <array>

// Standalone frontend: use ImGui's default style instead of emulator settings.
void gui_updateStyle() {}

int main(int argc, char **argv)
{
    bool resize = true;
    if (argc == 2 && std::strcmp(argv[1], "--no-resize") == 0) resize = false;
    else if (argc != 1) return 7;
    constexpr unsigned frameCount = 660;
    std::fprintf(stderr, "METAL_DRIVER_BEGIN frames=%u resize=%u\n", frameCount, resize);
    @autoreleasepool {
        SDL_SetMainReady();
        if (SDL_Init(SDL_INIT_VIDEO) != 0) return 1;
        SDL_Window *window = SDL_CreateWindow("Flycast Metal UI driver probe",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 640, 480,
            SDL_WINDOW_METAL | SDL_WINDOW_ALLOW_HIGHDPI | SDL_WINDOW_RESIZABLE);
        if (!window) { SDL_Quit(); return 2; }
        SDL_MetalView view = SDL_Metal_CreateView(window);
        if (!view) { SDL_DestroyWindow(window); SDL_Quit(); return 3; }
        CAMetalLayer *layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
        layer.device = MTLCreateSystemDefaultDevice();
        layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        layer.displaySyncEnabled = YES;
        layer.maximumDrawableCount = 3;
        bool failed = false;
        unsigned callbacks = 0, skipped = 0;
        unsigned reordered = 0;
        u64 previousSequence = 0;
        std::array<bool, frameCount> seen {};
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui::GetIO().IniFilename = nullptr;
        const auto uiThread = std::this_thread::get_id();
        std::shared_ptr<const rend::soft::PresentationFrame> selected;
        bool sync = true;
        try {
            auto driver = rend::soft::createMetalUiDriver((__bridge void*)layer, [&] { return selected; },
                [&](std::shared_ptr<const rend::soft::PresentationFrame> frame, double time) {
                    if (std::this_thread::get_id() != uiThread || !frame
                            || frame->sequence == 0 || frame->sequence > frameCount
                            || seen[frame->sequence - 1])
                        failed = true;
                    else {
                        seen[frame->sequence - 1] = true;
                        if (frame->sequence < previousSequence) ++reordered;
                        previousSequence = frame->sequence;
                        if (frame->width != 640 || frame->height != 480
                                || !(frame->pixels.front() == rend::soft::Color{u8(frame->sequence - 1), 32, 128, 0}))
                            failed = true;
                    }
                    ++callbacks;
                    if (time <= 0) ++skipped;
                    std::fprintf(stderr, "METAL_DRIVER_CALLBACK sequence=%llu time=%.9f\n",
                        frame ? (unsigned long long)frame->sequence : 0, time);
                }, {}, [&] { return sync; });
            const u8 icon[] = {0, 255, 0, 255};
            auto iconId = driver->updateTexture("probe", icon, 1, 1, true);
            if (!iconId || driver->getTexture("probe") != iconId) failed = true;
            for (unsigned frameId = 0; frameId < frameCount && !failed; ++frameId) {
                sync = frameId < 220 || frameId >= 440;
                SDL_Event event;
                while (SDL_PollEvent(&event)) if (event.type == SDL_QUIT) failed = true;
                if (resize && frameId == 60) SDL_SetWindowSize(window, 800, 600);
                int width, height, pointsW, pointsH;
                SDL_Metal_GetDrawableSize(window, &width, &height);
                SDL_GetWindowSize(window, &pointsW, &pointsH);
                if (width <= 0 || height <= 0 || pointsW <= 0 || pointsH <= 0) { failed = true; break; }
                layer.drawableSize = CGSizeMake(width, height);
                ImGui::GetIO().DisplaySize = ImVec2(pointsW, pointsH);
                ImGui::GetIO().DisplayFramebufferScale = ImVec2(float(width) / pointsW, float(height) / pointsH);
                ImGui::GetIO().DeltaTime = 1.f / 60;
                selected = rend::soft::PresentationFrame::create(640, 480, 4.f / 3.f, {16, 16, 16, 255},
                    std::vector<rend::soft::Color>(640 * 480, {u8(frameId), 32, 128, 0}), frameId + 1);
                driver->newFrame();
                if (bool(layer.displaySyncEnabled) != sync) failed = true;
                ImGui::NewFrame();
                ImGui::Begin("Native Metal UI");
                ImGui::Text("Frame %u / %u", frameId + 1, frameCount);
                ImGui::Text("Resize checkpoint: %s", resize && frameId >= 60 ? "800 x 600" : "640 x 480");
                ImGui::Image(ImTextureRef(iconId), ImVec2(24, 24));
                ImGui::End();
                ImGui::Render();
                driver->renderDrawData(ImGui::GetDrawData(), true);
                driver->setFrameRendered();
                driver->present();
            }
            // Drain observer callbacks on the UI thread without submitting new images.
            for (unsigned wait = 0; wait < 200 && callbacks < frameCount && !failed; ++wait) {
                SDL_PumpEvents(); SDL_Delay(10); driver->newFrame();
            }
            driver->deleteTexture("probe");
            if (driver->getTexture("probe")) failed = true;
            driver.reset(); // Must precede ImGui context and SDL view destruction.
        } catch (const std::exception& error) {
            std::fprintf(stderr, "METAL_DRIVER_ERROR %s\n", error.what());
            failed = true;
        }
        ImGui::DestroyContext();
        SDL_Metal_DestroyView(view);
        SDL_DestroyWindow(window);
        SDL_Quit();
        std::fprintf(stderr, "METAL_DRIVER_END callbacks=%u skipped=%u failed=%u reordered=%u\n", callbacks, skipped, failed, reordered);
        // Lifecycle success is deliberately distinct from lossless delivery.
        return !failed && callbacks == frameCount ? 0 : 4;
    }
}
