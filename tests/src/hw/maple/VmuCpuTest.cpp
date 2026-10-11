#include "gtest/gtest.h"

#include "hw/maple/vmu_cpu.h"

#include <array>

namespace
{

class TestMemory final : public vmu::Lc86Memory
{
public:
	u8 readProgram(u16 address) override { return program[address]; }
	u8 readData(u16 address) override { return data[address]; }
	u8 readDataLatch(u16 address) override
	{
		latchRead = true;
		return data[address];
	}
	void writeData(u16 address, u8 value) override { data[address] = value; }
	void writeDataLatch(u16 address, u8 value) override
	{
		latchWritten = true;
		data[address] = value;
	}
	u8 readFlash(u32 address) override { return flash[address]; }
	void switchProgramBank() override { programBankSwitched = true; }
	void writeFlashInstruction(u16 address, u8 value, u8 flashControl,
			u8 externalControl) override
	{
		flashWriteAddress = address;
		flashWriteValue = value;
		flashWriteControl = flashControl;
		flashWriteExternal = externalControl;
	}
	void returnFromInterrupt() override { interruptReturned = true; }

	std::array<u8, 65536> program {};
	std::array<u8, 512> data {};
	std::array<u8, 128 * 1024> flash {};
	u16 flashWriteAddress = 0;
	u8 flashWriteValue = 0;
	u8 flashWriteControl = 0;
	u8 flashWriteExternal = 0;
	bool interruptReturned = false;
	bool programBankSwitched = false;
	bool latchRead = false;
	bool latchWritten = false;
};

TEST(VmuCpuTest, BranchUsesSignedOffsetFromFollowingInstruction)
{
	TestMemory memory;
	memory.program[0x100] = 0x01;
	memory.program[0x101] = 0xfe;
	vmu::Lc86Cpu cpu(memory);
	cpu.setPc(0x100);
	auto result = cpu.step();
	EXPECT_TRUE(result.valid);
	EXPECT_EQ(2, result.cycles);
	EXPECT_EQ(0x100, cpu.pc());
}

TEST(VmuCpuTest, DirectAddressIncludesOpcodeNinthBit)
{
	TestMemory memory;
	memory.program[0] = 0x03; // LD $1ab
	memory.program[1] = 0xab;
	memory.data[0x1ab] = 0x5a;
	vmu::Lc86Cpu cpu(memory);
	EXPECT_TRUE(cpu.step().valid);
	EXPECT_EQ(0x5a, memory.data[0x100]);
}

TEST(VmuCpuTest, NearCallAndReturnPreserveFollowingPc)
{
	TestMemory memory;
	memory.program[0x4000] = 0x1a; // CALL $4a55 (address bit 11 in opcode bit 4)
	memory.program[0x4001] = 0x55;
	memory.program[0x4a55] = 0xa0; // RET
	memory.data[0x106] = 0x80;
	vmu::Lc86Cpu cpu(memory);
	cpu.setPc(0x4000);
	EXPECT_TRUE(cpu.step().valid);
	EXPECT_EQ(0x4a55, cpu.pc());
	EXPECT_EQ(0x82, memory.data[0x106]);
	EXPECT_EQ(0x02, memory.data[0x81]);
	EXPECT_EQ(0x40, memory.data[0x82]);
	EXPECT_TRUE(cpu.step().valid);
	EXPECT_EQ(0x4002, cpu.pc());
	EXPECT_EQ(0x80, memory.data[0x106]);
}

TEST(VmuCpuTest, FarJumpSwitchesProgramBankAfterReadingTarget)
{
	TestMemory memory;
	memory.program[0] = 0x21;
	memory.program[1] = 0x12;
	memory.program[2] = 0x34;
	vmu::Lc86Cpu cpu(memory);
	EXPECT_EQ(2, cpu.step().cycles);
	EXPECT_EQ(0x1234, cpu.pc());
	EXPECT_TRUE(memory.programBankSwitched);
}

TEST(VmuCpuTest, NopExecutesForOneCycle)
{
	TestMemory memory;
	memory.program[0] = 0;
	vmu::Lc86Cpu cpu(memory);
	auto result = cpu.step();
	EXPECT_TRUE(result.valid);
	EXPECT_EQ(1, result.cycles);
}

TEST(VmuCpuTest, AddSetsCarryAuxCarryAndSignedOverflow)
{
	TestMemory memory;
	memory.program[0] = 0x81; // ADD #$01
	memory.program[1] = 1;
	memory.data[0x100] = 0x7f;
	vmu::Lc86Cpu cpu(memory);
	EXPECT_TRUE(cpu.step().valid);
	EXPECT_EQ(0x80, memory.data[0x100]);
	EXPECT_EQ(0x44, memory.data[0x101] & 0xc4); // AC and OV, no CY

	memory.program[2] = 0x81;
	memory.program[3] = 0x80;
	cpu.setPc(2);
	EXPECT_TRUE(cpu.step().valid);
	EXPECT_EQ(0x00, memory.data[0x100]);
	EXPECT_EQ(0x84, memory.data[0x101] & 0xc4); // CY and signed OV, no AC
}

TEST(VmuCpuTest, SubtractWithCarryUsesBorrowInput)
{
	TestMemory memory;
	memory.program[0] = 0xb1; // SUBC #$0f
	memory.program[1] = 0x0f;
	memory.data[0x100] = 0x10;
	memory.data[0x101] = 0x80;
	vmu::Lc86Cpu cpu(memory);
	EXPECT_TRUE(cpu.step().valid);
	EXPECT_EQ(0, memory.data[0x100]);
	EXPECT_EQ(0x40, memory.data[0x101] & 0xc4); // nibble borrow only
}

TEST(VmuCpuTest, CompareBranchSetsCarryAndUsesSignedDisplacement)
{
	TestMemory memory;
	memory.program[0x20] = 0x31; // BE #$20, -3
	memory.program[0x21] = 0x20;
	memory.program[0x22] = 0xfd;
	memory.data[0x100] = 0x10;
	vmu::Lc86Cpu cpu(memory);
	cpu.setPc(0x20);
	EXPECT_TRUE(cpu.step().valid);
	EXPECT_EQ(0x23, cpu.pc()); // unequal, no branch
	EXPECT_NE(0, memory.data[0x101] & 0x80);

	memory.data[0x100] = 0x20;
	cpu.setPc(0x20);
	EXPECT_TRUE(cpu.step().valid);
	EXPECT_EQ(0x20, cpu.pc());
	EXPECT_EQ(0, memory.data[0x101] & 0x80);
}

TEST(VmuCpuTest, BitAddressingCarriesNinthAddressBit)
{
	TestMemory memory;
	memory.program[0] = 0xfd; // SET1 $1aa.5
	memory.program[1] = 0xaa;
	vmu::Lc86Cpu cpu(memory);
	EXPECT_TRUE(cpu.step().valid);
	EXPECT_EQ(0x20, memory.data[0x1aa]);

	memory.program[2] = 0xdd; // CLR1 $1aa.5
	memory.program[3] = 0xaa;
	cpu.setPc(2);
	EXPECT_TRUE(cpu.step().valid);
	EXPECT_EQ(0, memory.data[0x1aa]);
}

TEST(VmuCpuTest, ReadModifyWriteInstructionsUsePortLatchAccess)
{
	TestMemory memory;
	memory.program[0] = 0x62; // INC $44
	memory.program[1] = 0x44;
	memory.data[0x44] = 0x7f;
	vmu::Lc86Cpu cpu(memory);
	auto result = cpu.step();
	EXPECT_TRUE(result.valid);
	EXPECT_EQ(1, result.cycles);
	EXPECT_TRUE(memory.latchRead);
	EXPECT_TRUE(memory.latchWritten);
	EXPECT_EQ(0x80, memory.data[0x44]);
}

TEST(VmuCpuTest, MoveImmediateDirectTakesOneCycle)
{
	TestMemory memory;
	memory.program[0] = 0x22; // MOV #$a5, $44
	memory.program[1] = 0x44;
	memory.program[2] = 0xa5;
	vmu::Lc86Cpu cpu(memory);
	auto result = cpu.step();
	EXPECT_TRUE(result.valid);
	EXPECT_EQ(1, result.cycles);
	EXPECT_EQ(0xa5, memory.data[0x44]);
}

TEST(VmuCpuTest, MultiplyAndDivideRoundTripRegisterTuple)
{
	TestMemory memory;
	memory.program[0] = 0x30; // MUL
	memory.program[1] = 0x40; // DIV
	memory.data[0x100] = 0x01;
	memory.data[0x103] = 0x02;
	memory.data[0x102] = 3;
	vmu::Lc86Cpu cpu(memory);
	EXPECT_EQ(7, cpu.step().cycles);
	EXPECT_EQ(0x000306u, static_cast<u32>((memory.data[0x102] << 16)
			| (memory.data[0x100] << 8) | memory.data[0x103]));
	// Restore divisor; DIV consumes B as the divisor rather than the product's
	// high byte, so use the original 16-bit dividend for a direct check.
	memory.data[0x100] = 0x03;
	memory.data[0x103] = 0x06;
	memory.data[0x102] = 3;
	EXPECT_EQ(7, cpu.step().cycles);
	EXPECT_EQ(0x0102, memory.data[0x100] << 8 | memory.data[0x103]);
	EXPECT_EQ(0, memory.data[0x102]);
}

TEST(VmuCpuTest, EveryOpcodeDecodes)
{
	for (u32 opcode = 0; opcode < 256; opcode++)
	{
		TestMemory memory;
		memory.program[0] = opcode;
		memory.data[0x106] = 0x80;
		vmu::Lc86Cpu cpu(memory);
		const auto result = cpu.step();
		EXPECT_TRUE(result.valid) << "opcode 0x" << std::hex << opcode;
	}
}

TEST(VmuCpuTest, FlashInstructionsUseFullHardwareAddressAndRawControls)
{
	TestMemory memory;
	memory.program[0] = 0x50; // LDF
	memory.program[1] = 0x51; // STF
	memory.data[0x104] = 0x34;
	memory.data[0x105] = 0x12;
	memory.data[0x154] = 0x03; // bank 1 plus unlock
	memory.data[0x10d] = 0x08;
	memory.flash[0x11234] = 0xa5;
	vmu::Lc86Cpu cpu(memory);
	EXPECT_EQ(2, cpu.step().cycles);
	EXPECT_EQ(0xa5, memory.data[0x100]);
	EXPECT_EQ(2, cpu.step().cycles);
	EXPECT_EQ(0x1234, memory.flashWriteAddress);
	EXPECT_EQ(0xa5, memory.flashWriteValue);
	EXPECT_EQ(0x03, memory.flashWriteControl);
	EXPECT_EQ(0x08, memory.flashWriteExternal);
}

TEST(VmuCpuTest, RetiRestoresPcBeforeNotifyingInterruptController)
{
	TestMemory memory;
	memory.program[0] = 0xb0;
	memory.data[0x106] = 0x82;
	memory.data[0x81] = 0x78;
	memory.data[0x82] = 0x56;
	vmu::Lc86Cpu cpu(memory);
	EXPECT_EQ(2, cpu.step().cycles);
	EXPECT_EQ(0x5678, cpu.pc());
	EXPECT_TRUE(memory.interruptReturned);
}

} // namespace
