//Bus state controller registers

#include "types.h"
#include "hw/sh4/sh4_mmr.h"

#include "hw/naomi/naomi.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/modules/dmac.h"
#include "cfg/option.h"
#include "serialize.h"
#include "modules.h"
#include <algorithm>

BSCRegisters bsc;

static int refreshSched = -1;
static u64 refreshLastCycle;
static u32 refreshFraction;
static s32 sdramOpenRows[4] = { -1, -1, -1, -1 };
static bool sdramDmaPipeline;
static bool refreshPending;
static u64 refreshBusBusyUntil;
static u64 refreshNow();

static u32 refreshBusDuration()
{
	const u32 tras = (BSC_MCR.full >> 10) & 7;
	const u32 trc = (BSC_MCR.full >> 27) & 7;
	// On the 64-bit interface one request issues two REF commands. The first
	// interval is 4 + TRAS and the second-to-next-command interval is
	// 4 + TRAS + 3*TRC CKIO cycles.
	return 8 + 2 * tras + 3 * trc;
}

static void startRefreshBusCycle()
{
	refreshPending = false;
	bsc.closeSdramRows();
	refreshBusBusyUntil = std::max(refreshBusBusyUntil, refreshNow())
			+ 2 * refreshBusDuration();
}

int BSCRegisters::takeRefreshBusCycles()
{
	if (refreshPending)
		startRefreshBusCycle();
	const u64 now = refreshNow();
	if (refreshBusBusyUntil <= now)
		return 0;
	const int cycles = (refreshBusBusyUntil - now + 1) / 2;
	// The caller serializes this wait ahead of its access. Mark it claimed so
	// a dual-address DMA read/write pair does not charge the interval twice.
	refreshBusBusyUntil = now;
	return cycles;
}

void BSCRegisters::servicePendingRefresh()
{
	if (refreshPending)
		startRefreshBusCycle();
}

static int sdramAccessCycles(u32 addr, bool write, bool dma)
{
	// Main RAM is four banks with 2-Kbyte rows on a 64-bit bus (4-Mbyte
	// banks on retail Dreamcasts, 8-Mbyte banks on 32-Mbyte arcade systems).
	// Every access is a four-beat 32-byte SDRAM burst; byte masks suppress
	// unwanted data during sub-line writes.
	const unsigned bankShift = RAM_SIZE == 32_MB ? 23 : 22;
	addr &= RAM_SIZE - 1;
	const unsigned bank = (addr >> bankShift) & 3;
	const int row = (addr >> 11) & ((1u << (bankShift - 11)) - 1);
	const int oldRow = sdramOpenRows[bank];
	int cycles;
	if (dma && sdramDmaPipeline) {
		// Successive DMAC commands overlap setup with the preceding four data
		// beats. Opening a row adds three cycles; replacing one adds PRE's two.
		if (oldRow == row)
			cycles = 4;
		else if (oldRow < 0)
			cycles = 7;
		else
			cycles = 9;
	}
	else if (oldRow == row)
		cycles = write ? 6 : 7;
	else if (oldRow < 0)
		cycles = write ? 7 : 10;
	else
		cycles = write ? 9 : 12;
	sdramDmaPipeline = dma;

	if (BSC_MCR.RASD)
		sdramOpenRows[bank] = row;
	else
		sdramOpenRows[bank] = -1;
	return cycles;
}

int BSCRegisters::sdramReadCycles(u32 addr) { return sdramAccessCycles(addr, false, false); }
int BSCRegisters::sdramWriteCycles(u32 addr) { return sdramAccessCycles(addr, true, false); }
int BSCRegisters::sdramDmaReadCycles(u32 addr) { return sdramAccessCycles(addr, false, true); }
int BSCRegisters::sdramDmaWriteCycles(u32 addr) { return sdramAccessCycles(addr, true, true); }
void BSCRegisters::endSdramDmaPipeline() { sdramDmaPipeline = false; }
void BSCRegisters::closeSdramRows()
{
	sdramDmaPipeline = false;
	for (int& row : sdramOpenRows)
		row = -1;
}

static u64 refreshNow()
{
	return sh4_sched_now64() + sh4_sched_cpu_slice() - p_sh4rcb->cntx.cycle_counter;
}

static u32 refreshDivisor()
{
	static constexpr u16 busDivisors[8] = { 0, 4, 16, 64, 256, 1024, 2048, 4096 };
	return 2u * busDivisors[(BSC_RTCSR.full >> 3) & 7];
}

static void updateRefreshInterrupts()
{
	InterruptPend(sh4_REF_RCMI, (BSC_RTCSR.full & 0xc0) == 0xc0);
	InterruptPend(sh4_REF_ROVI, (BSC_RTCSR.full & 0x06) == 0x06);
}

