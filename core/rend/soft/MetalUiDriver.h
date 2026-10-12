#pragma once
#include "PresentationFrame.h"
#include "ui/imgui_driver.h"
#include <functional>

namespace rend::soft {
using MetalFrameProvider = std::function<std::shared_ptr<const PresentationFrame>()>;
using MetalPresentationObserver = std::function<void(std::shared_ptr<const PresentationFrame>, double)>;
// Experimental factory. The frontend owns the CAMetalLayer's window/view and
// updates drawableSize. No other ImGui renderer backend may remain initialized.
// Observer runs on the UI thread; time==0 reports a skipped drawable.
std::unique_ptr<ImGuiDriver> createMetalUiDriver(void *metalLayer,
    MetalFrameProvider provider, MetalPresentationObserver observer);
}
