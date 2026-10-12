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
	const u64 sequence;
	const u64 contentSequence;
	const std::vector<Color> pixels;

	static std::shared_ptr<const PresentationFrame> create(u32 width, u32 height,
			float aspectRatio, Color border, std::vector<Color> pixels, u64 sequence = 0,
			u64 contentSequence = 0)
	{
		if (width == 0 || height == 0 || !std::isfinite(aspectRatio)
				|| aspectRatio <= 0 || pixels.size() / width != height
				|| pixels.size() % width != 0)
			return {};
		return std::shared_ptr<const PresentationFrame>(new PresentationFrame(
				width, height, aspectRatio, border, std::move(pixels), sequence, contentSequence));
	}

private:
	PresentationFrame(u32 width, u32 height, float aspectRatio, Color border,
			std::vector<Color> pixels, u64 sequence, u64 contentSequence)
		: width(width), height(height), aspectRatio(aspectRatio), border(border), sequence(sequence),
		  contentSequence(contentSequence),
		  pixels(std::move(pixels)) {}
};

// UI-thread accounting of completed host swaps, not physical scanout. Sequence
// gaps count framebuffer snapshots that never reached a completed swap.
struct PresentationDelivery
{
	u64 lastSequence = 0;
	u64 unique = 0;
	u64 skipped = 0;
	u64 repeated = 0;
	u64 outOfOrder = 0;
	void complete(u64 sequence)
	{
		if (sequence == 0)
			return;
		if (sequence < lastSequence)
			++outOfOrder;
		else if (sequence == lastSequence)
			++repeated;
		else
		{
			skipped += sequence - lastSequence - 1;
			++unique;
			lastSequence = sequence;
		}
	}
};

// Latest completed image only: replacing a queued image never invalidates a
// consumer's retained snapshot. No renderer state is shared through this slot.
class PresentationMailbox
{
public:
	void publish(std::shared_ptr<const PresentationFrame> frame)
	{
		std::atomic_store(&latest, std::move(frame));
	}
	std::shared_ptr<const PresentationFrame> acquire() const
	{
		return std::atomic_load(&latest);
	}
	void clear() { publish({}); }
private:
	std::shared_ptr<const PresentationFrame> latest;
};

} // namespace rend::soft
