#include "gtest/gtest.h"
#include "types.h"
#include "hw/sh4/modules/modules.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_cycles.h"

namespace
{
bool schedulerCallbackCalled;
int schedulerCallbackJitter;

int schedulerCallback(int, int, int jitter, void *)
{
	schedulerCallbackCalled = true;
	schedulerCallbackJitter = jitter;
	return 0;
}
}

class TimerTest : public ::testing::Test {
	void SetUp() override {
		tmu.init();
	}
	void TearDown() override {
		tmu.term();
	}

protected:
	void tick(int ticks) {
		Sh4cntx.sh4_sched_next -= ticks;
		sh4_sched_tick(ticks);
	}
};

TEST_F(TimerTest, init)
{
	ASSERT_EQ(0xffffffff, tmu.read<u32>(TMU_TCNT0_addr));
	ASSERT_EQ(0xffffffff, tmu.read<u32>(TMU_TCNT1_addr));
	ASSERT_EQ(0xffffffff, tmu.read<u32>(TMU_TCNT2_addr));

	ASSERT_EQ(0xffffffff, tmu.read<u32>(TMU_TCOR0_addr));
	ASSERT_EQ(0xffffffff, tmu.read<u32>(TMU_TCOR1_addr));
	ASSERT_EQ(0xffffffff, tmu.read<u32>(TMU_TCOR2_addr));

	ASSERT_EQ(0, tmu.read<u16>(TMU_TCR0_addr));
	ASSERT_EQ(0, tmu.read<u16>(TMU_TCR1_addr));
	ASSERT_EQ(0, tmu.read<u16>(TMU_TCR2_addr));

	ASSERT_EQ(0, tmu.read<u8>(TMU_TSTR_addr));
}

TEST_F(TimerTest, CaptureRegisterStoresAndSurvivesManualReset)
{
	tmu.write<u32>(TMU_TCPR2_addr, 0x89abcdef);
	EXPECT_EQ(0x89abcdefu, tmu.read<u32>(TMU_TCPR2_addr));
	tmu.reset(false);
	EXPECT_EQ(0x89abcdefu, tmu.read<u32>(TMU_TCPR2_addr));
}

TEST_F(TimerTest, SchedulerFiresAtExactDeadline)
{
	const int schedulerId = sh4_sched_register(0, schedulerCallback);
	schedulerCallbackCalled = false;
	sh4_sched_request(schedulerId, 64);

	tick(63);
	EXPECT_FALSE(schedulerCallbackCalled);
	tick(1);
	EXPECT_TRUE(schedulerCallbackCalled);

	sh4_sched_unregister(schedulerId);
}

TEST_F(TimerTest, SchedulerRequestIncludesCurrentSliceProgress)
{
	const int schedulerId = sh4_sched_register(0, schedulerCallback);
	schedulerCallbackCalled = false;
	const bool wasRunning = Sh4cntx.CpuRunning;
	const int oldCycleCounter = Sh4cntx.cycle_counter;
	const int oldCpuSlice = sh4_sched_cpu_slice();
	sh4_sched_set_cpu_slice(SH4_TIMESLICE);
	Sh4cntx.CpuRunning = true;
	Sh4cntx.cycle_counter = SH4_TIMESLICE - 20;
	sh4_sched_request(schedulerId, 64);
	EXPECT_EQ(64, Sh4cntx.cycle_counter);
	EXPECT_EQ(84, sh4_sched_cpu_slice());

	// Deadline is 20 elapsed cycles plus 64 requested cycles from the current
	// scheduler boundary.
	tick(83);
	EXPECT_FALSE(schedulerCallbackCalled);
	tick(1);
	EXPECT_TRUE(schedulerCallbackCalled);

	sh4_sched_unregister(schedulerId);
	Sh4cntx.CpuRunning = wasRunning;
	Sh4cntx.cycle_counter = oldCycleCounter;
	sh4_sched_set_cpu_slice(oldCpuSlice);
}

