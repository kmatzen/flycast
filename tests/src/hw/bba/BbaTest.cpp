#include "hw/bba/bba.h"
#include "hw/bba/rtl8139c.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"
#include "serialize.h"

#include <gtest/gtest.h>

#include <array>
#include <vector>

namespace
{

constexpr u32 GapsRamBase = 0x840000;
constexpr u32 GapsRamSize = 0x8000;
constexpr u32 GapsRegistersBase = 0x1400;
constexpr u32 PciConfigBase = 0x1600;
constexpr u32 RtlRegistersBase = 0x1700;

TEST(Bba, PciDmaWriteWrapsAtEndOfGapsRam)
{
	constexpr std::array<u8, 8> data { 0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87 };

	pci_dma_write(nullptr, GapsRamSize - 4, data.data(), data.size());

	EXPECT_EQ(0x43322110u, bba_ReadMem(GapsRamBase + GapsRamSize - 4, 4));
	EXPECT_EQ(0x87766554u, bba_ReadMem(GapsRamBase, 4));
}

TEST(Bba, PciDmaReadWrapsAtEndOfGapsRam)
{
	bba_WriteMem(GapsRamBase + GapsRamSize - 4, 0x89abcdef, 4);
	bba_WriteMem(GapsRamBase, 0x01234567, 4);
	std::array<u8, 8> data {};

	pci_dma_read(nullptr, GapsRamSize - 4, data.data(), data.size());

	constexpr std::array<u8, 8> expected { 0xef, 0xcd, 0xab, 0x89, 0x67, 0x45, 0x23, 0x01 };
	EXPECT_EQ(expected, data);
}

class BbaPciTest : public testing::Test
{
protected:
	void SetUp() override
	{
		bba_Init();
	}

	void TearDown() override
	{
		bba_Term();
	}
};

class BbaTimerTest : public BbaPciTest
{
protected:
	void tick(int cycles)
	{
		Sh4cntx.sh4_sched_next -= cycles;
		sh4_sched_tick(cycles);
	}
};

TEST_F(BbaPciTest, ReportsDreamcastBbaIdentity)
{
	EXPECT_EQ(0x123411dbu, bba_ReadMem(PciConfigBase, 4));
	EXPECT_EQ(0x02000010u, bba_ReadMem(PciConfigBase + PCI_REVISION_ID, 4));
	EXPECT_EQ(0x74000000u, bba_ReadMem(RtlRegistersBase + 0x40, 4));
	EXPECT_EQ(0x10u, bba_ReadMem(RtlRegistersBase + 0x5e, 1));
	EXPECT_EQ(0x0200u, bba_ReadMem(PciConfigBase + PCI_STATUS, 2));
	EXPECT_EQ(0u, bba_ReadMem(PciConfigBase + PCI_CAPABILITY_LIST, 1));
	EXPECT_EQ(0u, bba_ReadMem(PciConfigBase + 0x50, 4));
	EXPECT_EQ(0x00000100u, bba_ReadMem(PciConfigBase + PCI_INTERRUPT_LINE, 2));
	EXPECT_EQ(0x2020u, bba_ReadMem(PciConfigBase + PCI_MIN_GNT, 2));
}

TEST_F(BbaPciTest, GapsResetRequiresMagicCommandAndReportsReady)
{
	constexpr u32 GapsReset = GapsRegistersBase + 0x18;
	constexpr u32 ReceiveConfiguration = RtlRegistersBase + 0x44;
	bba_WriteMem(ReceiveConfiguration, 0x0000ef0f, 4);
	ASSERT_NE(0u, bba_ReadMem(ReceiveConfiguration, 4));

	bba_WriteMem(GapsReset, 1, 4);
	EXPECT_EQ(0u, bba_ReadMem(GapsReset, 4));
	EXPECT_NE(0u, bba_ReadMem(ReceiveConfiguration, 4));

	bba_WriteMem(GapsReset, 0x5a14a501, 4);
	EXPECT_EQ(1u, bba_ReadMem(GapsReset, 4));
	EXPECT_EQ(0u, bba_ReadMem(ReceiveConfiguration, 4));
}

TEST_F(BbaPciTest, GapsDmaRangeRegistersDoNotRemapRam)
{
	constexpr u32 DmaBase = GapsRegistersBase + 0x28;
	constexpr u32 DmaEnd = GapsRegistersBase + 0x2c;
	bba_WriteMem(GapsRamBase, 0x44332211, 4);

	bba_WriteMem(DmaBase, 0x01840000, 4);
	bba_WriteMem(DmaEnd, 0x01841004, 4);

	EXPECT_EQ(0x01840000u, bba_ReadMem(DmaBase, 4));
	EXPECT_EQ(0x01841004u, bba_ReadMem(DmaEnd, 4));
	EXPECT_EQ(0x44332211u, bba_ReadMem(GapsRamBase + GapsRamSize, 4));
}

TEST_F(BbaPciTest, AppliesPciConfigWriteMasks)
{
	bba_WriteMem(PciConfigBase + PCI_COMMAND, 0xffff, 2);
	EXPECT_EQ(0x0147u, bba_ReadMem(PciConfigBase + PCI_COMMAND, 2));

	bba_WriteMem(PciConfigBase + PCI_BASE_ADDRESS_0, 0xffffffff, 4);
	EXPECT_EQ(0xffffff01u, bba_ReadMem(PciConfigBase + PCI_BASE_ADDRESS_0, 4));
	bba_WriteMem(PciConfigBase + PCI_BASE_ADDRESS_0, 0x00002001, 4);
	EXPECT_EQ(0x00002001u, bba_ReadMem(PciConfigBase + PCI_BASE_ADDRESS_0, 4));

	bba_WriteMem(PciConfigBase + PCI_LATENCY_TIMER, 0xf0, 1);
	EXPECT_EQ(0xf0u, bba_ReadMem(PciConfigBase + PCI_LATENCY_TIMER, 1));
	bba_WriteMem(PciConfigBase + PCI_CACHE_LINE_SIZE, 0xff, 1);
	EXPECT_EQ(0u, bba_ReadMem(PciConfigBase + PCI_CACHE_LINE_SIZE, 1));
	bba_WriteMem(PciConfigBase + PCI_BIST, 0x40, 1);
	EXPECT_EQ(0u, bba_ReadMem(PciConfigBase + PCI_BIST, 1));
	bba_WriteMem(PciConfigBase + PCI_STATUS, 0xf900, 2);
	EXPECT_EQ(0x0200u, bba_ReadMem(PciConfigBase + PCI_STATUS, 2));

	const u32 identity = bba_ReadMem(PciConfigBase, 4);
	bba_WriteMem(PciConfigBase, 0, 4);
	EXPECT_EQ(identity, bba_ReadMem(PciConfigBase, 4));
}

TEST_F(BbaPciTest, SoftwareResetPreservesProgrammedMacAndMulticastFilter)
{
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 ReceiveBufferStart = 0x30;
	constexpr u32 ReceiveConfiguration = 0x44;
	constexpr u32 SoftwareReset = 0x10;
	bba_WriteMem(RtlRegistersBase, 0x44332211, 4);
	bba_WriteMem(RtlRegistersBase + 4, 0x00006655, 4);
	bba_WriteMem(RtlRegistersBase + 8, 0xa5a55a5a, 4);
	bba_WriteMem(RtlRegistersBase + 12, 0x5a5aa5a5, 4);
	bba_WriteMem(RtlRegistersBase + ReceiveBufferStart, 0x01840000, 4);
	for (u32 descriptor = 0; descriptor < 4; ++descriptor)
		bba_WriteMem(RtlRegistersBase + 0x20 + descriptor * 4,
					 0x01846000 + descriptor * 0x800, 4);
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0x0000ef0f, 4);
	ASSERT_NE(0u, bba_ReadMem(RtlRegistersBase + ReceiveConfiguration, 4));

