/*
	PVR-SB handling
	DMA hacks are here
*/

#include "pvr_sb_regs.h"
#include "ta.h"
#include "ta_structs.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/modules/dmac.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_cycles.h"
#include "serialize.h"

static bool isPvrDmaAddress(u32 addr);

static void RegWrite_SB_C2DST(u32 addr, u32 data)
{
	if (data & 1)
	{
		if (SB_C2DST == 0)
		{
			SB_C2DST = 1;
			DMAC_Ch2St();
		}
	}
	else if (SB_C2DST != 0)
		// Hardware permits this write while active specifically to interrupt
		// ch2-DMA. No transfer-complete interrupt is generated.
		DMAC_AbortCh2Dma();
}

//PVR-DMA
static void do_pvr_dma()
{
	u32 src = SB_PDSTAR;	// System RAM address
	u32 dst = SB_PDSTAP;	// VRAM address
	// A zero length encodes the maximum 16 MiB transfer.
	u32 len = SB_PDLEN == 0 ? 0x01000000 : SB_PDLEN;

	if (!isPvrDmaAddress(dst) || ((src >> 26) & 7) != 3)
	{
		SB_PDST = 0;
		asic_RaiseInterrupt(holly_PVR_ILLADDR);
		return;
	}

	const u32 protectedStart = 0x08000000 | (((SB_PDAPRO >> 8) & 0x7f) << 20);
	const u32 protectedEnd = 0x080fffff | ((SB_PDAPRO & 0x7f) << 20);
	const u64 transferEnd = (u64)src + len - 1;
	if (src < protectedStart || transferEnd > protectedEnd)
	{
		SB_PDST = 0;
		asic_RaiseInterrupt(holly_PVR_OVERRUN);
		return;
	}

	if (DMAOR_DDT_ENABLED != (DMAC_DMAOR.full & DMAOR_MASK))
	{
		INFO_LOG(PVR, "DMAC: DMAOR has invalid settings (%X) !", DMAC_DMAOR.full);
		SB_PDST = 0;
		return;
	}
	DEBUG_LOG(PVR, "PVR-DMA %x %s %x len %x", src, SB_PDDIR ? "<-" : "->", dst, len);

	DMAC_StartPvrDma(src, dst, len, SB_PDDIR != 0);
}

static void RegWrite_SB_PDST(u32 addr, u32 data)
{
	// SB_PDST is only a start source in software-trigger mode. In hardware
	// mode the selected HOLLY interrupt drives the same start logic.
	if ((data & 1) && SB_PDEN && SB_PDTSEL == 0 && SB_PDST == 0)
	{
		SB_PDST = 1;
		do_pvr_dma();
	}
}

static bool isPvrDmaAddress(u32 addr)
{
	return (addr >= 0x005f8000 && addr <= 0x005f9fe0)
			|| (addr >= 0x025f8000 && addr <= 0x025f9fe0)
			|| (addr >= 0x04000000 && addr <= 0x047fffe0)
			|| (addr >= 0x05000000 && addr <= 0x057fffe0)
			|| (addr >= 0x06000000 && addr <= 0x067fffe0)
			|| (addr >= 0x07000000 && addr <= 0x077fffe0);
}

static void RegWrite_SB_PDSTAP(u32 addr, u32 data)
{
	SB_PDSTAP = data & 0x1fffffe0;
	if (!isPvrDmaAddress(data))
		asic_RaiseInterrupt(holly_PVR_ILLADDR);
}

static void RegWrite_SB_PDSTAR(u32 addr, u32 data)
{
	SB_PDSTAR = data & 0x1fffffe0;
	if (data < 0x0c000000 || data > 0x0fffffe0)
		asic_RaiseInterrupt(holly_PVR_ILLADDR);
}

static void RegWrite_SB_PDAPRO(u32 addr, u32 data)
{
	if ((data >> 16) == 0x6702)
		SB_PDAPRO = data & 0x00007f7f;
}

static void RegWrite_SB_PDEN(u32 addr, u32 data)
{
	SB_PDEN = data & 1;
	if (SB_PDEN == 0)
		DMAC_AbortPvrDma();
}

void pvr_sb_interrupt()
{
	if (SB_PDTSEL == 1 && SB_PDEN && SB_PDST == 0)
	{
		SB_PDST = 1;
		do_pvr_dma();
	}
}

// Auto sort DMA
int sort_dma_schid = -1;
static u32 sortDmaLinkAddress;
static bool sortDmaStopRequested;
static bool sortDmaHeaderPending;
static bool sortDmaObjectPending;
static bool sortDmaLegacyObject;
static u32 sortDmaObjectAddress;
static u32 sortDmaObjectSize;
static u32 sortDmaObjectIndex;
static u32 sortDmaNextAddress;
static u32 sortDmaTableBaseAddress;