TEST_F(TimerTest, SchedulerUsesEarliestInSliceDeadline)
{
	const int laterId = sh4_sched_register(0, schedulerCallback);
	const int earlierId = sh4_sched_register(0, schedulerCallback);
	const bool wasRunning = Sh4cntx.CpuRunning;
	const int oldCycleCounter = Sh4cntx.cycle_counter;
	const int oldCpuSlice = sh4_sched_cpu_slice();
	sh4_sched_set_cpu_slice(SH4_TIMESLICE);
	Sh4cntx.CpuRunning = true;
	Sh4cntx.cycle_counter = SH4_TIMESLICE - 20;

	sh4_sched_request(laterId, 64);
	sh4_sched_request(earlierId, 32);
	EXPECT_EQ(32, Sh4cntx.cycle_counter);
	EXPECT_EQ(52, sh4_sched_cpu_slice());

	sh4_sched_unregister(earlierId);
	sh4_sched_unregister(laterId);
	Sh4cntx.CpuRunning = wasRunning;
	Sh4cntx.cycle_counter = oldCycleCounter;
	sh4_sched_set_cpu_slice(oldCpuSlice);
}

TEST_F(TimerTest, InstructionOverrunAdvancesSchedulerToRetirement)
{
	const int schedulerId = sh4_sched_register(0, schedulerCallback);
	const bool wasRunning = Sh4cntx.CpuRunning;
	const int oldCycleCounter = Sh4cntx.cycle_counter;
	const int oldCpuSlice = sh4_sched_cpu_slice();
	Sh4cntx.CpuRunning = false;
	sh4_sched_set_cpu_slice(5);
	Sh4cntx.cycle_counter = 5;
	schedulerCallbackCalled = false;
	schedulerCallbackJitter = -1;
	sh4_sched_request(schedulerId, 5);

	// An indivisible eight-cycle instruction crosses a deadline that was five
	// cycles away. Backends replenish by SH4_TIMESLICE before UpdateSystem.
	Sh4cntx.cycle_counter -= 8;
	Sh4cntx.cycle_counter += SH4_TIMESLICE;
	UpdateSystem_INTC();

	EXPECT_TRUE(schedulerCallbackCalled);
	EXPECT_EQ(3, schedulerCallbackJitter);
	EXPECT_EQ(sh4_sched_cpu_slice(), Sh4cntx.cycle_counter);

	sh4_sched_unregister(schedulerId);
	Sh4cntx.CpuRunning = wasRunning;
	Sh4cntx.cycle_counter = oldCycleCounter;
	sh4_sched_set_cpu_slice(oldCpuSlice);
}

TEST_F(TimerTest, SleepingCpuStopsAtSchedulerDeadline)
{
	const int schedulerId = sh4_sched_register(0, schedulerCallback);
	schedulerCallbackCalled = false;
	schedulerCallbackJitter = -1;
	sh4_sched_request(schedulerId, 37);

	EXPECT_EQ(0, UpdateSystem_INTC_Sleep());
	EXPECT_TRUE(schedulerCallbackCalled);
	EXPECT_EQ(0, schedulerCallbackJitter);

	sh4_sched_unregister(schedulerId);
}

TEST(SerialTimingTest, UsesExactHardwareDivisor)
{
	EXPECT_EQ(128u, scifCyclesPerBit(0, 0));
	EXPECT_EQ(128u * 256u * 64u, scifCyclesPerBit(255, 3));
	EXPECT_EQ(128u * 18u * 4u, scifCyclesPerBit(17, 1));
}

class RtcTest : public ::testing::Test {
	static constexpr int BaseTickCycles = SH4_MAIN_CLOCK / 256;

	void SetUp() override {
		rtc.init();
	}
	void TearDown() override {
		rtc.term();
	}

protected:
	void baseTicks(int count) {
		while (count-- != 0) {
			Sh4cntx.sh4_sched_next -= BaseTickCycles;
			sh4_sched_tick(BaseTickCycles);
		}
	}
};

TEST_F(RtcTest, DividerRunsAt128HzAndCalendarAt1Hz)
{
	EXPECT_EQ(0, rtc.read<u8>(RTC_R64CNT_addr));
	baseTicks(1);
	EXPECT_EQ(0, rtc.read<u8>(RTC_R64CNT_addr));
	baseTicks(1);
	EXPECT_EQ(1, rtc.read<u8>(RTC_R64CNT_addr));
	baseTicks(254);
	EXPECT_EQ(0x40, rtc.read<u8>(RTC_R64CNT_addr));
	EXPECT_EQ(1, rtc.read<u8>(RTC_RSECCNT_addr));
	EXPECT_NE(0, rtc.read<u8>(RTC_RCR1_addr) & 0x80);
}