	bba_WriteMem(RtlRegistersBase + ChipCommand, SoftwareReset, 1);

	EXPECT_EQ(0x44332211u, bba_ReadMem(RtlRegistersBase, 4));
	EXPECT_EQ(0x6655u, bba_ReadMem(RtlRegistersBase + 4, 2));
	EXPECT_EQ(0xa5a55a5au, bba_ReadMem(RtlRegistersBase + 8, 4));
	EXPECT_EQ(0x5a5aa5a5u, bba_ReadMem(RtlRegistersBase + 12, 4));
	EXPECT_EQ(0x01840000u, bba_ReadMem(RtlRegistersBase + ReceiveBufferStart, 4));
	for (u32 descriptor = 0; descriptor < 4; ++descriptor)
		EXPECT_EQ(0x01846000u + descriptor * 0x800,
				  bba_ReadMem(RtlRegistersBase + 0x20 + descriptor * 4, 4));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + ReceiveConfiguration, 4));
}

TEST_F(BbaPciTest, MacAndMulticastFilterRequireDwordWrites)
{
	const u32 mac = bba_ReadMem(RtlRegistersBase, 4);
	const u32 multicast = bba_ReadMem(RtlRegistersBase + 8, 4);

	bba_WriteMem(RtlRegistersBase, 0xff, 1);
	bba_WriteMem(RtlRegistersBase + 2, 0xffff, 2);
	bba_WriteMem(RtlRegistersBase + 8, 0xff, 1);
	bba_WriteMem(RtlRegistersBase + 10, 0xffff, 2);
	EXPECT_EQ(mac, bba_ReadMem(RtlRegistersBase, 4));
	EXPECT_EQ(multicast, bba_ReadMem(RtlRegistersBase + 8, 4));

	bba_WriteMem(RtlRegistersBase, 0x44332211, 4);
	bba_WriteMem(RtlRegistersBase + 8, 0xa5a55a5a, 4);
	EXPECT_EQ(0x44332211u, bba_ReadMem(RtlRegistersBase, 4));
	EXPECT_EQ(0xa5a55a5au, bba_ReadMem(RtlRegistersBase + 8, 4));
}

TEST_F(BbaPciTest, LegacyTransmitRegistersRequireAlignedDwordAccess)
{
	constexpr u32 TransmitStatus0 = 0x10;
	constexpr u32 TransmitAddress0 = 0x20;
	const u32 status = bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4);

	bba_WriteMem(RtlRegistersBase + TransmitAddress0, 0x01846000, 4);
	EXPECT_EQ(0x01846000u,
			  bba_ReadMem(RtlRegistersBase + TransmitAddress0, 4));

	bba_WriteMem(RtlRegistersBase + TransmitStatus0 + 1, 0, 4);
	bba_WriteMem(RtlRegistersBase + TransmitAddress0 + 1, 0xdeadbeef, 4);
	EXPECT_EQ(status, bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4));
	EXPECT_EQ(0x01846000u,
			  bba_ReadMem(RtlRegistersBase + TransmitAddress0, 4));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + TransmitStatus0, 1));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + TransmitAddress0, 2));
}

