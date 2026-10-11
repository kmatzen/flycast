#include "types.h"

#if FEAT_AREC != DYNAREC_NONE
#include "hw/mem/addrspace.h"
#include "hw/arm7/arm7.h"
#include "hw/arm7/arm_mem.h"
#include "hw/aica/aica_if.h"
#include "hw/aica/aica.h"
#include "hw/aica/aica_mem.h"
#include "hw/aica/dsp.h"
#include "hw/aica/sgc_if.h"
#include "hw/arm7/arm7_rec.h"
#include "hw/hwreg.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"
#include "emulator.h"
#include "serialize.h"

#include "gtest/gtest.h"

static const u32 N_FLAG = 1 << 31;
static const u32 Z_FLAG = 1 << 30;
static const u32 C_FLAG = 1 << 29;
static const u32 V_FLAG = 1 << 28;
static const u32 NZCV_MASK = N_FLAG | Z_FLAG | C_FLAG | V_FLAG;

namespace aica {
extern int aica_schid;
extern int rtc_schid;
}

namespace aica::arm::recompiler {

extern void (*EntryPoints[])();

static u8 resetMidiByte;

static void captureResetMidiByte(u8 data)
{
	resetMidiByte = data;
}

class AicaArmTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		emu.dc_reset(true);
		Arm7Enabled = true;
	}

	void PrepareOp(u32 op)
	{
		PrepareOps(1, &op);
	}

	void PrepareOps(int count, u32 *ops)
	{
		arm_Reg[R15_ARM_NEXT].I = 0x1000;
		for (int i = 0; i < count; i++)
			*(u32*)&aica_ram[0x1000 + i * 4] = ops[i];
		*(u32*)&aica_ram[0x1000 + count * 4] = 0xea000000 | ((u32)(-count * 4 - 8) << 2);	// b pc+8-12
		flush();
		compile();
	}

	void RunOp()
	{
		arm_Reg[R15_ARM_NEXT].I = 0x1000;
		arm_Reg[CYCL_CNT].I = 1;
		arm_mainloop(arm_Reg, EntryPoints);
	}
	void ResetNZCV()
	{
		arm_Reg[RN_PSR_FLAGS].I &= ~NZCV_MASK;
	}
	void TickSh4(int cycles)
	{
		Sh4cntx.sh4_sched_next -= cycles;
		sh4_sched_tick(cycles);
	}
};
#define ASSERT_NZCV_EQ(expected) ASSERT_EQ(arm_Reg[RN_PSR_FLAGS].I & NZCV_MASK, (expected));

TEST_F(AicaArmTest, ResetStartsWithInterruptsMasked)
{
	arm7ClockTicks = -123;
	aica::arm::reset();

	ASSERT_EQ(arm7ClockTicks, 0);
	ASSERT_EQ(armMode, 0x13);
	ASSERT_FALSE(armIrqEnable);
	ASSERT_FALSE(armFiqEnable);
	ASSERT_EQ(arm_Reg[RN_CPSR].I, 0xd3);
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0);
}

TEST_F(AicaArmTest, ResetClearsRtcWriteEnableWithoutChangingTimestamp)
{
	const u32 low = readRtcReg<u32>(4) & 0xffff;
	writeRtcReg<u32>(8, 1);
	resetRtc(false);
	writeRtcReg<u32>(4, low ^ 0xffff);

	EXPECT_EQ(low, readRtcReg<u32>(4) & 0xffff);
}

TEST_F(AicaArmTest, HardResetPreservesExternalMidiConnection)
{
	resetMidiByte = 0;
	setMidiReceiver(captureResetMidiByte);

	aica::reset(true);
	writeRegInternal<u8>(0x280c, 0x5a);

	EXPECT_EQ(0, resetMidiByte);
	midiOutTransmitByte();
	EXPECT_EQ(0x5a, resetMidiByte);
	setMidiReceiver(nullptr);
}

TEST_F(AicaArmTest, RtcTimestampWritesLowFirstAndHighLocks)
{
	writeRtcReg<u32>(8, 1);
	writeRtcReg<u32>(4, 0x4567);
	writeRtcReg<u32>(0, 0x1234);
	EXPECT_EQ(0x1234u, readRtcReg<u32>(0));
	EXPECT_EQ(0x4567u, readRtcReg<u32>(4));

	// The high-half write closes the write window. Both halves ignore further
	// writes until software explicitly unlocks the timestamp again.
	writeRtcReg<u32>(4, 0xaaaa);
	writeRtcReg<u32>(0, 0xbbbb);
	EXPECT_EQ(0x1234u, readRtcReg<u32>(0));
	EXPECT_EQ(0x4567u, readRtcReg<u32>(4));

	// Writing high first is legal but immediately locks out the low half,
	// which is why firmware must program the timestamp low-first.
	writeRtcReg<u32>(8, 1);
	writeRtcReg<u32>(0, 0xabcd);
	writeRtcReg<u32>(4, 0xef01);
	EXPECT_EQ(0xabcdu, readRtcReg<u32>(0));
	EXPECT_EQ(0x4567u, readRtcReg<u32>(4));
}

TEST_F(AicaArmTest, LateSampleCallbackConsumesEveryElapsedSample)
{
	Arm7Enabled = false;
	setSampleClockPhase(0);
	sh4_sched_request(aica::aica_schid, SampleClock::BaseCycles);

	// Three consecutive base-length sample intervals elapse before dispatch.
	TickSh4(3 * SampleClock::BaseCycles);
	EXPECT_EQ(3 * SampleClock::CycleRemainder, sampleClockPhase());
}

TEST_F(AicaArmTest, LateRtcCallbackConsumesEveryElapsedSecond)
{
	writeRtcReg<u32>(8, 1);
	writeRtcReg<u32>(4, 0x5678);
	writeRtcReg<u32>(0, 0x1234);
	sh4_sched_request(aica::rtc_schid, SH4_MAIN_CLOCK);

	TickSh4(3 * SH4_MAIN_CLOCK);
	const u32 timestamp = readRtcReg<u32>(0) << 16 | readRtcReg<u32>(4);
	EXPECT_EQ(0x1234567bu, timestamp);
}

TEST_F(AicaArmTest, ArmInterruptUsesFixedSourcePriorityAndRelatchesNextSource)
{
	// MIDI input has fixed priority over timer A even though its programmed
	// autovector level is numerically lower.
	writeTimerAndIntReg<u16>(SCILV0_addr, 1 << 3);
	writeTimerAndIntReg<u16>(SCILV1_addr, 1 << 6);
	writeTimerAndIntReg<u16>(SCILV2_addr, 1 << 6);
	SCIPD->full = (1 << 3) | (1 << 6);
	writeTimerAndIntReg<u16>(SCIEB_addr, (1 << 3) | (1 << 6));

	ASSERT_TRUE(e68k_out);
	EXPECT_EQ(1u, e68k_reg_L);

	// Clearing the active source does not alter the held L value until the
	// ARM7 signals end-of-interrupt. Reserved M bits are not acknowledgements.
	writeTimerAndIntReg<u16>(SCIRE_addr, 1 << 3);
	EXPECT_EQ(1u, e68k_reg_L);
	writeReg<u8>(REG_M, 2);
	EXPECT_TRUE(e68k_out);
	EXPECT_EQ(1u, e68k_reg_L);

	// A one in M0 releases the latch. Since timer A remains pending, the
	// interrupt output is immediately asserted again with its level code.
	writeReg<u8>(REG_M, 1);
	EXPECT_TRUE(e68k_out);
	EXPECT_EQ(6u, e68k_reg_L);

	writeTimerAndIntReg<u16>(SCIRE_addr, 1 << 6);
	writeReg<u8>(REG_M, 1);
	EXPECT_FALSE(e68k_out);
}

TEST_F(AicaArmTest, ArmInterruptLevelZeroDoesNotAssert)
{
	SCIPD->full = (1 << 3) | (1 << 6);
	writeTimerAndIntReg<u16>(SCIEB_addr, (1 << 3) | (1 << 6));
	EXPECT_FALSE(e68k_out);

	// The lower-numbered MIDI source remains at disabled level zero. Raising
	// timer A while both are pending must skip MIDI and present timer A.
	writeTimerAndIntReg<u16>(SCILV1_addr, 1 << 6);
	EXPECT_TRUE(e68k_out);
	EXPECT_EQ(2u, e68k_reg_L);
}

TEST_F(AicaArmTest, ExhaustedTimesliceDoesNotExecuteInstruction)
{
	PrepareOp(0xe3a00001); // mov r0, #1
	arm_Reg[0].I = 0;
	arm_Reg[CYCL_CNT].I = 0;

	arm_mainloop(arm_Reg, EntryPoints);

	EXPECT_EQ(arm_Reg[0].I, 0u);
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1000u);
}

TEST_F(AicaArmTest, FiqEntryCanExhaustTimesliceBeforeVectorInstruction)
{
	PrepareOp(0xe1a00000); // dispatcher entry point at 0x1000
	armMode = 0x13;
	armIrqEnable = true;
	armFiqEnable = true;
	arm_Reg[RN_CPSR].I = 0x13;
	arm_Reg[INTR_PEND].I = 1;
	arm_Reg[CYCL_CNT].I = 2;

	arm_mainloop(arm_Reg, EntryPoints);

	EXPECT_EQ(armMode, 0x11);
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1cu);
	EXPECT_EQ(arm_Reg[CYCL_CNT].I, 0);
}

TEST_F(AicaArmTest, TimerInterruptsOnFfToZeroRollover)
{
	writeTimerAndIntReg<u32>(TIMER_A, 0xfd);
	SCIPD->TimerA = 0;
	timers[0].StepTimer(0);
	EXPECT_EQ(0xfd, aica_reg[TIMER_A]);
	EXPECT_EQ(0u, SCIPD->TimerA);

	timers[0].StepTimer(1);
	EXPECT_EQ(0xfe, aica_reg[TIMER_A]);
	EXPECT_EQ(0u, SCIPD->TimerA);

	timers[0].StepTimer(1);
	EXPECT_EQ(0xff, aica_reg[TIMER_A]);
	EXPECT_EQ(0u, SCIPD->TimerA);

	timers[0].StepTimer(1);
	EXPECT_EQ(0, aica_reg[TIMER_A]);
	EXPECT_EQ(1u, SCIPD->TimerA);

	writeTimerAndIntReg<u32>(SCIRE_addr, 1 << 6);
	EXPECT_EQ(0u, SCIPD->TimerA);
	timers[0].StepTimer(1);
	EXPECT_EQ(1, aica_reg[TIMER_A]);
	EXPECT_EQ(0u, SCIPD->TimerA);
}

TEST_F(AicaArmTest, MidiInputInterruptRequiresEmptyToNonemptyTransition)
{
	midiSend(0x11);
	midiSend(0x22);
	ASSERT_EQ(1u, SCIPD->MIDI_IN);
	ASSERT_EQ(1u, MCIPD->MIDI_IN);

	writeTimerAndIntReg<u16>(SCIRE_addr, 1 << 3);
	writeTimerAndIntReg<u16>(MCIRE_addr, 1 << 3);
	EXPECT_EQ(0u, SCIPD->MIDI_IN);
	EXPECT_EQ(0u, MCIPD->MIDI_IN);
	midiSend(0x33);
	EXPECT_EQ(0u, SCIPD->MIDI_IN);
	EXPECT_EQ(0u, MCIPD->MIDI_IN);

	u8 data = 0;
	EXPECT_TRUE(midiRead(data));
	EXPECT_TRUE(midiRead(data));
	EXPECT_TRUE(midiRead(data));
	EXPECT_FALSE(midiRead(data));
	midiSend(0x44);
	EXPECT_EQ(1u, SCIPD->MIDI_IN);
	EXPECT_EQ(1u, MCIPD->MIDI_IN);
}

