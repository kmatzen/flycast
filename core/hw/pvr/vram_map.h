#pragma once
#include "types.h"

// Convert the CPU/framebuffer 32-bit aperture to interleaved physical VRAM.
// Parameters preserve the platform-specific VRAM geometry (Dreamcast/Naomi).
inline u32 pvrMap32Address(u32 address, u32 mask, u32 bankBit)
{
	const u32 staticBits = mask - (bankBit * 2 - 1) + 3;
	const u32 offsetBits = (bankBit - 1) & ~3;
	return (address & staticBits) | ((address & offsetBits) * 2)
			| ((address & bankBit) / bankBit * 4);
}
