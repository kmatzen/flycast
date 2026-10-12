#pragma once
#include "SoftwareRasterizer.h"

namespace rend::soft {
// Decode only the explicit manual cadence-probe protocol. This is diagnostic
// metadata from a completed host swap's source image, not PVR hardware evidence.
constexpr int decodeCadenceProbeId(Color color)
{
	const unsigned concat = color.b & 7;
	const unsigned id = (color.r >> 3) | ((color.g >> 2) << 5);
	return color.a == 255 && (color.b >> 3) == 16 && (color.r & 7) == concat
			&& (color.g & 3) == (concat & 3) && id < 660 ? static_cast<int>(id) : -1;
}
}
