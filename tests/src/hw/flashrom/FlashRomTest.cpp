#include "gtest/gtest.h"
#include "hw/flashrom/flashrom.h"
#include <vector>

namespace {

class TestFlash : public DCFlashChip
{
public:
	using DCFlashChip::DCFlashChip;

	void advance(u64 elapsed) { cycles += elapsed; }

private:
	u64 current_cycle() const override { return cycles; }
	u64 cycles = 0;
};

void unlock(DCFlashChip& flash, u8 command)
{
	flash.Write(0x555, 0xaa, 1);
	flash.Write(0x2aa, 0x55, 1);
	flash.Write(0x555, command, 1);
}

void byteUnlock(DCFlashChip& flash, u8 command)
{
	flash.Write(0xaaa, 0xaa, 1);
	flash.Write(0x555, 0x55, 1);
	flash.Write(0xaaa, command, 1);
}

void eraseSector(DCFlashChip& flash, u32 address)
{
	unlock(flash, 0x80);
	flash.Write(0x555, 0xaa, 1);
	flash.Write(0x2aa, 0x55, 1);
	flash.Write(address, 0x30, 1);
}

} // namespace

TEST(FlashRomTest, AutoselectPersistsUntilReset)
{
	TestFlash flash(128_KB);
	unlock(flash, 0x90);

	EXPECT_EQ(0x04, flash.Read8(0));
	EXPECT_EQ(0x40, flash.Read8(1));
	EXPECT_EQ(0x04, flash.Read8(0));
	EXPECT_EQ(1, flash.Read8(0x1a002));
	EXPECT_EQ(0, flash.Read8(0x18002));

	flash.Write(0, 0xf0, 1);
	EXPECT_EQ(0xff, flash.Read8(0));
}

TEST(FlashRomTest, AcceptsByteAddressedCommandSequence)
{
	TestFlash flash(128_KB);
	byteUnlock(flash, 0x90);
	EXPECT_EQ(0x04, flash.Read8(0));
	EXPECT_EQ(0x40, flash.Read8(1));
	flash.Write(0, 0xf0, 1);

	flash.data[0x1234] = 0xff;
	byteUnlock(flash, 0xa0);
	flash.Write(0x1234, 0x3c, 1);
	flash.advance(8_sh4us);
	EXPECT_EQ(0x3c, flash.Read8(0x1234));
}

TEST(FlashRomTest, ResetAbortsIncompleteCommand)
{
	TestFlash flash(128_KB);
	flash.data[0x100] = 0xff;
	flash.Write(0x555, 0xaa, 1);
	flash.Write(0, 0xf0, 1);
	flash.Write(0x100, 0x00, 1);

	EXPECT_EQ(0xff, flash.Read8(0x100));
}

TEST(FlashRomTest, WideWritesAreByteBusTransactions)
{
	TestFlash flash(128_KB);
	flash.Write(0x555, 0x000000aa, 4);
	flash.Write(0x2aa, 0x55, 1);
	flash.Write(0x555, 0x90, 1);
	EXPECT_EQ(0xff, flash.Read8(0));

	flash.data[0x100] = 0xff;
	unlock(flash, 0xa0);
	flash.Write(0x100, 0x44332211, 4);
	flash.advance(8_sh4us);
	EXPECT_EQ(0x11, flash.Read8(0x100));
	EXPECT_EQ(0xff, flash.Read8(0x101));
}

TEST(FlashRomTest, FujitsuFastModeProgramsUntilExplicitExit)
{
	TestFlash flash(128_KB);
	flash.data[0x100] = flash.data[0x101] = flash.data[0x102] = 0xff;
	unlock(flash, 0x20);

	flash.Write(0, 0xa0, 1);
	flash.Write(0x100, 0x5a, 1);
	flash.advance(8_sh4us);
	EXPECT_EQ(0x5a, flash.Read8(0x100));

	flash.Write(0, 0xa0, 1);
	flash.Write(0x101, 0xa5, 1);
	flash.advance(8_sh4us);
	EXPECT_EQ(0xa5, flash.Read8(0x101));

	flash.Write(0, 0x90, 1);
	flash.Write(0, 0xf0, 1);
	flash.Write(0, 0xa0, 1);
	flash.Write(0x102, 0x00, 1);
	flash.advance(8_sh4us);
	EXPECT_EQ(0xff, flash.Read8(0x102));
}