static int sortDmaRamReadCycles(u32 address, u32 size)
{
	Sh4Cycles cycles;
	return std::max(1, cycles.dmaReadAccessCycles(
			0x0c000000 | (address & RAM_MASK), size));
}

static u32 calculate_start_link_addr()
{
	const u32 entrySize = SB_SDWLT == 0 ? sizeof(u16) : sizeof(u32);
	u8* base = &mem_b[(sortDmaTableBaseAddress + SB_SDDIV * entrySize) & RAM_MASK];
	u32 rv;
	if (SB_SDWLT == 0)
		//16b width
		rv = *(u16*)base;
	else
		//32b width
		rv = *(u32*)base;
	SB_SDDIV++; //next index
	// Start-link entries are fetched through 32-byte system-bus blocks. The
	// visible register advances to the next block as soon as one is fetched.
	const u32 consumed = SB_SDDIV * entrySize;
	SB_SDSTAW = ((sortDmaTableBaseAddress + ((consumed + 31) & ~31u)) & 0x07ffffe0)
			| 0x08000000;

	return rv;
}

static bool isValidSortDmaLink(u32 link)
{
	if (link <= 2)
		return true;
	if (SB_SDLAS == 0)
		// Unshifted links are byte offsets. Values 0x20, 0x40 and 0x60
		// are explicitly outside the hardware command format.
		return link >= 0x80 && link <= 0x07ffffe0 && (link & 0x1f) == 0;
	// Shifted links are 32-byte indices. Values 1 and 2 are terminators,
	// leaving 3 prohibited and 4 as the first object offset.
	return link >= 4 && link <= 0x003fffff;
}

static void finish_sort_dma(bool completed)
{
	bsc.endSdramDmaPipeline();
	SB_SDST = 0;
	sortDmaStopRequested = false;
	sortDmaHeaderPending = false;
	sortDmaObjectPending = false;
	sortDmaLegacyObject = false;
	if (completed)
	{
		asic_RaiseInterrupt(holly_PVR_SortDMA);
	}
}