TEST_F(AicaArmTest, MidiInputSaveRestorePreservesAcknowledgedPendingBits)
{
	midiSend(0x11);
	midiSend(0x22);
	writeTimerAndIntReg<u16>(SCIRE_addr, 1 << 3);
	writeTimerAndIntReg<u16>(MCIRE_addr, 1 << 3);
	ASSERT_EQ(0u, SCIPD->MIDI_IN);
	ASSERT_EQ(0u, MCIPD->MIDI_IN);

	std::vector<u8> state(1_MB);
	Serializer serializer(state.data(), state.size());
	sgc::serialize(serializer);

	// aica::deserialize restores aica_reg before restoring the SGC/FIFO state.
	// Model those authoritative serialized latch values here.
	midiSendBuffer.clear();
	SCIPD->MIDI_IN = 0;
	MCIPD->MIDI_IN = 0;
	Deserializer deserializer(state.data(), serializer.size());
	sgc::deserialize(deserializer);

	EXPECT_EQ(2u, midiSendBuffer.size());
	EXPECT_EQ(0u, SCIPD->MIDI_IN);
	EXPECT_EQ(0u, MCIPD->MIDI_IN);
}

TEST_F(AicaArmTest, TimerWriteRestartsPrescalerPhase)
{
	writeTimerAndIntReg<u32>(TIMER_A, (2 << 8) | 3);
	timers[0].StepTimer(2);
	EXPECT_EQ(2, timers[0].c_step);

	writeTimerAndIntReg<u32>(TIMER_A, (2 << 8) | 7);
	EXPECT_EQ(4, timers[0].c_step);
	timers[0].StepTimer(3);
	EXPECT_EQ(7, aica_reg[TIMER_A]);
	timers[0].StepTimer(1);
	EXPECT_EQ(8, aica_reg[TIMER_A]);
}

TEST_F(AicaArmTest, InternalDmaRegistersUseSparseFourByteEntries)
{
	// Post-MIG hardware exposes DMEA[15:2], DRGA[14:2], and DLG[14:2].
	// Each count advances one four-byte register-file entry, whose implemented
	// low halfword receives the corresponding sound-memory word.
	WriteMemArr(&aica_ram[0], 0x100, (u32)0xaaaa1234);
	WriteMemArr(&aica_ram[0], 0x104, (u32)0xbbbb5678);
	WriteMemArr(&aica_ram[0], 0x108, (u32)0xcccc9abc);
	*(u16 *)&aica_reg[0x400] = 0;
	*(u16 *)&aica_reg[0x404] = 0;
	*(u16 *)&aica_reg[0x408] = 0;

	writeAicaReg(0x2880, (u16)0);       // DMEA[22:16]
	writeAicaReg(0x2884, (u16)0x0100);  // DMEA[15:2]
	writeAicaReg(0x2888, (u16)0x0400);  // DRGA[14:2]
	writeAicaReg(0x288c, (u16)0x000d);  // DLG=3 entries, DEXE=1

	EXPECT_EQ(0, *(u16 *)&aica_reg[0x400]);
	EXPECT_EQ(1u, CommonData->DEXE);
	EXPECT_EQ(0u, SCIPD->DMA_END);
	TickSh4(2);
	EXPECT_EQ(0, *(u16 *)&aica_reg[0x400]);
	TickSh4(1);
	EXPECT_EQ(0x1234, *(u16 *)&aica_reg[0x400]);
	EXPECT_EQ(0, *(u16 *)&aica_reg[0x404]);
	EXPECT_EQ(1u, CommonData->DEXE);
	TickSh4(6);
	EXPECT_EQ(0x5678, *(u16 *)&aica_reg[0x404]);
	EXPECT_EQ(0x9abc, *(u16 *)&aica_reg[0x408]);
	EXPECT_EQ(0u, CommonData->DEXE);
	EXPECT_EQ(1u, SCIPD->DMA_END);
	EXPECT_EQ(1u, MCIPD->DMA_END);
}

TEST_F(AicaArmTest, InternalDmaWritesOnlyOneWordPerSparseEntry)
{
	WriteMemArr(&aica_ram[0], 0x100, (u32)0xaabb5678);
	WriteMemArr(aica_reg, 0x404, (u32)0xccdd1234);

	writeAicaReg(0x2880, (u16)0);
	writeAicaReg(0x2884, (u16)0x0100);
	writeAicaReg(0x2888, (u16)0x0404);
	writeAicaReg(0x288c, (u16)0x8005); // DDIR=1, DLG=1, DEXE=1
	TickSh4(3);

	EXPECT_EQ(0xaabb1234u, ReadMemArr<u32>(&aica_ram[0], 0x100));

	// DGATE uses the same word-wide payload and must preserve the other
	// halfword at the sparse sound-memory destination.
	WriteMemArr(&aica_ram[0], 0x100, (u32)0xeeff5678);
	writeAicaReg(0x2888, (u16)0x8404); // DGATE=1, DRGA=0x404
	writeAicaReg(0x288c, (u16)0x8005);
	TickSh4(3);

	EXPECT_EQ(0xeeff0000u, ReadMemArr<u32>(&aica_ram[0], 0x100));
}

TEST_F(AicaArmTest, ZeroLengthInternalDmaCompletesWithoutTransfer)
{
	WriteMemArr(&aica_ram[0], 0x100, (u32)0x12345678);
	WriteMemArr(aica_reg, 0x400, (u32)0xaaaa5555);

	writeAicaReg(0x2880, (u16)0);
	writeAicaReg(0x2884, (u16)0x0100);
	writeAicaReg(0x2888, (u16)0x0400);
	writeAicaReg(0x288c, (u16)0x0001); // DLG=0, DEXE=1

	EXPECT_EQ(0u, CommonData->DEXE);
	EXPECT_EQ(1u, SCIPD->DMA_END);
	EXPECT_EQ(1u, MCIPD->DMA_END);
	EXPECT_EQ(0x12345678u, ReadMemArr<u32>(&aica_ram[0], 0x100));
	EXPECT_EQ(0xaaaa5555u, ReadMemArr<u32>(aica_reg, 0x400));
}

TEST_F(AicaArmTest, InternalDmaStopsAtSoundRamEnd)
{
	const u32 lastEntry = soundRamMask() - 3;
	WriteMemArr(&aica_ram[0], lastEntry, (u32)0xaaaa1234);
	WriteMemArr(aica_reg, 0x400, (u32)0xbbbb5678);
	WriteMemArr(aica_reg, 0x404, (u32)0xcccc9abc);

	// Start three memory-to-register entries at the final aligned sound-RAM
	// address. DMEA does not wrap, so only the first entry is transferred.
	writeAicaReg(0x2880, static_cast<u16>((lastEntry >> 16) << 9));
	writeAicaReg(0x2884, static_cast<u16>(lastEntry));
	writeAicaReg(0x2888, (u16)0x0400);
	writeAicaReg(0x288c, (u16)0x000d); // DLG=3, DEXE=1

	TickSh4(3);
	EXPECT_EQ(0x1234u, ReadMemArr<u16>(aica_reg, 0x400));
	EXPECT_EQ(0x9abcu, ReadMemArr<u16>(aica_reg, 0x404));
	EXPECT_EQ(0u, CommonData->DEXE);
	EXPECT_EQ(1u, SCIPD->DMA_END);
	EXPECT_EQ(1u, MCIPD->DMA_END);
}

TEST_F(AicaArmTest, LateInternalDmaDispatchPreservesNextMemoryClockEdge)
{
	for (u32 i = 0; i < 5; ++i)
	{
		WriteMemArr(&aica_ram[0], 0x100 + i * 4, 0x11110000u + i);
		WriteMemArr(aica_reg, 0x400 + i * 4, 0xaaaaaaaau);
	}

	writeAicaReg(0x2880, (u16)0);
	writeAicaReg(0x2884, (u16)0x0100);
	writeAicaReg(0x2888, (u16)0x0400);
	writeAicaReg(0x288c, (u16)0x0015); // DLG=5 entries, DEXE=1

	// The first three transfer edges are at SH4 cycles 3, 6, and 9. A
	// dispatch at cycle 10 catches up those entries but must not expose the
	// fourth entry before its edge at cycle 12.
	TickSh4(10);
	EXPECT_EQ(0x0000u, ReadMemArr<u16>(aica_reg, 0x400));
	EXPECT_EQ(0x0001u, ReadMemArr<u16>(aica_reg, 0x404));
	EXPECT_EQ(0x0002u, ReadMemArr<u16>(aica_reg, 0x408));
	EXPECT_EQ(0xaaaau, ReadMemArr<u16>(aica_reg, 0x40c));
	EXPECT_TRUE(CommonData->DEXE);

	TickSh4(1);
	EXPECT_EQ(0xaaaau, ReadMemArr<u16>(aica_reg, 0x40c));
	TickSh4(1);
	EXPECT_EQ(0x0003u, ReadMemArr<u16>(aica_reg, 0x40c));
	EXPECT_EQ(0xaaaau, ReadMemArr<u16>(aica_reg, 0x410));
	EXPECT_TRUE(CommonData->DEXE);

	TickSh4(3);
	EXPECT_EQ(0x0004u, ReadMemArr<u16>(aica_reg, 0x410));
	EXPECT_FALSE(CommonData->DEXE);
	EXPECT_TRUE(SCIPD->DMA_END);
	EXPECT_TRUE(MCIPD->DMA_END);
}

TEST_F(AicaArmTest, InternalDmaSoundMemoryInhibitSuppressesOnlyMemoryAccess)
{
	*(u16 *)&aica_ram[0x100] = 0x1234;
	*(u16 *)&aica_reg[0x400] = 0x5678;

	writeAicaReg(0x2880, (u16)0x0008);  // inhibit MCPU DMA sound-memory access
	writeAicaReg(0x2884, (u16)0x0100);  // DMEA=0x100
	writeAicaReg(0x2888, (u16)0x0400);  // DRGA=0x400, memory to registers
	writeAicaReg(0x288c, (u16)0x0005);  // DLG=1 entry, DEXE=1

	EXPECT_EQ(1u, CommonData->DEXE);
	EXPECT_EQ(0u, SCIPD->DMA_END);
	TickSh4(3);
	EXPECT_EQ(0x5678, *(u16 *)&aica_reg[0x400]);
	EXPECT_EQ(0x1234, *(u16 *)&aica_ram[0x100]);
	EXPECT_EQ(0u, CommonData->DEXE);
	EXPECT_EQ(1u, SCIPD->DMA_END);
	EXPECT_EQ(1u, MCIPD->DMA_END);

	// DGATE clearing a register destination does not access sound memory;
	// MRWINH explicitly cannot prohibit register access.
	writeAicaReg(0x2888, (u16)0x8400);  // DGATE=1, DRGA=0x400
	writeAicaReg(0x288c, (u16)0x0005);
	EXPECT_EQ(1u, CommonData->DEXE);
	TickSh4(3);

	EXPECT_EQ(0, *(u16 *)&aica_reg[0x400]);
	EXPECT_EQ(0x1234, *(u16 *)&aica_ram[0x100]);
}

TEST_F(AicaArmTest, InternalDmaSamplesMemoryInhibitAtEachWordBoundary)
{
	*(u16 *)&aica_ram[0x100] = 0x1234;
	*(u16 *)&aica_ram[0x104] = 0x5678;
	WriteMemArr(aica_reg, 0x3400, static_cast<u16>(0xaaaa));
	WriteMemArr(aica_reg, 0x3404, static_cast<u16>(0xbbbb));

	writeAicaReg(0x2880, static_cast<u16>(0));
	writeAicaReg(0x2884, static_cast<u16>(0x0100));
	writeAicaReg(0x2888, static_cast<u16>(0x3400));
	writeAicaReg(0x288c, static_cast<u16>(0x0009)); // two entries, DEXE=1

	TickSh4(3);
	EXPECT_EQ(0x1234, ReadMemArr<u16>(aica_reg, 0x3400));
	EXPECT_TRUE(CommonData->DEXE);
	writeAicaReg(0x2880, static_cast<u16>(8));
	TickSh4(3);
	EXPECT_EQ(0xbbbb, ReadMemArr<u16>(aica_reg, 0x3404));
	EXPECT_FALSE(CommonData->DEXE);

	// Clearing the gate during a later transfer enables its next request.
	WriteMemArr(aica_reg, 0x3400, static_cast<u16>(0xaaaa));
	WriteMemArr(aica_reg, 0x3404, static_cast<u16>(0xbbbb));
	writeAicaReg(0x2884, static_cast<u16>(0x0100));
	writeAicaReg(0x2888, static_cast<u16>(0x3400));
	writeAicaReg(0x288c, static_cast<u16>(0x0009));
	TickSh4(3);
	EXPECT_EQ(0xaaaa, ReadMemArr<u16>(aica_reg, 0x3400));
	writeAicaReg(0x2880, static_cast<u16>(0));
	TickSh4(3);
	EXPECT_EQ(0x5678, ReadMemArr<u16>(aica_reg, 0x3404));
	EXPECT_FALSE(CommonData->DEXE);
}

