/*
	DMAC is not really emulated on nullDC. We just fake the dmas ;p
		Dreamcast uses sh4's dmac in ddt mode to multiplex ch0 and ch2 for dma access.
		nullDC just 'fakes' each dma as if it was a full channel, never bothering properly
		updating the dmac regs -- it works just fine really :|
*/
#include "types.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/pvr/pvr_mem.h"
#include "dmac.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_cycles.h"
#include "hw/holly/holly_intc.h"
#include "serialize.h"

DMACRegisters dmac;

static bool ch2DdtActive;
static u32 ch2DdtSrc;
static u32 ch2DdtDst;
static u32 ch2DdtRemaining;
static bool ch2DdtPath64;
static u32 ch2DdtBurstBytes;
static bool pvrDmaActive;
static bool pvrDmaToSystem;
static u32 pvrDmaSrc;
static u32 pvrDmaDst;
static u32 pvrDmaRemaining;
static void reevaluateDmacBus();

void DMAC_StartPvrDma(u32 src, u32 dst, u32 length, bool pvrToSystem)
{
	pvrDmaSrc = src;
	pvrDmaDst = dst;
	pvrDmaRemaining = length;
	pvrDmaToSystem = pvrToSystem;
	pvrDmaActive = length != 0;
	SB_PDSTARD = src;
	SB_PDSTAPD = dst;
	SB_PDLEND = 0;
	DMAC_CHCR(0).TE = 0;
	if (!pvrDmaActive) {
		SB_PDST = 0;
		asic_RaiseInterrupt(holly_PVR_DMA);
		return;
	}
	reevaluateDmacBus();
}

void DMAC_Ch2St()
{
	u32 dmaor = DMAC_DMAOR.full;

	const u32 src = DMAC_SAR(2) & 0x1fffffe0;
	const u32 dst = SB_C2DSTAT & 0x01ffffe0;
	const u32 length = SB_C2DLEN == 0 ? 0x01000000 : SB_C2DLEN;

	if (DMAOR_DDT_ENABLED != (dmaor & DMAOR_MASK))
	{
		INFO_LOG(SH4, "DMAC: DMAOR has invalid settings (%X) !", dmaor);
		return;
	}
	// HOLLY's request is accepted only when SH4 channel 2 is configured for
	// the documented 32-byte, burst, external memory-to-device transfer.
	// IE is independent of the transfer configuration and may be either value.
	constexpr u32 ch2ConfigMask = 0x0000fff3;
	constexpr u32 ch2Config = 0x000012c1;
	if ((DMAC_CHCR(2).full & ch2ConfigMask) != ch2Config
			|| DMAC_DMATCR(2) != length / 32)
	{
		INFO_LOG(SH4, "DMAC: invalid ch2 settings CHCR=%X count=%X len=%X",
				DMAC_CHCR(2).full, DMAC_DMATCR(2), SB_C2DLEN);
		return;
	}
	if ((src >> 26) != 3)
	{
		// Source address must be in system RAM
		WARN_LOG(SH4, "DMAC: invalid source address %x dest %x len %x", DMAC_SAR(2), SB_C2DSTAT, SB_C2DLEN);
		DMAC_DMAOR.AE = 1;
		asic_RaiseInterrupt(holly_CH2_DMA);
		return;
	}

	DEBUG_LOG(SH4, ">> DMAC: Ch2 DMA SRC=%X DST=%X LEN=%X", src, SB_C2DSTAT, SB_C2DLEN);
	ch2DdtSrc = src;
	ch2DdtDst = dst;
	ch2DdtRemaining = length;
	ch2DdtPath64 = SB_C2DSTAT & 0x02000000 ? SB_LMMODE1 == 0 : SB_LMMODE0 == 0;
	ch2DdtBurstBytes = 0;
	ch2DdtActive = ch2DdtRemaining != 0;
	DMAC_CHCR(2).TE = 0;
	if (!ch2DdtActive) {
		SB_C2DST = 0;
		asic_RaiseInterrupt(holly_CH2_DMA);
		return;
	}
	reevaluateDmacBus();
}