TEST_F(BbaTimerTest, LegacyTransmitCompletesAfterPciAndWireTime)
{
	constexpr u32 TransmitStatus0 = 0x10;
	constexpr u32 TransmitAddress0 = 0x20;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u32 FrameSize = 60;
	constexpr u32 TxHostOwns = 0x2000;
	constexpr u32 TxStatOk = 0x8000;
	constexpr u32 TxOk = 0x0004;
	// ERTXTH=0 starts the wire after two PCI dwords (8 bytes), then a
	// minimum frame, preamble/SFD, FCS, and the 84-bit IFG at 100 Mb/s.
	constexpr int CompletionCycles = 2 * 6 + ((60 + 8 + 4) * 8 + 84) * 2;

	for (u32 offset = 0; offset < FrameSize; offset += 4)
		bba_WriteMem(GapsRamBase + offset, 0x01020304 + offset, 4);
	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x04, 1);
	bba_WriteMem(RtlRegistersBase + TransmitAddress0, 0, 4);
	bba_WriteMem(RtlRegistersBase + TransmitStatus0, FrameSize, 4);

	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4)
			& (TxHostOwns | TxStatOk));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TxOk);
	/* PCI DMA reaches ERTXTH after 8 bytes, then continues in 32-byte bursts. */
	tick(2 * 6);
	tick(8 * 6);
	tick(5 * 6);
	EXPECT_EQ(TxHostOwns, bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4)
			& (TxHostOwns | TxStatOk));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TxOk);
	tick(CompletionCycles - 15 * 6 - 1);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TxOk);
	tick(1);
	EXPECT_EQ(TxHostOwns | TxStatOk,
			bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4)
					& (TxHostOwns | TxStatOk));
	EXPECT_EQ(TxOk, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TxOk);
}

TEST_F(BbaTimerTest, LateTransmitCallbackConsumesEveryElapsedEvent)
{
	constexpr u32 TransmitStatus0 = 0x10;
	constexpr u32 TransmitAddress0 = 0x20;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u32 FrameSize = 60;
	constexpr u32 TxHostOwns = 0x2000;
	constexpr u32 TxStatOk = 0x8000;
	constexpr u32 TxOk = 0x0004;
	constexpr int CompletionCycles = 2 * 6 + ((60 + 8 + 4) * 8 + 84) * 2;

	for (u32 offset = 0; offset < FrameSize; offset += 4)
		bba_WriteMem(GapsRamBase + offset, 0x10203040 + offset, 4);
	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x04, 1);
	bba_WriteMem(RtlRegistersBase + TransmitAddress0, 0, 4);
	bba_WriteMem(RtlRegistersBase + TransmitStatus0, FrameSize, 4);

	// Dispatch once at the final deadline. PCI threshold, remaining fetch
	// bursts, and wire completion must all catch up within this callback.
	tick(CompletionCycles);
	EXPECT_EQ(TxHostOwns | TxStatOk,
			bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4)
					& (TxHostOwns | TxStatOk));
	EXPECT_EQ(TxOk, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TxOk);
}

TEST_F(BbaTimerTest, LegacyTransmitSamplesMemoryAsPciBurstsArrive)
{
	constexpr u32 TransmitStatus0 = 0x10;
	constexpr u32 TransmitAddress0 = 0x20;
	constexpr u32 ReceiveBufferStart = 0x30;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 TransmitConfiguration = 0x40;
	constexpr u32 ReceiveConfiguration = 0x44;
	constexpr u32 FrameSize = 60;
	constexpr u32 RxOffset = 0x4000;
	constexpr int FirstThresholdCycles = 2 * 6;
	constexpr int SecondBurstCycles = 8 * 6;
	constexpr int FinalBurstCycles = 5 * 6;
	constexpr int WireCyclesAfterDma = ((60 + 8 + 4) * 8 + 84) * 2 - (15 - 2) * 6;
	constexpr int RxFirstBurstCycles = 8 * 6;

	for (u32 offset = 0; offset < FrameSize; offset += 4)
		bba_WriteMem(GapsRamBase + offset, offset < 8 ? 0xffffffff : 0, 4);
	bba_WriteMem(RtlRegistersBase + ReceiveBufferStart, 0x01840000 + RxOffset, 4);
	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x0c, 1);
	/* Promiscuous acceptance must not suppress broadcast classification. */
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0x01, 4);
	bba_WriteMem(RtlRegistersBase + TransmitConfiguration, 0x74060000, 4);
	bba_WriteMem(RtlRegistersBase + TransmitAddress0, 0x01840000, 4);
	bba_WriteMem(RtlRegistersBase + TransmitStatus0, FrameSize, 4);

	tick(FirstThresholdCycles);
	tick(SecondBurstCycles - 1);
	bba_WriteMem(GapsRamBase + 8, 0x11223344, 4);
	tick(1);
	bba_WriteMem(GapsRamBase + 8, 0x55667788, 4);
	tick(FinalBurstCycles - 1);
	bba_WriteMem(GapsRamBase + 44, 0x99aabbcc, 4);
	tick(1);
	bba_WriteMem(GapsRamBase + 44, 0xddeeff00, 4);
	tick(WireCyclesAfterDma);
	tick(RxFirstBurstCycles);
	tick(53);
	tick(1);

	// Four-byte receive header followed by the captured Ethernet frame.
	EXPECT_EQ(0x11223344u, bba_ReadMem(GapsRamBase + RxOffset + 12, 4));
	EXPECT_EQ(0x99aabbccu, bba_ReadMem(GapsRamBase + RxOffset + 48, 4));
}

