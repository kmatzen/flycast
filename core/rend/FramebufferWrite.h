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
