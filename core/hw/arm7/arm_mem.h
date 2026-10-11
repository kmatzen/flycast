#pragma once
#include "types.h"
#include "hw/aica/aica.h"
#include "hw/aica/aica_if.h"

namespace aica::arm
{

template <typename T> T readReg(u32 addr);
template <typename T> void writeReg(u32 addr, T data);

template<typename T>
static inline T DYNACALL readMem(u32 addr)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
	{
		T rv = *(T *)&aica_ram[addr & (soundRamMask() & ~(sizeof(T) - 1))];
		
		if (unlikely(sizeof(T) == 4 && (addr & 3) != 0))
		{
			u32 sf = (addr & 3) * 8;
			return (rv >> sf) | (rv << (32 - sf));
		}
		else
			return rv;
	}
	else
	{
		if constexpr (sizeof(T) == 4)
		{
			u32 rv = readReg<u32>(addr & ~3u);
			u32 shift = (addr & 3) * 8;
			return shift == 0 ? rv : (rv >> shift) | (rv << (32 - shift));
		}
		else
		{
			return readReg<T>(addr);
		}
	}
}

template<typename T>
static inline void DYNACALL writeMem(u32 addr, T data)
{
	addr &= 0x00FFFFFF;
	if (addr < 0x800000)
	{
		*(T *)&aica_ram[addr & (soundRamMask() & ~(sizeof(T) - 1))] = data;
	}
	else
	{
		if constexpr (sizeof(T) == 4)
			writeReg(addr & ~3u, data);
		else
			writeReg(addr, data);
	}
}

extern bool aica_interr;
extern u32 aica_reg_L;
extern bool e68k_out;
extern u32 e68k_reg_L;
extern u32 e68k_reg_M;

void update_armintc();
void interruptChange(u32 bits, u32 L);

} // namespace aica::arm