TEST_F(BbaTimerTest, LegacyTransmitPadsRuntFrameBeforeLoopback)
{
	constexpr u32 TransmitStatus0 = 0x10;
	constexpr u32 TransmitAddress0 = 0x20;
	constexpr u32 ReceiveBufferStart = 0x30;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u32 TransmitConfiguration = 0x40;
	constexpr u32 ReceiveConfiguration = 0x44;
	constexpr u32 FrameSize = 20;
	constexpr u32 ReceiveOffset = 0x4000;
	constexpr u16 ReceiveOk = 0x0001;
	constexpr u16 ReceiveBroadcast = 0x2000;
	constexpr int ThresholdCycles = 2 * 6;
	constexpr int FinalDmaCycles = 3 * 6;
	constexpr int TransmitCycles = 5 * 6 + ((60 + 8 + 4) * 8 + 84) * 2
			- (5 - 2) * 6;

	for (u32 offset = 0; offset < FrameSize; offset += 4)
		bba_WriteMem(GapsRamBase + offset, offset < 8 ? 0xffffffff : 0xaaaaaaaa, 4);
	bba_WriteMem(RtlRegistersBase + ReceiveBufferStart,
			0x01840000 + ReceiveOffset, 4);
	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x0c, 1);
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0x08, 4);
	bba_WriteMem(RtlRegistersBase + TransmitConfiguration, 0x74060000, 4);
	bba_WriteMem(RtlRegistersBase + TransmitAddress0, 0x01840000, 4);
	bba_WriteMem(RtlRegistersBase + TransmitStatus0, FrameSize, 4);

	tick(ThresholdCycles);
	tick(FinalDmaCycles);
	tick(TransmitCycles - ThresholdCycles - FinalDmaCycles);
	/* Three receive PCI bursts: 32, 32, and 4 bytes. */
	tick(48);
	tick(53);
	tick(1);
	EXPECT_EQ((64u << 16) | ReceiveBroadcast | ReceiveOk,
			bba_ReadMem(GapsRamBase + ReceiveOffset, 4));
	EXPECT_EQ(0xaaaaaaaau, bba_ReadMem(GapsRamBase + ReceiveOffset + 20, 4));
	EXPECT_EQ(0u, bba_ReadMem(GapsRamBase + ReceiveOffset + 24, 4));
	EXPECT_EQ(ReceiveOk,
			bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & ReceiveOk);
}

TEST_F(BbaTimerTest, LegacyTransmitHonorsForcedTenMegabitMode)
{
	constexpr u32 TransmitStatus0 = 0x10;
	constexpr u32 TransmitAddress0 = 0x20;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u32 BasicModeControl = 0x62;
	constexpr u32 FrameSize = 60;
	constexpr u32 TxOk = 0x0004;
	constexpr int CompletionCycles100 = 2 * 6 + ((60 + 8 + 4) * 8 + 84) * 2;
	constexpr int CompletionCycles10 = 2 * 6 + ((60 + 8 + 4) * 8 + 84) * 20;

	bba_WriteMem(RtlRegistersBase + BasicModeControl, 0, 2);
	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x04, 1);
	bba_WriteMem(RtlRegistersBase + TransmitAddress0, 0, 4);
	bba_WriteMem(RtlRegistersBase + TransmitStatus0, FrameSize, 4);

	tick(2 * 6);
	tick(8 * 6);
	tick(5 * 6);
	tick(CompletionCycles100 - 15 * 6);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TxOk);
	tick(CompletionCycles10 - CompletionCycles100 - 1);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TxOk);
	tick(1);
	EXPECT_EQ(TxOk, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TxOk);
}

TEST_F(BbaTimerTest, LegacyTransmitHonorsEarlyTransmitThreshold)
{
	constexpr u32 TransmitStatus0 = 0x10;
	constexpr u32 TransmitAddress0 = 0x20;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u32 FrameSize = 60;
	constexpr u32 TxOk = 0x0004;
	constexpr u32 EarlyThreshold64Bytes = 2 << 16;
	constexpr int DefaultThresholdCompletion = 2 * 6 + ((60 + 8 + 4) * 8 + 84) * 2;
	constexpr int FullDmaCompletion = 15 * 6 + ((60 + 8 + 4) * 8 + 84) * 2;

	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x04, 1);
	bba_WriteMem(RtlRegistersBase + TransmitAddress0, 0, 4);
	bba_WriteMem(RtlRegistersBase + TransmitStatus0,
				EarlyThreshold64Bytes | FrameSize, 4);

	tick(8 * 6);
	tick(7 * 6);
	tick(DefaultThresholdCompletion - 15 * 6);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TxOk);
	tick(FullDmaCompletion - DefaultThresholdCompletion - 1);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TxOk);
	tick(1);
	EXPECT_EQ(TxOk, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TxOk);
}

TEST_F(BbaTimerTest, QueuedTransmitDmaOverlapsEarlierWireFrame)
{
	constexpr u32 TransmitStatus0 = 0x10;
	constexpr u32 TransmitStatus1 = 0x14;
	constexpr u32 TransmitAddress0 = 0x20;
	constexpr u32 TransmitAddress1 = 0x24;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 FrameSize = 60;
	constexpr u32 TxHostOwns = 0x2000;
	constexpr u32 TxStatOk = 0x8000;

	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x04, 1);
	bba_WriteMem(RtlRegistersBase + TransmitAddress0, 0, 4);
	bba_WriteMem(RtlRegistersBase + TransmitAddress1, 0x100, 4);
	bba_WriteMem(RtlRegistersBase + TransmitStatus0, FrameSize, 4);
	bba_WriteMem(RtlRegistersBase + TransmitStatus1, FrameSize, 4);

	/* Descriptor 0: 8-byte threshold, 32-byte burst, then 20 bytes. */
	tick(2 * 6);
	tick(8 * 6);
	tick(5 * 6);
	EXPECT_EQ(TxHostOwns, bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4)
			& (TxHostOwns | TxStatOk));
	/* Descriptor 1 DMA proceeds while descriptor 0 remains on the wire. */
	tick(2 * 6);
	tick(8 * 6);
	tick(5 * 6);
	EXPECT_EQ(TxHostOwns, bba_ReadMem(RtlRegistersBase + TransmitStatus1, 4)
			& (TxHostOwns | TxStatOk));
	EXPECT_EQ(TxHostOwns, bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4)
			& (TxHostOwns | TxStatOk));

	/* The first wire frame completes at cycle 1332; only then can frame 1
	 * occupy the MAC, despite its DMA having completed much earlier. */
	tick(1152);
	EXPECT_EQ(TxHostOwns | TxStatOk,
			bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4)
					& (TxHostOwns | TxStatOk));
	EXPECT_EQ(TxHostOwns, bba_ReadMem(RtlRegistersBase + TransmitStatus1, 4)
			& (TxHostOwns | TxStatOk));
	tick(1320);
	EXPECT_EQ(TxHostOwns | TxStatOk,
			bba_ReadMem(RtlRegistersBase + TransmitStatus1, 4)
					& (TxHostOwns | TxStatOk));
}

