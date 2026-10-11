#pragma once
#include "types.h"

namespace aica
{

constexpr s32 signedDsp24(u32 bits)
{
	bits &= 0xffffff;
	return (bits & 0x800000) != 0 ? static_cast<s32>(bits | 0xff000000) : static_cast<s32>(bits);
}

constexpr s32 mergeDsp24TopByte(s32 current, u8 topByte, bool mirrorLowByte)
{
	u32 bits = (static_cast<u32>(current) & 0x00ffff) | (static_cast<u32>(topByte) << 16);
	if (mirrorLowByte)
		bits = (bits & 0xffff00) | topByte;
	return signedDsp24(bits);
}

constexpr s32 mergeDsp24HighWord(s32 current, u16 highWord, bool mirrorLowByte)
{
	const u8 lowByte = mirrorLowByte ? highWord >> 8 : static_cast<u8>(current);
	return signedDsp24((static_cast<u32>(highWord) << 8) | lowByte);
}

template<typename T> T readRegInternal(u32 addr);
template<typename T> void writeRegInternal(u32 addr, T data);

void initMem();
void termMem();

alignas(4) extern u8 aica_reg[0x8000];

}