TEST_F(RtcTest, StartStopsCalendarButNotDivider)
{
	rtc.write<u8>(RTC_RCR2_addr, 0x08);
	baseTicks(256);
	EXPECT_EQ(0x40, rtc.read<u8>(RTC_R64CNT_addr));
	EXPECT_EQ(0, rtc.read<u8>(RTC_RSECCNT_addr));
}

TEST_F(RtcTest, PeriodicFlagUsesSelected256HzRate)
{
	rtc.write<u8>(RTC_RCR2_addr, 0x19);
	baseTicks(1);
	EXPECT_NE(0, rtc.read<u8>(RTC_RCR2_addr) & 0x80);
	rtc.write<u8>(RTC_RCR2_addr, 0x19);
	EXPECT_EQ(0, rtc.read<u8>(RTC_RCR2_addr) & 0x80);
}

TEST_F(RtcTest, ResetBitClearsDividerPhase)
{
	baseTicks(3);
	EXPECT_EQ(1, rtc.read<u8>(RTC_R64CNT_addr));
	rtc.write<u8>(RTC_RCR2_addr, 0x0b);
	EXPECT_EQ(0, rtc.read<u8>(RTC_R64CNT_addr));
	baseTicks(1);
	EXPECT_EQ(0, rtc.read<u8>(RTC_R64CNT_addr));
	baseTicks(1);
	EXPECT_EQ(1, rtc.read<u8>(RTC_R64CNT_addr));
}

TEST_F(RtcTest, ManualResetHoldsCalendarAndDivider)
{
	rtc.write<u8>(RTC_RSECCNT_addr, 0x42);
	rtc.write<u8>(RTC_RMINCNT_addr, 0x17);
	baseTicks(3);
	ASSERT_EQ(1u, rtc.read<u8>(RTC_R64CNT_addr));

	rtc.reset(false);
	EXPECT_EQ(0x42u, rtc.read<u8>(RTC_RSECCNT_addr));
	EXPECT_EQ(0x17u, rtc.read<u8>(RTC_RMINCNT_addr));
	EXPECT_EQ(1u, rtc.read<u8>(RTC_R64CNT_addr));
	EXPECT_EQ(0u, rtc.read<u8>(RTC_RCR1_addr));
	EXPECT_EQ(0u, rtc.read<u8>(RTC_RCR2_addr));
	baseTicks(2);
	EXPECT_EQ(1u, rtc.read<u8>(RTC_R64CNT_addr));

	// Re-enabling the oscillator resumes the held half-divider phase.
	rtc.write<u8>(RTC_RCR2_addr, 0x09);
	baseTicks(1);
	EXPECT_EQ(2u, rtc.read<u8>(RTC_R64CNT_addr));
}

TEST_F(RtcTest, DelayedCallbackConsumesEveryOscillatorEdge)
{
	static constexpr int BaseTickCycles = SH4_MAIN_CLOCK / 256;
	Sh4cntx.sh4_sched_next -= 5 * BaseTickCycles;
	sh4_sched_tick(5 * BaseTickCycles);
	EXPECT_EQ(2, rtc.read<u8>(RTC_R64CNT_addr));

	// The late dispatch retains the original phase: one more base tick reaches
	// the next edge rather than restarting a full interval from dispatch time.
	Sh4cntx.sh4_sched_next -= BaseTickCycles;
	sh4_sched_tick(BaseTickCycles);
	EXPECT_EQ(3, rtc.read<u8>(RTC_R64CNT_addr));
}

class WatchdogTest : public ::testing::Test {
	void SetUp() override {
		cpg.init();
	}
	void TearDown() override {
		cpg.term();
	}

protected:
	void tick(int cycles) {
		Sh4cntx.sh4_sched_next -= cycles;
		sh4_sched_tick(cycles);
	}
};

TEST_F(WatchdogTest, RequiresProtectedWordWrites)
{
	cpg.write<u16>(CPG_WTCNT_addr, 0x1234);
	EXPECT_EQ(0, cpg.read<u8>(CPG_WTCNT_addr));
	cpg.write<u16>(CPG_WTCNT_addr, 0x5a34);
	EXPECT_EQ(0x34, cpg.read<u8>(CPG_WTCNT_addr));

	cpg.write<u16>(CPG_WTCSR_addr, 0x1280);
	EXPECT_EQ(0, cpg.read<u8>(CPG_WTCSR_addr));
	cpg.write<u16>(CPG_WTCSR_addr, 0xa580);
	EXPECT_EQ(0x80, cpg.read<u8>(CPG_WTCSR_addr));
}

