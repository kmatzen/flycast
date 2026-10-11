#include "hw/hwreg.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_interrupts.h"
#include "emulator.h"
#include "serialize.h"
#include "modules.h"

CPGRegisters cpg;

static int wdt_sched = -1;
static u64 wdt_last_cycle;
static u32 wdt_fraction;
static bool wdt_power_on_reset;

static u64 wdtNow()
{
	return sh4_sched_now64() + sh4_sched_cpu_slice() - p_sh4rcb->cntx.cycle_counter;
}

static u32 wdtDivisor()
{
	return 32u << (CPG_WTCSR & 7);
}

static void updateWdtInterrupt()
{
	InterruptPend(sh4_WDT_ITI, (CPG_WTCSR & 0x48) == 0x08);
}

static void scheduleWdt()
{
	if ((CPG_WTCSR & 0x80) == 0) {
		sh4_sched_request(wdt_sched, -1);
		return;
	}
	const u32 divisor = wdtDivisor();
	const u32 cycles = (256u - (CPG_WTCNT & 0xff)) * divisor - wdt_fraction;
	sh4_sched_request(wdt_sched, cycles);
}

static void syncWdt()
{
	const u64 now = wdtNow();
	if ((CPG_WTCSR & 0x80) == 0) {
		wdt_last_cycle = now;
		return;
	}

	const u32 divisor = wdtDivisor();
	const u64 totalCycles = wdt_fraction + now - wdt_last_cycle;
	const u64 increments = totalCycles / divisor;
	wdt_fraction = totalCycles % divisor;
	wdt_last_cycle = now;
	if (increments == 0)
		return;

	const u64 totalCount = (CPG_WTCNT & 0xff) + increments;
	CPG_WTCNT = totalCount & 0xff;
	if (totalCount >= 256) {
		if ((CPG_WTCSR & 0x40) == 0) {
			CPG_WTCSR |= 0x08;
			updateWdtInterrupt();
		}
		else {
			CPG_WTCSR |= 0x10;
			wdt_power_on_reset = (CPG_WTCSR & 0x20) == 0;
			emu.requestReset(wdt_power_on_reset);
		}
	}
}

static int wdtCallback(int, int, int, void *)
{
	syncWdt();
	scheduleWdt();
	return 0;
}

static u8 readWtcnt(u32)
{
	syncWdt();
	scheduleWdt();
	return CPG_WTCNT;
}

static u8 readWtcsr(u32)
{
	syncWdt();
	scheduleWdt();
	return CPG_WTCSR;
}

static void writeWtcnt(u32, u16 data)
{
	if ((data >> 8) != 0x5a)
		return;
	syncWdt();
	CPG_WTCNT = data & 0xff;
	wdt_fraction = 0;
	wdt_last_cycle = wdtNow();
	scheduleWdt();
}

static void writeWtcsr(u32, u16 data)
{
	if ((data >> 8) != 0xa5)
		return;
	syncWdt();
	const u8 old = CPG_WTCSR;
	// Overflow flags are cleared by writing zero; writing one preserves them.
	CPG_WTCSR = (data & 0xe7) | (old & data & 0x18);
	// Writes which only change the mode/reset selection or acknowledge an
	// overflow do not restart the selected-clock prescaler.  A changed clock
	// source or timer enable state starts a new timing interval in this model.
	if (((old ^ CPG_WTCSR) & 0x87) != 0)
		wdt_fraction = 0;
	wdt_last_cycle = wdtNow();
	updateWdtInterrupt();
	scheduleWdt();
}

static void writeStbcr2(u32, u8 data)
{
	const u8 old = CPG_STBCR2;
	// SH7750S exposes DSLP plus module-stop controls for the store queue and
	// UBC. Other bits are reserved and always read as zero.
	CPG_STBCR2 = data & 0x83;
	if (((old ^ CPG_STBCR2) & 1) != 0)
	{
		// Dynarec blocks omit instruction UBC boundaries when matching is not
		// active. Stopping or restarting the UBC therefore changes block shape.
		emu.getSh4Executor()->ResetCache();
	}
}

static void writeStbcr(u32, u8 data)
{
	const u8 old = CPG_STBCR;
	CPG_STBCR = data;
	if ((old & 4) == 0 && (data & 4) != 0)
		TMU_EnterModuleStandby();
}

void CPGRegisters::init()
{
	super::init();
	wdt_sched = sh4_sched_register(0, wdtCallback);

	//CPG FRQCR H'FFC0 0000 H'1FC0 0000 16 *2 Held Held Held Pclk
	setRW<CPG_FRQCR_addr, u16, 0x0fff>();

	//CPG STBCR H'FFC0 0004 H'1FC0 0004 8 H'00 Held Held Held Pclk
	setWriteHandler<CPG_STBCR_addr, u8>(writeStbcr);

	//CPG WTCNT H'FFC0 0008 H'1FC0 0008 8/16*3 H'00 Held Held Held Pclk
	getRegister<CPG_WTCNT_addr>().setReadHandler(readWtcnt);
	getRegister<CPG_WTCNT_addr>().setWriteHandler(writeWtcnt);

	//CPG WTCSR H'FFC0 000C H'1FC0 000C 8/16*3 H'00 Held Held Held Pclk
	getRegister<CPG_WTCSR_addr>().setReadHandler(readWtcsr);
	getRegister<CPG_WTCSR_addr>().setWriteHandler(writeWtcsr);

	//CPG STBCR2 H'FFC0 0010 H'1FC0 0010 8 H'00 Held Held Held Pclk
	setWriteHandler<CPG_STBCR2_addr, u8>(writeStbcr2);

	reset(true);
}

void CPGRegisters::reset(bool hard)
{
	if (!hard)
		syncWdt();
	u32 oldRegisters[std::size(CPG)];
	memcpy(oldRegisters, CPG, sizeof(oldRegisters));
	const u8 oldWtcsr = CPG_WTCSR;
	const u8 oldWtcnt = CPG_WTCNT;
	const u32 oldFraction = wdt_fraction;
	super::reset();
	if (!hard) {
		// All CPG registers are held across manual reset.
		memcpy(CPG, oldRegisters, sizeof(oldRegisters));
		wdt_fraction = oldFraction;
	}
	else {
		wdt_fraction = 0;
		// An internal watchdog reset does not reset the watchdog itself: WTCSR
		// is retained and WTCNT continues from the overflow edge. This remains
		// true when RSTS selects power-on initialization for the other modules.
		if (wdt_power_on_reset) {
			CPG_WTCSR = oldWtcsr;
			CPG_WTCNT = oldWtcnt;
			wdt_fraction = oldFraction;
		}
	}
	wdt_power_on_reset = false;
	wdt_last_cycle = wdtNow();
	updateWdtInterrupt();
	scheduleWdt();
}

void CPGRegisters::term()
{
	sh4_sched_unregister(wdt_sched);
	wdt_sched = -1;
	super::term();
}

void CPGRegisters::serialize(Serializer& ser)
{
	syncWdt();
	ser << wdt_last_cycle;
	ser << wdt_fraction;
	sh4_sched_serialize(ser, wdt_sched);
}

void CPGRegisters::deserialize(Deserializer& deser)
{
	if (deser.version() >= Deserializer::V71) {
		deser >> wdt_last_cycle;
		deser >> wdt_fraction;
		sh4_sched_deserialize(deser, wdt_sched);
	}
	else {
		wdt_last_cycle = wdtNow();
		wdt_fraction = 0;
		scheduleWdt();
	}
	updateWdtInterrupt();
}
