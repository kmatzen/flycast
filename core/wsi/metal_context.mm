#include "context.h"
#include "metal_context.h"
#include "sdl/sdl.h"
#include "cfg/option.h"
#include "rend/soft/MetalUiDriver.h"
#include "rend/soft/SoftwareRenderer.h"
#include "rend/soft/CadenceProbe.h"
#include <SDL_metal.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#include <cstdio>
#include <cstdlib>

extern Renderer *renderer;
namespace {
bool active;
class NativeMetalContext final : public GraphicsContext {
public:
    NativeMetalContext(void *window, void *display) : GraphicsContext(window, display)
    {
        if (!config::ThreadedRendering || !config::EmulateFramebuffer
                || !std::getenv("FLYCAST_PVR_DECOUPLED_PRESENT")
                || config::Rotate90)
            throw FlycastException("Experimental Metal presentation requires threaded EFB, decoupled presentation, and no rotation");
        try {
            if (!sdl_recreate_window(SDL_WINDOW_METAL))
                throw FlycastException("Cannot create Metal window");
            view = SDL_Metal_CreateView((SDL_Window*)this->window);
            if (!view) throw FlycastException("Cannot create Metal view");
            layer = (__bridge CAMetalLayer*)SDL_Metal_GetLayer(view);
            layer.device = MTLCreateSystemDefaultDevice();
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.displaySyncEnabled = YES;
            layer.maximumDrawableCount = 3;
            resize();
            imguiDriver.reset();
            imguiDriver = rend::soft::createMetalUiDriver((__bridge void*)layer,
                [] { return rend::soft::selectedPresentation(renderer); },
                [](auto frame, double time) {
                    // No renderer pointer is captured: callbacks can outlive its epoch.
                    if (frame) {
                        const int id = frame->width == 640 && frame->height == 480
                            ? rend::soft::decodeCadenceProbeId(frame->pixels[240 * 640 + 320]) : -1;
                        std::fprintf(stderr, "PVR_METAL_PRESENT sequence=%llu id=%d time=%.9f\n",
                            (unsigned long long)frame->sequence, id, time);
                    }
                }, [] { return bool(config::LinearInterpolation); });
            active = true;
            std::fprintf(stderr, "PVR_METAL_CONTEXT active=1\n");
        } catch (...) {
            if (view) SDL_Metal_DestroyView(view);
            throw;
        }
    }
    ~NativeMetalContext() override
    {
        imguiDriver.reset();
        active = false;
        SDL_Metal_DestroyView(view);
    }
    void resize() override
    {
        sdl_update_display_metrics((SDL_Window*)window, SDL_WINDOW_METAL);
        layer.drawableSize = CGSizeMake(settings.display.width, settings.display.height);
    }
    std::string getDriverName() override { return "Experimental native Metal"; }
    std::string getDriverVersion() override { return "prototype"; }
    bool isAMD() override { return false; }
private:
    SDL_MetalView view = nullptr;
    CAMetalLayer *layer;
};
}
bool nativeMetalPresentationActive() { return active; }
void createNativeMetalContext(void *window, void *display)
{
    new NativeMetalContext(window, display); // GraphicsContext::Term owns destruction.
}
