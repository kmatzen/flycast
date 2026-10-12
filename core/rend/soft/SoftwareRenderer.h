#pragma once
#include <memory>

struct Renderer;

Renderer *rend_software();

namespace rend::soft {
struct PresentationFrame;
std::shared_ptr<const PresentationFrame> retainPresentation(Renderer *renderer);
void restorePresentation(Renderer *renderer, std::shared_ptr<const PresentationFrame> frame);
}
