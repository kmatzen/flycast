#pragma once

#include "types.h"
#include <algorithm>
#include <cstdint>
#include <cstring>

// Linear destinations retain ordinary pointer addressing. Destinations inside
// VRAM use byte offsets so both pixels and row advances wrap at its boundary.
// memorySize must be a power of two, as on all supported VRAM configurations.
class TexturePixelWriter
{
public:
	TexturePixelWriter(u16 *destination, u8 *memory, u32 memorySize)
		: destination(reinterpret_cast<u8 *>(destination)), memory(memory), memorySize(memorySize)
	{
		const auto address = reinterpret_cast<uintptr_t>(destination);
		const auto base = reinterpret_cast<uintptr_t>(memory);
		wrapped = address >= base && address - base < memorySize;
		if (wrapped)
			offset = static_cast<u32>(address - base);
	}
	void write(u16 value)
	{
		if (wrapped)
		{
			memory[offset & (memorySize - 1)] = static_cast<u8>(value);
			memory[(offset + 1) & (memorySize - 1)] = static_cast<u8>(value >> 8);
			offset = (offset + 2) & (memorySize - 1);
		}
		else
		{
			std::memcpy(destination, &value, sizeof(value));
			destination += sizeof(value);
		}
	}
	void advance(u32 bytes)
	{
		if (wrapped)
			offset = (offset + bytes) & (memorySize - 1);
		else
			destination += bytes;
	}
private:
	u8 *destination;
	u8 *memory;
	u32 memorySize;
	u32 offset = 0;
	bool wrapped = false;
};

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
