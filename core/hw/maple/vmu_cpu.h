/*
 * Standalone Sanyo LC86K87/LC8670 CPU core used by the Dreamcast VMU.
 *
 * Opcode encodings and semantics are based on the Sega VMU hardware manual
 * and the MIT-licensed libevmu ISA work by Falco Girgis.
 */
#pragma once

#include "types.h"

namespace vmu
{

class Lc86Memory
{
public:
	virtual ~Lc86Memory() = default;
	virtual u8 readProgram(u16 address) = 0;
	virtual u8 readData(u16 address) = 0;
	virtual u8 readDataLatch(u16 address) { return readData(address); }
	virtual void writeData(u16 address, u8 value) = 0;
	virtual void writeDataLatch(u16 address, u8 value) { writeData(address, value); }
	virtual u8 readFlash(u32 address) = 0;
	virtual void switchProgramBank() {}
	// The flash device owns the three-write unlock sequence and 128-byte
	// programming window; the CPU supplies the raw registers used by STF.
	virtual void writeFlashInstruction(u16 address, u8 value, u8 flashControl,
			u8 externalControl) = 0;
	virtual void returnFromInterrupt() = 0;
};

class Lc86Cpu
{
public:
	struct StepResult
	{
		u8 cycles;
		bool valid;
	};

	explicit Lc86Cpu(Lc86Memory& memory) : memory(memory) { reset(); }

	void reset();
	StepResult step();
	void acceptInterrupt(u16 vector);
	void serialize(Serializer& serializer) const;
	void deserialize(Deserializer& deserializer);

	u16 pc() const { return programCounter; }
	void setPc(u16 value) { programCounter = value; }

private:
	static constexpr u16 ACC = 0x100;
	static constexpr u16 PSW = 0x101;
	static constexpr u16 SP = 0x106;
	static constexpr u16 EXT = 0x10d;
	static constexpr u16 TRL = 0x104;
	static constexpr u16 TRH = 0x105;
	static constexpr u16 FPR = 0x154;

	u8 fetch8() { return memory.readProgram(programCounter++); }
	u16 direct9(u8 opcode) { return static_cast<u16>((opcode & 1) << 8) | fetch8(); }
	u16 bitDirect9(u8 opcode) { return static_cast<u16>((opcode & 0x10) << 4) | fetch8(); }
	u16 indirect(u8 opcode);
	u16 absolute12(u8 opcode);
	void setFlag(u8 mask, bool set);
	void add(u8 value, bool withCarry);
	void subtract(u8 value, bool withCarry);
	void compareBranch(u8 lhs, u8 rhs, bool equal, s8 displacement);
	void push(u8 value);
	u8 pop();
	void pushPc();
	void popPc();

	Lc86Memory& memory;
	u16 programCounter = 0;
};

} // namespace vmu