TEST_F(BbaTimerTest, TransmitQueuedMidWireStartsDmaImmediately)
{
	constexpr u32 TransmitStatus0 = 0x10;
	constexpr u32 TransmitStatus1 = 0x14;
	constexpr u32 TransmitAddress0 = 0x20;
	constexpr u32 TransmitAddress1 = 0x24;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 FrameSize = 60;
	constexpr u32 TxHostOwns = 0x2000;
	constexpr u32 TxStatOk = 0x8000;

	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x04, 1);
	bba_WriteMem(RtlRegistersBase + TransmitAddress0, 0, 4);
	bba_WriteMem(RtlRegistersBase + TransmitAddress1, 0x100, 4);
	bba_WriteMem(RtlRegistersBase + TransmitStatus0, FrameSize, 4);
	tick(2 * 6);
	tick(8 * 6);
	tick(5 * 6);
	ASSERT_EQ(TxHostOwns, bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4)
			& (TxHostOwns | TxStatOk));

	/* Submit descriptor 1 after 600 otherwise idle wire cycles. */
	tick(600);
	bba_WriteMem(RtlRegistersBase + TransmitStatus1, FrameSize, 4);
	tick(2 * 6);
	tick(8 * 6);
	tick(5 * 6);
	EXPECT_EQ(TxHostOwns, bba_ReadMem(RtlRegistersBase + TransmitStatus1, 4)
			& (TxHostOwns | TxStatOk));
	EXPECT_EQ(TxHostOwns, bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4)
			& (TxHostOwns | TxStatOk));

	/* Descriptor 0 still completes at its original cycle-1332 deadline. */
	tick(552);
	EXPECT_EQ(TxHostOwns | TxStatOk,
			bba_ReadMem(RtlRegistersBase + TransmitStatus0, 4)
					& (TxHostOwns | TxStatOk));
	tick(1320);
	EXPECT_EQ(TxHostOwns | TxStatOk,
			bba_ReadMem(RtlRegistersBase + TransmitStatus1, 4)
					& (TxHostOwns | TxStatOk));
}

TEST_F(BbaTimerTest, SaveStateResumesPartialTransmitDma)
{
	constexpr u32 TransmitStatus0 = 0x10;
	constexpr u32 TransmitAddress0 = 0x20;
	constexpr u32 ReceiveBufferStart = 0x30;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 TransmitConfiguration = 0x40;
	constexpr u32 ReceiveConfiguration = 0x44;
	constexpr u32 FrameSize = 60;
	constexpr u32 ReceiveOffset = 0x4000;
	constexpr int WireCyclesAfterDma = ((60 + 8 + 4) * 8 + 84) * 2
			- (15 - 2) * 6;

	for (u32 offset = 0; offset < FrameSize; offset += 4)
		bba_WriteMem(GapsRamBase + offset, offset < 8 ? 0xffffffff : 0, 4);
	bba_WriteMem(RtlRegistersBase + ReceiveBufferStart,
			0x01840000 + ReceiveOffset, 4);
	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x0c, 1);
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0x01, 4);
	bba_WriteMem(RtlRegistersBase + TransmitConfiguration, 0x74060000, 4);
	bba_WriteMem(RtlRegistersBase + TransmitAddress0, 0x01840000, 4);
	bba_WriteMem(RtlRegistersBase + TransmitStatus0, FrameSize, 4);

	/* Save halfway between the 8-byte threshold and the following 32-byte
	 * PCI burst. Neither elapsed wire time nor the burst deadline may reset. */
	tick(2 * 6);
	tick(4 * 6);
	Serializer sizing;
	bba_Serialize(sizing);
	std::vector<u8> state(sizing.size());
	Serializer serializer(state.data(), state.size());
	bba_Serialize(serializer);
	Deserializer deserializer(state.data(), state.size());
	bba_Deserialize(deserializer);

	bba_WriteMem(GapsRamBase + 8, 0x12345678, 4);
	tick(4 * 6);
	bba_WriteMem(GapsRamBase + 8, 0x87654321, 4);
	tick(5 * 6);
	tick(WireCyclesAfterDma);
	tick(8 * 6);
	tick(53);
	tick(1);

	EXPECT_EQ(0x12345678u,
			bba_ReadMem(GapsRamBase + ReceiveOffset + 12, 4));
}

TEST_F(BbaPciTest, Rtl8139cTreatsLegacyHaltClockOffsetAsReserved)
{
	constexpr u32 Reserved5B = 0x5b;
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + Reserved5B, 1));

	bba_WriteMem(RtlRegistersBase + Reserved5B, 'H', 1);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + Reserved5B, 1));
	bba_WriteMem(RtlRegistersBase + Reserved5B, 'R', 1);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + Reserved5B, 1));
}