TEST(FlashRomTest, ProgrammingOnlyClearsBitsAndProtectsFactorySector)
{
	TestFlash flash(128_KB);
	flash.data[0x100] = 0xf0;
	unlock(flash, 0xa0);
	flash.Write(0x100, 0x5a, 1);
	flash.advance(8_sh4us);
	EXPECT_EQ(0x50, flash.Read8(0x100));

	flash.data[0x1a100] = 0xff;
	unlock(flash, 0xa0);
	flash.Write(0x1a100, 0x00, 1);
	flash.advance(2_sh4us);
	EXPECT_EQ(0xff, flash.Read8(0x1a100));
}

TEST(FlashRomTest, SectorEraseUsesTopBootGeometryAndHonorsProtection)
{
	TestFlash flash(128_KB);
	flash.data[0x10000] = 0;
	flash.data[0x17fff] = 0;
	flash.data[0x18000] = 0;
	eraseSector(flash, 0x10000);
	flash.advance(SH4_MAIN_CLOCK);
	EXPECT_EQ(0xff, flash.Read8(0x10000));
	EXPECT_EQ(0xff, flash.Read8(0x17fff));
	EXPECT_EQ(0x00, flash.Read8(0x18000));

	flash.data[0x1a000] = 0;
	eraseSector(flash, 0x1a000);
	flash.advance(200_sh4us);
	EXPECT_EQ(0x00, flash.Read8(0x1a000));
}

TEST(FlashRomTest, AtomiswaveUsesMacronixIdentityAndUniformSectors)
{
	TestFlash flash(128_KB, 64_KB, DCFlashChip::Type::Macronix29L001MC);
	unlock(flash, 0x90);
	EXPECT_EQ(0xc2, flash.Read8(0));
	EXPECT_EQ(0x51, flash.Read8(1));
	flash.Write(0, 0xf0, 1);

	flash.data[0] = 0;
	flash.data[0x10000] = 0;
	flash.data[0x1ffff] = 0;
	eraseSector(flash, 0x10000);
	flash.advance(SH4_MAIN_CLOCK);
	EXPECT_EQ(0x00, flash.Read8(0));
	EXPECT_EQ(0xff, flash.Read8(0x10000));
	EXPECT_EQ(0xff, flash.Read8(0x1ffff));

	flash.data[0] = 0;
	eraseSector(flash, 0);
	flash.advance(200_sh4us);
	EXPECT_EQ(0x00, flash.Read8(0));
}

TEST(FlashRomTest, ChipErasePreservesOnlyProtectedSectors)
{
	TestFlash dc(128_KB);
	dc.data[0] = dc.data[0x1a000] = dc.data[0x1c000] = 0;
	unlock(dc, 0x80);
	dc.Write(0x555, 0xaa, 1);
	dc.Write(0x2aa, 0x55, 1);
	dc.Write(0x555, 0x10, 1);
	dc.advance(31 * (u64)SH4_MAIN_CLOCK / 10);
	EXPECT_EQ(0xff, dc.Read8(0));
	EXPECT_EQ(0x00, dc.Read8(0x1a000));
	EXPECT_EQ(0xff, dc.Read8(0x1c000));

	TestFlash aw(128_KB, 64_KB, DCFlashChip::Type::Macronix29L001MC);
	aw.data[0] = aw.data[0x10000] = 0;
	unlock(aw, 0x80);
	aw.Write(0x555, 0xaa, 1);
	aw.Write(0x2aa, 0x55, 1);
	aw.Write(0x555, 0x10, 1);
	aw.advance(31 * (u64)SH4_MAIN_CLOCK / 10);
	EXPECT_EQ(0x00, aw.Read8(0));
	EXPECT_EQ(0xff, aw.Read8(0x10000));
}

TEST(FlashRomTest, ProgramAndEraseExposePollingStatusUntilDeadline)
{
	TestFlash flash(128_KB);
	flash.data[0x1234] = 0xff;
	unlock(flash, 0xa0);
	flash.Write(0x1234, 0x80, 1);
	u8 first = flash.Read8(0x1234);
	u8 second = flash.Read8(0x1234);
	EXPECT_EQ(0, first & 0x80); // complement of programmed DQ7
	EXPECT_NE(first & 0x40, second & 0x40);
	EXPECT_EQ(0xff, flash.data[0x1234]);
	flash.advance(8_sh4us);
	EXPECT_EQ(0x80, flash.Read8(0x1234));

	flash.data[0x10000] = 0;
	eraseSector(flash, 0x10000);
	first = flash.Read8(0x10000);
	second = flash.Read8(0x10000);
	EXPECT_NE(first & 0x40, second & 0x40); // DQ6 toggles
	EXPECT_NE(first & 0x04, second & 0x04); // DQ2 toggles in erased sector
	EXPECT_EQ(0, first & 0x08);            // sector selection window is open
	EXPECT_EQ(0, flash.data[0x10000]);
	flash.advance(50_sh4us);
	EXPECT_NE(0, flash.Read8(0x10000) & 0x08); // embedded erase has started
	flash.advance(SH4_MAIN_CLOCK - 50_sh4us);
	EXPECT_EQ(0xff, flash.Read8(0x10000));
}

