#include "aica_mem.h"
#include "aica.h"
#include "aica_if.h"
#include "dsp.h"
#include "sgc_if.h"
#include "hw/arm7/arm7_rec.h"
#include "hw/hwreg.h"

namespace aica
{

alignas(4) u8 aica_reg[0x8000];

CommonData_struct * const CommonData = (CommonData_struct *)&aica_reg[0x2800];
DSPData_struct * const DSPData = (DSPData_struct *)&aica_reg[0x3000];
//slave cpu (arm7)
InterruptInfo * const SCIEB = (InterruptInfo *)&aica_reg[SCIEB_addr];
InterruptInfo * const SCIPD = (InterruptInfo *)&aica_reg[SCIPD_addr];
InterruptInfo * const SCIRE = (InterruptInfo *)&aica_reg[SCIRE_addr];
//Main cpu (sh4)
InterruptInfo * const MCIEB = (InterruptInfo *)&aica_reg[MCIEB_addr];
InterruptInfo * const MCIPD = (InterruptInfo *)&aica_reg[MCIPD_addr];
InterruptInfo * const MCIRE = (InterruptInfo *)&aica_reg[MCIRE_addr];
DSP_OUT_VOL_REG const * const dsp_out_vol = (DSP_OUT_VOL_REG *)&aica_reg[0x2000];

static void (*midiReceiver)(u8 data);

static constexpr u16 channelRegisterMask(u32 reg)
{
	switch (reg & 0x7c)
	{
	case 0x00: return 0xc7ff;
	case 0x04:
	case 0x08:
	case 0x0c:
	case 0x1c: return 0xffff;
	case 0x10: return 0xffdf;
	case 0x14: return 0x7fff;
	case 0x18: return 0xffff; // bit 15 is saved; hardware also accepts FNS bit 10
	case 0x20: return 0x00ff;
	case 0x24: return 0xffff; // unused fields are physically saved
	case 0x28: return 0xffff; // bit 7 is unknown but physically saved
	case 0x2c:
	case 0x30:
	case 0x34:
	case 0x38:
	case 0x3c: return 0x1fff;
	case 0x40:
	case 0x44: return 0x1f1f;
	default: return 0;
	}
}

static constexpr u16 channelRegisterReadMask(u32 reg)
{
	const u16 mask = channelRegisterMask(reg);
	return (reg & 0x7c) == 0 ? mask & ~0x8000 : mask; // KYONEX is a write-only strobe
}

static constexpr bool isDsp16BitRegister(u32 addr)
{
	return (addr >= 0x3000 && addr < 0x3300) // COEF and MADRS
			|| (addr >= 0x3400 && addr < 0x3c00) // MPRO
			|| (addr >= 0x4580 && addr < 0x45c8); // EFREG and EXTS
}

//Aica read/write (both sh4 & arm)

//00000000~007FFFFF @DRAM_AREA*
//00800000~008027FF @CHANNEL_DATA 
//00802800~00802FFF @COMMON_DATA 
//00803000~00807FFF @DSP_DATA 
template<typename T>
T readRegInternal(u32 addr)
{
	addr &= 0x7FFF;
	auto readMasked16 = [&](u16 readableBits) -> T
	{
		const u16 value = ReadMemArr<u16>(aica_reg, addr & ~1u) & readableBits;
		if constexpr (sizeof(T) == 1)
			return static_cast<T>(value >> ((addr & 1) * 8));
		else
			return static_cast<T>(value);
	};
	if (addr < 0x2000)
	{
		const u32 reg = addr & 0x7f;
		if ((reg & 3) >= 2)
			return 0;
		const u16 value = ReadMemArr<u16>(aica_reg, addr & ~1u)
				& channelRegisterReadMask(reg);
		if constexpr (sizeof(T) == 1)
			return static_cast<T>(value >> ((addr & 1) * 8));
		else
			return static_cast<T>(value);
	}
	if (addr < 0x2800)
	{
		const u32 offset = addr - 0x2000;
		if (offset >= 18 * 4 || (offset & 3) >= 2)
			return 0;
		const u16 value = ReadMemArr<u16>(aica_reg, addr & ~1u) & 0x0f1f;
		if constexpr (sizeof(T) == 1)
			return static_cast<T>(value >> ((addr & 1) * 8));
		else
			return static_cast<T>(value);
	}
	if (addr >= 0x2800 && addr < 0x2802)
	{
		// MVOL, DAC18B, MEM8MB and MONO are write-only. Only the fixed
		// AICA revision nibble is driven when the common control register
		// is read.
		if constexpr (sizeof(T) == 1)
			return addr == 0x2800 ? static_cast<T>(0x10) : 0;
		else
			return static_cast<T>(0x0010);
	}

	if (addr >= 0x2800 && addr < 0x2818)
	{
		const u16 readFlags = sgc::ReadCommonReg(addr, sizeof(T) == 1);
		if constexpr (sizeof(T) >= 2)
		{
			if (addr == 0x2808)
				return static_cast<T>((ReadMemArr<T>(aica_reg, addr) & ~static_cast<T>(0x1f00)) | readFlags);
		}
	}
	else if ((addr >= 0x2818 && addr < 0x2880)
			|| (addr >= 0x28c0 && addr < 0x2c00)
			|| (addr >= 0x2c04 && addr < 0x2d00)
			|| (addr >= 0x2d08 && addr < 0x3000))
	{
		// Unassigned common-data space is not backed by storage. ARMRST/VREG
		// at 2c00 and the ARM-only L/M registers at 2d00/2d04 are handled by
		// the CPU-specific front ends before reaching this function.
		return 0;
	}
	else if (addr >= 0x4000 && addr < 0x4580)
	{
		if (addr & 2)
		{
			INFO_LOG(AICA, "Unaligned DSP register read @ %x", addr);
			return 0;
		}
		DEBUG_LOG(AICA, "DSP register read @ %x", addr);
		// DSP TEMP/MEMS
		u32 v;
		if (addr < 0x4500)
		{
			v = addr < 0x4400 ? dsp::state.TEMP[(addr - 0x4000) / 8] : dsp::state.MEMS[(addr - 0x4400) / 8];
			if (addr & 4)
				v = (v >> 8) & 0xffff;
			else
				v &= 0xff;
		}
		// DSP MIXS
		else
		{
			v = dsp::state.MIXS[(addr - 0x4500) / 8];
			if (addr & 4)
				v = (v >> 4) & 0xffff;
			else
				v &= 0xf;
		}
		if constexpr (sizeof(T) == 1)
		{
			if (addr & 1)
				v >>= 8;
			else
				v &= 0xff;
		}
		return v;
	}
	if ((addr >= 0x3300 && addr < 0x3400)
			|| (addr >= 0x3c00 && addr < 0x4000)
			|| addr >= 0x45c8
			|| (isDsp16BitRegister(addr) && (addr & 2)))
		return 0;
	if (isDsp16BitRegister(addr))
	{
		u16 value = ReadMemArr<u16>(aica_reg, addr & ~1u);
		if (addr < 0x3200)
			value &= ~7u; // COEF implements bits 15:3 only.
		if constexpr (sizeof(T) == 1)
			return static_cast<T>(value >> ((addr & 1) * 8));
		else
			return static_cast<T>(value);
	}

	// The DMA address/count fields, timers, interrupt reset registers, and
	// interrupt-level selectors are write-only. DGATE, DDIR, and DEXE share
	// words with write-only fields but remain readable status/control bits.
	switch (addr & ~1u)
	{
	case 0x2804: // RBL, RBP
	case 0x280c: // AFSEL, MSLC, MOBUF
		return 0;
	case 0x2880: // DMEA[22:16], MRWINH
	case 0x2884: // DMEA[15:2]
	case TIMER_A:
	case TIMER_B:
	case TIMER_C:
	case SCIRE_addr:
	case SCILV0_addr:
	case SCILV1_addr:
	case SCILV2_addr:
	case MCIRE_addr:
		return 0;
	case 0x2888: // DGATE is readable; DRGA is write-only
		return readMasked16(0x8000);
	case 0x288c: // DDIR and DEXE are readable; DLG is write-only
		return readMasked16(0x8001);
	default:
		break;
	}
	return ReadMemArr<T>(aica_reg, addr);
}
template u8 readRegInternal<u8>(u32 addr);
template u16 readRegInternal<u16>(u32 addr);
template u32 readRegInternal<u32>(u32 addr);

static void writeCommonReg8(u32 reg, u8 data)
{
	// Only control fields in the common-data monitor block accept writes.
	// MIBUF/status, EG/SGC/LP, CA, and the holes in this range are read-only.
	if (reg != 0x2800 && reg != 0x2801
			&& reg != 0x2804 && reg != 0x2805
			&& reg != 0x280c && reg != 0x280d)
		return;

	const u32 oldRamMask = soundRamMask();
	if (reg == 0x2800)
		// VER[3:0] is a read-only chip revision field in the high nibble.
		data = (data & 0x0f) | (CommonData->VER << 4);
	WriteMemArr(aica_reg, reg, data);
	if (reg == 0x2804 || reg == 0x2805)
	{
		using namespace dsp;
		state.RBL = (8192 << CommonData->RBL) - 1;
		state.RBP = (CommonData->RBP * 2048) & soundRamMask();
		state.dirty = true;
	}
	else if (reg == 0x2801 && oldRamMask != soundRamMask())
	{
		// The DSP JIT folds the memory mask and ring-buffer base into its
		// generated code, so a DRAM mode change requires recompilation.
		dsp::state.RBP = (CommonData->RBP * 2048) & soundRamMask();
		dsp::state.dirty = true;
#if FEAT_AREC != DYNAREC_NONE
		if (arm::recompiler::ICache != nullptr)
			arm::recompiler::flush();
#endif
	}
	else if (reg == 0x280c) {	// MOBUF
		midiOutWrite(data);
	}
}

template<typename T>
void writeRegInternal(u32 addr, T data)
{
	constexpr size_t sz = sizeof(T);
	addr &= 0x7FFF;

	if (addr < 0x2000)
	{
		//Channel data
		u32 chan = addr >> 7;
		u32 reg = addr & 0x7F;
		if ((reg & 3) >= 2)
			return;
		const u32 alignedAddr = addr & ~1u;
		u16 value = ReadMemArr<u16>(aica_reg, alignedAddr);
		if constexpr (sz == 1)
		{
			const u32 shift = (addr & 1) * 8;
			value = (value & ~(0xffu << shift)) | ((data & 0xffu) << shift);
		}
		else
			value = static_cast<u16>(data);
		WriteMemArr(aica_reg, alignedAddr,
				static_cast<u16>(value & channelRegisterMask(reg)));
		sgc::WriteChannelReg(chan, reg, std::min(sz, sizeof(u16)));
		return;
	}

	if (addr < 0x2800)
	{
		const u32 offset = addr - 0x2000;
		if (offset >= 18 * 4 || (offset & 3) >= 2)
			return;
		const u32 alignedAddr = addr & ~1u;
		u16 value = ReadMemArr<u16>(aica_reg, alignedAddr);
		if constexpr (sz == 1)
		{
			const u32 shift = (addr & 1) * 8;
			value = (value & ~(0xffu << shift)) | ((data & 0xffu) << shift);
		}
		else
			value = static_cast<u16>(data);
		WriteMemArr(aica_reg, alignedAddr, static_cast<u16>(value & 0x0f1f));
		return;
	}

	if (addr < 0x2818)
	{
		writeCommonReg8(addr, data & 0xFF);
		if constexpr (sz >= 2)
			writeCommonReg8(addr + 1, data >> 8);
		return;
	}

	if (addr >= 0x3000)
	{
		if ((addr >= 0x3300 && addr < 0x3400)
				|| (addr >= 0x3c00 && addr < 0x4000)
				|| addr >= 0x45c8
				|| (isDsp16BitRegister(addr) && (addr & 2)))
			return;
		// EXTS is driven by the external digital-audio input and is read-only
		// from both CPUs. Internal sample generation updates DSPData directly.
		if (addr >= 0x45c0 && addr < 0x45c8)
			return;
		if (addr & 2)
		{
			INFO_LOG(AICA, "Unaligned DSP register write @ %x", addr);
			return;
		}
		if (addr >= 0x4000 && addr < 0x4580)
		{
			// DSP TEMP/MEMS
			if (addr < 0x4500)
			{
				const bool mems = addr >= 0x4400;
				s32 &v = mems ? dsp::state.MEMS[(addr - 0x4400) / 8] : dsp::state.TEMP[(addr - 0x4000) / 8];
				if (addr & 4)
				{
					if constexpr (sz == 1)
					{
						if (addr & 1)
							// MEMS copies a CPU write to bits 23:16 into bits 7:0.
							v = mergeDsp24TopByte(v, data, mems);
						else
							v = (v & 0xffff00ff) | ((data & 0xff) << 8);
					}
					else
					{
						v = mergeDsp24HighWord(v, static_cast<u16>(data), mems);
					}
				}
				else
				{
					if (sz != 1 || (addr & 1) == 0)
						v = (v & ~0xff) | (data & 0xff);
					// else ignored
				}
				DEBUG_LOG(AICA, "DSP TEMP/MEMS register write<%d> @ %x = %d", (int)sz, addr, v);
			}
			// DSP MIXS
			else
			{
				s32 &v = dsp::state.MIXS[(addr - 0x4500) / 8];
				if (addr & 4)
				{
					if (sz == 1)
					{
						if (addr & 1)
							v = (v & 0x00000fff) | (((s32)data << 24) >> 12);
						else
							v = (v & 0xfffff00f) | ((data & 0xff) << 4);
					}
					else
					{
						v = (v & 0xf) | (((s32)data << 16) >> 12);
					}
				}
				else
				{
					if (sz != 1 || (addr & 1) == 0)
						v = (v & ~0xf) | (data & 0xf);
					// else ignored
				}
				DEBUG_LOG(AICA, "DSP MIXS register write<%d> @ %x = %d", (int)sz, addr, v);
			}
			return;
		}

		if (isDsp16BitRegister(addr))
		{
			u16 value;
			if constexpr (sz == 1)
			{
				value = ReadMemArr<u16>(aica_reg, addr & ~1u);
				const u32 shift = (addr & 1) * 8;
				value = (value & ~(0xffu << shift)) | ((data & 0xffu) << shift);
			}
			else
				value = static_cast<u16>(data);
			if (addr < 0x3200)
				value &= ~7u;
			WriteMemArr(aica_reg, addr & ~1u, value);
		}
		else
			WriteMemArr(aica_reg, addr, data);
		dsp::writeProg(addr);
		if constexpr (sz == 2)
			dsp::writeProg(addr + 1);
		return;
	}
	writeTimerAndIntReg(addr, data);
}
template void writeRegInternal<>(u32 addr, u8 data);
template void writeRegInternal<>(u32 addr, u16 data);
template void writeRegInternal<>(u32 addr, u32 data);

void initMem()
{
	memset(aica_reg, 0, sizeof(aica_reg));
	midiSendBuffer.clear();
	midiOutputBuffer.clear();
	CommonData->VER = 1;
	CommonData->MIEMP = 1;
	CommonData->MOEMP = 1;
	aica_ram[ARAM_SIZE - 1] = 1;
	aica_ram.zero();
}

void termMem()
{
	// The receiver models the external MIDI connection, so chip resets must
	// not detach it. Tear it down only with the AICA memory subsystem.
	midiReceiver = nullptr;
}

void setMidiReceiver(void (*handler)(u8 data)) {
	midiReceiver = handler;
}

void deliverMidiByte(u8 data)
{
	if (midiReceiver != nullptr)
		midiReceiver(data);
}

} // namespace aica