static const InterruptID dmac_itr[] = { sh4_DMAC_DMTE0, sh4_DMAC_DMTE1, sh4_DMAC_DMTE2, sh4_DMAC_DMTE3 };
static int dmacSched[4] = { -1, -1, -1, -1 };
static bool dmacActive[4];
static int dmacCurrentChannel = -1;
static u8 dmacRoundRobinOrder[4] = { 0, 1, 2, 3 };
static bool dmacAwaitingBusGrant;
static int dmacBurstOwner = -1;
static constexpr int dmacBusGrantCycles = 2; // one 100 MHz external-bus cycle

bool DMAC_IsBusTransferInProgress()
{
	return dmacSched[0] >= 0 && dmacCurrentChannel >= 0 && !dmacAwaitingBusGrant
			&& sh4_sched_is_scheduled(dmacSched[0]);
}

static u32 transferSize(int ch)
{
	switch (DMAC_CHCR(ch).TS)
	{
	case 0: return 8;
	case 1: return 1;
	case 2: return 2;
	case 4: return 32;
	default: return 4;
	}
}

static int addressIncrement(u32 mode, u32 size)
{
	return mode == 1 ? size : mode == 2 ? -(int)size : 0;
}

static int transferCycles(int ch)
{
	static const Sh4Cycles busCycles;
	if (ch == 0 && pvrDmaActive) {
		const u32 readAddress = pvrDmaToSystem ? pvrDmaDst : pvrDmaSrc;
		const u32 writeAddress = pvrDmaToSystem ? pvrDmaSrc : pvrDmaDst;
		return std::max(1, busCycles.dmaReadAccessCycles(readAddress, 32)
				+ busCycles.dmaWriteAccessCycles(writeAddress, 32));
	}
	const bool ddt = ch == 2 && ch2DdtActive;
	const u32 size = ddt ? 32 : transferSize(ch);
	const u32 src = ddt ? ch2DdtSrc : DMAC_SAR(ch);
	const u32 dst = ddt ? (0x10000000 | ch2DdtDst) : DMAC_DAR(ch);
	return std::max(1, busCycles.dmaReadAccessCycles(src, size)
			+ busCycles.dmaWriteAccessCycles(dst, size));
}

static int beginTransferCycles(int ch)
{
	if (dmacBurstOwner < 0 && (DMAC_CHCR(ch).TM || (ch == 0 && pvrDmaActive)
			|| (ch == 2 && ch2DdtActive)))
		dmacBurstOwner = ch;
	return transferCycles(ch);
}

static bool channelEnabled(int ch)
{
	if (ch == 0 && pvrDmaActive)
		return true;
	if (ch == 2 && ch2DdtActive)
		return true;
	return DMAC_CHCR(ch).RS == 4 && DMAC_CHCR(ch).DE && !DMAC_CHCR(ch).TE
			&& DMAC_DMAOR.DME && !DMAC_DMAOR.AE && !DMAC_DMAOR.NMIF;
}

static void updateDmacInterrupt(int ch)
{
	InterruptPend(dmac_itr[ch], DMAC_CHCR(ch).TE);
	InterruptMask(dmac_itr[ch], DMAC_CHCR(ch).IE);
}

static int selectChannel()
{
	static constexpr u8 fixedPriority[3][4] = {
		{ 0, 1, 2, 3 }, { 0, 2, 3, 1 }, { 2, 0, 1, 3 }
	};
	const unsigned priorityMode = (DMAC_DMAOR.full >> 8) & 3;
	const u8 *order = priorityMode == 3 ? dmacRoundRobinOrder : fixedPriority[priorityMode];
	for (int i = 0; i < 4; ++i) {
		const int ch = order[i];
		if (dmacActive[ch] && channelEnabled(ch))
			return ch;
	}
	return -1;
}

static void lowerRoundRobinPriority(int ch)
{
	if (((DMAC_DMAOR.full >> 8) & 3) != 3)
		return;
	int position = 0;
	while (position < 4 && dmacRoundRobinOrder[position] != ch)
		++position;
	for (; position < 3; ++position)
		dmacRoundRobinOrder[position] = dmacRoundRobinOrder[position + 1];
	dmacRoundRobinOrder[3] = ch;
}