TEST_F(AicaArmTest, InternalDmaCannotOverwriteItsParameterRegisters)
{
	for (u32 address = 0x100; address < 0x110; address += 4)
		WriteMemArr(&aica_ram[0], address, 0xffffffffu);

	writeAicaReg(0x2880, (u16)0);       // DMEA[22:16]
	writeAicaReg(0x2884, (u16)0x0100);  // DMEA=0x100
	writeAicaReg(0x2888, (u16)0x2880);  // DRGA=0x2880
	writeAicaReg(0x288c, (u16)0x0011);  // DLG=4 entries, DEXE=1

	TickSh4(12);

	EXPECT_EQ(0x0000, ReadMemArr<u16>(aica_reg, 0x2880));
	EXPECT_EQ(0x0100, ReadMemArr<u16>(aica_reg, 0x2884));
	EXPECT_EQ(0x2880, ReadMemArr<u16>(aica_reg, 0x2888));
	EXPECT_EQ(0x0010, ReadMemArr<u16>(aica_reg, 0x288c));
	EXPECT_EQ(0u, CommonData->DEXE);
	EXPECT_EQ(1u, SCIPD->DMA_END);
	EXPECT_EQ(1u, MCIPD->DMA_END);
}

TEST_F(AicaArmTest, DspMemoryInhibitInvalidatesCompiledProgramOnGateChanges)
{
	dsp::state.dirty = false;
	writeAicaReg(0x2880, static_cast<u16>(2)); // sound-source inhibit only
	EXPECT_FALSE(dsp::state.dirty);

	writeAicaReg(0x2880, static_cast<u16>(3)); // enable DSP inhibit
	EXPECT_TRUE(dsp::state.dirty);
	dsp::state.dirty = false;

	writeAicaReg(0x2880, static_cast<u16>(3)); // unchanged DSP gate
	EXPECT_FALSE(dsp::state.dirty);
	writeAicaReg(0x2880, static_cast<u16>(2)); // disable DSP inhibit
	EXPECT_TRUE(dsp::state.dirty);
}

TEST_F(AicaArmTest, SoundCpuMemoryInhibitStallsExecution)
{
	PrepareOp(0xe3a00001); // mov r0, #1
	arm_Reg[0].I = 0;
	arm_Reg[CYCL_CNT].I = 0;
	CommonData->MRWINH = 4;

	aica::arm::run(1);
	EXPECT_EQ(arm_Reg[0].I, 0u);
	// The gated ARM clock is stopped, not merely prevented from dispatching.
	// It must not accumulate a deferred timeslice that runs after MRWINH clears.
	EXPECT_EQ(arm_Reg[CYCL_CNT].I, 0);

	CommonData->MRWINH = 0;
	aica::arm::run(1);
	EXPECT_EQ(arm_Reg[0].I, 1u);
}

TEST_F(AicaArmTest, ByteWritesPreserveRegisterLanesAndSideEffects)
{
	SCIEB->full = 0x055;
	writeTimerAndIntReg<u8>(SCIEB_addr + 1, 0x03);
	EXPECT_EQ(0x355u, SCIEB->full);
	writeTimerAndIntReg<u8>(SCIEB_addr, 0xaa);
	EXPECT_EQ(0x3aau, SCIEB->full);

	writeTimerAndIntReg<u32>(TIMER_A, 5);
	writeTimerAndIntReg<u8>(TIMER_A + 1, 3);
	EXPECT_EQ(5, aica_reg[TIMER_A]);
	EXPECT_EQ(8, timers[0].c_step);
}

TEST_F(AicaArmTest, ReservedCommonRegisterBitsDoNotLatch)
{
	writeTimerAndIntReg<u16>(TIMER_A, 0xffff);
	EXPECT_EQ(0xffu, CommonData->TIMA);
	EXPECT_EQ(7u, CommonData->TACTL);
	EXPECT_EQ(0, readRegInternal<u16>(TIMER_A));

	writeTimerAndIntReg<u16>(SCILV0_addr, 0xffff);
	EXPECT_EQ(0xffu, CommonData->SCILV0);
	EXPECT_EQ(0, readRegInternal<u16>(SCILV0_addr));
	writeTimerAndIntReg<u8>(SCILV0_addr + 1, 0xff);
	EXPECT_EQ(0xffu, CommonData->SCILV0);
	EXPECT_EQ(0, readRegInternal<u16>(SCILV0_addr));

	writeTimerAndIntReg<u16>(0x2880, 0xffff);
	EXPECT_EQ(0xfu, CommonData->MRWINH);
	EXPECT_EQ(0x7fu, CommonData->DMEA_hi);
	EXPECT_EQ(0, readRegInternal<u16>(0x2880));
	writeTimerAndIntReg<u16>(0x2884, 0xffff);
	EXPECT_EQ(0x7fffu, CommonData->DMEA_lo);
	EXPECT_EQ(0, readRegInternal<u16>(0x2884));
	writeTimerAndIntReg<u16>(0x2888, 0xffff);
	EXPECT_EQ(1u, CommonData->DGATE);
	EXPECT_EQ(0x3fffu, CommonData->DRGA);
	EXPECT_EQ(0x8000, readRegInternal<u16>(0x2888));

	writeTimerAndIntReg<u16>(0x288c, 0xfffe); // do not start DMA
	EXPECT_EQ(1u, CommonData->DDIR);
	EXPECT_EQ(0x3fffu, CommonData->DLG);
	EXPECT_EQ(0x8000, readRegInternal<u16>(0x288c));
}

TEST_F(AicaArmTest, InterruptResetRegistersAreWriteOnly)
{
	SCIPD->full = 0x7ff;
	MCIPD->full = 0x7ff;
	writeTimerAndIntReg<u16>(SCIRE_addr, 0x055);
	writeTimerAndIntReg<u16>(MCIRE_addr, 0x155);

	EXPECT_EQ(0, readRegInternal<u16>(SCIRE_addr));
	EXPECT_EQ(0, readRegInternal<u16>(MCIRE_addr));
	EXPECT_EQ(0x7aau, SCIPD->full);
	EXPECT_EQ(0x6aau, MCIPD->full);
}

TEST_F(AicaArmTest, AicaVersionIsFixedAndReadOnly)
{
	EXPECT_EQ(1u, CommonData->VER);
	EXPECT_EQ(0x10, readRegInternal<u8>(0x2800));

	writeRegInternal<u8>(0x2800, 0xaf);
	EXPECT_EQ(0xfu, CommonData->MVOL);
	EXPECT_EQ(1u, CommonData->VER);
	EXPECT_EQ(0x10, readRegInternal<u8>(0x2800));

	writeRegInternal<u16>(0x2800, 0x8305);
	EXPECT_EQ(5u, CommonData->MVOL);
	EXPECT_EQ(1u, CommonData->VER);
	EXPECT_EQ(1u, CommonData->DAC18B);
	EXPECT_EQ(1u, CommonData->MEM8MB);
	EXPECT_EQ(1u, CommonData->Mono);
	EXPECT_EQ(0x0010, readRegInternal<u16>(0x2800));
	EXPECT_EQ(0, readRegInternal<u8>(0x2801));
}

TEST_F(AicaArmTest, VregAndArmResetDiscardReservedBits)
{
	writeAicaReg(0x2c01, static_cast<u8>(0xff));
	EXPECT_EQ(3, readAicaReg<u8>(0x2c01));

	writeAicaReg(0x2c00, static_cast<u16>(0xffff));
	EXPECT_EQ(0x0301, readAicaReg<u16>(0x2c00));

	writeAicaReg(0x2c00, static_cast<u32>(0xffff0300));
	EXPECT_EQ(0x0300u, readAicaReg<u32>(0x2c00));
}

TEST_F(AicaArmTest, ArmSharesVregButCannotControlItsOwnReset)
{
	writeAicaReg(0x2c00, static_cast<u16>(0x0101));
	EXPECT_EQ(0x0101, readReg<u16>(0x2c00));

	writeReg(0x2c00, static_cast<u8>(0));
	EXPECT_EQ(0x0101, readAicaReg<u16>(0x2c00));

	writeReg(0x2c00, static_cast<u16>(0x0300));
	EXPECT_EQ(0x0301, readAicaReg<u16>(0x2c00));
	EXPECT_EQ(3, readReg<u8>(0x2c01));

	writeReg(0x2c01, static_cast<u8>(2));
	EXPECT_EQ(0x0201, readAicaReg<u16>(0x2c00));
}

TEST_F(AicaArmTest, ArmReadProtectBitUsesWriteOnlyHighLaneOfMRegister)
{
	CommonData->RP = 0;
	e68k_reg_M = 0;

	writeReg<u16>(REG_M, 0x100);
	EXPECT_EQ(1u, CommonData->RP);
	EXPECT_EQ(0, readReg<u16>(REG_M));
	EXPECT_EQ(0, readReg<u8>(REG_M + 1));
	writeAicaReg<u16>(REG_M, 0);
	EXPECT_EQ(1u, CommonData->RP);

	// A low-byte completion write must not disturb RP.
	writeReg<u8>(REG_M, 0);
	EXPECT_EQ(1u, CommonData->RP);
	writeReg<u8>(REG_M + 1, 0);
	EXPECT_EQ(0u, CommonData->RP);
}

TEST_F(AicaArmTest, ArmReadProtectBitBlocksMainCpuSoundRamWrites)
{
	constexpr u32 sh4SoundRam = 0x00800100;
	*(u32 *)&aica_ram[0x100] = 0x11223344;

	CommonData->RP = 0;
	addrspace::write32(sh4SoundRam, 0x55667788);
	EXPECT_EQ(0x55667788u, *(u32 *)&aica_ram[0x100]);

	CommonData->RP = 1;
	addrspace::write32(sh4SoundRam, 0xaabbccdd);
	EXPECT_EQ(0x55667788u, *(u32 *)&aica_ram[0x100]);
	EXPECT_EQ(0x55667788u, addrspace::read32(sh4SoundRam));

	// ARM7 retains normal DRAM access while the main-CPU path is protected.
	writeMem<u32>(0x100, 0xdeadbeef);
	EXPECT_EQ(0xdeadbeefu, *(u32 *)&aica_ram[0x100]);
}

TEST_F(AicaArmTest, MainCpuMemoryInhibitBlocksSoundRamReadsAndWrites)
{
	constexpr u32 sh4SoundRam = 0x00800140;
	*(u32 *)&aica_ram[0x140] = 0x11223344;
	CommonData->RP = 0;
	CommonData->MRWINH = 8;

	EXPECT_EQ(0u, addrspace::read32(sh4SoundRam));
	addrspace::write32(sh4SoundRam, 0xaabbccdd);
	EXPECT_EQ(0x11223344u, *(u32 *)&aica_ram[0x140]);

	CommonData->MRWINH = 0;
	EXPECT_EQ(0x11223344u, addrspace::read32(sh4SoundRam));
	addrspace::write32(sh4SoundRam, 0xaabbccdd);
	EXPECT_EQ(0xaabbccddu, *(u32 *)&aica_ram[0x140]);
}