static void checkRefreshCountLimit()
{
	// The 1024 limit is detected by the ten-bit counter's carry in
	// applyRefreshMatches(). The 512 limit can also become true immediately
	// after software changes LMTS or writes RFCR.
	if (BSC_RTCSR.LMTS && BSC_RFCR.VALUE > 512) {
		BSC_RFCR.VALUE = 0;
		BSC_RTCSR.OVF = 1;
	}
}

static void applyRefreshMatches(u64 matches)
{
	if (matches == 0)
		return;
	if (BSC_MCR.RFSH && !BSC_MCR.RMODE) {
		if (DMAC_IsBusTransferInProgress())
			refreshPending = true;
		else
			startRefreshBusCycle();
	}
	BSC_RTCSR.CMF = 1;
	// RFCR is ten bits wide. With LMTS clear, its carry out on the 1024th
	// refresh is the overflow condition. With LMTS set, 512 itself is still a
	// representable count and the following refresh exceeds that limit.
	const u32 overflowCount = BSC_RTCSR.LMTS ? 513 : 1024;
	const u64 total = BSC_RFCR.VALUE + matches;
	if (total >= overflowCount) {
		BSC_RTCSR.OVF = 1;
		BSC_RFCR.VALUE = total % overflowCount;
	}
	else
		BSC_RFCR.VALUE = total;
	updateRefreshInterrupts();
}

static void syncRefreshTimer()
{
	const u64 now = refreshNow();
	const u32 divisor = refreshDivisor();
	if (divisor == 0) {
		refreshLastCycle = now;
		return;
	}
	const u64 totalCycles = refreshFraction + now - refreshLastCycle;
	u64 increments = totalCycles / divisor;
	refreshFraction = totalCycles % divisor;
	refreshLastCycle = now;
	if (increments == 0)
		return;

	u32 current = BSC_RTCNT.VALUE;
	const u32 compare = BSC_RTCOR.VALUE;
	u32 firstMatch = (compare - current) & 0xff;
	if (firstMatch == 0)
		firstMatch = 256;
	if (increments < firstMatch) {
		BSC_RTCNT.VALUE = (current + increments) & 0xff;
		return;
	}
	increments -= firstMatch;
	const u32 period = compare == 0 ? 256 : compare;
	const u64 matches = 1 + increments / period;
	BSC_RTCNT.VALUE = increments % period;
	applyRefreshMatches(matches);
}

static void scheduleRefreshTimer()
{
	const u32 divisor = refreshDivisor();
	if (divisor == 0) {
		sh4_sched_request(refreshSched, -1);
		return;
	}
	u32 distance = (BSC_RTCOR.VALUE - BSC_RTCNT.VALUE) & 0xff;
	if (distance == 0)
		distance = 256;
	sh4_sched_request(refreshSched, distance * divisor - refreshFraction);
}

static int refreshCallback(int, int, int, void *)
{
	syncRefreshTimer();
	scheduleRefreshTimer();
	return 0;
}

static u16 readRtcsr(u32) { syncRefreshTimer(); scheduleRefreshTimer(); return BSC_RTCSR.full; }
static u16 readRtcnt(u32) { syncRefreshTimer(); scheduleRefreshTimer(); return BSC_RTCNT.full; }
static u16 readRtcor(u32) { return BSC_RTCOR.full; }
static u16 readRfcr(u32) { syncRefreshTimer(); scheduleRefreshTimer(); return BSC_RFCR.full; }

static void writeRtcsr(u32, u16 data)
{
	if ((data >> 8) != 0xa5)
		return;
	syncRefreshTimer();
	const u8 old = BSC_RTCSR.full;
	BSC_RTCSR.full = (data & 0x7b) | (old & data & 0x84);
	checkRefreshCountLimit();
	refreshFraction = 0;
	refreshLastCycle = refreshNow();
	updateRefreshInterrupts();
	scheduleRefreshTimer();
}

static void writeRtcnt(u32, u16 data)
{
	if ((data >> 8) != 0xa5)
		return;
	syncRefreshTimer();
	BSC_RTCNT.VALUE = data;
	refreshFraction = 0;
	refreshLastCycle = refreshNow();
	scheduleRefreshTimer();
}

static void writeRtcor(u32, u16 data)
{
	if ((data >> 8) != 0xa5)
		return;
	syncRefreshTimer();
	BSC_RTCOR.VALUE = data;
	scheduleRefreshTimer();
}

static void writeRfcr(u32, u16 data)
{
	if ((data & 0xfc00) != 0xa400)
		return;
	syncRefreshTimer();
	BSC_RFCR.VALUE = data;
	checkRefreshCountLimit();
	updateRefreshInterrupts();
	scheduleRefreshTimer();
}

