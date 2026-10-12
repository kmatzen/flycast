#pragma once

#include "SoftwareRasterizer.h"
#include <cmath>
#include <memory>
#include <mutex>
#include <deque>

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

// Experimental bounded handoff. Consecutive identical images do not consume
// queue slots. Never wait for display on the producer thread; overflow drops
// the oldest queued image and remains observable through dropped().
class OrderedPresentationQueue
{
public:
	void publish(std::shared_ptr<const PresentationFrame> frame)
	{
		if (!frame) return;
		std::lock_guard<std::mutex> lock(mutex);
		if (previous && previous->width == frame->width && previous->height == frame->height
				&& previous->aspectRatio == frame->aspectRatio && previous->border == frame->border
				&& previous->pixels == frame->pixels)
			return;
		previous = frame;
		if (frames.size() == 3) { frames.pop_front(); ++drops; }
		frames.push_back(std::move(frame));
	}
	std::shared_ptr<const PresentationFrame> take()
	{
		std::lock_guard<std::mutex> lock(mutex);
		if (frames.empty()) return {};
		auto frame = std::move(frames.front());
		frames.pop_front();
		return frame;
	}
	u64 dropped() const { std::lock_guard<std::mutex> lock(mutex); return drops; }
	void clear()
	{
		std::lock_guard<std::mutex> lock(mutex);
		frames.clear(); previous.reset(); drops = 0;
	}
private:
	mutable std::mutex mutex;
	std::deque<std::shared_ptr<const PresentationFrame>> frames;
	std::shared_ptr<const PresentationFrame> previous;
	u64 drops = 0;
};

} // namespace rend::soft