static void resetRoundRobinPriorityIfIdle()
{
	if (((DMAC_DMAOR.full >> 8) & 3) != 3)
		return;
	for (int ch = 0; ch < 4; ++ch)
		if (dmacActive[ch] && channelEnabled(ch))
			return;
	for (int ch = 0; ch < 4; ++ch)
		dmacRoundRobinOrder[ch] = ch;
}

static void reevaluateDmacBus()
{
	for (int ch = 0; ch < 4; ++ch)
		dmacActive[ch] = channelEnabled(ch);
	if (dmacCurrentChannel >= 0 && !dmacActive[dmacCurrentChannel]) {
		if (!dmacAwaitingBusGrant && sh4_sched_is_scheduled(dmacSched[0]))
			// A transfer request has already been accepted. Stop conditions are
			// sampled at the next transfer-unit boundary, so let it complete.
			return;
		sh4_sched_request(dmacSched[0], -1);
		dmacCurrentChannel = -1;
		dmacAwaitingBusGrant = false;
	}
	if (dmacBurstOwner >= 0 && !channelEnabled(dmacBurstOwner)
			&& dmacAwaitingBusGrant)
		dmacBurstOwner = -1;
	if (dmacAwaitingBusGrant && dmacCurrentChannel >= 0)
		dmacCurrentChannel = selectChannel();
	if (dmacCurrentChannel < 0) {
		dmacCurrentChannel = selectChannel();
		if (dmacCurrentChannel >= 0) {
			dmacAwaitingBusGrant = false;
			sh4_sched_request(dmacSched[0], beginTransferCycles(dmacCurrentChannel));
		}
		else
		{
			resetRoundRobinPriorityIfIdle();
			bsc.endSdramDmaPipeline();
		}
	}
}

void DMAC_AbortPvrDma()
{
	if (!pvrDmaActive)
	{
		SB_PDST = 0;
		return;
	}

	pvrDmaActive = false;
	SB_PDST = 0;
	if (dmacCurrentChannel == 0)
	{
		sh4_sched_request(dmacSched[0], -1);
		dmacCurrentChannel = -1;
		dmacAwaitingBusGrant = false;
		dmacBurstOwner = -1;
		bsc.endSdramDmaPipeline();
	}
	reevaluateDmacBus();
}

void DMAC_AbortCh2Dma()
{
	if (!ch2DdtActive)
	{
		SB_C2DST = 0;
		return;
	}

	ch2DdtActive = false;
	ch2DdtBurstBytes = 0;
	SB_C2DST = 0;
	if (dmacCurrentChannel == 2)
	{
		sh4_sched_request(dmacSched[0], -1);
		dmacCurrentChannel = -1;
		dmacAwaitingBusGrant = false;
		dmacBurstOwner = -1;
		bsc.endSdramDmaPipeline();
	}
	reevaluateDmacBus();
}