TEST_F(BbaTimerTest, ReceiveWritesLegacyRingEntryAndRaisesRxOk)
{
	constexpr u32 ReceiveBufferStart = 0x30;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 CurrentBufferAddress = 0x3a;
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u32 ReceiveConfiguration = 0x44;
	constexpr u16 ReceiveOk = 0x0001;
	constexpr u16 ReceiveBroadcast = 0x2000;
	std::array<u8, 60> frame {};
	frame.fill(0xff);

	bba_WriteMem(RtlRegistersBase + ReceiveBufferStart, 0x01840000, 4);
	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x08, 1);
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0x08, 4);
	bba_WriteMem(GapsRamBase, 0, 4);

	ASSERT_EQ(1, bba_recv_frame(frame.data(), frame.size()));
	/* A filter change after arrival cannot retroactively reject the frame. */
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0, 4);
	tick(47);
	EXPECT_EQ(0u, bba_ReadMem(GapsRamBase, 4));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & ReceiveOk);
	tick(1);
	EXPECT_EQ((64u << 16) | ReceiveBroadcast | ReceiveOk,
			  bba_ReadMem(GapsRamBase, 4));
	EXPECT_EQ(32u, bba_ReadMem(RtlRegistersBase + CurrentBufferAddress, 2));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & ReceiveOk);
	tick(53);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & ReceiveOk);
	tick(1);
	EXPECT_EQ((64u << 16) | ReceiveBroadcast | ReceiveOk,
			  bba_ReadMem(GapsRamBase, 4));
	EXPECT_EQ(0xffffffffu, bba_ReadMem(GapsRamBase + 4, 4));
	EXPECT_EQ(68u, bba_ReadMem(RtlRegistersBase + CurrentBufferAddress, 2));
	EXPECT_EQ(ReceiveOk,
			  bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & ReceiveOk);

	/* Updating packet filters must not discard unread ring contents. */
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0x0a, 4);
	EXPECT_EQ(68u, bba_ReadMem(RtlRegistersBase + CurrentBufferAddress, 2));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + ChipCommand, 1) & 1);

	/* Changing the configured ring length reinitializes its pointers. */
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0x080a, 4);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + CurrentBufferAddress, 2));
	EXPECT_EQ(1u, bba_ReadMem(RtlRegistersBase + ChipCommand, 1) & 1);
}

TEST_F(BbaTimerTest, SaveStateResumesPartialReceiveDma)
{
	constexpr u32 ReceiveBufferStart = 0x30;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 CurrentBufferAddress = 0x3a;
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u32 ReceiveConfiguration = 0x44;
	constexpr u16 ReceiveOk = 0x0001;
	std::array<u8, 60> frame {};
	frame.fill(0xff);

	bba_WriteMem(RtlRegistersBase + ReceiveBufferStart, 0x01840000, 4);
	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x08, 1);
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0x08, 4);
	bba_WriteMem(GapsRamBase + 32, 0, 4);
	ASSERT_EQ(1, bba_recv_frame(frame.data(), frame.size()));
	tick(8 * 6);
	ASSERT_EQ(32u,
			bba_ReadMem(RtlRegistersBase + CurrentBufferAddress, 2));
	tick(4 * 6);

	Serializer sizing;
	bba_Serialize(sizing);
	std::vector<u8> state(sizing.size());
	Serializer serializer(state.data(), state.size());
	bba_Serialize(serializer);
	Deserializer deserializer(state.data(), state.size());
	bba_Deserialize(deserializer);

	tick(4 * 6 - 1);
	EXPECT_EQ(0u, bba_ReadMem(GapsRamBase + 32, 4));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & ReceiveOk);
	tick(1);
	EXPECT_EQ(0xffffffffu, bba_ReadMem(GapsRamBase + 32, 4));
	EXPECT_EQ(64u,
			bba_ReadMem(RtlRegistersBase + CurrentBufferAddress, 2));
	tick(5);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & ReceiveOk);
	tick(1);
	EXPECT_EQ(68u,
			bba_ReadMem(RtlRegistersBase + CurrentBufferAddress, 2));
	EXPECT_EQ(ReceiveOk,
			bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & ReceiveOk);
}

TEST_F(BbaTimerTest, LateReceiveCallbackConsumesEveryPciBurst)
{
	constexpr u32 ReceiveBufferStart = 0x30;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 CurrentBufferAddress = 0x3a;
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u32 ReceiveConfiguration = 0x44;
	constexpr u16 ReceiveOk = 0x0001;
	constexpr u16 ReceiveBroadcast = 0x2000;
	std::array<u8, 60> frame {};
	frame.fill(0xff);

	bba_WriteMem(RtlRegistersBase + ReceiveBufferStart, 0x01840000, 4);
	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x08, 1);
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0x08, 4);
	bba_WriteMem(GapsRamBase, 0, 4);
	ASSERT_EQ(1, bba_recv_frame(frame.data(), frame.size()));

	// A 68-byte ring entry uses 32-, 32-, and 4-byte PCI writes: 48+48+6
	// SH-4 cycles. One late dispatch must make every elapsed burst visible.
	tick(102);
	EXPECT_EQ((64u << 16) | ReceiveBroadcast | ReceiveOk,
			bba_ReadMem(GapsRamBase, 4));
	EXPECT_EQ(68u, bba_ReadMem(RtlRegistersBase + CurrentBufferAddress, 2));
	EXPECT_EQ(ReceiveOk,
			bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & ReceiveOk);
}

