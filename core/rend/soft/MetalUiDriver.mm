#include "MetalUiDriver.h"
#include "MetalPresentationFrame.h"
#include "backends/imgui_impl_metal.h"
#import <QuartzCore/CAMetalLayer.h>
#include <deque>
#include <mutex>
#include <algorithm>

namespace rend::soft {
namespace {
struct CompletionQueue {
    std::mutex mutex;
    std::deque<std::pair<std::shared_ptr<const PresentationFrame>, double>> frames;
};

class API_AVAILABLE(macos(10.15.4)) MetalUiDriver final : public ImGuiDriver {
public:
    MetalUiDriver(CAMetalLayer *layer, MetalFrameProvider provider, MetalPresentationObserver observer,
            std::function<bool()> linearFiltering, std::function<bool()> vsync)
        : layer(layer), provider(std::move(provider)), observer(std::move(observer)), linearFiltering(std::move(linearFiltering)), vsync(std::move(vsync))
    {
        if (!layer || !layer.device || !this->provider || ImGui::GetIO().BackendRendererUserData)
            throw FlycastException("Metal UI requires a configured layer and an unowned ImGui backend");
        queue = [layer.device newCommandQueue];
        NSError *error = nil;
        pipeline = createMetalPresentationPipeline(layer.device, layer.pixelFormat, &error);
        if (!queue || !pipeline) throw FlycastException("Metal UI pipeline initialization failed");
        if (!ImGui_ImplMetal_Init(layer.device)) throw FlycastException("Metal ImGui initialization failed");
    }
    ~MetalUiDriver() override
    {
        // Completion blocks own only their queue and packet, never this driver
        // or an emulator renderer. Late callbacks therefore cannot use either.
        [lastCommand waitUntilCompleted];
        ImGui_ImplMetal_Shutdown();
    }
    void newFrame() override
    {
        drain();
        prepare();
        ImGui_ImplMetal_NewFrame(pass);
    }
    void renderDrawData(ImDrawData *data, bool guiOpen) override
    {
        ui = data;
        frameRendered |= guiOpen;
    }
    void setFrameRendered() override { frameRendered = true; }
    void present() override
    {
        drain();
        if (!frameRendered) { ui = nullptr; drawable = nil; pass = nil; return; }
        prepare();
        auto frame = provider();
        id<MTLTexture> image = frame ? uploadMetalPresentationFrame(layer.device, *frame) : nil;
        if (frame && !image) throw FlycastException("Metal framebuffer upload failed");
        const Color border = frame ? frame->border : Color{0, 0, 0, 255};
        pass.colorAttachments[0].clearColor = MTLClearColorMake(border.r / 255., border.g / 255., border.b / 255., 1);
        id<MTLCommandBuffer> command = [queue commandBuffer];
        if (!command || !command.retainedReferences)
            throw FlycastException("Metal UI requires a resource-retaining command buffer");
        id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
        if (!encoder) throw FlycastException("Metal UI encoder creation failed");
        if (frame) {
            const double width = drawable.texture.width, height = drawable.texture.height;
            const double imageWidth = std::min(width, height * frame->aspectRatio);
            const double imageHeight = imageWidth / frame->aspectRatio;
            if (!encodeMetalPresentation(encoder, pipeline, image,
                MTLViewport{(width - imageWidth) / 2, (height - imageHeight) / 2, imageWidth, imageHeight, 0, 1},
                linearFiltering && linearFiltering())) {
                [encoder endEncoding];
                throw FlycastException("Metal framebuffer encoding failed");
            }
        }
        if (ui) ImGui_ImplMetal_RenderDrawData(ui, command, encoder);
        [encoder endEncoding];
        auto completions = completed;
        [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
            std::lock_guard<std::mutex> lock(completions->mutex);
            completions->frames.emplace_back(frame, shown.presentedTime);
        }];
        if (syncEnabled) [command presentDrawable:drawable afterMinimumDuration:1.0 / 60.0];
        else [command presentDrawable:drawable];
        [command commit];
        lastCommand = command;
        // Bound command/texture ownership; do not equate this with display.
        [command waitUntilCompleted];
        if (command.status != MTLCommandBufferStatusCompleted)
            throw FlycastException("Metal UI command failed");
        frameRendered = false;
        ui = nullptr;
        drawable = nil;
        pass = nil;
    }
    ImTextureID getTexture(const std::string& name) override
    {
        const auto it = textures.find(name);
        return it == textures.end() ? ImTextureID{} : (ImTextureID)(uintptr_t)(__bridge void*)it->second;
    }
    ImTextureID updateTexture(const std::string& name, const u8 *data, int width, int height, bool nearest) override
    {
        if (!data || width <= 0 || height <= 0 || width > 16384 || height > 16384)
            throw FlycastException("Invalid Metal UI texture");
        MTLTextureDescriptor *description = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
            width:width height:height mipmapped:NO];
        description.usage = MTLTextureUsageShaderRead;
        description.storageMode = MTLStorageModeShared;
        id<MTLTexture> texture = [layer.device newTextureWithDescriptor:description];
        if (!texture) throw FlycastException("Metal UI texture allocation failed");
        [texture replaceRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0 withBytes:data bytesPerRow:size_t(width) * 4];
        deleteTexture(name);
        textures[name] = texture;
        ImGui_ImplMetal_SetTextureNearest(getTexture(name), nearest);
        return getTexture(name);
    }
    void deleteTexture(const std::string& name) override
    {
        if (auto id = getTexture(name)) ImGui_ImplMetal_SetTextureNearest(id, false);
        textures.erase(name);
    }
private:
    void prepare()
    {
        syncEnabled = !vsync || vsync();
        layer.displaySyncEnabled = syncEnabled;
        if (drawable) return;
        drawable = [layer nextDrawable];
        if (!drawable) throw FlycastException("Metal drawable unavailable");
        pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = drawable.texture;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    }
    void drain()
    {
        std::deque<std::pair<std::shared_ptr<const PresentationFrame>, double>> ready;
        { std::lock_guard<std::mutex> lock(completed->mutex); ready.swap(completed->frames); }
        if (observer) for (auto& result : ready) observer(std::move(result.first), result.second);
    }
    CAMetalLayer *layer;
    id<MTLCommandQueue> queue;
    id<MTLRenderPipelineState> pipeline;
    id<MTLCommandBuffer> lastCommand;
    id<CAMetalDrawable> drawable;
    MTLRenderPassDescriptor *pass;
    MetalFrameProvider provider;
    MetalPresentationObserver observer;
    std::function<bool()> linearFiltering;
    std::function<bool()> vsync;
    bool syncEnabled = true;
    std::shared_ptr<CompletionQueue> completed = std::make_shared<CompletionQueue>();
    std::unordered_map<std::string, id<MTLTexture>> textures;
    ImDrawData *ui = nullptr;
    bool frameRendered = false;
};
}
std::unique_ptr<ImGuiDriver> createMetalUiDriver(void *layer, MetalFrameProvider provider, MetalPresentationObserver observer,
        std::function<bool()> linearFiltering, std::function<bool()> vsync)
{
    if (@available(macOS 10.15.4, *))
        return std::make_unique<MetalUiDriver>((__bridge CAMetalLayer*)layer, std::move(provider), std::move(observer), std::move(linearFiltering), std::move(vsync));
    throw FlycastException("Metal UI requires macOS 10.15.4");
}
}
