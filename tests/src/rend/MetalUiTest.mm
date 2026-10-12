#import <Metal/Metal.h>
#include "imgui.h"
#include "backends/imgui_impl_metal.h"
#include <gtest/gtest.h>
#include <vector>

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
                pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 1, 1);
                ImGui::GetIO().DisplaySize = ImVec2(size, size);
                ImGui::GetIO().DeltaTime = 1.f / 60.f;
                ImGui_ImplMetal_NewFrame(pass);
                ImGui::NewFrame();
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