TEST_F(AicaArmTest, ArithmeticOpsTest)
{
	PrepareOp(0xe0810002);	// add r0, r1, r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0xbaad0000;
	arm_Reg[2].I = 0x0000cafe;
	arm_Reg[RN_PSR_FLAGS].I |= NZCV_MASK;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xbaadcafe);
	ASSERT_NZCV_EQ(NZCV_MASK);

	PrepareOp(0xe0810000);	// add r0, r1, r0
	arm_Reg[0].I = 11;
	arm_Reg[1].I = 22;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 33);

	PrepareOp(0xe0410002);	// sub r0, r1, r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0xbaadcafe;
	arm_Reg[2].I = 0x0000cafe;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xbaad0000);
	ASSERT_NZCV_EQ(NZCV_MASK);

	PrepareOp(0xe0410000);	// sub r0, r1, r0
	arm_Reg[0].I = 11;
	arm_Reg[1].I = 22;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 11);
	ASSERT_EQ(arm_Reg[1].I, 22);

	PrepareOp(0xe0e00001); // rsc r0, r0, r1
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG;
	arm_Reg[0].I = 3;
	arm_Reg[1].I = 10;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 7);
	ASSERT_EQ(arm_Reg[1].I, 10);

	PrepareOp(0xe0c10000); // sbc r0, r1, r0
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG;
	arm_Reg[0].I = 11;
	arm_Reg[1].I = 22;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 11);
	ASSERT_EQ(arm_Reg[1].I, 22);

	PrepareOp(0xe0910002);	// adds r0, r1, r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x80000000;
	arm_Reg[2].I = 0x80000000;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG | V_FLAG);	// Z,C,V

	PrepareOp(0xe0510002);	// subs r0, r1, r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0xbaadcafe;
	arm_Reg[2].I = 0x0000cafe;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xbaad0000);
	ASSERT_NZCV_EQ(N_FLAG | C_FLAG);	// N,C
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0xbaadcafe;
	arm_Reg[2].I = 0xbaadcafe;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);	// Z,C
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x80000000;
	arm_Reg[2].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x7fffffff);
	ASSERT_NZCV_EQ(C_FLAG | V_FLAG);	// C,V
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0xffffffff;
	arm_Reg[2].I = 0xffffffff;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);	// Z,C

	PrepareOp(0xe0b10002);	// adcs r0, r1, r2
	arm_Reg[RN_PSR_FLAGS].I &= ~NZCV_MASK;
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 3);
	ASSERT_NZCV_EQ(0);	//
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG; // set C
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 4);
	ASSERT_NZCV_EQ(0);	//

	// from armwrestler
	PrepareOp(0xe0d22002);	// sbcs r2,r2,r2
	ResetNZCV();
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG;	// set C
	arm_Reg[2].I = 0xFFFFFFFF;
	RunOp();
	ASSERT_EQ(arm_Reg[2].I, 0);
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);

	// from armwrestler
	PrepareOp(0xe2d22000);	// sbcs r2,r2,#0
	ResetNZCV();
	arm_Reg[2].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[2].I, -1);
	ASSERT_NZCV_EQ(N_FLAG);

	PrepareOp(0xe0d10002);	// sbcs r0, r1, r2
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, -2);
	ASSERT_NZCV_EQ(N_FLAG);	// N

	PrepareOp(0xe0710002);	// rsbs r0, r1, r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	ASSERT_NZCV_EQ(C_FLAG);	// C, confirmed by interpreter and online arm sim

	// Reverse subtraction must derive C from subtraction, not from an
	// equivalent negate-plus-add sequence (which differs at zero).
	PrepareOp(0xe2710000); // rsbs r0, r1, #0
	ResetNZCV();
	arm_Reg[1].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0u);
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);

	PrepareOp(0xe2f10000); // rscs r0, r1, #0
	arm_Reg[RN_PSR_FLAGS].I = C_FLAG;
	arm_Reg[1].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0u);
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);
	ResetNZCV();
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xffffffffu);
	ASSERT_NZCV_EQ(N_FLAG);

	PrepareOp(0xe0f10002);	// rscs r0, r1, r2
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);	// Z,C
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG; // C set
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	ASSERT_NZCV_EQ(C_FLAG);	// C

	PrepareOp(0xe1500001);	// cmp r0, r1
	ResetNZCV();
	arm_Reg[0].I = 2;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_NZCV_EQ(C_FLAG);	// C

	PrepareOp(0xe1700001);	// cmn r0, r1
	ResetNZCV();
	arm_Reg[0].I = 2;
	arm_Reg[1].I = -1;
	RunOp();
	ASSERT_NZCV_EQ(C_FLAG);	// C
}

TEST_F(AicaArmTest, LogicOpsTest)
{
	PrepareOp(0xe1a00001);	// mov r0, r1
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0xbaadcafe;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xbaadcafe);

	PrepareOp(0xe3c100ff);	// bic r0, r1, 0xff
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0xffff;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xff00);

	PrepareOp(0xe0010002);	// and r0, r1, r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0xff0f;
	arm_Reg[2].I = 0xf0f0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xf000);

	PrepareOp(0xe0010000);	// and r0, r1, r0
	arm_Reg[0].I = 0xf0f0f0f0;
	arm_Reg[1].I = 0xffffffff;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xf0f0f0f0);

	PrepareOp(0xe1a00251);	// asr r0, r1, r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0xfffffff8;
	arm_Reg[2].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xfffffffe);

	PrepareOp(0xe1a00241); // asr r0, r1, 4
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x0ffffff0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x00ffffff);

	PrepareOp(0xe0210002);	// eor r0, r1, r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0xffff0000;
	arm_Reg[2].I = 0xf0f0f0f0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x0f0ff0f0);

	PrepareOp(0xe0210000);	// eor r0, r1, r0
	arm_Reg[0].I = 0xf0f0f0f0;
	arm_Reg[1].I = 0xffffffff;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x0f0f0f0f);

	PrepareOp(0xe1810000);	// orr r0, r1, r0
	arm_Reg[0].I = 0xf0f0f0f0;
	arm_Reg[1].I = 0x0f0f0f0f;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xffffffff);

	PrepareOp(0xe1a00211);	// lsl r0, r1, r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 8;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x100);

	PrepareOp(0xe1a00231);	// lsr r0, r1, r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x100;
	arm_Reg[2].I = 4;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x10);

	PrepareOp(0xe1a00271);	// ror r0, r1, r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x12345678;
	arm_Reg[2].I = 16;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x56781234);

	PrepareOp(0xe1300001);	// teq r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_NZCV_EQ(0);
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG | V_FLAG;	// set C,V
	arm_Reg[0].I = 1;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG | V_FLAG);	// Z,C,V

	PrepareOp(0xe1100001);	// tst r0, r1
	ResetNZCV();
	arm_Reg[0].I = 1;
	arm_Reg[1].I = 2;
	RunOp();
	ASSERT_NZCV_EQ(0x40000000);	// Z
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG | V_FLAG;	// set C,V
	arm_Reg[0].I = 3;
	arm_Reg[1].I = 2;
	RunOp();
	ASSERT_NZCV_EQ(C_FLAG | V_FLAG);	// C,V
}

TEST_F(AicaArmTest, Armv4InstructionsAreUndefined)
{
	PrepareOp(0xe1d100b2); // ldrh r0, [r1, #2]
	arm_Reg[0].I = 0x12345678;
	arm_Reg[1].I = 0x10000;
	*(u16*)&aica_ram[0x10002] = 0xcafe;
	RunOp();
	ASSERT_EQ(armMode, 0x1b);
	ASSERT_EQ(arm_Reg[0].I, 0x12345678);
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x04);
	ASSERT_EQ(arm_Reg[14].I, 0x1004);

	aica::arm::reset();
	PrepareOp(0xe0f32190); // smlals r2, r3, r0, r1
	arm_Reg[0].I = 1;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0xffffffff;
	arm_Reg[3].I = 0x7fffffff;
	RunOp();
	ASSERT_EQ(armMode, 0x1b);
	ASSERT_EQ(arm_Reg[2].I, 0xffffffff);
	ASSERT_EQ(arm_Reg[3].I, 0x7fffffff);
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x04);

	aica::arm::reset();
	PrepareOp(0xe12fff10); // bx r0 (ARMv4T)
	arm_Reg[0].I = 0x1201;
	RunOp();
	ASSERT_EQ(armMode, 0x1b);
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x04);
	ASSERT_EQ(arm_Reg[14].I, 0x1004);

	aica::arm::reset();
	PrepareOp(0xe16f1f10); // clz r1, r0 (ARMv5)
	arm_Reg[0].I = 1;
	arm_Reg[1].I = 0x12345678;
	RunOp();
	ASSERT_EQ(armMode, 0x1b);
	ASSERT_EQ(arm_Reg[1].I, 0x12345678u);
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x04);
}

TEST_F(AicaArmTest, LogicalFlagOpsPreserveOverflow)
{
	PrepareOp(0xe0110002); // ands r0, r1, r2
	arm_Reg[RN_PSR_FLAGS].I = V_FLAG;
	arm_Reg[1].I = 0xffffffff;
	arm_Reg[2].I = 0x80000000;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x80000000u);
	EXPECT_EQ(arm_Reg[RN_PSR_FLAGS].I & (N_FLAG | V_FLAG), N_FLAG | V_FLAG);

	PrepareOp(0xe1b00001); // movs r0, r1
	arm_Reg[RN_PSR_FLAGS].I = V_FLAG | C_FLAG;
	arm_Reg[1].I = 0;
	RunOp();
	EXPECT_EQ(arm_Reg[RN_PSR_FLAGS].I & NZCV_MASK, Z_FLAG | C_FLAG | V_FLAG);

	// #2 encoded as 0x20 ROR #4 has a real shifter carry-out even though
	// its decoded value fits in eight bits. It must clear, not preserve, C.
	PrepareOp(0xe3b00220); // movs r0, #0x20, 4
	arm_Reg[RN_PSR_FLAGS].I = V_FLAG | C_FLAG;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 2u);
	EXPECT_EQ(arm_Reg[RN_PSR_FLAGS].I & NZCV_MASK, V_FLAG);

	PrepareOp(0xe3b00480); // movs r0, #0x80, 8
	arm_Reg[RN_PSR_FLAGS].I = V_FLAG;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x80000000u);
	EXPECT_EQ(arm_Reg[RN_PSR_FLAGS].I & NZCV_MASK, N_FLAG | C_FLAG | V_FLAG);
}

TEST_F(AicaArmTest, MultiplyDestinationAliasing)
{
	PrepareOp(0xe0000190); // mul r0, r0, r1
	arm_Reg[0].I = 7;
	arm_Reg[1].I = 9;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);

	PrepareOp(0xe0100190); // muls r0, r0, r1
	ResetNZCV();
	arm_Reg[0].I = 7;
	arm_Reg[1].I = 9;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	ASSERT_NZCV_EQ(Z_FLAG);
}

TEST_F(AicaArmTest, Arm7DiMultiplyTiming)
{
	PrepareOp(0xe1a00000); // mov r0, r0
	RunOp();
	const s32 baseCost = 1 - arm_Reg[CYCL_CNT].I;

	auto multiplyCost = [this](u32 multiplier, bool accumulate) {
		PrepareOp(accumulate ? 0xe0203291 : 0xe0000291); // mla/mul r0, r1, r2[, r3]
		arm_Reg[1].I = 3;
		arm_Reg[2].I = multiplier;
		arm_Reg[3].I = 7;
		RunOp();
		return 1 - arm_Reg[CYCL_CNT].I;
	};

	EXPECT_EQ(multiplyCost(0, false), baseCost + 1);
	EXPECT_EQ(multiplyCost(2, false), baseCost + 2);
	EXPECT_EQ(multiplyCost(8, false), baseCost + 3);
	EXPECT_EQ(multiplyCost(1u << 29, false), baseCost + 16);
	EXPECT_EQ(multiplyCost(0xffffffff, false), baseCost + 1);
	// Unlike the later ARM7DMI, ARM7DI overlaps accumulation with the first
	// Booth cycle, so MLA has the same 1S+mI timing as MUL.
	EXPECT_EQ(multiplyCost(8, true), baseCost + 3);
}

TEST_F(AicaArmTest, BitFourSetSingleTransferEncodingIsUndefined)
{
	PrepareOp(0xe7910012); // 011-class register-offset LDR with reserved bit 4
	arm_Reg[1].I = 0x1100;
	arm_Reg[2].I = 0;
	*(u32*)&aica_ram[0x1100] = 0x12345678;
	RunOp();
	EXPECT_EQ(armMode, 0x1b);
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x04u);
	EXPECT_EQ(arm_Reg[14].I, 0x1004u);
}

TEST_F(AicaArmTest, Arm7DiMultiplyBitSixAliasDoesNotTrap)
{
	PrepareOp(0xe00201d0); // MUL r2, r0, r1 with documented undefined bit 6 set
	arm_Reg[0].I = 7;
	arm_Reg[1].I = 9;
	arm_Reg[2].I = 0;
	RunOp();
	EXPECT_EQ(armMode, 0x13);
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1004u);
}