static int sort_dma_sched(int tag, int cycles, int jitter, void *arg)
{
	int remainingJitter = std::max(0, jitter);
	for (;;)
	{
	if (sortDmaObjectPending)
	{
		if (sortDmaLegacyObject) {
			// Pre-V104 states scheduled one deadline for the complete object.
			// Preserve that already-elapsed timing when importing such a state.
			while (sortDmaObjectIndex < sortDmaObjectSize) {
				const SQBuffer *transfer = (const SQBuffer *)&mem_b[
						(sortDmaObjectAddress + sortDmaObjectIndex * sizeof(SQBuffer)) & RAM_MASK];
				ta_vtx_data32(transfer);
				sortDmaObjectIndex++;
			}
			sortDmaLegacyObject = false;
		}
		else {
			const SQBuffer *transfer = (const SQBuffer *)&mem_b[
					(sortDmaObjectAddress + sortDmaObjectIndex * sizeof(SQBuffer)) & RAM_MASK];
			ta_vtx_data32(transfer);
			sortDmaObjectIndex++;
		}
		if (sortDmaObjectIndex < sortDmaObjectSize) {
			const u32 nextAddress = sortDmaObjectAddress
					+ sortDmaObjectIndex * sizeof(SQBuffer);
			const int nextCycles = sortDmaRamReadCycles(nextAddress, sizeof(SQBuffer));
			if (nextCycles > remainingJitter)
				return std::max(0, jitter) + nextCycles - remainingJitter;
			remainingJitter -= nextCycles;
			continue;
		}
		sortDmaLinkAddress = sortDmaNextAddress;
		sortDmaObjectPending = false;
	}
	if (sortDmaStopRequested)
	{
		// Stop at the current object boundary without a completion interrupt.
		finish_sort_dma(false);
		return 0;
	}
	if (sortDmaHeaderPending)
	{
		const u32 headerAddress = sortDmaObjectAddress
				+ sortDmaObjectIndex * sizeof(SQBuffer);
		u32 *parameters = (u32 *)&mem_b[headerAddress & RAM_MASK];
		PCW pcw;
		pcw.full = parameters[0];
		// HOLLY starts forwarding the list before it has located the Global
		// Parameter. Control Parameters preceding it therefore reach the TA too.
		ta_vtx_data32((const SQBuffer *)parameters);
		sortDmaObjectIndex++;
		if (pcw.ParaType != ParamType_Polygon_or_Modifier_Volume
				&& pcw.ParaType != ParamType_Sprite)
		{
			// The Global Parameter must start less than 128 bytes from the
			// linked address, so offsets 0, 32, 64 and 96 are searched.
			if (sortDmaObjectIndex >= 4)
			{
				finish_sort_dma(false);
				asic_RaiseInterrupt(holly_SORT_DMA_ERROR);
				return 0;
			}
			const int nextCycles = sortDmaRamReadCycles(
					sortDmaObjectAddress + sortDmaObjectIndex * sizeof(SQBuffer),
					sizeof(SQBuffer));
			if (nextCycles > remainingJitter)
				return std::max(0, jitter) + nextCycles - remainingJitter;
			remainingJitter -= nextCycles;
			continue;
		}

		const u32 rawSize = parameters[0x18 >> 2];
		u32 size = rawSize;
		if (size == 0)
			size = 256;
		const bool polygonType1 = pcw.ParaType == ParamType_Polygon_or_Modifier_Volume
				&& pcw.Volume == 0 && pcw.Col_Type == 1 && pcw.Offset == 0;
		if (polygonType1 || rawSize > 0xff || size < 4
				|| size < sortDmaObjectIndex)
		{
			finish_sort_dma(false);
			asic_RaiseInterrupt(holly_SORT_DMA_ERROR);
			return 0;
		}

		sortDmaHeaderPending = false;
		sortDmaObjectPending = true;
		sortDmaLegacyObject = false;
		sortDmaObjectSize = size;
		// Preserve reserved upper bits so the next iteration can reject an
		// incorrectly formatted link instead of silently aliasing it into RAM.
		sortDmaNextAddress = parameters[0x1c >> 2];
		if (sortDmaObjectIndex == sortDmaObjectSize)
		{
			sortDmaLinkAddress = sortDmaNextAddress;
			sortDmaObjectPending = false;
			continue;
		}
		const int nextCycles = sortDmaRamReadCycles(
				sortDmaObjectAddress + sortDmaObjectIndex * sizeof(SQBuffer),
				sizeof(SQBuffer));
		if (nextCycles > remainingJitter)
			return std::max(0, jitter) + nextCycles - remainingJitter;
		remainingJitter -= nextCycles;
		continue;
	}
	int linkReadCycles = 0;
	if (sortDmaLinkAddress == 1) {
		linkReadCycles = sortDmaRamReadCycles(SB_SDSTAW,
				SB_SDWLT == 0 ? sizeof(u16) : sizeof(u32));
		sortDmaLinkAddress = calculate_start_link_addr();
	}
	if (sortDmaLinkAddress == 2)
	{
		if (linkReadCycles != 0) {
			if (linkReadCycles > remainingJitter)
				return std::max(0, jitter) + linkReadCycles - remainingJitter;
			remainingJitter -= linkReadCycles;
			continue;
		}
		finish_sort_dma(true);
		return 0;
	}
	if (!isValidSortDmaLink(sortDmaLinkAddress))
	{
		// A start-table entry is not interpreted until its SDRAM read has
		// completed. Object links arrive here with no additional table delay.
		if (linkReadCycles != 0) {
			if (linkReadCycles > remainingJitter)
				return std::max(0, jitter) + linkReadCycles - remainingJitter;
			remainingJitter -= linkReadCycles;
			continue;
		}
		finish_sort_dma(false);
		asic_RaiseInterrupt(holly_SORT_DMA_ERROR);
		return 0;
	}

	u32 linkAddress = sortDmaLinkAddress;
	if (SB_SDLAS == 1)
		linkAddress *= 32;
	else
		linkAddress &= ~31;

	sortDmaHeaderPending = true;
	sortDmaObjectAddress = (SB_SDBAAW + linkAddress) & RAM_MASK;
	sortDmaObjectIndex = 0;
	const int nextCycles = linkReadCycles
			+ sortDmaRamReadCycles(sortDmaObjectAddress, sizeof(SQBuffer));
	if (nextCycles > remainingJitter)
		return std::max(0, jitter) + nextCycles - remainingJitter;
	remainingJitter -= nextCycles;
	}
}

static void RegWrite_SB_SDST(u32 addr, u32 data)
{
	if ((data & 1) && SB_SDST == 0)
	{
		DEBUG_LOG(PVR, "Sort-DMA SB_SDSTAW %x SB_SDBAAW %x", SB_SDSTAW, SB_SDBAAW);
		SB_SDST = 1;
		SB_SDDIV = 0;
		sortDmaTableBaseAddress = SB_SDSTAW;
		sortDmaStopRequested = false;
		sortDmaHeaderPending = false;
		sortDmaObjectPending = false;
		sortDmaLinkAddress = 1; // Fetch the first start-link entry on the bus event.
		sh4_sched_request(sort_dma_schid, 1);
	}
	else if (!(data & 1) && SB_SDST != 0)
	{
		sortDmaStopRequested = true;
		if (!sortDmaObjectPending)
			sh4_sched_request(sort_dma_schid, 1);
	}
}