static void transferUnit(int ch)
{
	if (ch == 0 && pvrDmaActive) {
		const u32 amount = std::min(32u, pvrDmaRemaining);
		if (pvrDmaToSystem)
			WriteMemBlock_nommu_dma(pvrDmaSrc, pvrDmaDst, amount);
		else
			WriteMemBlock_nommu_dma(pvrDmaDst, pvrDmaSrc, amount);
		pvrDmaSrc += amount;
		pvrDmaDst += amount;
		pvrDmaRemaining -= amount;
		SB_PDSTARD = pvrDmaSrc;
		SB_PDSTAPD = pvrDmaDst;
		SB_PDLEND += amount;
		DMAC_SAR(0) = pvrDmaSrc;
		if (pvrDmaRemaining == 0) {
			pvrDmaActive = false;
			DMAC_CHCR(0).TE = 1;
			DMAC_DMATCR(0) = 0;
			SB_PDST = 0;
			asic_RaiseInterrupt(holly_PVR_DMA);
		}
		return;
	}
	if (ch == 2 && ch2DdtActive) {
		const u32 amount = std::min(32u, ch2DdtRemaining);
		if ((ch2DdtDst & 0x01000000) == 0) {
			SQBuffer buffer{};
			for (u32 offset = 0; offset < amount; ++offset)
				((u8 *)&buffer)[offset] = addrspace::read8(ch2DdtSrc + offset);
			TAWrite(ch2DdtDst, &buffer, 1);
		}
		else if (ch2DdtPath64) {
			const u32 dst = (ch2DdtDst & 0x00ffffff) | 0xa4000000;
			WriteMemBlock_nommu_dma(dst, ch2DdtSrc, amount);
		}
		else {
			const u32 dst = (ch2DdtDst & 0x00ffffff) | 0xa5000000;
			for (u32 offset = 0; offset < amount; offset += 4)
				pvr_write32p<u32>(dst + offset, ReadMem32_nommu(ch2DdtSrc + offset));
		}
		ch2DdtSrc += amount;
		if (ch2DdtDst & 0x01000000)
			ch2DdtDst += amount;
		ch2DdtRemaining -= amount;
		ch2DdtBurstBytes += amount;
		DMAC_SAR(2) = ch2DdtSrc;
		DMAC_DMATCR(2) = ch2DdtRemaining / 32;
		SB_C2DLEN = ch2DdtRemaining;
		if (ch2DdtDst & 0x01000000)
			SB_C2DSTAT += amount;
		if (ch2DdtRemaining == 0) {
			ch2DdtActive = false;
			ch2DdtBurstBytes = 0;
			DMAC_CHCR(2).TE = 1;
			DMAC_DMATCR(2) = 0;
			SB_C2DST = 0;
			SB_C2DLEN = 0;
			asic_RaiseInterrupt(holly_CH2_DMA);
		}
		else
		{
			const u32 maxBurst = SB_C2DMAXL == 1 ? 128
					: SB_C2DMAXL == 2 ? 256 : 1024;
			if (ch2DdtBurstBytes >= maxBurst)
			{
				ch2DdtBurstBytes = 0;
				if (dmacBurstOwner == 2)
					dmacBurstOwner = -1;
			}
		}
		return;
	}
	const u32 size = transferSize(ch);
	u32 src = DMAC_SAR(ch);
	u32 dst = DMAC_DAR(ch);
	if (size == 32) {
		for (u32 offset = 0; offset < 32; offset += 4)
			addrspace::write32(dst + offset, addrspace::read32(src + offset));
	}
	else if (size == 8)
		addrspace::write64(dst, addrspace::read64(src));
	else if (size == 4)
		addrspace::write32(dst, addrspace::read32(src));
	else if (size == 2)
		addrspace::write16(dst, addrspace::read16(src));
	else
		addrspace::write8(dst, addrspace::read8(src));

	DMAC_SAR(ch) += addressIncrement(DMAC_CHCR(ch).SM, size);
	DMAC_DAR(ch) += addressIncrement(DMAC_CHCR(ch).DM, size);
	const u32 oldCount = DMAC_DMATCR(ch) & 0x00ffffff;
	DMAC_DMATCR(ch) = oldCount == 0 ? 0x00ffffff : oldCount - 1;
	if (oldCount == 1) {
		DMAC_CHCR(ch).TE = 1;
		dmacActive[ch] = false;
		updateDmacInterrupt(ch);
	}
}

static int dmacCallback(int, int, int jitter, void *)
{
	if (dmacCurrentChannel < 0
			|| (dmacAwaitingBusGrant && !channelEnabled(dmacCurrentChannel))) {
		dmacCurrentChannel = -1;
		dmacAwaitingBusGrant = false;
		dmacBurstOwner = -1;
		return 0;
	}
	int remainingJitter = jitter;
	while (true) {
		if (dmacAwaitingBusGrant) {
			dmacAwaitingBusGrant = false;
			const int transfer = beginTransferCycles(dmacCurrentChannel);
			if (remainingJitter < transfer)
				return jitter + transfer - remainingJitter;
			remainingJitter -= transfer;
		}

		const int ch = dmacCurrentChannel;
		transferUnit(ch);
		bsc.servicePendingRefresh();
		lowerRoundRobinPriority(ch);
		for (int candidate = 0; candidate < 4; ++candidate)
			dmacActive[candidate] = channelEnabled(candidate);
		if (dmacBurstOwner >= 0 && !channelEnabled(dmacBurstOwner))
			dmacBurstOwner = -1;
		dmacCurrentChannel = selectChannel();
		if (dmacCurrentChannel < 0) {
			resetRoundRobinPriorityIfIdle();
			bsc.endSdramDmaPipeline();
			return 0;
		}

		const bool releaseBus = dmacBurstOwner < 0;
		dmacAwaitingBusGrant = releaseBus;
		if (releaseBus)
			bsc.endSdramDmaPipeline();
		const int nextCycles = releaseBus ? dmacBusGrantCycles
				: beginTransferCycles(dmacCurrentChannel);
		if (remainingJitter < nextCycles)
			// handle_cb subtracts the original jitter from this return value.
			return jitter + nextCycles - remainingJitter;
		remainingJitter -= nextCycles;
	}
}