TEST(FlashRomTest, SectorEraseWindowAcceptsMultipleSectors)
{
	TestFlash flash(128_KB);
	flash.data[0] = flash.data[0x10000] = flash.data[0x18000] = 0;
	eraseSector(flash, 0);
	flash.Write(0x10000, 0x30, 1);

	u8 first = flash.Read8(0x10000);
	u8 second = flash.Read8(0x10000);
	EXPECT_NE(first & 0x04, second & 0x04);
	first = flash.Read8(0x18000);
	second = flash.Read8(0x18000);
	EXPECT_EQ(first & 0x04, second & 0x04);

	flash.advance(SH4_MAIN_CLOCK);
	EXPECT_EQ(0xff, flash.Read8(0));
	EXPECT_EQ(0xff, flash.Read8(0x10000));
	EXPECT_EQ(0x00, flash.Read8(0x18000));
}

TEST(FlashRomTest, SaveStatePreservesInFlightOperation)
{
	TestFlash source(128_KB);
	source.data[0x1234] = 0xff;
	unlock(source, 0xa0);
	source.Write(0x1234, 0x5a, 1);
	source.advance(3_sh4us);

	std::vector<u8> buffer(256_KB);
	Serializer serializer(buffer.data(), buffer.size());
	static_cast<MemChip&>(source).Serialize(serializer);

	TestFlash restored(128_KB);
	Deserializer deserializer(buffer.data(), serializer.size());
	static_cast<MemChip&>(restored).Deserialize(deserializer);
	restored.advance(3_sh4us); // scheduler time is restored separately at the same instant
	EXPECT_EQ(0xff, restored.data[0x1234]);
	restored.advance(5_sh4us);
	EXPECT_EQ(0x5a, restored.Read8(0x1234));
}

TEST(FlashRomTest, EraseSuspendAllowsReadsAndProgrammingOutsideSelectedSectors)
{
	TestFlash flash(128_KB);
	flash.data[0x100] = 0x00;
	flash.data[0x10100] = 0xff;
	eraseSector(flash, 0x100);
	flash.advance(100_sh4us);
	flash.Write(0, 0xb0, 1);

	u8 first = flash.Read8(0x100);
	u8 second = flash.Read8(0x100);
	EXPECT_NE(0, first & 0x80);
	EXPECT_EQ(0, first & 0x40);
	EXPECT_EQ(0, second & 0x40);
	EXPECT_NE(first & 0x04, second & 0x04);
	EXPECT_EQ(0xff, flash.Read8(0x10100));

	unlock(flash, 0xa0);
	flash.Write(0x10100, 0x25, 1);
	flash.advance(8_sh4us);
	EXPECT_EQ(0x25, flash.Read8(0x10100));
	EXPECT_EQ(0x00, flash.data[0x100]);

	unlock(flash, 0x90);
	EXPECT_EQ(0x04, flash.Read8(0));
	EXPECT_EQ(0x40, flash.Read8(1));
	flash.Write(0, 0xf0, 1);
	EXPECT_NE(0, flash.Read8(0x100) & 0x80);

	flash.Write(0, 0x30, 1);
	flash.advance(SH4_MAIN_CLOCK - 100_sh4us);
	EXPECT_EQ(0xff, flash.Read8(0x100));
	EXPECT_EQ(0x25, flash.Read8(0x10100));
}

TEST(FlashRomTest, SuspendDuringSelectionWindowPreservesFullEraseDuration)
{
	TestFlash flash(128_KB);
	flash.data[0] = 0;
	eraseSector(flash, 0);
	flash.Write(0, 0xb0, 1);
	flash.advance(SH4_MAIN_CLOCK);
	EXPECT_EQ(0x00, flash.data[0]);
	flash.Write(0, 0x30, 1);
	flash.advance(SH4_MAIN_CLOCK);
	EXPECT_EQ(0xff, flash.Read8(0));
}