void pvr_sb_Init()
{
	sort_dma_schid = sh4_sched_register(0, sort_dma_sched);
	// PVR-DMA validates both endpoints when they are programmed, not only
	// when a transfer is started.
	hollyRegs.setWriteHandler<SB_PDSTAP_addr>(RegWrite_SB_PDSTAP);
	hollyRegs.setWriteHandler<SB_PDSTAR_addr>(RegWrite_SB_PDSTAR);
	hollyRegs.setWriteOnly<SB_PDAPRO_addr>(RegWrite_SB_PDAPRO);
	hollyRegs.setWriteHandler<SB_PDEN_addr>(RegWrite_SB_PDEN);

	//0x005F7C18    SB_PDST RW  PVR-DMA start
	hollyRegs.setWriteHandler<SB_PDST_addr>(RegWrite_SB_PDST);

	//0x005F6808    SB_C2DST RW  ch2-DMA start 
	hollyRegs.setWriteHandler<SB_C2DST_addr>(RegWrite_SB_C2DST);

	//0x005F6820    SB_SDST RW  Sort-DMA start
	hollyRegs.setWriteHandler<SB_SDST_addr>(RegWrite_SB_SDST);
}

void pvr_sb_Term()
{
	sh4_sched_unregister(sort_dma_schid);
	sort_dma_schid = -1;
}

void pvr_sb_Reset(bool hard)
{
	if (sort_dma_schid != -1)
		sh4_sched_request(sort_dma_schid, -1);
	bsc.endSdramDmaPipeline();
	sortDmaLinkAddress = 0;
	sortDmaStopRequested = false;
	sortDmaHeaderPending = false;
	sortDmaObjectPending = false;
	sortDmaLegacyObject = false;
	sortDmaObjectAddress = sortDmaObjectSize = sortDmaObjectIndex = sortDmaNextAddress = 0;
	sortDmaTableBaseAddress = 0;
	SB_C2DST = 0;
	SB_SDST = 0;
	SB_PDDIR = 0;
	SB_PDTSEL = 0;
	SB_PDEN = 0;
	SB_PDST = 0;
}

void pvr_sb_Serialize(Serializer& ser)
{
	ser << sortDmaLinkAddress << sortDmaStopRequested << sortDmaObjectPending
			<< sortDmaLegacyObject
			<< sortDmaObjectAddress << sortDmaObjectSize << sortDmaObjectIndex
			<< sortDmaNextAddress << sortDmaTableBaseAddress << sortDmaHeaderPending;
}

void pvr_sb_Deserialize(Deserializer& deser)
{
	if (deser.version() >= Deserializer::V104)
		deser >> sortDmaLinkAddress >> sortDmaStopRequested >> sortDmaObjectPending
				>> sortDmaLegacyObject
				>> sortDmaObjectAddress >> sortDmaObjectSize >> sortDmaObjectIndex
				>> sortDmaNextAddress;
	else if (deser.version() >= Deserializer::V94) {
		deser >> sortDmaLinkAddress >> sortDmaStopRequested >> sortDmaObjectPending
				>> sortDmaObjectAddress >> sortDmaObjectSize >> sortDmaNextAddress;
		sortDmaObjectIndex = 0;
		sortDmaLegacyObject = sortDmaObjectPending;
	}
	else if (deser.version() >= Deserializer::V92) {
		deser >> sortDmaLinkAddress >> sortDmaStopRequested;
		sortDmaObjectPending = false;
		sortDmaLegacyObject = false;
		sortDmaObjectAddress = sortDmaObjectSize = sortDmaObjectIndex = sortDmaNextAddress = 0;
	}
	else
	{
		sortDmaLinkAddress = 0;
		sortDmaStopRequested = false;
		sortDmaObjectPending = false;
		sortDmaLegacyObject = false;
		sortDmaObjectAddress = sortDmaObjectSize = sortDmaObjectIndex = sortDmaNextAddress = 0;
		SB_SDST = 0;
	}
	if (deser.version() >= Deserializer::V121)
		deser >> sortDmaTableBaseAddress;
	else
		sortDmaTableBaseAddress = SB_SDSTAW;
	if (deser.version() >= Deserializer::V140)
		deser >> sortDmaHeaderPending;
	else
		sortDmaHeaderPending = false;
	// Pre-V144 states did not track progress while searching for a Global
	// Parameter. Restart that at the linked address when importing one.
	if (deser.version() < Deserializer::V144 && sortDmaHeaderPending)
		sortDmaObjectIndex = 0;
}