static void writeMcr(u32, u32 data)
{
	const bool oldRasDown = BSC_MCR.RASD;
	BSC_MCR.full = data & 0xf8bbffff;
	if (!BSC_MCR.RASD || oldRasDown != (bool)BSC_MCR.RASD)
		bsc.closeSdramRows();
}

//u32 port_out_data;
static void write_BSC_PDTRA_arcade(u32 addr, u16 data)
{
	BSC_PDTRA.full = data;
	NaomiBoardIDWrite(data);
}

static void write_BSC_PDTRA(u32 addr, u16 data)
{
	BSC_PDTRA.full = data;
}

static u16 read_BSC_PDTRA_arcade(u32 addr)
{
	return NaomiBoardIDRead();
}

static u16 read_BSC_PDTRA(u32 addr)
{
	/* as seen on chankast */
	u32 tpctra = BSC_PCTRA.full;
	u32 tpdtra = BSC_PDTRA.full;

	u16 tfinal = 0;
	// magic values
	if ((tpctra & 0xf) == 0x8)
		tfinal = 3;
	else if ((tpctra & 0xf) == 0xB)
		tfinal = 3;
	else
		tfinal = 0;

	if ((tpctra & 0xf) == 0xB && (tpdtra & 0xf) == 2)
		tfinal = 0;
	else if ((tpctra & 0xf) == 0xC && (tpdtra & 0xf) == 2)
		tfinal = 3;

	tfinal |= config::Cable << 8;

	return tfinal;
}

void BSCRegisters::init()
{
	super::init();
	refreshSched = sh4_sched_register(0, refreshCallback);

	//BSC BCR1 0xFF800000 0x1F800000 32 0x00000000 Held Held Held Bclk
	setRW<BSC_BCR1_addr, u32, 0x033efffd>();

	//BSC BCR2 0xFF800004 0x1F800004 16 0x3FFC Held Held Held Bclk
	setRW<BSC_BCR2_addr, u16, 0x3ffd>();

	//BSC WCR1 0xFF800008 0x1F800008 32 0x77777777 Held Held Held Bclk
	setRW<BSC_WCR1_addr, u32, 0x77777777>();

	//BSC WCR2 0xFF80000C 0x1F80000C 32 0xFFFEEFFF Held Held Held Bclk
	setRW<BSC_WCR2_addr, u32, 0xfffeefff>();

	//BSC WCR3 0xFF800010 0x1F800010 32 0x07777777 Held Held Held Bclk
	setRW<BSC_WCR3_addr, u32, 0x07777777>();

	//BSC MCR 0xFF800014 0x1F800014 32 0x00000000 Held Held Held Bclk
	setWriteHandler<BSC_MCR_addr>(writeMcr);

	//BSC PCR 0xFF800018 0x1F800018 16 0x0000 Held Held Held Bclk
	setRW<BSC_PCR_addr, u16>();

	//BSC RTCSR 0xFF80001C 0x1F80001C 16 0x0000 Held Held Held Bclk
	setHandlers<BSC_RTCSR_addr>(readRtcsr, writeRtcsr);

	//BSC RTCNT 0xFF800020 0x1F800020 16 0x0000 Held Held Held Bclk
	setHandlers<BSC_RTCNT_addr>(readRtcnt, writeRtcnt);

	//BSC RTCOR 0xFF800024 0x1F800024 16 0x0000 Held Held Held Bclk
	setHandlers<BSC_RTCOR_addr>(readRtcor, writeRtcor);

	//BSC RFCR 0xFF800028 0x1F800028 16 0x0000 Held Held Held Bclk
	setHandlers<BSC_RFCR_addr>(readRfcr, writeRfcr);

	//BSC PCTRA 0xFF80002C 0x1F80002C 32 0x00000000 Held Held Held Bclk
	// Naomi BIOS writes u16 in this register but ignoring them doesn't seem to hurt
	setRW<BSC_PCTRA_addr, u32>();

	//BSC PDTRA 0xFF800030 0x1F800030 16 Undefined Held Held Held Bclk
	setRW<BSC_PDTRA_addr, u16>();

	//BSC PCTRB 0xFF800040 0x1F800040 32 0x00000000 Held Held Held Bclk
	setRW<BSC_PCTRB_addr, u32, 0x000000ff>();

	//BSC PDTRB 0xFF800044 0x1F800044 16 Undefined Held Held Held Bclk
	setRW<BSC_PDTRB_addr, u16, 0x000f>();

	//BSC GPIOIC 0xFF800048 0x1F800048 16 0x00000000 Held Held Held Bclk
	setRW<BSC_GPIOIC_addr, u16>();

	reset(true);
}

