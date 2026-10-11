#include "gtest/gtest.h"

#include "hw/maple/vmu_timers.h"

#include <array>

namespace
{

TEST(VmuTimersTest, Timer0PrescalerReloadsAndRaisesLowInterrupt)
{
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	vmu::Lc86Cpu cpu(memory);
	vmu::Lc86InterruptController pic(cpu, memory);
	vmu::Lc86Timers timers(memory, pic);
	memory.writeData(0x111, 0xff); // one CPU cycle per timer tick
	memory.writeData(0x113, 0xfe);
	memory.writeData(0x110, 0x41); // run low, enable low interrupt
	timers.advance(1); // documented one-cycle start delay
	EXPECT_EQ(0xfe, memory.readData(0x112));
	timers.advance(2);
	EXPECT_EQ(0xfe, memory.readData(0x112));
	EXPECT_NE(0, memory.readSfrRaw(0x110) & 0x02);
	EXPECT_NE(0, pic.pendingMask() &
			(1u << static_cast<unsigned>(vmu::Lc86InterruptController::Irq::External2Timer0Low)));
}

TEST(VmuTimersTest, Timer1UsesInstructionCyclesAndRaisesSharedInterrupt)
{
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	vmu::Lc86Cpu cpu(memory);
	vmu::Lc86InterruptController pic(cpu, memory);
	vmu::Lc86Timers timers(memory, pic);
	memory.writeData(0x11b, 0xfe);
	memory.writeData(0x118, 0x41); // run low, enable low interrupt
	timers.advance(1);
	timers.advance(2);
	EXPECT_EQ(0xfe, memory.readData(0x11b));
	EXPECT_NE(0, memory.readSfrRaw(0x118) & 0x02);
	EXPECT_NE(0, pic.pendingMask() &
			(1u << static_cast<unsigned>(vmu::Lc86InterruptController::Irq::Timer1)));
}

TEST(VmuTimersTest, Timer1LongModeChainsLowOverflowIntoHighCounter)
{
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	vmu::Lc86Cpu cpu(memory);
	vmu::Lc86InterruptController pic(cpu, memory);
	vmu::Lc86Timers timers(memory, pic);
	memory.writeData(0x11b, 0xff);
	memory.writeData(0x11d, 0xfe);
	memory.writeData(0x118, 0xe4); // long mode, both halves running, high IRQ
	timers.advance(1);
	timers.advance(2);
	EXPECT_EQ(0xff, memory.readData(0x11b));
	EXPECT_EQ(0xfe, memory.readData(0x11d));
	EXPECT_EQ(0x0a, memory.readSfrRaw(0x118) & 0x0a);
	EXPECT_NE(0, pic.pendingMask() &
			(1u << static_cast<unsigned>(vmu::Lc86InterruptController::Irq::Timer1)));
}

TEST(VmuTimersTest, BaseTimerCycleClockRaisesChannelZeroInterrupt)
{
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	vmu::Lc86Cpu cpu(memory);
	vmu::Lc86InterruptController pic(cpu, memory);
	vmu::Lc86Timers timers(memory, pic);
	memory.writeData(0x15f, 0x10); // CPU-cycle source
	memory.writeData(0x17f, 0xc1); // run, 6-bit channel 0, interrupt enabled
	timers.advance(65); // one start-delay cycle plus 64 timer clocks
	EXPECT_NE(0, memory.readSfrRaw(0x17f) & 0x02);
	EXPECT_NE(0, pic.pendingMask() &
			(1u << static_cast<unsigned>(vmu::Lc86InterruptController::Irq::External3BaseTimer)));
}

TEST(VmuTimersTest, BaseTimerFastChannelOneUsesTwoTickRate)
{
	std::array<u8, vmu::Lc86MemoryMap::RomSize> rom {};
	std::array<u8, vmu::Lc86MemoryMap::FlashSize> flash {};
	vmu::Lc86MemoryMap memory(rom.data(), rom.size(), flash.data(), flash.size());
	vmu::Lc86Cpu cpu(memory);
	vmu::Lc86InterruptController pic(cpu, memory);
	vmu::Lc86Timers timers(memory, pic);
	memory.writeData(0x15f, 0x10);
	memory.writeData(0x17f, 0xe4); // run, fast channel 1, interrupt enabled
	timers.advance(3);
	EXPECT_NE(0, memory.readSfrRaw(0x17f) & 0x08);
	EXPECT_NE(0, pic.pendingMask() &
			(1u << static_cast<unsigned>(vmu::Lc86InterruptController::Irq::External3BaseTimer)));
}

} // namespace
