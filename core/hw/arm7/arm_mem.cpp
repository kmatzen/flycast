#include "arm_mem.h"
#include "hw/aica/aica.h"
#include "hw/aica/aica_mem.h"

namespace aica::arm
{

#define REG_L (0x2D00)
#define REG_M (0x2D04)
#define REG_ARM_CONTROL (0x2C00)

//Set to true when aica interrupt is pending
bool aica_interr;
u32 aica_reg_L;
//Set to true when the out of the intc is 1
bool e68k_out;
u32 e68k_reg_L;
u32 e68k_reg_M; //constant ?

void update_e68k()
{
	if (!e68k_out && aica_interr)
	{
		//Set the pending signal
		//Is L register held here too ?
		e68k_out = true;
		e68k_reg_L = aica_reg_L;

		update_armintc();
	}
}

void interruptChange(u32 bits,u32 L)
{
	aica_interr = bits != 0;
	if (aica_interr)
		aica_reg_L = L;
	update_e68k();
}

void e68k_AcceptInterrupt()
{
	e68k_out = false;
	update_e68k();
	update_armintc();
}

//Reg reads from arm side ..
template <typename T>
T readReg(u32 addr)
{
	addr &= 0x7FFF;
	if (addr == REG_ARM_CONTROL
			|| (addr == REG_ARM_CONTROL + 1 && sizeof(T) == 1))
		return readAicaReg<T>(addr);
	else if (addr == REG_L)
		return (T)e68k_reg_L;
	else if (addr == REG_M)
	{
		// M is an interrupt-completion command and RP is a write-only control
		// bit in its high lane; neither drives the register read bus.
		return 0;
	}
	else if (addr == REG_M + 1)
		return 0;
	else if (sizeof(T) == 4)
		return readRegInternal<u16>(addr);
	else
		return readRegInternal<T>(addr);
}

template <typename T>
void writeReg(u32 addr, T data)
{
	addr &= 0x7FFF;
	if (addr == REG_ARM_CONTROL)
	{
		// ARMRST in the low byte is controlled by the main CPU only. VREG
		// remains a shared read/write field in bits 9:8.
		if constexpr (sizeof(T) > 1)
			writeAicaReg<u8>(REG_ARM_CONTROL + 1, (data >> 8) & 3);
		return;
	}
	else if (addr == REG_ARM_CONTROL + 1)
	{
		writeAicaReg<u8>(addr, data & 3);
		return;
	}
	else if (addr == REG_L)
	{
		return; // Shouldn't really happen (read only)
	}
	else if (addr == REG_M)
	{
		//accept interrupts
		if (data & 1)
			e68k_AcceptInterrupt();
		if constexpr (sizeof(T) > 1)
			CommonData->RP = (data >> 8) & 1;
	}
	else if (addr == REG_M + 1)
	{
		// RP is the only implemented bit in the high byte.
		CommonData->RP = data & 1;
	}
	else
	{
		if (sizeof(T) == 4)
			writeRegInternal(addr, (u16)data);
		else
			writeRegInternal(addr, data);
	}
}

//00000000~007FFFFF @DRAM_AREA* 
//00800000~008027FF @CHANNEL_DATA 
//00802800~00802FFF @COMMON_DATA 
//00803000~00807FFF @DSP_DATA 

template u8 readReg<u8>(u32 adr);
template u16 readReg<u16>(u32 adr);
template u32 readReg<u32>(u32 adr);

template void writeReg<>(u32 adr, u8 data);
template void writeReg<>(u32 adr, u16 data);
template void writeReg<>(u32 adr, u32 data);

} // namespace aica::arm