void DMAC_AddBusAccessCycles(Sh4Context *ctx, int accessCycles)
{
	if (dmacCurrentChannel < 0 || !sh4_sched_is_scheduled(dmacSched[0])) {
		ctx->cycle_counter -= accessCycles;
		return;
	}
	if (dmacAwaitingBusGrant) {
		// Cycle steal has returned the bus. A pending CPU transaction wins this
		// arbitration opportunity and postpones the next DMAC grant.
		sh4_sched_request(dmacSched[0], -1);
		ctx->cycle_counter -= accessCycles;
		sh4_sched_request(dmacSched[0], dmacBusGrantCycles);
		return;
	}

	// A transfer unit already accepted by the DMAC owns the external bus. Wait
	// for it, commit it before the CPU access, and cancel its normal callback so
	// that the same unit cannot be committed twice.
	int remaining = std::max(0, sh4_sched_cycles_until(dmacSched[0]));
	ctx->cycle_counter -= remaining;
	sh4_sched_request(dmacSched[0], -1);
	while (dmacCurrentChannel >= 0) {
		const int ch = dmacCurrentChannel;
		transferUnit(ch);
		bsc.servicePendingRefresh();
		lowerRoundRobinPriority(ch);
		for (int candidate = 0; candidate < 4; ++candidate)
			dmacActive[candidate] = channelEnabled(candidate);
		dmacCurrentChannel = selectChannel();
		if (dmacBurstOwner >= 0 && !channelEnabled(dmacBurstOwner))
			dmacBurstOwner = -1;

		// Cycle-steal mode returns the bus after one transfer unit. A burst owner
		// keeps it even while round-robin arbitration interleaves another channel.
		if (dmacBurstOwner < 0) {
			dmacAwaitingBusGrant = dmacCurrentChannel >= 0;
			bsc.endSdramDmaPipeline();
			break;
		}
		if (dmacCurrentChannel < 0)
			break;
		ctx->cycle_counter -= beginTransferCycles(dmacCurrentChannel);
	}

	if (dmacCurrentChannel < 0)
	{
		resetRoundRobinPriorityIfIdle();
		bsc.endSdramDmaPipeline();
	}

	// The CPU now owns the bus for this access. Only after it finishes may a
	// pending cycle-steal request or the next burst acquire the bus.
	ctx->cycle_counter -= accessCycles;
	if (dmacCurrentChannel >= 0)
		sh4_sched_request(dmacSched[0], dmacAwaitingBusGrant
				? dmacBusGrantCycles : beginTransferCycles(dmacCurrentChannel));
}

template<u32 ch>
static void WriteCHCR(u32 addr, u32 data)
{
	if constexpr (ch == 0 || ch == 1)
		DMAC_CHCR(ch).full = data & 0xff0ffff7;
	else
		// no AL or RL on channels 2 and 3
		DMAC_CHCR(ch).full = data & 0xff0afff7;
	if constexpr (ch == 2)
	{
		// Clearing DE terminates a transfer without setting TE. A CPU write
		// reaches this handler only after any already accepted bus unit has
		// completed through DMAC_AddBusAccessCycles().
		if (!DMAC_CHCR(2).DE && SB_C2DST != 0)
			DMAC_AbortCh2Dma();
	}

	updateDmacInterrupt(ch);
	reevaluateDmacBus();
}

