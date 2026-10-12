#pragma once
#include <memory>

struct Renderer;

Renderer *rend_software();

namespace rend::soft {
struct PresentationFrame;
// UI thread only. Returns the immutable packet accepted by Present(), not the
// producer's latest publication. The packet can outlive the renderer; callbacks
// must not retain or later dereference the Renderer pointer.
std::shared_ptr<const PresentationFrame> selectedPresentation(Renderer *renderer);
std::shared_ptr<const PresentationFrame> retainPresentation(Renderer *renderer);
void restorePresentation(Renderer *renderer, std::shared_ptr<const PresentationFrame> frame);
}
