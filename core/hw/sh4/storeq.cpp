/*
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
#include "sh4_if.h"
#include "sh4_mem.h"
#include "modules/mmu.h"
#include "hw/pvr/pvr_mem.h"
#include "sh4_cycles.h"

static u32 CCN_QACR_TR[2];

static void chargeSqWrite(Sh4Context *ctx, u32 address)
{
	Sh4Cycles cycles;
	cycles.init(ctx);
	cycles.addWriteAccessCycles(address, sizeof(SQBuffer));
}

template<bool mmu_on>
static void DYNACALL sqWrite(u32 dest, Sh4Context *ctx)
{
	u32 address;
	//Translate the SQ addresses as needed
	if (mmu_on)
	{
		mmu_TranslateSQW(dest, &address);
	}
	else
	{
		u32 QACR = CCN_QACR_TR[(dest >> 5) & 1];
		//QACR has already 0xE000_0000
		address = QACR + (dest & ~0x1f);
	}

	if (((address >> 26) & 7) != 4)//Area 4
	{
		chargeSqWrite(ctx, address);
		const SQBuffer *sq = &ctx->sq_buffer[(dest >> 5) & 1];
		WriteMemBlock_nommu_sq(address, sq);
	}
	else
	{
		chargeSqWrite(ctx, address);
		// Translation preserves bit 5 for every supported page size, so the
		// PVR helper can select the originating queue from the final address.
		TAWriteSQ(address, ctx->sq_buffer);
	}
}

//yes, this micro optimization makes a difference
static void DYNACALL sqWrite_nommu_area_3(u32 dest, Sh4Context *ctx)
{
	chargeSqWrite(ctx, 0x0c000000 | (dest & RAM_MASK));
	SQBuffer *pmem = (SQBuffer *)((u8 *)ctx + sizeof(Sh4Context) + 0x0C000000);
	pmem += (dest & (RAM_SIZE_MAX - 1)) >> 5;
	*pmem = ctx->sq_buffer[(dest >> 5) & 1];
}

static void DYNACALL sqWrite_nommu_area_3_nonvmem(u32 dest, Sh4Context *ctx)
{
	chargeSqWrite(ctx, 0x0c000000 | (dest & RAM_MASK));
	u8* pmem = &mem_b[0];

	memcpy((SQBuffer *)&pmem[dest & (RAM_MASK - 0x1F)], &ctx->sq_buffer[(dest >> 5) & 1], sizeof(SQBuffer));
}

static void DYNACALL sqWriteTA(u32 dest, Sh4Context *ctx)
{
	chargeSqWrite(ctx, 0x10000000 | (dest & 0x03ffffe0));
	TAWriteSQ(dest, ctx->sq_buffer);
}

void setSqwHandler()
{
	Sh4Context& ctx = p_sh4rcb->cntx;
	if (CCN_MMUCR.AT == 1)
	{
		ctx.doSqWrite = &sqWrite<true>;
	}
	else
	{
		CCN_QACR_TR[0] = (CCN_QACR0.Area << 26) - 0xE0000000;
		CCN_QACR_TR[1] = (CCN_QACR1.Area << 26) - 0xE0000000;

		if (CCN_QACR0.Area != CCN_QACR1.Area)
		{
			ctx.doSqWrite = &sqWrite<false>;
			return;
		}

		switch (CCN_QACR0.Area)
		{
		case 3:
			if (addrspace::virtmemEnabled())
				ctx.doSqWrite = &sqWrite_nommu_area_3;
			else
				ctx.doSqWrite = &sqWrite_nommu_area_3_nonvmem;
			break;

		case 4:
			ctx.doSqWrite = &sqWriteTA;
			break;

		default:
			ctx.doSqWrite = &sqWrite<false>;
			break;
		}
	}
}