static void writeDMAOR(u32, u32 data)
{
	DMAC_DMAOR.full = data & 0x00008307;
	if ((DMAC_DMAOR.full & DMAOR_MASK) != DMAOR_DDT_ENABLED)
	{
		// DDT transfers end without TE when DME is cleared, AE/NMIF is set,
		// or DDT mode itself is cleared.
		DMAC_AbortCh2Dma();
		DMAC_AbortPvrDma();
	}
	reevaluateDmacBus();
}

void DMAC_Nmi()
{
	DMAC_DMAOR.NMIF = 1;
	// NMI terminates direct data transfers without setting TE. Normal DMA
	// channels stop at their next transfer-unit boundary through the NMIF gate.
	DMAC_AbortCh2Dma();
	DMAC_AbortPvrDma();
	reevaluateDmacBus();
}

//Init term res
void DMACRegisters::init()
{
	super::init();
	for (int ch = 0; ch < 4; ++ch)
		dmacSched[ch] = sh4_sched_register(ch, dmacCallback);

	//DMAC SAR0 0xFFA00000 0x1FA00000 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_SAR0_addr>();

	//DMAC DAR0 0xFFA00004 0x1FA00004 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_DAR0_addr>();

	//DMAC DMATCR0 0xFFA00008 0x1FA00008 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_DMATCR0_addr, u32, 0x00ffffff>();

	//DMAC CHCR0 0xFFA0000C 0x1FA0000C 32 0x00000000 0x00000000 Held Held Bclk
	setWriteHandler<DMAC_CHCR0_addr>(WriteCHCR<0>);

	//DMAC SAR1 0xFFA00010 0x1FA00010 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_SAR1_addr>();

	//DMAC DAR1 0xFFA00014 0x1FA00014 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_DAR1_addr>();

	//DMAC DMATCR1 0xFFA00018 0x1FA00018 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_DMATCR1_addr, u32, 0x00ffffff>();

	//DMAC CHCR1 0xFFA0001C 0x1FA0001C 32 0x00000000 0x00000000 Held Held Bclk
	setWriteHandler<DMAC_CHCR1_addr>(WriteCHCR<1>);

	//DMAC SAR2 0xFFA00020 0x1FA00020 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_SAR2_addr>();

	//DMAC DAR2 0xFFA00024 0x1FA00024 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_DAR2_addr>();

	//DMAC DMATCR2 0xFFA00028 0x1FA00028 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_DMATCR2_addr, u32, 0x00ffffff>();

	//DMAC CHCR2 0xFFA0002C 0x1FA0002C 32 0x00000000 0x00000000 Held Held Bclk
	setWriteHandler<DMAC_CHCR2_addr>(WriteCHCR<2>);

	//DMAC SAR3 0xFFA00030 0x1FA00030 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_SAR3_addr>();

	//DMAC DAR3 0xFFA00034 0x1FA00034 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_DAR3_addr>();

	//DMAC DMATCR3 0xFFA00038 0x1FA00038 32 Undefined Undefined Held Held Bclk
	setRW<DMAC_DMATCR3_addr, u32, 0x00ffffff>();

	//DMAC CHCR3 0xFFA0003C 0x1FA0003C 32 0x00000000 0x00000000 Held Held Bclk
	setWriteHandler<DMAC_CHCR3_addr>(WriteCHCR<3>);

	//DMAC DMAOR 0xFFA00040 0x1FA00040 32 0x00000000 0x00000000 Held Held Bclk
	setWriteHandler<DMAC_DMAOR_addr>(writeDMAOR);

	reset();
}

void DMACRegisters::reset()
{
	super::reset();
	for (int ch = 0; ch < 4; ++ch) {
		dmacActive[ch] = false;
		sh4_sched_request(dmacSched[ch], -1);
		updateDmacInterrupt(ch);
		dmacRoundRobinOrder[ch] = ch;
	}
	dmacCurrentChannel = -1;
	dmacAwaitingBusGrant = false;
	dmacBurstOwner = -1;
	ch2DdtActive = false;
	ch2DdtSrc = ch2DdtDst = ch2DdtRemaining = 0;
	ch2DdtPath64 = false;
	ch2DdtBurstBytes = 0;
	pvrDmaActive = pvrDmaToSystem = false;
	pvrDmaSrc = pvrDmaDst = pvrDmaRemaining = 0;
}

