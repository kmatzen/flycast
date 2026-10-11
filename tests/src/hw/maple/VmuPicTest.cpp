#include "gtest/gtest.h"

#include "hw/maple/vmu_pic.h"

#include <array>

namespace
{

class PicMemory final : public vmu::Lc86Memory
{
public:
	u8 readProgram(u16 address) override { return program[address]; }
	u8 readData(u16 address) override { return data[address]; }
	void writeData(u16 address, u8 value) override { data[address] = value; }
	u8 readFlash(u32) override { return 0xff; }
	void writeFlashInstruction(u16, u8, u8, u8) override {}
	void returnFromInterrupt() override {}

	std::array<u8, 65536> program {};
	std::array<u8, 512> data {};
};

TEST(VmuPicTest, AcceptingInterruptPushesPcClearsHaltAndUsesVector)
{
	PicMemory memory;
	memory.data[0x106] = 0x80;
	memory.data[0x107] = 1;
	memory.data[0x108] = 0x80;
	memory.data[0x109] = 0x08; // Timer 1 is high priority
	vmu::Lc86Cpu cpu(memory);
	cpu.setPc(0x1234);
	vmu::Lc86InterruptController pic(cpu, memory);
	pic.raise(vmu::Lc86InterruptController::Irq::Timer1);
	EXPECT_TRUE(pic.poll());
	EXPECT_EQ(0x2b, cpu.pc());
	EXPECT_EQ(0, memory.data[0x107] & 1);
	EXPECT_EQ(0x82, memory.data[0x106]);
	EXPECT_EQ(0x34, memory.data[0x81]);
	EXPECT_EQ(0x12, memory.data[0x82]);
}

TEST(VmuPicTest, MasterEnableMasksLowButNotHighPriority)
{
	PicMemory memory;
	memory.data[0x106] = 0x80;
	memory.data[0x108] = 0; // master low-priority enable is clear
	vmu::Lc86Cpu cpu(memory);
	vmu::Lc86InterruptController pic(cpu, memory);
	pic.raise(vmu::Lc86InterruptController::Irq::Timer0High);
	EXPECT_FALSE(pic.poll());
	EXPECT_NE(0, pic.pendingMask());
	memory.data[0x109] = 0x04; // promote Timer 0 high
	EXPECT_TRUE(pic.poll());
	EXPECT_EQ(0x23, cpu.pc());
}

TEST(VmuPicTest, NonMaskableExternalInterruptIgnoresMasterEnable)
{
	PicMemory memory;
	memory.data[0x106] = 0x80;
	memory.data[0x108] = 0;
	vmu::Lc86Cpu cpu(memory);
	vmu::Lc86InterruptController pic(cpu, memory);
	pic.raise(vmu::Lc86InterruptController::Irq::External0);
	EXPECT_TRUE(pic.poll());
	EXPECT_EQ(0x03, cpu.pc());
}

TEST(VmuPicTest, RetiDefersAcceptanceForOneInstructionBoundary)
{
	PicMemory memory;
	memory.data[0x106] = 0x80;
	memory.data[0x108] = 0x80;
	memory.data[0x109] = 0x0c; // timers 0 high and 1 are high priority
	vmu::Lc86Cpu cpu(memory);
	vmu::Lc86InterruptController pic(cpu, memory);
	pic.raise(vmu::Lc86InterruptController::Irq::Timer0High);
	EXPECT_TRUE(pic.poll());
	pic.raise(vmu::Lc86InterruptController::Irq::Timer1);
	pic.returned();
	EXPECT_FALSE(pic.poll());
	EXPECT_TRUE(pic.poll());
	EXPECT_EQ(0x2b, cpu.pc());
}

} // namespace
