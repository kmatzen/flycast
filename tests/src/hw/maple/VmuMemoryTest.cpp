#include "gtest/gtest.h"

#include "hw/maple/vmu_memory.h"

#include <array>

namespace
{

class VmuMemoryTest : public testing::Test
{
protected:
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
};

TEST_F(VmuMemoryTest, ProgramSourceTracksExtAndFlashBank)
{
	rom[0x1234] = 0x11;
	flash[0x1234] = 0x22;
	flash[0x11234] = 0x33;
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	EXPECT_EQ(0x11, memory.readProgram(0x1234));
	memory.writeData(0x10d, 1);
	EXPECT_EQ(0x11, memory.readProgram(0x1234));
	memory.switchProgramBank();
	EXPECT_EQ(0x22, memory.readProgram(0x1234));
	memory.writeData(0x10d, 0);
	memory.switchProgramBank();
	EXPECT_EQ(0x33, memory.readProgram(0x1234));
}

TEST_F(VmuMemoryTest, ResetUsesDocumentedBaseTimerDefaults)
{
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	EXPECT_EQ(0x41, memory.readData(0x17f));
}

TEST_F(VmuMemoryTest, PswSelectsRamBankAndAccumulatorUpdatesParity)
{
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	memory.writeData(0x101, 2);
	memory.writeData(0x20, 0x12);
	memory.writeData(0x101, 0);
	memory.writeData(0x20, 0x34);
	EXPECT_EQ(0x34, memory.readData(0x20));
	memory.writeData(0x101, 2);
	EXPECT_EQ(0x12, memory.readData(0x20));
	memory.writeData(0x100, 0x07);
	EXPECT_EQ(1, memory.readData(0x101) & 1);
	memory.writeData(0x100, 0x03);
	EXPECT_EQ(0, memory.readData(0x101) & 1);
}

TEST_F(VmuMemoryTest, XramBankingAndDisplayLockAreEnforced)
{
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	memory.writeData(0x180, 0x11);
	memory.writeData(0x125, 1);
	memory.writeData(0x180, 0x22);
	EXPECT_EQ(0x11, memory.xramBank(0)[0]);
	EXPECT_EQ(0x22, memory.xramBank(1)[0]);
	memory.writeData(0x127, 0x40);
	memory.writeData(0x180, 0x33);
	EXPECT_EQ(0xff, memory.readData(0x180));
	EXPECT_EQ(0x22, memory.xramBank(1)[0]);
}

TEST_F(VmuMemoryTest, XramUsesSparseLcdAndIconBankAddressing)
{
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	memory.writeData(0x18b, 0x11);
	memory.writeData(0x18c, 0x22); // gap between physical LCD rows
	memory.writeData(0x190, 0x33);
	EXPECT_EQ(0x11, memory.xramBank(0)[11]);
	EXPECT_EQ(0x33, memory.xramBank(0)[12]);
	EXPECT_EQ(0xff, memory.readData(0x18c));

	memory.writeData(0x125, 2);
	memory.writeData(0x185, 0x44);
	memory.writeData(0x186, 0x55);
	EXPECT_EQ(0x44, memory.xramBank(2)[5]);
	EXPECT_EQ(0xff, memory.readData(0x186));
}

TEST_F(VmuMemoryTest, LcdFramebufferCombinesTwoBanksAndBlanksWhenDisabled)
{
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	std::array<u8, 192> image;
	image.fill(0xaa);
	memory.writeData(0x180, 0x80);
	memory.writeData(0x125, 1);
	memory.writeData(0x180, 0x01);
	memory.copyLcdFramebuffer(image.data());
	EXPECT_EQ(0, image[0]);
	EXPECT_EQ(0, image[96]);

	memory.writeData(0x120, 0x08);
	memory.writeData(0x127, 0x80);
	memory.copyLcdFramebuffer(image.data());
	EXPECT_EQ(0x80, image[0]);
	EXPECT_EQ(0x01, image[96]);

	memory.writeData(0x127, 0);
	memory.copyLcdFramebuffer(image.data());
	for (u8 value : image)
		EXPECT_EQ(0, value);
}

TEST_F(VmuMemoryTest, WorkRamPortAutoIncrementsAcrossNinthAddressBit)
{
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	memory.writeData(0x163, 0x10);
	memory.writeData(0x164, 0xff);
	memory.writeData(0x165, 0);
	memory.writeData(0x166, 0xa5);
	EXPECT_EQ(0xa5, memory.workRam()[0xff]);
	EXPECT_EQ(0, memory.readData(0x164));
	EXPECT_EQ(0xff, memory.readData(0x165));
}

TEST_F(VmuMemoryTest, PortReadAndLatchReadRemainDistinct)
{
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	memory.writeData(0x14c, 0xff);
	memory.writeData(0x14d, 0);
	memory.setButtons(1 << 4); // A is active-low on P3.4
	EXPECT_EQ(0xef, memory.readData(0x14c));
	EXPECT_EQ(0xff, memory.readDataLatch(0x14c));
	memory.writeData(0x14c, 0xa0);
	memory.writeData(0x14d, 0xf0);
	EXPECT_EQ(0xaf, memory.readData(0x14c));
	EXPECT_EQ(0xa0, memory.readDataLatch(0x14c));
}

TEST_F(VmuMemoryTest, FlashProgrammingRequiresUnlockAndAlignedWindow)
{
	flash.fill(0xff);
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	memory.writeFlashInstruction(0x0080, 0x12, 0, 0);
	EXPECT_EQ(0xff, flash[0x80]);
	memory.writeFlashInstruction(0x5555, 0xaa, 2, 0);
	memory.writeFlashInstruction(0x2aaa, 0x55, 2, 0);
	memory.writeFlashInstruction(0x5555, 0xa0, 2, 0);
	memory.writeFlashInstruction(0x0080, 0x12, 0, 0);
	memory.writeFlashInstruction(0x0081, 0x34, 0, 0);
	EXPECT_EQ(0x12, flash[0x80]);
	EXPECT_EQ(0x34, flash[0x81]);
	EXPECT_TRUE(memory.takeFlashDirty());
	EXPECT_FALSE(memory.takeFlashDirty());
}

} // namespace
