#include "gtest/gtest.h"

#include "hw/maple/vmu_emulator.h"

#include <array>

namespace
{

TEST(VmuEmulatorTest, ExecutesFirmwareAndAdvancesTimersByInstructionCycles)
{
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
	rom[0] = 0x01; // BR +0, two cycles
	rom[1] = 0;
	vmu::Emulator emulator(rom.data(), rom.size(), flash.data(), flash.size());
	emulator.memory().writeData(0x11b, 0xfe);
	emulator.memory().writeData(0x118, 0x40);
	EXPECT_EQ(2, emulator.step().cycles);
	// Starting a timer consumes one cycle, leaving one timer tick.
	EXPECT_EQ(0xff, emulator.memory().readData(0x11b));
}

TEST(VmuEmulatorTest, HaltKeepsTimersRunningUntilInterruptWakesCpu)
{
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
	rom[0x2b] = 0x00;
	vmu::Emulator emulator(rom.data(), rom.size(), flash.data(), flash.size());
	emulator.memory().writeData(0x106, 0x80);
	emulator.memory().writeData(0x108, 0x80);
	emulator.memory().writeData(0x109, 0x08);
	emulator.memory().writeData(0x107, 0x01);
	EXPECT_EQ(1, emulator.step().cycles);
	EXPECT_EQ(0, emulator.cpu().pc());
	emulator.raiseInterrupt(vmu::Lc86InterruptController::Irq::Timer1);
	EXPECT_EQ(1, emulator.step().cycles);
	EXPECT_EQ(0x2c, emulator.cpu().pc());
	EXPECT_EQ(0, emulator.memory().readData(0x107) & 1);
}

TEST(VmuEmulatorTest, HoldStopsInstructionAndPeripheralCycles)
{
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
	vmu::Emulator emulator(rom.data(), rom.size(), flash.data(), flash.size());
	emulator.memory().writeData(0x107, 0x02);
	EXPECT_EQ(0, emulator.step().cycles);
	EXPECT_EQ(0, emulator.cpu().pc());
}

TEST(VmuEmulatorTest, ButtonPressSetsPortSourceWakesHoldAndRequestsInterrupt)
{
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
	vmu::Emulator emulator(rom.data(), rom.size(), flash.data(), flash.size());
	emulator.memory().writeData(0x14c, 0xff);
	emulator.memory().writeData(0x14d, 0x00);
	emulator.memory().writeData(0x14e, 0x05); // detection and IRQ enabled
	emulator.memory().writeData(0x107, 0x02);
	emulator.setButtons(0x10);
	EXPECT_EQ(0, emulator.memory().readSfrRaw(0x107) & 0x02);
	EXPECT_NE(0, emulator.memory().readSfrRaw(0x14e) & 0x02);
	EXPECT_NE(0, emulator.pic().pendingMask() &
			(1u << static_cast<unsigned>(vmu::Lc86InterruptController::Irq::Port3)));
}

TEST(VmuEmulatorTest, WallTimeUsesSelectedOscillatorAndDivider)
{
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
	vmu::Emulator emulator(rom.data(), rom.size(), flash.data(), flash.size());
	EXPECT_EQ(50000u, emulator.clockHz()); // 600 kHz RC divided by 12
	EXPECT_EQ(1u, emulator.runForNanoseconds(10'000));
	EXPECT_EQ(0u, emulator.runForNanoseconds(10'000));
	EXPECT_EQ(1, emulator.cpu().pc());

	emulator.memory().writeData(0x10e, 0x90); // 6 MHz crystal divided by 6
	EXPECT_EQ(1'000'000u, emulator.clockHz());
	EXPECT_EQ(1u, emulator.runForNanoseconds(1'000));
	EXPECT_EQ(2, emulator.cpu().pc());
}

TEST(VmuEmulatorTest, BuzzerOutputFollowsTimer1PwmRouting)
{
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
	vmu::Emulator emulator(rom.data(), rom.size(), flash.data(), flash.size());
	u8 period = 0xff;
	u8 duty = 0xff;
	EXPECT_FALSE(emulator.buzzerOutput(period, duty));
	EXPECT_EQ(0, period);
	EXPECT_EQ(0, duty);

	emulator.memory().writeData(0x144, 0x00);
	emulator.memory().writeData(0x145, 0x80);
	emulator.memory().writeData(0x146, 0x80);
	emulator.memory().writeData(0x11b, 0xc0);
	emulator.memory().writeData(0x11a, 0xe0);
	emulator.memory().writeData(0x118, 0x40);
	EXPECT_TRUE(emulator.buzzerOutput(period, duty));
	EXPECT_EQ(0xc0, period);
	EXPECT_EQ(0xe0, duty);

	emulator.memory().writeData(0x118, 0x60); // 16-bit mode cannot drive PWM
	EXPECT_FALSE(emulator.buzzerOutput(period, duty));
}

} // namespace
