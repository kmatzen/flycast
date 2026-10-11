/*
	Copyright 2023 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/
#include "sh4_cycles.h"
#include "modules/dmac.h"
#include "modules/modules.h"

void Sh4Cycles::addReadAccessCycles(u32 addr, u32 size) const
{
	const int cycles = readAccessCycles(addr, size);
	if (cycles > 0)
		DMAC_AddBusAccessCycles(ctx, cycles);
}

void Sh4Cycles::addWriteAccessCycles(u32 addr, u32 size) const
{
	const int cycles = writeAccessCycles(addr, size);
	if (cycles > 0)
		DMAC_AddBusAccessCycles(ctx, cycles);
}

static bool usesExternalBus(u32 addr)
{
	if ((addr & 0xfc000000) == 0xe0000000)
		return false; // store queues
	return ((addr & 0x1fffffff) >> 26) != 7; // on-chip registers
}

int Sh4Cycles::readAccessCycles(u32 addr, u32 size) const
{
	const int refresh = usesExternalBus(addr) ? bsc.takeRefreshBusCycles() : 0;
	return (refresh + readExternalAccessCycles(addr, size)) * 2 * cpuRatio;
}

int Sh4Cycles::writeAccessCycles(u32 addr, u32 size) const
{
	const int refresh = usesExternalBus(addr) ? bsc.takeRefreshBusCycles() : 0;
	return (refresh + writeExternalAccessCycles(addr, size)) * 2 * cpuRatio;
}

int Sh4Cycles::dmaReadAccessCycles(u32 addr, u32 size) const
{
	if (((addr & 0x1fffffff) >> 26) == 3)
		return (bsc.takeRefreshBusCycles() + bsc.sdramDmaReadCycles(addr)) * 2 * cpuRatio;
	return readAccessCycles(addr, size);
}

int Sh4Cycles::dmaWriteAccessCycles(u32 addr, u32 size) const
{
	if (((addr & 0x1fffffff) >> 26) == 3)
		return (bsc.takeRefreshBusCycles() + bsc.sdramDmaWriteCycles(addr)) * 2 * cpuRatio;
	return writeAccessCycles(addr, size);
}

static bool parameterReadsGpr(DecParam parameter, u16 op, u32 reg)
{
	switch (parameter)
	{
	case PRM_R0:
		return reg == 0;
	case PRM_RN:
	case PRM_RN_D4_x1:
	case PRM_RN_D4_x2:
	case PRM_RN_D4_x4:
		return reg == ((op >> 8) & 0xf);
	case PRM_RM:
	case PRM_RM_D4_x1:
	case PRM_RM_D4_x2:
	case PRM_RM_D4_x4:
		return reg == ((op >> 4) & 0xf);
	case PRM_RN_R0:
		return reg == 0 || reg == ((op >> 8) & 0xf);
	case PRM_RM_R0:
		return reg == 0 || reg == ((op >> 4) & 0xf);
	default:
		return false;
	}
}

static bool readsGpr(u16 op, u32 reg)
{
	const u64 decode = OpDesc[op]->decode;
	if (decode == 0)
		return false;
	const DecMode mode = (DecMode)((decode >> 24) & 0xff);
	const DecParam destination = (DecParam)((decode >> 16) & 0xff);
	const DecParam source = (DecParam)((decode >> 8) & 0xff);
	const bool readsDestination = mode == DM_BinaryOp || mode == DM_WriteM
			|| mode == DM_WriteTOp || mode == DM_DT || mode == DM_Shift
			|| mode == DM_Rot || mode == DM_MUL || mode == DM_DIV0
			|| mode == DM_ADC || mode == DM_NEGC;
	return (readsDestination && parameterReadsGpr(destination, op, reg))
			|| parameterReadsGpr(source, op, reg);
}

static int loadedGpr(u16 op)
{
	const u64 decode = OpDesc[op]->decode;
	if (decode == 0 || (DecMode)((decode >> 24) & 0xff) != DM_ReadM)
		return -1;
	switch ((DecParam)((decode >> 16) & 0xff))
	{
	case PRM_R0:
		return 0;
	case PRM_RN:
		return (op >> 8) & 0xf;
	default:
		return -1;
	}
}

int Sh4Cycles::countCycles(u16 op)
{
	sh4_opcodelistentry *opcode = OpDesc[op];
	int cycles = 0;
	const int loadedReg = lastUnit == CO ? -1 : loadedGpr(lastOpcode);
	const bool loadUseDependency = loadedReg >= 0 && readsGpr(op, loadedReg);

	if (lastUnit == CO
			|| opcode->unit == CO
			|| (lastUnit == opcode->unit && lastUnit != MT)
			|| loadUseDependency)
	{
		// cannot run in parallel
		lastUnit = opcode->unit;
		cycles += loadUseDependency
				? std::max(opcode->IssueCycles, OpDesc[lastOpcode]->LatencyCycles)
				: opcode->IssueCycles;
		lastOpcode = op;
	}
	else
	{
		// can run in parallel
		lastUnit = CO;
		lastOpcode = 0;
	}
	return cycles * cpuRatio;
}

// TODO additional wait cycles depending on area?:
// Area       Wait cycles (not including external wait)
// 0          3
// 1 VRAM     3
// 2 reserved 3
// 3 SDRAM    0         CAS latency 3
// 4 TA,YUV   1
// 5 G2 ext   3
// 6 reserved 3

int Sh4Cycles::readExternalAccessCycles(u32 addr, u32 size)
{
	if ((addr & 0xfc000000) == 0xe0000000)
		// store queues
		return 0;

	addr &= 0x1fffffff;
	if ((addr >> 26) != 3)
		bsc.endSdramDmaPipeline();
	switch (addr >> 26)
	{
	case 0:
		if (!settings.platform.isAtomiswave())
		{
			// Dreamcast, Naomi
			if (addr < 0x00200000)
			{
				// system rom
				switch (size)
				{
				case 1:
					return 44;
				case 2:
					return 63;
				case 4:
					return 99;
				case 32:
				default:
					return 618;
				}
			}
			if (addr < 0x00200000 + settings.platform.flash_size)
			{
				// flash
				switch (size)
				{
				case 1:
					return 41;
				case 2:
					return 55;
				case 4:
					return 83;
				case 32:
				default:
					return 489;
				}
			}
		}
		else
		{
			// Atomiswave
			if (addr < 0x00020000 || (addr >= 0x00200000 && addr < 0x00200000 + settings.platform.flash_size))
			{
				// flash
				switch (size)
				{
				case 1:
					return 41;
				case 2:
					return 55;
				case 4:
					return 83;
				case 32:
				default:
					return 489;
				}
			}
		}
		addr &= 0x01ffffff;
		if (addr >= 0x005f6800 && addr <= 0x005f69ff)
		{
			// holly system control regs
			if (size != 4)
				INFO_LOG(SH4, "holly system reg: Invalid read size %d @ %07x", size, addr);
			return 5;
		}
		if (addr >= 0x005f6c00 && addr <= 0x005f6cff)
		{
			// maple regs
			if (size != 4)
				INFO_LOG(SH4, "maple reg: Invalid read size %d @ %07x", size, addr);
			return 22;
		}
		if (addr >= 0x005f7000 && addr <= 0x005f70ff)
		{
			if (settings.platform.isArcade())
				// naomi/aw cart
				return 20; // ???
			else
			{
				// gd-rom
				if (size > 2)
					INFO_LOG(SH4, "gd-rom: Invalid read size %d @ %07x", size, addr);
				return 39;
			}
		}
		if (addr >= 0x005f7400 && addr <= 0x005f74ff)
		{
			// G1 I/F control regs
			if (settings.platform.isConsole())
			{
				if (size != 4) // unknown for aw/naomi
					INFO_LOG(SH4, "G1 I/F: Invalid read size %d @ %07x", size, addr);
			}
			else
			{
				// unknown for aw/naomi. seeing size 1 and 4 at least
			}
			return 24;
		}
		if (addr >= 0x005f7800 && addr <= 0x005f78ff)
		{
			// G2 I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "G2 I/F: Invalid read size %d @ %07x", size, addr);
			return 38;
		}
		if (addr >= 0x005f7c00 && addr <= 0x005f7cff)
		{
			// PVR I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "PVR I/F: Invalid read size %d @ %07x", size, addr);
			return 24;
		}
		if (addr >= 0x005f8000 && addr <= 0x005f9fff)
		{
			// TA/PVR core control regs, Palette RAM, fog table
			if (size != 4)
				// TODO 32-byte access allowed for palette and fog tables?
				INFO_LOG(SH4, "PVR/TA core: Invalid read size %d @ %07x", size, addr);
			return 34;
		}
		if (addr >= 0x00600000 && addr <= 0x006007ff)
		{
			if (settings.platform.isConsole())
				// AW registers
				return 20; // ???
			else
			{
				// modem
				if (size != 1)
					INFO_LOG(SH4, "modem: Invalid read size %d @ %07x", size, addr);
				return 67;
			}
		}
		if (addr >= 0x00700000 && addr <= 0x00ffffff)
		{
			// aica regs and ram
			if (size < 4)
				INFO_LOG(SH4, "aica: Invalid read size %d @ %07x", size, addr);
			return 40 * size / 4;	// undocumented
		}
		if (addr >= 0x01000000 && addr <= 0x01ffffff)
		{
			// G2 external area
			switch (size)
			{
			case 1:
			case 2:
				return 56;
			case 4:
				return 60;
			case 32:
			default:
				return 84;
			}
		}
		break;

	case 1:
		// VRAM
		switch (size)
		{
		case 1:
			INFO_LOG(SH4, "vram: Invalid read size 1 @ %07x", addr);
			return 41;
		case 2:
		case 4:
			return 41;
		case 32:
		default:
			return 61;
		}

	case 2:
		// Area 2
		INFO_LOG(SH4, "Invalid read from area 2 @ %07x", addr);
		return 60;

	case 3:
		// System RAM
		return bsc.sdramReadCycles(addr);

	case 4:
		// TA FIFO
		if (size != 32)
			INFO_LOG(SH4, "Invalid read size %d from area 4 (TA FIFO) @ %07x", size, addr);
		if ((addr >= 0x11000000 && addr <= 0x11ffffff) || (addr >= 0x13000000 && addr <= 0x13ffffff))
			// VRAM (64 bits)
			return 61;	// undocumented
		break;

	case 5:
		// Ext device
		switch (size)
		{
		case 1:
		case 2:
			return 56;
		case 4:
			return 60;
		case 32:
		default:
			return 84;
		}

	case 6:
		// Area 6
		INFO_LOG(SH4, "Invalid read from area 6 @ %07x", addr);
		return 60;

	case 7:
		// SH4 registers
		return 0;
	}

	INFO_LOG(SH4, "Unmapped read @ %08x", addr);
	return 60;
}


int Sh4Cycles::writeExternalAccessCycles(u32 addr, u32 size)
{
	if ((addr & 0xfc000000) == 0xe0000000)
		// store queues
		return 0;

	addr &= 0x1fffffff;
	if ((addr >> 26) != 3)
		bsc.endSdramDmaPipeline();
	switch (addr >> 26)
	{
	case 0:
		if (!settings.platform.isAtomiswave())
		{
			if (addr < 0x00200000)
			{
				// system rom
				INFO_LOG(SH4, "Invalid write to rom @ %07x", addr);
				return 99;
			}
			if (addr < 0x00200000 + settings.platform.flash_size)
			{
				// flash
				if (size != 1)
					INFO_LOG(SH4, "flashrom: Invalid write size %d @ %07x", size, addr);
				return 28;
			}
		}
		else
		{
			if (addr < 0x00020000)
			{
				// flash
				if (size != 1)
					INFO_LOG(SH4, "flashrom: Invalid write size %d @ %07x", size, addr);
				return 28;
			}
			if (addr >= 0x00200000 && addr < 0x00200000 + settings.platform.flash_size)
			{
				// nvmem
				return 14; // ????
			}
		}
		addr &= 0x01ffffff;
		if (addr >= 0x005f6800 && addr <= 0x005f69ff)
		{
			// holly system control regs
			if (size != 4)
				INFO_LOG(SH4, "holly system reg: Invalid write size %d @ %07x", size, addr);
			return 5;
		}
		if (addr >= 0x005f6c00 && addr <= 0x005f6cff)
		{
			// maple regs
			if (size != 4)
				INFO_LOG(SH4, "maple reg: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f7000 && addr <= 0x005f70ff)
		{
			if (settings.platform.isArcade())
				// naomi/aw cart
				return 14; // ???
			else
			{
				// gd-rom
				if (size > 2)
					INFO_LOG(SH4, "gd-rom: Invalid write size %d @ %07x", size, addr);
				return 28;
			}
		}
		if (addr >= 0x005f7400 && addr <= 0x005f74ff)
		{
			// G1 I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "G1 I/F: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f7800 && addr <= 0x005f78ff)
		{
			// G2 I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "G2 I/F: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f7c00 && addr <= 0x005f7cff)
		{
			// PVR I/F control regs
			if (size != 4)
				INFO_LOG(SH4, "PVR I/F: Invalid write size %d @ %07x", size, addr);
			return 12;
		}
		if (addr >= 0x005f8000 && addr <= 0x005f9fff)
		{
			// TA/PVR core control regs, Palette RAM, fog table
			if (size != 4)
				// TODO 32-byte access allowed for palette and fog tables?
				INFO_LOG(SH4, "PVR/TA core: Invalid write size %d @ %07x", size, addr);
			return 14;
		}
		if (addr >= 0x00600000 && addr <= 0x006007ff)
		{
			if (settings.platform.isAtomiswave())
				// AW registers
				return 14; // ???
			else
			{
				// modem
				if (size != 1)
					INFO_LOG(SH4, "modem: Invalid write size %d @ %07x", size, addr);
				return 44;
			}
		}
		if (addr >= 0x00700000 && addr <= 0x00ffffff)
		{
			// aica regs and ram
			if (size < 4)
				INFO_LOG(SH4, "aica: Invalid read size %d @ %07x", size, addr);
			return 12 * size / 4;	// undocumented
		}
		if (addr >= 0x01000000 && addr <= 0x01ffffff)
		{
			// G2 external area
			switch (size)
			{
			case 1:
			case 2:
			case 4:
				return 28;
			case 32:
			default:
				return 52;
			}
		}
		break;

	case 1:
		// VRAM
		switch (size)
		{
		case 1:
			INFO_LOG(SH4, "vram: Invalid write size 1 @ %07x", addr);
			return 12;
		case 2:
		case 4:
			return 12;
		case 32:
		default:
			return 38;
		}

	case 2:
		// Area 2
		INFO_LOG(SH4, "Invalid read to area 2 @ %07x", addr);
		return 12;

	case 3:
		// System RAM
		return bsc.sdramWriteCycles(addr);

	case 4:
		// TA FIFO
		if (size != 32)
			INFO_LOG(SH4, "Invalid write size %d to area 4 (TA FIFO) @ %07x", size, addr);
		if ((addr >= 0x10000000 && addr <= 0x107fffff) || (addr >= 0x12000000 && addr <= 0x127fffff))
			// TA polygon data
			return 7;	// undocumented
		if ((addr >= 0x10800000 && addr <= 0x10ffffff) || (addr >= 0x12800000 && addr <= 0x12ffffff))
			// YUV converter
			return 9;	// 858 cycles for 3072 bytes (YUV420)
		if ((addr >= 0x11000000 && addr <= 0x11ffffff) || (addr >= 0x13000000 && addr <= 0x13ffffff))
			// VRAM (64 bits)
			return 5;	// 8 for 32-bit access (LMMODE0/1)
		break;

	case 5:
		// Ext device
		switch (size)
		{
		case 1:
		case 2:
		case 4:
			return 28;
		case 32:
		default:
			return 52;
		}

	case 6:
		// Area 6
		INFO_LOG(SH4, "Invalid write to area 6 @ %07x", addr);
		return 14;

	case 7:
		// SH4 registers
		return 0;
	}

	INFO_LOG(SH4, "Unmapped read @ %08x", addr);
	return 14;
}