TEST_F(AicaArmTest, SegaFillWordIsNotTreatedAsANop)
{
	PrepareOp(0x41474553); // reserved MI encoding, also ARAM's "SEGA" fill word
	arm_Reg[RN_PSR_FLAGS].I = N_FLAG;
	RunOp();
	EXPECT_EQ(armMode, 0x1b);
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x04u);
	EXPECT_EQ(arm_Reg[14].I, 0x1004u);

	PrepareOp(0x41474553); // condition false: architectural no-op
	arm_Reg[RN_PSR_FLAGS].I = 0;
	RunOp();
	EXPECT_EQ(armMode, 0x13);
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1004u);
}

TEST_F(AicaArmTest, SystemModeIsInvalidOnArm7Di)
{
	PrepareOp(0xe129f000); // msr cpsr_all, r0
	arm_Reg[0].I = 0x1f;
	ASSERT_TRUE(Arm7Enabled);

	RunOp();

	ASSERT_FALSE(Arm7Enabled);
	ASSERT_EQ(armMode, 0x1f);
}

TEST_F(AicaArmTest, Arm7DiPsrTransfers)
{
	PrepareOp(0xe129f000); // msr cpsr_all, r0
	arm_Reg[0].I = 0xa5bcded3;
	RunOp();
	CPUUpdateCPSR();
	ASSERT_EQ(arm_Reg[RN_CPSR].I, 0xa5bcded3);
	ASSERT_NZCV_EQ(N_FLAG | C_FLAG);
	PrepareOp(0xe10f1000); // mrs r1, cpsr
	arm_Reg[1].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[1].I, 0xa5bcded3);

	aica::arm::reset();
	Arm7Enabled = true;
	arm_Reg[RN_CPSR].I = 0x012345d3;
	PrepareOp(0xe128f000); // msr cpsr_flg, r0
	arm_Reg[0].I = 0xf0000000;
	RunOp();
	CPUUpdateCPSR();
	ASSERT_EQ(arm_Reg[RN_CPSR].I, 0xf12345d3);
	ASSERT_NZCV_EQ(NZCV_MASK);

	aica::arm::reset();
	Arm7Enabled = true;
	PrepareOp(0xe122f000); // ARMv4 msr cpsr_x, r0
	RunOp();
	ASSERT_EQ(armMode, 0x1b);
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x04);
}

TEST_F(AicaArmTest, ForbiddenMrsPcDoesNotAbortDynarec)
{
	// ARM7DI says R15 shall not be the MRS destination. Its resulting state is
	// not architecturally defined, but malformed guest code must remain inside
	// the emulated CPU rather than firing a host verify in the decoder.
	PrepareOp(0xe10ff000); // mrs pc, cpsr
	RunOp();
	EXPECT_TRUE(Arm7Enabled);
}

TEST_F(AicaArmTest, TeqpUsesArm7Di32BitCompatibilitySemantics)
{
	PrepareOp(0xe33ff000); // teqp pc, #0
	armMode = 0x13;
	armIrqEnable = false;
	armFiqEnable = false;
	arm_Reg[RN_PSR_FLAGS].I = N_FLAG;
	arm_Reg[RN_CPSR].I = N_FLAG | 0xd3;
	arm_Reg[RN_SPSR].I = Z_FLAG | C_FLAG | 0x10;
	RunOp();
	EXPECT_EQ(armMode, 0x10);
	EXPECT_EQ(arm_Reg[RN_CPSR].I, Z_FLAG | C_FLAG | 0x10);
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1004u);
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);

	PrepareOp(0xe33ff000); // User-mode TEQP is ignored
	armMode = 0x10;
	armIrqEnable = true;
	armFiqEnable = true;
	arm_Reg[RN_PSR_FLAGS].I = N_FLAG | V_FLAG;
	arm_Reg[RN_CPSR].I = N_FLAG | V_FLAG | 0x10;
	arm_Reg[RN_SPSR].I = Z_FLAG | C_FLAG | 0x13;
	RunOp();
	EXPECT_EQ(armMode, 0x10);
	EXPECT_EQ(arm_Reg[RN_CPSR].I, N_FLAG | V_FLAG | 0x10);
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1004u);
	ASSERT_NZCV_EQ(N_FLAG | V_FLAG);
}

TEST_F(AicaArmTest, PsrTransfersUseSingleSequentialCycle)
{
	PrepareOp(0xe1a01000); // mov r1, r0
	RunOp();
	const s32 sequentialCyclesRemaining = arm_Reg[CYCL_CNT].I;

	PrepareOp(0xe10f1000); // mrs r1, cpsr
	RunOp();
	EXPECT_EQ(arm_Reg[CYCL_CNT].I, sequentialCyclesRemaining);

	PrepareOp(0xe128f000); // msr cpsr_flg, r0
	RunOp();
	EXPECT_EQ(arm_Reg[CYCL_CNT].I, sequentialCyclesRemaining);

	PrepareOp(0xe328f000); // msr cpsr_flg, #0
	RunOp();
	EXPECT_EQ(arm_Reg[CYCL_CNT].I, sequentialCyclesRemaining);
}

TEST_F(AicaArmTest, SwapUsesLoadStoreAndInternalCycles)
{
	PrepareOp(0xe1a00000); // mov r0, r0
	RunOp();
	const s32 baseCost = 1 - arm_Reg[CYCL_CNT].I;

	PrepareOp(0xe5910000); // ldr r0, [r1]
	arm_Reg[1].I = 0x1100;
	*(u32*)&aica_ram[0x1100] = 0x12345678;
	RunOp();
	const s32 loadCost = 1 - arm_Reg[CYCL_CNT].I;

	PrepareOp(0xe5810000); // str r0, [r1]
	arm_Reg[0].I = 0x87654321;
	arm_Reg[1].I = 0x1100;
	RunOp();
	const s32 storeCost = 1 - arm_Reg[CYCL_CNT].I;

	PrepareOp(0xe1012093); // swp r2, r3, [r1]
	arm_Reg[1].I = 0x1100;
	arm_Reg[2].I = 0;
	arm_Reg[3].I = 0xcafebabe;
	*(u32*)&aica_ram[0x1100] = 0xbaadf00d;
	RunOp();
	const s32 swapCost = 1 - arm_Reg[CYCL_CNT].I;

	EXPECT_EQ(arm_Reg[2].I, 0xbaadf00du);
	EXPECT_EQ(*(u32*)&aica_ram[0x1100], 0xcafebabeu);
	EXPECT_EQ(swapCost, loadCost + storeCost - baseCost);
}

TEST_F(AicaArmTest, SoftwareInterruptSavesNextInstruction)
{
	PrepareOp(0xef000000); // swi 0
	RunOp();

	ASSERT_EQ(armMode, 0x13);
	ASSERT_EQ(arm_Reg[14].I, 0x1004);
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x08);
	ASSERT_FALSE(armIrqEnable);
}

TEST_F(AicaArmTest, UndefinedInstructionRefillsPipeline)
{
	PrepareOp(0xef000000); // swi 0
	RunOp();
	const s32 softwareInterruptCost = 1 - arm_Reg[CYCL_CNT].I;

	aica::arm::reset();
	Arm7Enabled = true;
	PrepareOp(0xe12fff10); // bx r0 is Undefined on ARM7DI
	RunOp();

	EXPECT_EQ(armMode, 0x1b);
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x04u);
	EXPECT_EQ(1 - arm_Reg[CYCL_CNT].I, softwareInterruptCost + 1);
}

TEST_F(AicaArmTest, DynarecFallbackExceptionUsesInstructionAddress)
{
	// Model a fallback several instructions into a translated block. Its
	// exception return address must not depend on the dispatcher's block PC.
	arm_Reg[R15_ARM_NEXT].I = 0x1000;
	arm_Reg[RN_PC].I = 0x1048; // architectural PC is instruction address + 8
	interpret(0xef000000); // swi 0 at 0x1040

	ASSERT_EQ(armMode, 0x13);
	ASSERT_EQ(arm_Reg[14].I, 0x1044);
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x08);
}

TEST_F(AicaArmTest, Operand2ImmTest)
{
	PrepareOp(0xe2810003);	// add r0, r1, #3
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 4);

	PrepareOp(0xe2a10003);	// adc r0, r1, #3
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 5);

	PrepareOp(0xe2410003);	// sub r0, r1, #3
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 7;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 4);

	PrepareOp(0xe2610003);	// rsb r0, r1, #3
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 2);

	PrepareOp(0xe2c10003);	// sbc r0, r1, #3
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 10;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 6);

	PrepareOp(0xe2e10010);	// rsc r0, r1, #16
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 6;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 9);

	PrepareOp(0xe3500010);	// cmp r0, #16
	ResetNZCV();
	arm_Reg[0].I = 16;
	RunOp();
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);

	PrepareOp(0xe3700001);	// cmn r0, #1
	ResetNZCV();
	arm_Reg[0].I = 0xffffffff;
	RunOp();
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);

	PrepareOp(0xe2010001);	// and r0, r1, #1
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 3;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);

	PrepareOp(0xe3810001);	// orr r0, r1, #1
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 3);

	PrepareOp(0xe2210001);	// eor r0, r1, #1
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 3;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 2);

	PrepareOp(0xe3c10001);	// bic r0, r1, #1
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 3;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 2);

	PrepareOp(0xe3100001);	// tst r0, #1
	ResetNZCV();
	arm_Reg[0].I = 2;
	RunOp();
	ASSERT_NZCV_EQ(Z_FLAG);
	ResetNZCV();
	arm_Reg[0].I = 1;
	RunOp();
	ASSERT_NZCV_EQ(0);

	PrepareOp(0xe3300001);	// teq r0, #1
	ResetNZCV();
	arm_Reg[0].I = 1;
	RunOp();
	ASSERT_NZCV_EQ(Z_FLAG);
	ResetNZCV();
	arm_Reg[0].I = 2;
	RunOp();
	ASSERT_NZCV_EQ(0);

	PrepareOp(0xe2522001);	// subs r2, #1
	ResetNZCV();
	arm_Reg[2].I = 1;
	RunOp();
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);
	ResetNZCV();
	arm_Reg[2].I = 2;
	RunOp();
	ASSERT_NZCV_EQ(C_FLAG);
	ResetNZCV();
	arm_Reg[2].I = 0;
	RunOp();
	ASSERT_NZCV_EQ(N_FLAG);
}

TEST_F(AicaArmTest, UnrotatedImmediatePreservesLogicalCarry)
{
	PrepareOp(0xe3b00001); // movs r0, #1 (rotate field is zero)
	arm_Reg[RN_PSR_FLAGS].I = C_FLAG | V_FLAG;
	RunOp();

	EXPECT_EQ(arm_Reg[0].I, 1u);
	ASSERT_NZCV_EQ(C_FLAG | V_FLAG);
}

TEST_F(AicaArmTest, PcFirstOperandAllowsDestinationAliasing)
{
	PrepareOp(0xe18f0000); // orr r0, pc, r0
	arm_Reg[0].I = 1;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x1009u);

	PrepareOp(0xe08f0000); // add r0, pc, r0
	arm_Reg[0].I = 1;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x1009u);

	PrepareOp(0xe04f0000); // sub r0, pc, r0
	arm_Reg[0].I = 1;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x1007u);
}

TEST_F(AicaArmTest, PcImmediateShiftZeroFieldsMeanShiftBy32)
{
	PrepareOp(0xe1a0002f); // mov r0, pc, lsr #32
	arm_Reg[0].I = 0xffffffff;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0u);

	PrepareOp(0xe1a0004f); // mov r0, pc, asr #32
	arm_Reg[0].I = 0xffffffff;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0u);

	PrepareOp(0xe081002f); // add r0, r1, pc, lsr #32
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x12345678;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x12345678u);
}

