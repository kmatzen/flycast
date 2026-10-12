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
	const u32 xmax = std::min<u32>(width, clip.origin.x + clip.size.x);
	const u32 ymax = std::min<u32>(height, clip.origin.y + clip.size.y);
	for (u32 y = clip.origin.y; y < ymax; ++y)
	{
		const u8 *source = data + (static_cast<size_t>(y) * width + clip.origin.x) * 4;
		const u32 address = framebufferWriteRowAddress(base, width, bytesPerPixel,
				lineStride, y) + clip.origin.x * bytesPerPixel;
		writeRow(address, source, clip.origin.x, xmax, y);
	}
}