TEST_F(WatchdogTest, CountsAtSelectedClockExactly)
{
	cpg.write<u16>(CPG_WTCSR_addr, 0xa580); // enable, CPU clock / 32
	tick(31);
	EXPECT_EQ(0, cpg.read<u8>(CPG_WTCNT_addr));
	tick(1);
	EXPECT_EQ(1, cpg.read<u8>(CPG_WTCNT_addr));

	cpg.write<u16>(CPG_WTCSR_addr, 0xa500);
	cpg.write<u16>(CPG_WTCNT_addr, 0x5a00);
	cpg.write<u16>(CPG_WTCSR_addr, 0xa581); // enable, CPU clock / 64
	tick(63);
	EXPECT_EQ(0, cpg.read<u8>(CPG_WTCNT_addr));
	tick(1);
	EXPECT_EQ(1, cpg.read<u8>(CPG_WTCNT_addr));
}

TEST_F(WatchdogTest, IntervalOverflowRaisesAndClearsFlag)
{
	cpg.write<u16>(CPG_WTCNT_addr, 0x5aff);
	cpg.write<u16>(CPG_WTCSR_addr, 0xa580);
	tick(32);
	EXPECT_EQ(0, cpg.read<u8>(CPG_WTCNT_addr));
	EXPECT_NE(0, cpg.read<u8>(CPG_WTCSR_addr) & 0x08);

	cpg.write<u16>(CPG_WTCSR_addr, 0xa580);
	EXPECT_EQ(0, cpg.read<u8>(CPG_WTCSR_addr) & 0x08);
}

TEST_F(WatchdogTest, StatusWritePreservesPrescalerPhase)
{
	cpg.write<u16>(CPG_WTCSR_addr, 0xa580);
	tick(31);

	// A protected write with the same enable and clock settings must not
	// postpone the next count edge.
	cpg.write<u16>(CPG_WTCSR_addr, 0xa580);
	tick(1);
	EXPECT_EQ(1, cpg.read<u8>(CPG_WTCNT_addr));

	// Reset selection is independent of the count-clock divider as well.
	tick(31);
	cpg.write<u16>(CPG_WTCSR_addr, 0xa5a0);
	tick(1);
	EXPECT_EQ(2, cpg.read<u8>(CPG_WTCNT_addr));
}

TEST_F(WatchdogTest, ManualResetRetainsWatchdogState)
{
	cpg.write<u8>(CPG_STBCR_addr, 0x68);
	cpg.write<u16>(CPG_WTCNT_addr, 0x5a7b);
	cpg.write<u16>(CPG_WTCSR_addr, 0xa501);
	cpg.reset(false);
	EXPECT_EQ(0x7b, cpg.read<u8>(CPG_WTCNT_addr));
	EXPECT_EQ(0x01, cpg.read<u8>(CPG_WTCSR_addr));
	EXPECT_EQ(0x68, cpg.read<u8>(CPG_STBCR_addr));
}

class SciTimingTest : public ::testing::Test {
	void SetUp() override {
		sci.init();
	}
	void TearDown() override {
		sci.term();
	}

protected:
	void tick(int cycles) {
		Sh4cntx.sh4_sched_next -= cycles;
		sh4_sched_tick(cycles);
	}

	void startByte(u8 value) {
		sci.write<u8>(SCI_SCTDR1_addr, value);
		sci.write<u8>(SCI_SCSSR1_addr, sci.read<u8>(SCI_SCSSR1_addr) & ~0x80);
	}
};

TEST_F(SciTimingTest, TransmitEndOccursAtExactFrameBoundary)
{
	sci.write<u8>(SCI_SCBRR1_addr, 0);
	sci.write<u8>(SCI_SCSCR1_addr, 0xa4); // TE, TIE, TEIE
	startByte(0x55);
	EXPECT_NE(0, sci.read<u8>(SCI_SCSSR1_addr) & 0x80); // TDR copied immediately
	EXPECT_EQ(0, sci.read<u8>(SCI_SCSSR1_addr) & 0x04);

	// 8 data bits, start and stop at 128 SH-4 cycles per bit.
	tick(1279);
	EXPECT_EQ(0, sci.read<u8>(SCI_SCSSR1_addr) & 0x04);
	tick(1);
	EXPECT_NE(0, sci.read<u8>(SCI_SCSSR1_addr) & 0x04);
}