void DMACRegisters::term()
{
	for (int& sched : dmacSched) {
		sh4_sched_unregister(sched);
		sched = -1;
	}
	dmacCurrentChannel = -1;
	dmacAwaitingBusGrant = false;
	dmacBurstOwner = -1;
	ch2DdtActive = false;
	pvrDmaActive = false;
	super::term();
}

void DMACRegisters::serialize(Serializer& ser)
{
	ser << dmacActive;
	for (int sched : dmacSched)
		sh4_sched_serialize(ser, sched);
	ser << dmacCurrentChannel;
	ser << dmacRoundRobinOrder;
	ser << dmacAwaitingBusGrant;
	ser << dmacBurstOwner;
	ser << ch2DdtActive << ch2DdtSrc << ch2DdtDst << ch2DdtRemaining << ch2DdtPath64;
	ser << ch2DdtBurstBytes;
	ser << pvrDmaActive << pvrDmaToSystem << pvrDmaSrc << pvrDmaDst << pvrDmaRemaining;
}

void DMACRegisters::deserialize(Deserializer& deser)
{
	if (deser.version() >= Deserializer::V74) {
		deser >> dmacActive;
		for (int sched : dmacSched)
			sh4_sched_deserialize(deser, sched);
		if (deser.version() >= Deserializer::V76) {
			deser >> dmacCurrentChannel;
			deser >> dmacRoundRobinOrder;
			if (deser.version() >= Deserializer::V81)
				deser >> dmacAwaitingBusGrant >> dmacBurstOwner;
			else {
				dmacAwaitingBusGrant = false;
				dmacBurstOwner = dmacCurrentChannel >= 0
						&& DMAC_CHCR(dmacCurrentChannel).TM ? dmacCurrentChannel : -1;
			}
			if (deser.version() >= Deserializer::V83)
				deser >> ch2DdtActive >> ch2DdtSrc >> ch2DdtDst
						>> ch2DdtRemaining >> ch2DdtPath64;
			else {
				ch2DdtActive = false;
				ch2DdtSrc = ch2DdtDst = ch2DdtRemaining = 0;
				ch2DdtPath64 = false;
			}
			if (deser.version() >= Deserializer::V101)
				deser >> ch2DdtBurstBytes;
			else
				ch2DdtBurstBytes = 0;
			if (deser.version() >= Deserializer::V85)
				deser >> pvrDmaActive >> pvrDmaToSystem >> pvrDmaSrc
						>> pvrDmaDst >> pvrDmaRemaining;
			else {
				pvrDmaActive = pvrDmaToSystem = false;
				pvrDmaSrc = pvrDmaDst = pvrDmaRemaining = 0;
			}
		}
		else {
			dmacCurrentChannel = -1;
			dmacAwaitingBusGrant = false;
			dmacBurstOwner = -1;
			ch2DdtActive = false;
			ch2DdtSrc = ch2DdtDst = ch2DdtRemaining = 0;
			ch2DdtPath64 = false;
			ch2DdtBurstBytes = 0;
			pvrDmaActive = pvrDmaToSystem = false;
			pvrDmaSrc = pvrDmaDst = pvrDmaRemaining = 0;
			for (int ch = 0; ch < 4; ++ch) {
				sh4_sched_request(dmacSched[ch], -1);
				dmacRoundRobinOrder[ch] = ch;
			}
			reevaluateDmacBus();
		}
	}
	else {
		for (int ch = 0; ch < 4; ++ch) {
			dmacActive[ch] = false;
			dmacRoundRobinOrder[ch] = ch;
		}
		dmacCurrentChannel = -1;
		dmacAwaitingBusGrant = false;
		dmacBurstOwner = -1;
		ch2DdtActive = false;
		ch2DdtSrc = ch2DdtDst = ch2DdtRemaining = 0;
		ch2DdtPath64 = false;
		ch2DdtBurstBytes = 0;
		pvrDmaActive = pvrDmaToSystem = false;
		pvrDmaSrc = pvrDmaDst = pvrDmaRemaining = 0;
		reevaluateDmacBus();
	}
	for (int ch = 0; ch < 4; ++ch)
		updateDmacInterrupt(ch);
}