void BSCRegisters::reset(bool hard)
{
	if (!hard)
		syncRefreshTimer();
	u32 oldRegisters[std::size(BSC)];
	memcpy(oldRegisters, BSC, sizeof(oldRegisters));
	const u32 oldFraction = refreshFraction;
	super::reset();
	closeSdramRows();
	refreshPending = false;
	refreshBusBusyUntil = 0;

	/*
	BSC BCR1 H'FF80 0000 H'1F80 0000 32 H'0000 0000*2 Held Held Held Bclk
	BSC BCR2 H'FF80 0004 H'1F80 0004 16 H'3FFC*2 Held Held Held Bclk
	BSC WCR1 H'FF80 0008 H'1F80 0008 32 H'7777 7777 Held Held Held Bclk
	BSC WCR2 H'FF80 000C H'1F80 000C 32 H'FFFE EFFF Held Held Held Bclk
	BSC WCR3 H'FF80 0010 H'1F80 0010 32 H'0777 7777 Held Held Held Bclk

	BSC MCR H'FF80 0014 H'1F80 0014 32 H'0000 0000 Held Held Held Bclk
	BSC PCR H'FF80 0018 H'1F80 0018 16 H'0000 Held Held Held Bclk
	BSC RTCSR H'FF80 001C H'1F80 001C 16 H'0000 Held Held Held Bclk
	BSC RTCNT H'FF80 0020 H'1F80 0020 16 H'0000 Held Held Held Bclk
	BSC RTCOR H'FF80 0024 H'1F80 0024 16 H'0000 Held Held Held Bclk
	BSC RFCR H'FF80 0028 H'1F80 0028 16 H'0000 Held Held Held Bclk
	BSC PCTRA H'FF80 002C H'1F80 002C 32 H'0000 0000 Held Held Held Bclk
	BSC PDTRA H'FF80 0030 H'1F80 0030 16 Undefined Held Held Held Bclk
	BSC PCTRB H'FF80 0040 H'1F80 0040 32 H'0000 0000 Held Held Held Bclk
	BSC PDTRB H'FF80 0044 H'1F80 0044 16 Undefined Held Held Held Bclk
	BSC GPIOIC H'FF80 0048 H'1F80 0048 16 H'0000 0000 Held Held Held Bclk
	BSC SDMR2 H'FF90 xxxx H'1F90 xxxx 8 Write-only Bclk
	BSC SDMR3 H'FF94 xxxx H'1F94 xxxx 8 Bclk
	*/
	BSC_BCR2.full = 0x3FFC;
	BSC_WCR1.full = 0x77777777;
	BSC_WCR2.full = 0xFFFEEFFF;
	BSC_WCR3.full = 0x07777777;

	if (!hard) {
		// The complete BSC register bank is held across manual reset.
		memcpy(BSC, oldRegisters, sizeof(oldRegisters));
		refreshFraction = oldFraction;
	}
	else
		refreshFraction = 0;
	refreshLastCycle = refreshNow();
	updateRefreshInterrupts();
	scheduleRefreshTimer();

	if (settings.platform.isNaomi() || settings.platform.isSystemSP())
		setHandlers<BSC_PDTRA_addr>(read_BSC_PDTRA_arcade, write_BSC_PDTRA_arcade);
	else
		setHandlers<BSC_PDTRA_addr>(read_BSC_PDTRA, write_BSC_PDTRA);
}

void BSCRegisters::term()
{
	sh4_sched_unregister(refreshSched);
	refreshSched = -1;
	super::term();
}

void BSCRegisters::serialize(Serializer& ser)
{
	syncRefreshTimer();
	ser << refreshLastCycle;
	ser << refreshFraction;
	ser << sdramOpenRows;
	ser << sdramDmaPipeline;
	ser << refreshPending;
	ser << refreshBusBusyUntil;
	sh4_sched_serialize(ser, refreshSched);
}

void BSCRegisters::deserialize(Deserializer& deser)
{
	if (deser.version() >= Deserializer::V73) {
		deser >> refreshLastCycle;
		deser >> refreshFraction;
		if (deser.version() >= Deserializer::V79) {
			deser >> sdramOpenRows >> sdramDmaPipeline;
			if (deser.version() >= Deserializer::V82)
				deser >> refreshPending >> refreshBusBusyUntil;
			else {
				refreshPending = false;
				refreshBusBusyUntil = 0;
			}
		}
		else {
			closeSdramRows();
			refreshPending = false;
			refreshBusBusyUntil = 0;
		}
		sh4_sched_deserialize(deser, refreshSched);
	}
	else {
		refreshLastCycle = refreshNow();
		refreshFraction = 0;
		refreshPending = false;
		refreshBusBusyUntil = 0;
		scheduleRefreshTimer();
	}
	updateRefreshInterrupts();
}
