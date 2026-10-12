#pragma once

#include "SoftwareRasterizer.h"
#include <cmath>
#include <memory>

namespace rend::soft {

// An owned display image, after framebuffer read-format conversion. It never
// aliases VRAM, a rasterizer accumulation buffer, or a borrowed GPU surface.
// Const storage lets a consumer retain a frame while the producer replaces it.
struct PresentationFrame final
{
	const u32 width;
	const u32 height;
	const float aspectRatio;
	const Color border;
	const std::vector<Color> pixels;

	static std::shared_ptr<const PresentationFrame> create(u32 width, u32 height,
			float aspectRatio, Color border, std::vector<Color> pixels)
	{
		if (width == 0 || height == 0 || !std::isfinite(aspectRatio)
				|| aspectRatio <= 0 || pixels.size() / width != height
				|| pixels.size() % width != 0)
			return {};
		return std::shared_ptr<const PresentationFrame>(new PresentationFrame(
				width, height, aspectRatio, border, std::move(pixels)));
	}

private:
	PresentationFrame(u32 width, u32 height, float aspectRatio, Color border,
			std::vector<Color> pixels)
		: width(width), height(height), aspectRatio(aspectRatio), border(border),
		  pixels(std::move(pixels)) {}
};

} // namespace rend::soft