TEST_F(AicaArmTest, FlagSettingPcImmediateShiftsUseTheShifter)
{
	PrepareOp(0xe1b0002f); // movs r0, pc, lsr #32
	arm_Reg[0].I = 0xffffffff;
	arm_Reg[RN_PSR_FLAGS].I = C_FLAG | V_FLAG;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0u);
	ASSERT_NZCV_EQ(Z_FLAG | V_FLAG);

	PrepareOp(0xe1b0008f); // movs r0, pc, lsl #1
	arm_Reg[0].I = 0;
	arm_Reg[RN_PSR_FLAGS].I = C_FLAG | V_FLAG;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x2010u);
	ASSERT_NZCV_EQ(V_FLAG);

	PrepareOp(0xe1a0026f); // mov r0, pc, ror #4
	arm_Reg[0].I = 0;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x80000100u);

	PrepareOp(0xe081026f); // add r0, r1, pc, ror #4
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 3;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x80000103u);

	PrepareOp(0xe1b0006f); // movs r0, pc, rrx
	arm_Reg[RN_PSR_FLAGS].I = C_FLAG | V_FLAG;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x80000804u);
	ASSERT_NZCV_EQ(N_FLAG | V_FLAG);
}

TEST_F(AicaArmTest, Operand2ShiftImmTest)
{
	PrepareOp(0xe0810202);	// add r0, r1, r2, LSL #4
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x21);

	PrepareOp(0xe0810142); // add r0, r1, r2, ASR #2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = -8;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, -1);

	PrepareOp(0xe08100a2); // add r0, r1, r2, LSR #1
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x80000000;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x40000001);

	PrepareOp(0xe0810862); // add r0, r1, r2, ROR #16
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x56771234;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x12345678);

	PrepareOp(0xe0810062);	// add r0, r1, r2, RRX
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x22222221;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x11111111);
	ASSERT_NZCV_EQ(0);	//

	PrepareOp(0xe1910062);	// orrs r0, r1, r2, RRX
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG;	// set C
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x22222221;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x91111111);
	ASSERT_NZCV_EQ(N_FLAG | C_FLAG); // N,C

	// When an Operand2 constant is used with the instructions MOVS, MVNS, ANDS, ORRS, ORNS, EORS, BICS, TEQ or TST, the carry flag is updated to bit[31] of the constant,
	// if the constant is greater than 255 and can be produced by shifting an 8-bit value.
	PrepareOp(0xe3b00102);	// movs r0, #0x80000000
	ResetNZCV();
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x80000000);
	ASSERT_NZCV_EQ(N_FLAG | C_FLAG); // N,C

	PrepareOp(0xe3f00000);	// mvns r0, #0
	arm_Reg[RN_PSR_FLAGS].I &= ~NZCV_MASK;
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xffffffff);
	ASSERT_NZCV_EQ(N_FLAG); // N
}

TEST_F(AicaArmTest, Operand2RegShiftTest)
{
	PrepareOp(0xe1810312);	// orr r0, r1, r2, LSL r3
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x10;
	arm_Reg[3].I = 8;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x1001);

	PrepareOp(0xe1810352);	// orr r0, r1, r2, ASR r3
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x80000000;
	arm_Reg[3].I = 30;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xffffffff);

	PrepareOp(0xe1810332);	// orr r0, r1, r2, LSR r3
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x80000000;
	arm_Reg[3].I = 30;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 3);

	PrepareOp(0xe1810372);	// orr r0, r1, r2, ROR r3
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x43208765;
	arm_Reg[3].I = 16;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x87654321);

	PrepareOp(0xe1b0431f);	// movs r4, r15, lsl r3
	ResetNZCV();
	arm_Reg[3].I = 0;
	arm_Reg[4].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[4].I, 0x1000 + 12);
	ASSERT_NZCV_EQ(0);

	PrepareOp(0xe1b0400f);	// movs r4, r15, lsl #0
	ResetNZCV();
	arm_Reg[3].I = 0;
	arm_Reg[4].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[4].I, 0x1000 + 8);
	ASSERT_NZCV_EQ(0);

	// Register-controlled shifts use only the low byte of Rs.
	PrepareOp(0xe1a00211); // mov r0, r1, lsl r2
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x12345678;
	arm_Reg[2].I = 0x100;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x12345678);

	// A zero register-controlled ROR preserves carry rather than taking
	// carry from bit 31 of Rm.
	PrepareOp(0xe1b00271); // movs r0, r1, ror r2
	ResetNZCV();
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG;
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	ASSERT_NZCV_EQ(C_FLAG);
}

TEST_F(AicaArmTest, CarryTest)
{
	PrepareOp(0xe1910022); // orrs r0, r1, r2, LSR #32
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x80000000;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	ASSERT_NZCV_EQ(C_FLAG); // C

	PrepareOp(0xe1910822); // orrs r0, r1, r2, LSR #16
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x80008000;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x8001);
	ASSERT_NZCV_EQ(C_FLAG); // C

	PrepareOp(0xe1910042); // orrs r0, r1, r2, ASR #32
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x80000000;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xffffffff);
	ASSERT_NZCV_EQ(N_FLAG | C_FLAG); // N,C

	PrepareOp(0xe1910842); // orrs r0, r1, r2, ASR #16
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x00008000;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	ASSERT_NZCV_EQ(C_FLAG); // C
	arm_Reg[RN_PSR_FLAGS].I &= ~NZCV_MASK;
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x00004000;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	ASSERT_NZCV_EQ(0); //

	PrepareOp(0xe1910802); // orrs r0, r1, r2, LSL #16
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	arm_Reg[2].I = 0x00010001;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x10001);
	ASSERT_NZCV_EQ(C_FLAG); // C
}

TEST_F(AicaArmTest, MemoryTest)
{
	PrepareOp(0xe5910004); // ldr r0, [r1, #4]
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x10000;
	*(u32*)&aica_ram[0x10004] = 0xbaadcafe;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xbaadcafe);

	PrepareOp(0xe5010008); // str r0, [r1, #-8]
	arm_Reg[0].I = 0xbaadcafe;
	arm_Reg[1].I = 0x10008;
	*(u32*)&aica_ram[0x10000] = 0;
	RunOp();
	ASSERT_EQ(*(u32*)&aica_ram[0x10000], 0xbaadcafe);

	// Conditional native ops use the interpreter fallback. STR must expose
	// R15 as this instruction's address + 12 in both execution engines.
	PrepareOp(0x0581f000); // streq pc, [r1]
	arm_Reg[RN_PSR_FLAGS].I = Z_FLAG;
	arm_Reg[1].I = 0x10020;
	*(u32*)&aica_ram[0x10020] = 0;
	RunOp();
	ASSERT_EQ(*(u32*)&aica_ram[0x10020], 0x100cu);

	PrepareOp(0x05c1f000); // strbeq pc, [r1]
	arm_Reg[RN_PSR_FLAGS].I = Z_FLAG;
	arm_Reg[1].I = 0x10024;
	aica_ram[0x10024] = 0;
	RunOp();
	ASSERT_EQ(aica_ram[0x10024], 0x0cu);

	PrepareOp(0xe5b10004);	// ldr r0, [r1, #4]!
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x10000;
	*(u32*)&aica_ram[0x10004] = 0xbaadcafe;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xbaadcafe);
	ASSERT_EQ(arm_Reg[1].I, 0x10004);

	// Word accesses to the AICA register window obey the ARM7DI's aligned
	// bus-address and load-rotation rules just like accesses to audio RAM.
	aica::aica_reg[0x100] = 0xca;
	aica::aica_reg[0x101] = 0xfe;
	PrepareOp(0xe5910000); // ldr r0, [r1]
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x800101;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xca0000feu);

	PrepareOp(0xe5810000); // str r0, [r1]
	arm_Reg[0].I = 0x12345678;
	arm_Reg[1].I = 0x800102;
	RunOp();
	ASSERT_EQ(aica::aica_reg[0x100], 0x78);
	ASSERT_EQ(aica::aica_reg[0x101], 0x56);

	PrepareOp(0xe4110004);	// ldr r0, [r1], #-4
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x10004;
	*(u32*)&aica_ram[0x10004] = 0xbaadcafe;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xbaadcafe);
	ASSERT_EQ(arm_Reg[1].I, 0x10000);

	PrepareOp(0xe4900004);	// ldr r0, [r0], #4
	arm_Reg[0].I = 0x10004;
	*(u32*)&aica_ram[0x10004] = 0xbaadcafe;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xbaadcafe);

	PrepareOp(0xe5b00004);	// ldr r0, [r0, #4]!
	arm_Reg[0].I = 0x10000;
	*(u32*)&aica_ram[0x10000] = 0xbaadcafe;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0xbaadcafe);

	PrepareOp(0xe51f0004);	// ldr r0, [r15, #-4]
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);

	PrepareOp(0xe79000a4);	// ldr r0, [r0, r4, LSR #1]
	arm_Reg[0].I = 0x1003;
	arm_Reg[4].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);

	PrepareOp(0xe7900084);	// ldr r0, [r0, r4, LSL #1]
	arm_Reg[0].I = 0x1002;
	arm_Reg[4].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);

	PrepareOp(0xe7900024);	// ldr r0, [r0, r4, LSR #32]
	arm_Reg[0].I = 0x1004;
	arm_Reg[4].I = 0x12345678;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);
	ASSERT_EQ(arm_Reg[4].I, 0x12345678);

	PrepareOp(0xe7900044);	// ldr r0, [r0, r4, ASR #32]
	arm_Reg[0].I = 0x1005;
	arm_Reg[4].I = 0x80000000;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);
	ASSERT_EQ(arm_Reg[4].I, 0x80000000);

	PrepareOp(0xe7920002);	// ldr r0,[r2,r2]
	arm_Reg[0].I = 5;
	arm_Reg[2].I = 0x1004 / 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);
	ASSERT_EQ(arm_Reg[2].I, 0x1004 / 2);

	PrepareOp(0xe7920063);	// ldr r0,[r2,r3, rrx]
	ResetNZCV();
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG;	// set C
	arm_Reg[0].I = 5;
	arm_Reg[2].I = 0x1006;
	arm_Reg[3].I = -4;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);
	ASSERT_EQ(arm_Reg[2].I, 0x1006);
	ASSERT_EQ(arm_Reg[3].I, -4);

	// unaligned read
	PrepareOp(0xe7900002);	// ldr r0,[r0,r2]
	arm_Reg[0].I = 0x1004;
	arm_Reg[2].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, (*(u32*)&aica_ram[0x1004]) >> 16 | (*(u32*)&aica_ram[0x1004]) << 16);
	ASSERT_EQ(arm_Reg[2].I, 2);

	PrepareOp(0xe69000a4);	// ldr r0,[r0],r4, lsr #1
	arm_Reg[0].I = 0x1004;
	arm_Reg[4].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);
	ASSERT_EQ(arm_Reg[4].I, 2);

	PrepareOp(0xe6920104);	// ldr r0,[r2],r4, lsl #2
	arm_Reg[0].I = 0;
	arm_Reg[2].I = 0x1004;
	arm_Reg[4].I = 2;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);
	ASSERT_EQ(arm_Reg[2].I, 0x1004 + 8);
	ASSERT_EQ(arm_Reg[4].I, 2);

	PrepareOp(0xe6920000);	// ldr r0,[r2],r0
	arm_Reg[0].I = 123;
	arm_Reg[2].I = 0x1004;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);
	ASSERT_EQ(arm_Reg[2].I, 0x1004 + 123);

	PrepareOp(0xe6920043);	// ldr r0,[r2],r3, asr #32
	arm_Reg[0].I = 0;
	arm_Reg[2].I = 0x1004;
	arm_Reg[3].I = 0xc0000000;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);
	ASSERT_EQ(arm_Reg[2].I, 0x1004 - 1);
	ASSERT_EQ(arm_Reg[3].I, 0xc0000000);

	PrepareOp(0xe6920063);	// ldr r0,[r2],r3, rrx
	ResetNZCV();
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG;	// set C
	arm_Reg[0].I = 5;
	arm_Reg[2].I = 0x1004;
	arm_Reg[3].I = 0xfffffffc;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, *(u32*)&aica_ram[0x1004]);
	ASSERT_EQ(arm_Reg[2].I, 0x1002);
	ASSERT_NZCV_EQ(C_FLAG); // C
	ASSERT_EQ(arm_Reg[3].I, 0xfffffffc);

	// unaligned read
	PrepareOp(0xe6900002);	// ldr r0,[r0],r2
	arm_Reg[0].I = 0x1006;
	arm_Reg[2].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, (*(u32*)&aica_ram[0x1004]) >> 16 | (*(u32*)&aica_ram[0x1004]) << 16);
	ASSERT_EQ(arm_Reg[2].I, 1);

	// conditional with write-back, false condition
	PrepareOp(0x04910004);	// ldreq r0, [r1], #4
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x1004;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	ASSERT_EQ(arm_Reg[1].I, 0x1004);

}