TEST_F(SciTimingTest, BufferedByteContinuesWithoutPrematureEnd)
{
	sci.write<u8>(SCI_SCBRR1_addr, 0);
	sci.write<u8>(SCI_SCSCR1_addr, 0x20);
	startByte(0x11);
	startByte(0x22);

	tick(1280);
	EXPECT_NE(0, sci.read<u8>(SCI_SCSSR1_addr) & 0x80);
	EXPECT_EQ(0, sci.read<u8>(SCI_SCSSR1_addr) & 0x04);
	tick(1280);
	EXPECT_NE(0, sci.read<u8>(SCI_SCSSR1_addr) & 0x04);
}

TEST_F(SciTimingTest, ExternalClockDoesNotAdvanceFromCpuCycles)
{
	sci.write<u8>(SCI_SCBRR1_addr, 0);
	sci.write<u8>(SCI_SCSCR1_addr, 0x22); // TE, external clock input
	startByte(0x55);
	tick(1280);
	EXPECT_EQ(0, sci.read<u8>(SCI_SCSSR1_addr) & 0x80);
	EXPECT_NE(0, sci.read<u8>(SCI_SCSSR1_addr) & 0x04);
}

TEST_F(SciTimingTest, LateCallbackCompletesBufferedSynchronousFrame)
{
	sci.write<u8>(SCI_SCSMR1_addr, 0x80); // synchronous, CPU clock / 16
	sci.write<u8>(SCI_SCBRR1_addr, 0);
	sci.write<u8>(SCI_SCSCR1_addr, 0x20);
	startByte(0x11);
	startByte(0x22);

	// Both eight-bit frames elapse during one indivisible CPU operation.
	tick(256);
	EXPECT_EQ(0x84, sci.read<u8>(SCI_SCSSR1_addr) & 0x84);
}

class RefreshTimerTest : public ::testing::Test {
	void SetUp() override {
		bsc.init();
	}
	void TearDown() override {
		bsc.term();
	}

protected:
	void tick(int cycles) {
		Sh4cntx.sh4_sched_next -= cycles;
		sh4_sched_tick(cycles);
	}
};

TEST_F(RefreshTimerTest, RequiresProtectedWordWrites)
{
	bsc.write<u16>(BSC_RTCNT_addr, 0x1234);
	EXPECT_EQ(0, bsc.read<u16>(BSC_RTCNT_addr));
	bsc.write<u16>(BSC_RTCNT_addr, 0xa534);
	EXPECT_EQ(0x34, bsc.read<u16>(BSC_RTCNT_addr));

	bsc.write<u16>(BSC_RFCR_addr, 0x1234);
	EXPECT_EQ(0, bsc.read<u16>(BSC_RFCR_addr));
	bsc.write<u16>(BSC_RFCR_addr, 0xa523);
	EXPECT_EQ(0x123, bsc.read<u16>(BSC_RFCR_addr));
}

TEST_F(RefreshTimerTest, MatchOccursOnExactSelectedClockEdge)
{
	bsc.write<u16>(BSC_RTCOR_addr, 0xa502);
	bsc.write<u16>(BSC_RTCSR_addr, 0xa508); // CKIO / 4 = 8 SH-4 cycles
	tick(7);
	EXPECT_EQ(0, bsc.read<u16>(BSC_RTCNT_addr));
	tick(1);
	EXPECT_EQ(1, bsc.read<u16>(BSC_RTCNT_addr));
	tick(8);
	EXPECT_EQ(0, bsc.read<u16>(BSC_RTCNT_addr));
	EXPECT_NE(0, bsc.read<u16>(BSC_RTCSR_addr) & 0x80);
	EXPECT_EQ(1, bsc.read<u16>(BSC_RFCR_addr));

	bsc.write<u16>(BSC_RTCSR_addr, 0xa508);
	EXPECT_EQ(0, bsc.read<u16>(BSC_RTCSR_addr) & 0x80);
}

