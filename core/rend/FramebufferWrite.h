#pragma once

#include "types.h"
#include <algorithm>

// Byte address of a clipped starting row. Packed RGB888 rows can have
// padding that is not a whole pixel; never divide that padding by bpp.
inline u32 framebufferWriteRowAddress(u32 base, u32 width, u32 bytesPerPixel,
		u32 lineStride, u32 row)
{
	return base + row * std::max(lineStride, width * bytesPerPixel);
}

// Shared production traversal, with storage supplied by the caller. The
// source is RGBA8888; destination row stride is measured in bytes.
template<typename Clip, typename WriteRow>
void forEachFramebufferWriteRow(u32 width, u32 height, const u8 *data,
		u32 base, u32 bytesPerPixel, u32 lineStride, const Clip& clip, WriteRow writeRow)
{
	if (clip.size.x <= 0 || clip.size.y <= 0)
		return;
	const u32 xmin = static_cast<u32>(std::clamp<s64>(clip.origin.x, 0, width));
	const u32 ymin = static_cast<u32>(std::clamp<s64>(clip.origin.y, 0, height));
	const u32 xmax = static_cast<u32>(std::clamp<s64>(
			static_cast<s64>(clip.origin.x) + clip.size.x, 0, width));
	const u32 ymax = static_cast<u32>(std::clamp<s64>(
			static_cast<s64>(clip.origin.y) + clip.size.y, 0, height));
	if (xmin >= xmax || ymin >= ymax)
		return;
	for (u32 y = ymin; y < ymax; ++y)
	{
		const u8 *source = data + (static_cast<size_t>(y) * width + xmin) * 4;
		const u32 address = framebufferWriteRowAddress(base, width, bytesPerPixel,
				lineStride, y) + xmin * bytesPerPixel;
		writeRow(address, source, xmin, xmax, y);
	}
}