TEST_F(BbaTimerTest, PendingReceiveDmaReservesRingSpace)
{
	constexpr u32 ReceiveBufferStart = 0x30;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 CurrentAddress = 0x3a;
	constexpr u32 CurrentReadAddress = 0x38;
	constexpr u32 InterruptMask = 0x3c;
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u32 ReceiveConfiguration = 0x44;
	constexpr u32 MissedPacketCounter = 0x4c;
	constexpr u16 ReceiveOverflow = 0x0010;
	std::array<u8, 60> frame {};
	frame.fill(0xff);

	bba_WriteMem(RtlRegistersBase + ReceiveBufferStart, 0x01840000, 4);
	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x08, 1);
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0x08, 4);
	bba_WriteMem(RtlRegistersBase + InterruptMask, ReceiveOverflow, 2);
	/* CAPR is reported 16 bytes behind the actual consumer pointer. */
	bba_WriteMem(RtlRegistersBase + CurrentReadAddress, 84, 2);

	ASSERT_EQ(1, bba_recv_frame(frame.data(), frame.size()));
	ASSERT_EQ(1, bba_recv_frame(frame.data(), frame.size()));
	EXPECT_EQ(ReceiveOverflow,
			bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & ReceiveOverflow);
	EXPECT_EQ(1u, bba_ReadMem(RtlRegistersBase + MissedPacketCounter, 4));

	tick(48);
	tick(53);
	tick(1);
	EXPECT_EQ(68u, bba_ReadMem(RtlRegistersBase + CurrentAddress, 2));
}

TEST_F(BbaTimerTest, TimerCountsAtPciClockAndRaisesInterrupt)
{
	constexpr u16 PciTimeout = 0x4000;
	bba_WriteMem(RtlRegistersBase + 0x3c, PciTimeout, 2);
	bba_WriteMem(RtlRegistersBase + 0x48, 0, 4);
	bba_WriteMem(RtlRegistersBase + 0x54, 10, 4);

	tick(59);
	EXPECT_EQ(9u, bba_ReadMem(RtlRegistersBase + 0x48, 4));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + 0x3e, 2) & PciTimeout);

	tick(1);
	EXPECT_EQ(10u, bba_ReadMem(RtlRegistersBase + 0x48, 4));
	EXPECT_EQ(PciTimeout, bba_ReadMem(RtlRegistersBase + 0x3e, 2) & PciTimeout);
}

TEST_F(BbaTimerTest, SaveStatePreservesPciTimerDeadline)
{
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u32 Timer = 0x48;
	constexpr u32 TimerInterrupt = 0x54;
	constexpr u16 TimerExpired = 0x4000;

	bba_WriteMem(RtlRegistersBase + Timer, 0, 4);
	bba_WriteMem(RtlRegistersBase + TimerInterrupt, 10, 4);
	tick(4 * 6);
	ASSERT_EQ(4u, bba_ReadMem(RtlRegistersBase + Timer, 4));

	Serializer sizing;
	bba_Serialize(sizing);
	std::vector<u8> state(sizing.size());
	Serializer serializer(state.data(), state.size());
	bba_Serialize(serializer);
	Deserializer deserializer(state.data(), state.size());
	bba_Deserialize(deserializer);

	tick(6 * 6 - 1);
	EXPECT_EQ(9u, bba_ReadMem(RtlRegistersBase + Timer, 4));
	EXPECT_EQ(0u,
			bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TimerExpired);
	tick(1);
	EXPECT_EQ(10u, bba_ReadMem(RtlRegistersBase + Timer, 4));
	EXPECT_EQ(TimerExpired,
			bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & TimerExpired);
}

TEST_F(BbaTimerTest, InterruptAcknowledgePreservesOtherPendingCauses)
{
	constexpr u32 ReceiveBufferStart = 0x30;
	constexpr u32 ChipCommand = 0x37;
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u32 ReceiveConfiguration = 0x44;
	constexpr u32 Timer = 0x48;
	constexpr u32 TimerInterrupt = 0x54;
	constexpr u16 ReceiveOk = 0x0001;
	constexpr u16 TimerExpired = 0x4000;
	constexpr u32 PendingCauses = ReceiveOk | TimerExpired;
	std::array<u8, 60> frame {};
	frame.fill(0xff);

	bba_WriteMem(RtlRegistersBase + ReceiveBufferStart, 0x01840000, 4);
	bba_WriteMem(RtlRegistersBase + ChipCommand, 0x08, 1);
	bba_WriteMem(RtlRegistersBase + ReceiveConfiguration, 0x08, 4);
	bba_WriteMem(RtlRegistersBase + Timer, 0, 4);
	bba_WriteMem(RtlRegistersBase + TimerInterrupt, 1, 4);
	ASSERT_EQ(1, bba_recv_frame(frame.data(), frame.size()));
	tick(48);
	tick(53);
	tick(1);

	ASSERT_EQ(PendingCauses,
			bba_ReadMem(RtlRegistersBase + InterruptStatus, 2)
					& PendingCauses);
	bba_WriteMem(RtlRegistersBase + InterruptStatus, ReceiveOk, 2);
	EXPECT_EQ(static_cast<u32>(TimerExpired),
			bba_ReadMem(RtlRegistersBase + InterruptStatus, 2)
					& PendingCauses);
	bba_WriteMem(RtlRegistersBase + InterruptStatus, TimerExpired, 2);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2)
			& PendingCauses);
}