TEST_F(RefreshTimerTest, AutoRefreshOccupiesExternalBus)
{
	// RAS-down, auto-refresh, TRAS=1, TRC=1: two REF commands occupy
	// 8 + 2*TRAS + 3*TRC = 13 CKIO cycles.
	bsc.write<u32>(BSC_MCR_addr, 0x88000404);
	bsc.write<u16>(BSC_RTCOR_addr, 0xa501);
	bsc.write<u16>(BSC_RTCSR_addr, 0xa508);
	Sh4Cycles cycles;
	bsc.closeSdramRows();
	EXPECT_EQ(20, cycles.readAccessCycles(0x0c001000, 4));
	tick(8);
	// 26 SH-4 refresh cycles, followed by a 20-cycle no-row read.
	EXPECT_EQ(46, cycles.readAccessCycles(0x0c001004, 4));
}

TEST_F(RefreshTimerTest, RefreshCountOverflowUsesSelectedLimit)
{
	bsc.write<u16>(BSC_RFCR_addr, 0xa600); // protected write of 512
	bsc.write<u16>(BSC_RTCOR_addr, 0xa501);
	bsc.write<u16>(BSC_RTCSR_addr, 0xa509); // 512 limit, CKIO / 4
	tick(8);
	EXPECT_EQ(0, bsc.read<u16>(BSC_RFCR_addr));
	EXPECT_NE(0, bsc.read<u16>(BSC_RTCSR_addr) & 0x04);
}

TEST_F(RefreshTimerTest, RefreshCountDetectsTenBitCarry)
{
	bsc.write<u16>(BSC_RFCR_addr, 0xa7ff); // protected write of 1023
	bsc.write<u16>(BSC_RTCOR_addr, 0xa501);
	bsc.write<u16>(BSC_RTCSR_addr, 0xa508); // 1024 limit, CKIO / 4
	tick(8);
	EXPECT_EQ(0, bsc.read<u16>(BSC_RFCR_addr));
	EXPECT_NE(0, bsc.read<u16>(BSC_RTCSR_addr) & 0x04);
}

TEST_F(RefreshTimerTest, RefreshCountLimitChangeIsImmediate)
{
	bsc.write<u16>(BSC_RFCR_addr, 0xa601); // protected write of 513
	bsc.write<u16>(BSC_RTCSR_addr, 0xa501); // select the 512 limit
	EXPECT_EQ(0, bsc.read<u16>(BSC_RFCR_addr));
	EXPECT_NE(0, bsc.read<u16>(BSC_RTCSR_addr) & 0x04);

	// Clearing OVF and then writing an already excessive count must perform
	// the same comparison without requiring a refresh-clock edge.
	bsc.write<u16>(BSC_RTCSR_addr, 0xa501);
	bsc.write<u16>(BSC_RFCR_addr, 0xa601);
	EXPECT_EQ(0, bsc.read<u16>(BSC_RFCR_addr));
	EXPECT_NE(0, bsc.read<u16>(BSC_RTCSR_addr) & 0x04);
}

TEST_F(RefreshTimerTest, ManualResetRetainsCounterAndPrescalerPhase)
{
	bsc.write<u32>(BSC_BCR1_addr, 0x6789abcd);
	bsc.write<u16>(BSC_RTCOR_addr, 0xa510);
	bsc.write<u16>(BSC_RTCSR_addr, 0xa508);
	tick(7);
	bsc.reset(false);
	tick(1);
	EXPECT_EQ(1, bsc.read<u16>(BSC_RTCNT_addr));
	EXPECT_EQ(0x10, bsc.read<u16>(BSC_RTCOR_addr));
	EXPECT_EQ(0x6789abcdu, bsc.read<u32>(BSC_BCR1_addr));
}

TEST_F(TimerTest, precision)
{
	tmu.write<u32>(TMU_TCOR0_addr, 1000);
	tmu.write<u32>(TMU_TCNT0_addr, 1000);
	tmu.write<u8>(TMU_TSTR_addr, 1);

	ASSERT_EQ(1000, tmu.read<u32>(TMU_TCOR0_addr));
	ASSERT_EQ(1000, tmu.read<u32>(TMU_TCNT0_addr));
	ASSERT_EQ(1, tmu.read<u8>(TMU_TSTR_addr));
	ASSERT_EQ(0, tmu.read<u16>(TMU_TCR0_addr));

	tick(999 << 4);
	ASSERT_EQ(1, tmu.read<u32>(TMU_TCNT0_addr));
	ASSERT_EQ(0, tmu.read<u16>(TMU_TCR0_addr));

	tick(2 << 4);
	 // underflow flag
	ASSERT_EQ(0x0100, tmu.read<u16>(TMU_TCR0_addr));
	// counter reloaded with TCOR
	ASSERT_EQ(1000, tmu.read<u32>(TMU_TCNT0_addr));

	Sh4cntx.cycle_counter -= 1 << 4;
	// current cycle counter taken into account for increased precision
	ASSERT_EQ(999, tmu.read<u32>(TMU_TCNT0_addr));
}

