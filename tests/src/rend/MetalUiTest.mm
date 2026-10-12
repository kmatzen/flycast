#import <Metal/Metal.h>
#include "imgui.h"
#include "backends/imgui_impl_metal.h"
#include <gtest/gtest.h>
#include <vector>
#include "rend/soft/MetalPresentationFrame.h"

// This stub implements only the allocation message used by the uploader, to exercise
// allocation failure without exhausting GPU/system memory.
@interface PvrFailingTextureDevice : NSObject
@property(nonatomic) unsigned attempts;
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)descriptor;
@end
@implementation PvrFailingTextureDevice
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor*)descriptor
{
    self.attempts += 1;
    return nil;
}
@end

TEST(MetalUi, AllocationFailureReturnsNoTexture)
{
    @autoreleasepool {
        PvrFailingTextureDevice *device = [PvrFailingTextureDevice new];
        auto frame = rend::soft::PresentationFrame::create(1, 1, 1.f, {},
            std::vector<rend::soft::Color>(1));
        ASSERT_TRUE(frame);
        EXPECT_EQ(rend::soft::uploadMetalPresentationFrame((id<MTLDevice>)device, *frame), nil);
        EXPECT_EQ(device.attempts, 1u);
    }
}

TEST(MetalUi, RejectsOversizedUploadsBeforeCallingMetal)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        ASSERT_NE(device, nil);
        for (bool tall : {false, true}) {
            auto frame = rend::soft::PresentationFrame::create(tall ? 1 : 16385,
                tall ? 16385 : 1, 1.f, {}, std::vector<rend::soft::Color>(16385));
            ASSERT_TRUE(frame);
            EXPECT_EQ(rend::soft::uploadMetalPresentationFrame(device, *frame), nil);
        }
    }
}

TEST(MetalUi, ConvertedFramebufferUploadPreservesBytesAndOwnership)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        ASSERT_NE(device, nil);
        using namespace rend::soft;
        // Odd widths exercise row pitch; all channels differ to catch RGBA/BGRA
        // confusion, row reversal, and accidental color-space conversion.
        for (u32 width : {3u, 17u, 640u}) {
            const u32 height = 7;
            std::vector<Color> source(width * height);
            for (size_t i = 0; i < source.size(); ++i)
                source[i] = {u8(i), u8(i >> 3), u8(255 - i), u8(i * 7)};
            auto frame = PresentationFrame::create(width, height, float(width) / height, {}, source);
            ASSERT_TRUE(frame);
            EXPECT_EQ(uploadMetalPresentationFrame(nil, *frame), nil);
            id<MTLTexture> first = uploadMetalPresentationFrame(device, *frame);
            ASSERT_NE(first, nil);
            auto replacement = PresentationFrame::create(width, height, 1.f, {},
                std::vector<Color>(source.size(), {0, 0, 0, 255}));
            id<MTLTexture> second = uploadMetalPresentationFrame(device, *replacement);
            ASSERT_NE(second, nil);
            EXPECT_NE(first, second);
            frame.reset();
            replacement.reset();
            std::vector<Color> readback(source.size());
            [first getBytes:readback.data() bytesPerRow:width * sizeof(Color)
                fromRegion:MTLRegionMake2D(0, 0, width, height) mipmapLevel:0];
            EXPECT_EQ(readback, source);
        }
    }
}

// Host UI composition only: this does not validate Dreamcast pixel rules,
// drawable scheduling, physical scanout, or the emulator window lifecycle.
TEST(MetalUi, OffscreenCompositionSurvivesResizeAndBackendRecreation)
{
    @autoreleasepool {
        id<MTLDevice> device = MTLCreateSystemDefaultDevice();
        ASSERT_NE(device, nil);
        id<MTLCommandQueue> queue = [device newCommandQueue];
        ASSERT_NE(queue, nil);
        for (int generation = 0; generation < 2; ++generation) {
            IMGUI_CHECKVERSION();
            ImGui::CreateContext();
            ImGui::GetIO().IniFilename = nullptr;
            ASSERT_TRUE(ImGui_ImplMetal_Init(device));
            for (int size : {64, 128}) {
                MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
                    texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                    width:size height:size mipmapped:NO];
                descriptor.usage = MTLTextureUsageRenderTarget;
                descriptor.storageMode = MTLStorageModeShared;
                id<MTLTexture> target = [device newTextureWithDescriptor:descriptor];
                ASSERT_NE(target, nil);
                MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
                pass.colorAttachments[0].texture = target;
                pass.colorAttachments[0].loadAction = MTLLoadActionClear;
                pass.colorAttachments[0].storeAction = MTLStoreActionStore;
                pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 1, 0, 1);
                auto frame = rend::soft::PresentationFrame::create(size, size, 1.f, {},
                    std::vector<rend::soft::Color>(size * size, {0, 0, 255, 255}));
                ASSERT_TRUE(frame);
                id<MTLTexture> background = rend::soft::uploadMetalPresentationFrame(device, *frame);
                ASSERT_NE(background, nil);
                ImGui::GetIO().DisplaySize = ImVec2(size, size);
                ImGui::GetIO().DeltaTime = 1.f / 60.f;
                ImGui_ImplMetal_NewFrame(pass);
                ImGui::NewFrame();
                ImGui::GetBackgroundDrawList()->AddImage(
                    ImTextureRef((ImTextureID)(uintptr_t)(__bridge void*)background),
                    ImVec2(0, 0), ImVec2(size, size));
                ImGui::GetForegroundDrawList()->AddRectFilled(ImVec2(8, 8), ImVec2(24, 24), IM_COL32(255, 0, 0, 255));
                ImGui::GetForegroundDrawList()->AddText(ImVec2(2, 32), IM_COL32_WHITE, "PVR UI");
                ImGui::Render();
                ASSERT_GT(ImGui::GetDrawData()->TotalVtxCount, 0);
                id<MTLCommandBuffer> command = [queue commandBuffer];
                id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
                ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), command, encoder);
                [encoder endEncoding];
                [command commit];
                [command waitUntilCompleted];
                ASSERT_EQ(command.status, MTLCommandBufferStatusCompleted);
                std::vector<unsigned char> pixels(size * size * 4);
                [target getBytes:pixels.data() bytesPerRow:size * 4
                    fromRegion:MTLRegionMake2D(0, 0, size, size) mipmapLevel:0];
                const size_t inside = (16 * size + 16) * 4;
                EXPECT_EQ(pixels[inside], 255);
                EXPECT_EQ(pixels[inside + 1], 0);
                EXPECT_EQ(pixels[inside + 2], 0);
                EXPECT_EQ(pixels[0], 0);
                EXPECT_EQ(pixels[1], 0);
                EXPECT_EQ(pixels[2], 255);
                size_t textPixels = 0;
                for (int y = 32; y < size; ++y)
                    for (int x = 0; x < size; ++x) {
                        const size_t offset = (y * size + x) * 4;
                        if (pixels[offset] != 0 || pixels[offset + 1] != 0)
                            ++textPixels;
                    }
                EXPECT_GT(textPixels, 0u); // Dynamic font texture was sampled.
            }
            ImGui_ImplMetal_Shutdown();
            ImGui::DestroyContext();
        }
    }
}