TEST_F(BbaTimerTest, PhyResetAndAutoNegotiationRestartAreSelfClearing)
{
	constexpr u32 BasicModeControl = 0x62;
	constexpr u32 BasicModeStatus = 0x64;
	constexpr u32 InterruptStatus = 0x3e;
	constexpr u16 LinkChange = 0x20;
	constexpr u16 LinkAndNegotiationComplete = 0x24;

	/* Speed, duplex, and auto-negotiation enable are normal R/W fields. */
	bba_WriteMem(RtlRegistersBase + BasicModeControl, 0x2100, 2);
	EXPECT_EQ(0x2100u, bba_ReadMem(RtlRegistersBase + BasicModeControl, 2));

	/* A PHY reset restores BMCR defaults and ignores the other command bits. */
	bba_WriteMem(RtlRegistersBase + BasicModeControl, 0x9200, 2);
	EXPECT_EQ(0x1000u, bba_ReadMem(RtlRegistersBase + BasicModeControl, 2));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + BasicModeStatus, 2)
				  & LinkAndNegotiationComplete);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & LinkChange);

	tick(1999999);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & LinkChange);
	tick(1);
	EXPECT_EQ(LinkAndNegotiationComplete,
			  bba_ReadMem(RtlRegistersBase + BasicModeStatus, 2)
				  & LinkAndNegotiationComplete);
	EXPECT_EQ(LinkChange, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & LinkChange);

	bba_WriteMem(RtlRegistersBase + InterruptStatus, LinkChange, 2);
	bba_WriteMem(RtlRegistersBase + BasicModeControl, 0x1200, 2);
	EXPECT_EQ(0x1000u, bba_ReadMem(RtlRegistersBase + BasicModeControl, 2));
	tick(2000000);
	EXPECT_EQ(LinkChange, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & LinkChange);

	/* Restart is ignored when the same write disables auto-negotiation. */
	bba_WriteMem(RtlRegistersBase + InterruptStatus, LinkChange, 2);
	bba_WriteMem(RtlRegistersBase + BasicModeControl, 0x0200, 2);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + BasicModeControl, 2));
	tick(2000000);
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + InterruptStatus, 2) & LinkChange);
}

TEST_F(BbaPciTest, PhyStatusRegistersAreReadOnly)
{
	constexpr u32 BasicModeStatus = 0x64;
	constexpr u32 LinkPartnerAbility = 0x68;
	constexpr u32 AutoNegotiationExpansion = 0x6a;
	const u32 status = bba_ReadMem(RtlRegistersBase + BasicModeStatus, 2);
	const u32 partner = bba_ReadMem(RtlRegistersBase + LinkPartnerAbility, 2);
	const u32 expansion = bba_ReadMem(RtlRegistersBase + AutoNegotiationExpansion, 2);

	bba_WriteMem(RtlRegistersBase + BasicModeStatus, 0xffff, 2);
	bba_WriteMem(RtlRegistersBase + LinkPartnerAbility, 0xffff, 2);
	bba_WriteMem(RtlRegistersBase + AutoNegotiationExpansion, 0xffff, 2);

	EXPECT_EQ(status, bba_ReadMem(RtlRegistersBase + BasicModeStatus, 2));
	EXPECT_EQ(partner, bba_ReadMem(RtlRegistersBase + LinkPartnerAbility, 2));
	EXPECT_EQ(expansion, bba_ReadMem(RtlRegistersBase + AutoNegotiationExpansion, 2));
}

TEST_F(BbaPciTest, ConfigurationRegistersRequire9346WriteEnable)
{
	constexpr u32 Configuration9346 = 0x50;
	constexpr u32 Configuration1 = 0x52;
	constexpr u32 Configuration4 = 0x5a;
	constexpr u32 Configuration5 = 0xd8;

	EXPECT_EQ(0x0cu, bba_ReadMem(RtlRegistersBase + Configuration1, 1));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + Configuration4, 1));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + Configuration5, 1));
	bba_WriteMem(RtlRegistersBase + Configuration1, 0x20, 1);
	bba_WriteMem(RtlRegistersBase + Configuration4, 0x80, 1);
	bba_WriteMem(RtlRegistersBase + Configuration5, 0x04, 1);
	EXPECT_EQ(0x0cu, bba_ReadMem(RtlRegistersBase + Configuration1, 1));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + Configuration4, 1));
	EXPECT_EQ(0u, bba_ReadMem(RtlRegistersBase + Configuration5, 1));

	bba_WriteMem(RtlRegistersBase + Configuration9346, 0xc0, 1);
	bba_WriteMem(RtlRegistersBase + Configuration1, 0x20, 1);
	bba_WriteMem(RtlRegistersBase + Configuration4, 0x80, 1);
	bba_WriteMem(RtlRegistersBase + Configuration5, 0x04, 1);
	EXPECT_EQ(0x2cu, bba_ReadMem(RtlRegistersBase + Configuration1, 1));
	EXPECT_EQ(0x80u, bba_ReadMem(RtlRegistersBase + Configuration4, 1));
	EXPECT_EQ(0x04u, bba_ReadMem(RtlRegistersBase + Configuration5, 1));

	bba_WriteMem(RtlRegistersBase + Configuration9346, 0, 1);
	bba_WriteMem(RtlRegistersBase + Configuration1, 0, 1);
	bba_WriteMem(RtlRegistersBase + Configuration4, 0, 1);
	bba_WriteMem(RtlRegistersBase + Configuration5, 0, 1);
	EXPECT_EQ(0x2cu, bba_ReadMem(RtlRegistersBase + Configuration1, 1));
	EXPECT_EQ(0x80u, bba_ReadMem(RtlRegistersBase + Configuration4, 1));
	EXPECT_EQ(0x04u, bba_ReadMem(RtlRegistersBase + Configuration5, 1));
}

}