TEST_F(TimerTest, smallTCOR)
{
	for (int i = 0; i < 16; i++)
		tick(0x7fffffff);
	tmu.write<u32>(TMU_TCOR0_addr, 2);
	tmu.write<u32>(TMU_TCNT0_addr, 2);
	tmu.write<u8>(TMU_TSTR_addr, 1);

	tick(5 << 4);
	 // underflow flag
	ASSERT_EQ(0x0100, tmu.read<u16>(TMU_TCR0_addr));
	tmu.write<u16>(TMU_TCR0_addr, 0);
	ASSERT_EQ(0, tmu.read<u32>(TMU_TCNT0_addr));

	tick(1 << 4);
	 // underflow flag
	ASSERT_EQ(0x0100, tmu.read<u16>(TMU_TCR0_addr));
	ASSERT_EQ(2, tmu.read<u32>(TMU_TCNT0_addr));
}

TEST_F(TimerTest, underflow)
{
	tmu.write<u32>(TMU_TCOR0_addr, 100);
	tmu.write<u32>(TMU_TCNT0_addr, 100);
	tmu.write<u8>(TMU_TSTR_addr, 1);

	tick(100 << 4);
	// no underflow yet
	ASSERT_EQ(0, tmu.read<u16>(TMU_TCR0_addr));
	ASSERT_EQ(0, tmu.read<u32>(TMU_TCNT0_addr));

	Sh4cntx.cycle_counter -= 1 << 4;
	// this shouldn't affect the timer
	tmu.write<u8>(TMU_TSTR_addr, tmu.read<u8>(TMU_TSTR_addr));
	// now read to reload the timer
	ASSERT_EQ(99, tmu.read<u32>(TMU_TCNT0_addr));
	// underflow set
	ASSERT_EQ(0x100, tmu.read<u16>(TMU_TCR0_addr));
}

TEST_F(TimerTest, ZeroUnderflowsAtNextPrescalerEdge)
{
	// Default TPSC=0 is Pphi/4, or one decrement per 16 SH-4 cycles.
	const u64 now = sh4_sched_now64() + sh4_sched_cpu_slice() - Sh4cntx.cycle_counter;
	const int cyclesToEdge = 16 - (now & 15);
	tmu.write<u32>(TMU_TCOR0_addr, 0);
	tmu.write<u32>(TMU_TCNT0_addr, 0);
	tmu.write<u8>(TMU_TSTR_addr, 1);

	if (cyclesToEdge > 1)
	{
		tick(cyclesToEdge - 1);
		EXPECT_EQ(0, tmu.read<u16>(TMU_TCR0_addr) & 0x100);
	}
	tick(1);
	EXPECT_EQ(0x100, tmu.read<u16>(TMU_TCR0_addr) & 0x100);
}