TEST_F(AicaArmTest, EverySingleLoadFormBranchesWhenDestinationIsPc)
{
	PrepareOp(0xe5d1f000); // ldrb pc, [r1]
	arm_Reg[1].I = 0x1100;
	aica_ram[0x1100] = 0x43;
	RunOp();
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x40u);
	EXPECT_EQ(arm_Reg[RN_PC].I, 0x44u);

	PrepareOp(0xe4b1f004); // ldrt pc, [r1], #4
	arm_Reg[1].I = 0x1110;
	*(u32 *)&aica_ram[0x1110] = 0x1237;
	RunOp();
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1234u);
	EXPECT_EQ(arm_Reg[RN_PC].I, 0x1238u);
	EXPECT_EQ(arm_Reg[1].I, 0x1114u);

	PrepareOp(0xe4f1f004); // ldrbt pc, [r1], #4
	arm_Reg[1].I = 0x1120;
	aica_ram[0x1120] = 0x87;
	RunOp();
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x84u);
	EXPECT_EQ(arm_Reg[RN_PC].I, 0x88u);
	EXPECT_EQ(arm_Reg[1].I, 0x1124u);
}

TEST_F(AicaArmTest, PcRelativeTest)
{
	PrepareOp(0xe38f0010);	// orr r0, r15, #16
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x1008 + 16);

	PrepareOp(0xe180011f); // orr r0, r15, LSL r1
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0x100C << 1);

	// A register-controlled shift exposes PC+12 only through the shifted Rm.
	// The independent Rn operand still reads the ordinary PC+8 value.
	PrepareOp(0xe08f0211); // add r0, pc, r1, lsl r2
	arm_Reg[1].I = 3;
	arm_Reg[2].I = 1;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x100eu);
	EXPECT_EQ(arm_Reg[RN_PC].I, 0x1008u);

	PrepareOp(0xe11f0211); // tst pc, r1, lsl r2
	arm_Reg[1].I = 2;
	arm_Reg[2].I = 1;
	ResetNZCV();
	RunOp();
	ASSERT_NZCV_EQ(Z_FLAG);

	PrepareOp(0xe28f5c7d); // add r5, r15, #32000
	arm_Reg[5].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[5].I, 32000 + 0x1008);
}

TEST_F(AicaArmTest, ConditionalTest)
{
	PrepareOp(0x01a00001);	// moveq r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	arm_Reg[RN_PSR_FLAGS].I |= Z_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);

	PrepareOp(0x11a00001);	// movne r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	arm_Reg[RN_PSR_FLAGS].I |= Z_FLAG;
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);

	PrepareOp(0x21a00001);	// movcs r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);

	PrepareOp(0x31a00001);	// movcc r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG;
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);

	PrepareOp(0x41a00001);	// movmi r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	arm_Reg[RN_PSR_FLAGS].I |= N_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);

	PrepareOp(0x51a00001);	// movpl r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	arm_Reg[RN_PSR_FLAGS].I |= N_FLAG;
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);

	PrepareOp(0x61a00001);	// movvs r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	arm_Reg[RN_PSR_FLAGS].I |= V_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);

	PrepareOp(0x71a00001);	// movvc r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	arm_Reg[RN_PSR_FLAGS].I |= V_FLAG;
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);

	PrepareOp(0x81a00001);	// movhi r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	arm_Reg[RN_PSR_FLAGS].I |= Z_FLAG;
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);

	PrepareOp(0x91a00001);	// movls r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	arm_Reg[RN_PSR_FLAGS].I |= C_FLAG;
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	arm_Reg[RN_PSR_FLAGS].I = Z_FLAG;
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);

	PrepareOp(0xa1a00001);	// movge r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	arm_Reg[RN_PSR_FLAGS].I |= N_FLAG;
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	arm_Reg[RN_PSR_FLAGS].I |= V_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);

	PrepareOp(0xb1a00001);	// movlt r0, r1
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	arm_Reg[RN_PSR_FLAGS].I |= N_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	arm_Reg[RN_PSR_FLAGS].I |= V_FLAG;
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);

	PrepareOp(0xc1a00001);	// movgt r0, r1
	// Z==0 && N==V
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	ResetNZCV();
	arm_Reg[RN_PSR_FLAGS].I |= Z_FLAG;
	arm_Reg[0].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	ResetNZCV();
	arm_Reg[RN_PSR_FLAGS].I |= V_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	arm_Reg[RN_PSR_FLAGS].I |= N_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);

	PrepareOp(0xd1a00001);	// movle r0, r1
	// Z==1 || N!=V
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 1;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
	arm_Reg[RN_PSR_FLAGS].I |= Z_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	ResetNZCV();
	arm_Reg[0].I = 0;
	arm_Reg[RN_PSR_FLAGS].I |= V_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 1);
	arm_Reg[0].I = 0;
	arm_Reg[RN_PSR_FLAGS].I |= N_FLAG;
	RunOp();
	ASSERT_EQ(arm_Reg[0].I, 0);
}

TEST_F(AicaArmTest, FailedConditionalOpsUseBaseCycleCost)
{
	ResetNZCV(); // EQ condition is false
	PrepareOp(0x01a00001); // moveq r0, r1
	RunOp();
	const s32 simpleCyclesRemaining = arm_Reg[CYCL_CNT].I;

	aica::arm::reset();
	Arm7Enabled = true;
	ResetNZCV();
	PrepareOp(0x05910000); // ldreq r0, [r1]
	arm_Reg[0].I = 0x12345678;
	arm_Reg[1].I = 0x100;
	RunOp();

	EXPECT_EQ(arm_Reg[0].I, 0x12345678u);
	EXPECT_EQ(arm_Reg[CYCL_CNT].I, simpleCyclesRemaining);
}

TEST_F(AicaArmTest, CycleBudgetStopsAtInstructionBoundary)
{
	u32 ops[] = {
		0xe3a00001, // mov r0, #1
		0xe3a01002, // mov r1, #2
	};
	PrepareOps(std::size(ops), ops);
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0;

	RunOp(); // starts with one remaining cycle

	EXPECT_EQ(arm_Reg[0].I, 1u);
	EXPECT_EQ(arm_Reg[1].I, 0u);
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1004u);
}

TEST_F(AicaArmTest, JumpTest)
{
	PrepareOp(0xea00003e);	// b +248
	RunOp();
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1100);

	PrepareOp(0xeb00003e);	// bl +248
	arm_Reg[14].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1100);
	ASSERT_EQ(arm_Reg[14].I, 0x1004);

	PrepareOp(0xe1a0f000);	// mov pc, r0
	arm_Reg[0].I = 0x100;
	RunOp();
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x100);

	PrepareOp(0xe280f003); // add pc, r0, #3
	arm_Reg[0].I = 0x1200;
	RunOp();
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1200);

	PrepareOp(0xc1a0f000);	// movgt pc, r0
	ResetNZCV();
	arm_Reg[RN_PSR_FLAGS].I |= N_FLAG;
	arm_Reg[0].I = 0x100;
	RunOp();
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1004);
	ResetNZCV();
	RunOp();
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x100);

	PrepareOp(0xe590f000); // ldr r15, [r0]
	arm_Reg[0].I = 0x10000;
	*(u32*)&aica_ram[0x10000] = 0xbaadcafc;
	RunOp();
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0xbaadcafc);

	PrepareOp(0x1b00003e);	// blne +248
	ResetNZCV();
	arm_Reg[14].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1100);
	ASSERT_EQ(arm_Reg[14].I, 0x1004);
	ResetNZCV();
	arm_Reg[RN_PSR_FLAGS].I |= Z_FLAG;
	arm_Reg[14].I = 0;
	RunOp();
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1004);
	ASSERT_EQ(arm_Reg[14].I, 0);
}

TEST_F(AicaArmTest, BranchOffsetsAreSignExtendedFrom24Bits)
{
	PrepareOp(0xeafffffe); // b to this instruction: -8 from architectural pc
	RunOp();
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1000u);

	PrepareOp(0xea7fffff); // largest positive imm24
	RunOp();
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x02001004u);

	PrepareOp(0xea800000); // most-negative imm24
	RunOp();
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0xfe001008u);
}

TEST_F(AicaArmTest, FiqPreservesCpsrAndBanksRegisters)
{
	armMode = 0x13;
	armIrqEnable = true;
	armFiqEnable = true;
	arm_Reg[RN_PSR_FLAGS].I = N_FLAG | C_FLAG;
	arm_Reg[RN_SPSR].I = Z_FLAG | 0x10;
	arm_Reg[8].I = 0x88888888;
	arm_Reg[13].I = 0x13131313;
	arm_Reg[14].I = 0x14141414;
	arm_Reg[R8_FIQ].I = 0xf8f8f8f8;
	arm_Reg[R13_FIQ].I = 0xf13f13f1;
	arm_Reg[R14_FIQ].I = 0xf14f14f1;
	arm_Reg[R15_ARM_NEXT].I = 0x1000;
	arm_Reg[CYCL_CNT].I = 10;

	CPUFiq();

	ASSERT_EQ(armMode, 0x11);
	ASSERT_EQ(arm_Reg[8].I, 0xf8f8f8f8);
	ASSERT_EQ(arm_Reg[13].I, 0xf13f13f1);
	ASSERT_EQ(arm_Reg[14].I, 0x1004);
	ASSERT_EQ(arm_Reg[R8_FIQ].I, 0x88888888);
	ASSERT_EQ(arm_Reg[R13_SVC].I, 0x13131313);
	ASSERT_EQ(arm_Reg[R14_SVC].I, 0x14141414);
	ASSERT_EQ(arm_Reg[RN_SPSR].I, N_FLAG | C_FLAG | 0x13);
	ASSERT_EQ(arm_Reg[RN_CPSR].I, N_FLAG | C_FLAG | 0xd1);
	ASSERT_FALSE(armIrqEnable);
	ASSERT_FALSE(armFiqEnable);
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1c);
	ASSERT_EQ(arm_Reg[CYCL_CNT].I, 8);
}

TEST_F(AicaArmTest, ExceptionReturnRestoresUserState)
{
	PrepareOp(0xe1b0f000); // movs pc, r0
	armMode = 0x13;
	armIrqEnable = false;
	armFiqEnable = false;
	arm_Reg[RN_PSR_FLAGS].I = N_FLAG;
	arm_Reg[RN_CPSR].I = N_FLAG | 0xd3;
	arm_Reg[RN_SPSR].I = Z_FLAG | C_FLAG | 0x10;
	arm_Reg[0].I = 0x1236;
	arm_Reg[13].I = 0x13131313;
	arm_Reg[14].I = 0x14141414;
	arm_Reg[R13_USR].I = 0x10101010;
	arm_Reg[R14_USR].I = 0x11111111;

	RunOp();

	ASSERT_EQ(armMode, 0x10);
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1234);
	ASSERT_EQ(arm_Reg[13].I, 0x10101010);
	ASSERT_EQ(arm_Reg[14].I, 0x11111111);
	ASSERT_EQ(arm_Reg[R13_SVC].I, 0x13131313);
	ASSERT_EQ(arm_Reg[R14_SVC].I, 0x14141414);
	ASSERT_EQ(arm_Reg[RN_CPSR].I, Z_FLAG | C_FLAG | 0x10);
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);
	ASSERT_TRUE(armIrqEnable);
	ASSERT_TRUE(armFiqEnable);
}

TEST_F(AicaArmTest, DataProcessingPcWritesRefillPipeline)
{
	PrepareOp(0xe1a01000); // mov r1, r0
	RunOp();
	const s32 baseCost = 1 - arm_Reg[CYCL_CNT].I;

	PrepareOp(0xe1a0f000); // mov pc, r0
	arm_Reg[0].I = 0x1200;
	RunOp();
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1200u);
	EXPECT_EQ(1 - arm_Reg[CYCL_CNT].I, baseCost + 3);

	PrepareOp(0xe1b0f000); // movs pc, r0
	armMode = 0x13;
	armIrqEnable = false;
	armFiqEnable = false;
	arm_Reg[RN_CPSR].I = 0xd3;
	arm_Reg[RN_SPSR].I = 0x10;
	arm_Reg[0].I = 0x1300;
	RunOp();
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1300u);
	EXPECT_EQ(1 - arm_Reg[CYCL_CNT].I, baseCost + 3);
}

TEST_F(AicaArmTest, RegisterShiftedPcWritesRemainBlockTerminating)
{
	PrepareOp(0xe1a0f110); // mov pc, r0, lsl r1
	arm_Reg[0].I = 0x1237;
	arm_Reg[1].I = 0;
	RunOp();
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1234u);
	EXPECT_EQ(arm_Reg[RN_PC].I, 0x1238u);

	PrepareOp(0xe1b0f110); // movs pc, r0, lsl r1
	armMode = 0x13;
	armIrqEnable = false;
	armFiqEnable = false;
	arm_Reg[RN_CPSR].I = 0xd3;
	arm_Reg[RN_SPSR].I = Z_FLAG | C_FLAG | 0x10;
	arm_Reg[0].I = 0x2347;
	arm_Reg[1].I = 0;
	RunOp();
	EXPECT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x2344u);
	EXPECT_EQ(armMode, 0x10);
	ASSERT_NZCV_EQ(Z_FLAG | C_FLAG);
}

TEST_F(AicaArmTest, LdmStmTest)
{
	PrepareOp(0xe8bd8000);	// ldm sp!, {pc}
	arm_Reg[13].I = 0x1100;
	*(u32*)&aica_ram[0x1100] = 0x1237;
	RunOp();
	ASSERT_EQ(arm_Reg[R15_ARM_NEXT].I, 0x1234);
	ASSERT_EQ(arm_Reg[RN_PC].I, 0x1238);

	PrepareOp(0xe92d8000);	// stmdb sp!, {pc}
	arm_Reg[13].I = 0x1104;
	RunOp();
	ASSERT_EQ(arm_Reg[13].I, 0x1100);
	ASSERT_EQ(*(u32*)&aica_ram[0x1100], 0x1000 + 12);

	// ARM7Wrestler verifies that ^ without pc accesses the User bank while
	// leaving the current exception bank selected.
	PrepareOp(0xe8f04008); // ldmia r0!, {r3, r14}^
	armMode = 0x12;
	armIrqEnable = false;
	armFiqEnable = false;
	arm_Reg[RN_CPSR].I = 0xd2;
	arm_Reg[0].I = 0x1120;
	arm_Reg[14].I = 0x12121212;
	arm_Reg[R14_USR].I = 0x10101010;
	*(u32*)&aica_ram[0x1120] = 0x33333333;
	*(u32*)&aica_ram[0x1124] = 0x14141414;
	RunOp();
	ASSERT_EQ(armMode, 0x12);
	ASSERT_EQ(arm_Reg[3].I, 0x33333333);
	ASSERT_EQ(arm_Reg[14].I, 0x12121212);
	ASSERT_EQ(arm_Reg[R14_USR].I, 0x14141414);
	ASSERT_EQ(arm_Reg[0].I, 0x1128);
}

TEST_F(AicaArmTest, StoreBaseRegisterStillWritesBack)
{
	PrepareOp(0xe4811004); // str r1, [r1], #4
	arm_Reg[1].I = 0x1180;
	*(u32*)&aica_ram[0x1180] = 0;
	RunOp();
	EXPECT_EQ(*(u32*)&aica_ram[0x1180], 0x1180u);
	EXPECT_EQ(arm_Reg[1].I, 0x1184u);

	PrepareOp(0xe5211004); // str r1, [r1, #-4]!
	arm_Reg[1].I = 0x1190;
	*(u32*)&aica_ram[0x118c] = 0;
	RunOp();
	EXPECT_EQ(*(u32*)&aica_ram[0x118c], 0x118cu);
	EXPECT_EQ(arm_Reg[1].I, 0x118cu);
}

TEST_F(AicaArmTest, UserBankLdmWritesBackDistinctExceptionBase)
{
	PrepareOp(0xe8fd2000); // ldmia r13!, {r13}^
	armMode = 0x12;
	armIrqEnable = false;
	armFiqEnable = false;
	arm_Reg[RN_CPSR].I = 0xd2;
	arm_Reg[13].I = 0x11a0;
	arm_Reg[R13_USR].I = 0x10101010;
	*(u32*)&aica_ram[0x11a0] = 0x20202020;
	RunOp();
	EXPECT_EQ(armMode, 0x12);
	EXPECT_EQ(arm_Reg[13].I, 0x11a4u);
	EXPECT_EQ(arm_Reg[R13_USR].I, 0x20202020u);

	PrepareOp(0xe8f80100); // ldmia r8!, {r8}^
	armMode = 0x11;
	armIrqEnable = false;
	armFiqEnable = false;
	arm_Reg[RN_CPSR].I = 0xd1;
	arm_Reg[8].I = 0x11c0;
	arm_Reg[R8_FIQ].I = 0x30303030;
	*(u32*)&aica_ram[0x11c0] = 0x40404040;
	RunOp();
	EXPECT_EQ(armMode, 0x11);
	EXPECT_EQ(arm_Reg[8].I, 0x11c4u);
	EXPECT_EQ(arm_Reg[R8_FIQ].I, 0x40404040u);
}

TEST_F(AicaArmTest, SingleRegisterBlockTransferSemanticsAndCycles)
{
	*(u32*)&aica_ram[0x1100] = 0xbaadcafe;
	PrepareOp(0xe5910000); // ldr r0, [r1]
	arm_Reg[1].I = 0x1100;
	RunOp();
	const s32 ldrCyclesRemaining = arm_Reg[CYCL_CNT].I;

	PrepareOp(0xe8b10001); // ldmia r1!, {r0}
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x1100;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0xbaadcafeu);
	EXPECT_EQ(arm_Reg[1].I, 0x1104u);
	EXPECT_EQ(arm_Reg[CYCL_CNT].I, ldrCyclesRemaining);

	// A post-indexed block transfer without W does not update its base.
	PrepareOp(0xe8910001); // ldmia r1, {r0}
	arm_Reg[0].I = 0;
	arm_Reg[1].I = 0x1100;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0xbaadcafeu);
	EXPECT_EQ(arm_Reg[1].I, 0x1100u);

	PrepareOp(0xe5810000); // str r0, [r1]
	arm_Reg[0].I = 0x12345678;
	arm_Reg[1].I = 0x1110;
	RunOp();
	const s32 strCyclesRemaining = arm_Reg[CYCL_CNT].I;

	PrepareOp(0xe8a10001); // stmia r1!, {r0}
	arm_Reg[0].I = 0x87654321;
	arm_Reg[1].I = 0x1120;
	RunOp();
	EXPECT_EQ(*(u32*)&aica_ram[0x1120], 0x87654321u);
	EXPECT_EQ(arm_Reg[1].I, 0x1124u);
	EXPECT_EQ(arm_Reg[CYCL_CNT].I, strCyclesRemaining);

	PrepareOp(0xe8810001); // stmia r1, {r0}
	arm_Reg[0].I = 0xcafebabe;
	arm_Reg[1].I = 0x1130;
	RunOp();
	EXPECT_EQ(*(u32*)&aica_ram[0x1130], 0xcafebabeu);
	EXPECT_EQ(arm_Reg[1].I, 0x1130u);

	// When the base is the first stored register, hardware stores its old
	// value and still performs writeback.
	PrepareOp(0xe8a10002); // stmia r1!, {r1}
	arm_Reg[1].I = 0x1140;
	RunOp();
	EXPECT_EQ(*(u32*)&aica_ram[0x1140], 0x1140u);
	EXPECT_EQ(arm_Reg[1].I, 0x1144u);

	// LDM always lets a listed base register's loaded value overwrite the
	// writeback, even when the base is the first transferred register.
	PrepareOp(0xe8b10006); // ldmia r1!, {r1, r2}
	arm_Reg[1].I = 0x1150;
	*(u32*)&aica_ram[0x1150] = 0x11111111;
	*(u32*)&aica_ram[0x1154] = 0x22222222;
	RunOp();
	EXPECT_EQ(arm_Reg[1].I, 0x11111111u);
	EXPECT_EQ(arm_Reg[2].I, 0x22222222u);

	// The same rule applies when the base is later in the transfer list.
	PrepareOp(0xe8b10003); // ldmia r1!, {r0, r1}
	arm_Reg[1].I = 0x1160;
	*(u32*)&aica_ram[0x1160] = 0x33333333;
	*(u32*)&aica_ram[0x1164] = 0x44444444;
	RunOp();
	EXPECT_EQ(arm_Reg[0].I, 0x33333333u);
	EXPECT_EQ(arm_Reg[1].I, 0x44444444u);
}

TEST_F(AicaArmTest, MemoryModeChangeFlushesCompiledArmCode)
{
	settings.platform.aram_size = 8_MB;
	settings.platform.aram_mask = 8_MB - 1;
	CommonData->MEM8MB = 0;
	PrepareOp(0xe1a00000); // mov r0, r0
	ASSERT_NE(EntryPoints[0x1000 / 4], arm_compilecode);

	writeRegInternal<u8>(0x2801, 0x02); // MEM8MB=1
	EXPECT_EQ(soundRamMask(), 8_MB - 1);
	EXPECT_EQ(EntryPoints[0x1000 / 4], arm_compilecode);

	// Restore platform metadata before fixture teardown.
	settings.platform.aram_size = 2_MB;
	settings.platform.aram_mask = 2_MB - 1;
	writeRegInternal<u8>(0x2801, 0);
}

TEST_F(AicaArmTest, RegAllocTest)
{
	u32 ops[] = {
			0xe3a00000,	// mov r0, #0
			0xe3a01001,	// mov r1, #1
			0xe3a02002,	// mov r2, #2
			0xe3a03003,	// mov r3, #3
			0xe3a04004,	// mov r4, #4
			0xe3a05005,	// mov r5, #5
			0xe3a06006,	// mov r6, #6
			0xe0800001,	// add r0, r0, r1
			0xe0811002,	// add r1, r1, r2
			0xe0822003,	// add r2, r2, r3
			0xe0833004,	// add r3, r3, r4
			0xe0844005,	// add r4, r4, r5
			0xe0855006,	// add r5, r5, r6
			0xe0866000,	// add r6, r6, r0
	};
	PrepareOps(std::size(ops), ops);
	for (int i = 0; i < 15; i++)
		arm_Reg[i].I = 0;

	RunOp();

	ASSERT_EQ(arm_Reg[0].I, 1);
	ASSERT_EQ(arm_Reg[1].I, 3);
	ASSERT_EQ(arm_Reg[2].I, 5);
	ASSERT_EQ(arm_Reg[3].I, 7);
	ASSERT_EQ(arm_Reg[4].I, 9);
	ASSERT_EQ(arm_Reg[5].I, 11);
	ASSERT_EQ(arm_Reg[6].I, 7);
}

TEST_F(AicaArmTest, ConditionRegAllocTest)
{
	u32 ops1[] = {
			0x03a0004d,	// moveq r0, #77
			0xe1a01000	// mov r1, r0
	};
	PrepareOps(std::size(ops1), ops1);
	arm_Reg[0].I = 22;
	arm_Reg[1].I = 22;
	ResetNZCV();

	RunOp();

	ASSERT_EQ(arm_Reg[0].I, 22);
	ASSERT_EQ(arm_Reg[1].I, 22);

	u32 ops2[] = {
			0x01a01000,	// moveq r1, r0
			0xe1a02000	// mov r2, r0
	};
	PrepareOps(std::size(ops2), ops2);
	arm_Reg[0].I = 22;
	arm_Reg[1].I = 0;
	arm_Reg[2].I = 0;
	ResetNZCV();

	RunOp();

	ASSERT_EQ(arm_Reg[0].I, 22);
	ASSERT_EQ(arm_Reg[1].I, 0);
	ASSERT_EQ(arm_Reg[2].I, 22);
}
}
#endif