TEST_F(TimerTest, RtcClockModeCountsAt16384Hz)
{
	auto cyclesToNextRtcEdge = []() {
		const u64 current = sh4_sched_now64() + sh4_sched_cpu_slice()
				- Sh4cntx.cycle_counter;
		const u64 tick = current / SH4_MAIN_CLOCK * 16'384
				+ current % SH4_MAIN_CLOCK * 16'384 / SH4_MAIN_CLOCK + 1;
		const u64 edge = tick / 16'384 * SH4_MAIN_CLOCK
				+ ((tick % 16'384) * SH4_MAIN_CLOCK + 16'383) / 16'384;
		return (int)(edge - current);
	};

	tmu.write<u16>(TMU_TCR0_addr, 6);
	tmu.write<u32>(TMU_TCOR0_addr, 1);
	tmu.write<u32>(TMU_TCNT0_addr, 1);
	tmu.write<u8>(TMU_TSTR_addr, 1);
	int edgeCycles = cyclesToNextRtcEdge();
	tick(edgeCycles - 1);
	EXPECT_EQ(1u, tmu.read<u32>(TMU_TCNT0_addr));
	tick(1);
	EXPECT_EQ(0u, tmu.read<u32>(TMU_TCNT0_addr));
	edgeCycles = cyclesToNextRtcEdge();
	tick(edgeCycles - 1);
	EXPECT_EQ(0u, tmu.read<u16>(TMU_TCR0_addr) & 0x100);
	tick(1);
	EXPECT_EQ(0x100u, tmu.read<u16>(TMU_TCR0_addr) & 0x100);
}

TEST_F(TimerTest, ExternalClockModeDoesNotConsumePeripheralCycles)
{
	tmu.write<u16>(TMU_TCR0_addr, 7);
	tmu.write<u32>(TMU_TCNT0_addr, 3);
	tmu.write<u8>(TMU_TSTR_addr, 1);
	tick(100'000);
	EXPECT_EQ(3u, tmu.read<u32>(TMU_TCNT0_addr));
	EXPECT_EQ(0u, tmu.read<u16>(TMU_TCR0_addr) & 0x100);
}

class TmuModuleStandbyTest : public ::testing::Test {
	void SetUp() override {
		tmu.init();
		cpg.init();
	}
	void TearDown() override {
		cpg.term();
		tmu.term();
	}

protected:
	void tick(int cycles) {
		Sh4cntx.sh4_sched_next -= cycles;
		sh4_sched_tick(cycles);
	}
};

TEST_F(TmuModuleStandbyTest, InternalClockStopsWhileRtcClockContinues)
{
	tmu.write<u16>(TMU_TCR0_addr, 0); // Pck / 4 (16 CPU cycles)
	tmu.write<u32>(TMU_TCNT0_addr, 100000);
	tmu.write<u16>(TMU_TCR1_addr, 6); // RTCCLK
	tmu.write<u32>(TMU_TCNT1_addr, 100);
	tmu.write<u8>(TMU_TSTR_addr, 3);

	cpg.write<u8>(CPG_STBCR_addr, 4); // MSTP2
	EXPECT_EQ(2, tmu.read<u8>(TMU_TSTR_addr));
	const u32 stoppedInternalCount = tmu.read<u32>(TMU_TCNT0_addr);
	const u64 current = sh4_sched_now64() + sh4_sched_cpu_slice()
			- Sh4cntx.cycle_counter;
	const u64 nextRtcTick = current / SH4_MAIN_CLOCK * 16'384
			+ current % SH4_MAIN_CLOCK * 16'384 / SH4_MAIN_CLOCK + 1;
	const u64 nextRtcEdge = nextRtcTick / 16'384 * SH4_MAIN_CLOCK
			+ ((nextRtcTick % 16'384) * SH4_MAIN_CLOCK + 16'383) / 16'384;
	tick(nextRtcEdge - current);
	EXPECT_EQ(stoppedInternalCount, tmu.read<u32>(TMU_TCNT0_addr));
	EXPECT_EQ(99u, tmu.read<u32>(TMU_TCNT1_addr));

	// Leaving module standby does not restart channels whose TSTR bits were
	// initialized by entry. Software must explicitly start them again.
	cpg.write<u8>(CPG_STBCR_addr, 0);
	EXPECT_EQ(2, tmu.read<u8>(TMU_TSTR_addr));
}

TEST_F(TmuModuleStandbyTest, FullStandbyStopsRtcClockChannelsToo)
{
	tmu.write<u16>(TMU_TCR0_addr, 6); // RTCCLK
	tmu.write<u32>(TMU_TCNT0_addr, 100);
	tmu.write<u8>(TMU_TSTR_addr, 1);

	TMU_EnterStandby();
	EXPECT_EQ(0, tmu.read<u8>(TMU_TSTR_addr));
	const u32 stoppedCount = tmu.read<u32>(TMU_TCNT0_addr);
	const u64 current = sh4_sched_now64() + sh4_sched_cpu_slice()
			- Sh4cntx.cycle_counter;
	const u64 nextRtcTick = current / SH4_MAIN_CLOCK * 16'384
			+ current % SH4_MAIN_CLOCK * 16'384 / SH4_MAIN_CLOCK + 1;
	const u64 nextRtcEdge = nextRtcTick / 16'384 * SH4_MAIN_CLOCK
			+ ((nextRtcTick % 16'384) * SH4_MAIN_CLOCK + 16'383) / 16'384;
	tick(nextRtcEdge - current);
	EXPECT_EQ(stoppedCount, tmu.read<u32>(TMU_TCNT0_addr));
}
