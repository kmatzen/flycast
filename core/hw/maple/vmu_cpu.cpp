/*
 * Standalone Sanyo LC86K87/LC8670 CPU core used by the Dreamcast VMU.
 * See vmu_cpu.h for provenance.
 */
#include "vmu_cpu.h"
#include "serialize.h"

namespace vmu
{

void Lc86Cpu::reset()
{
	programCounter = 0;
}

void Lc86Cpu::acceptInterrupt(u16 vector)
{
	pushPc();
	programCounter = vector;
}

void Lc86Cpu::serialize(Serializer& serializer) const
{
	serializer << programCounter;
}

void Lc86Cpu::deserialize(Deserializer& deserializer)
{
	deserializer >> programCounter;
}

u16 Lc86Cpu::indirect(u8 opcode)
{
	const u8 mode = opcode & 3;
	const u8 psw = memory.readData(PSW);
	const u16 pointerAddress = static_cast<u16>(mode | ((psw & 0x18) >> 1));
	return static_cast<u16>(memory.readData(pointerAddress) | ((mode & 2) << 7));
}

u16 Lc86Cpu::absolute12(u8 opcode)
{
	// Address bit 11 is opcode bit 4; bits 10..8 are opcode bits 2..0.
	return static_cast<u16>(((opcode & 0x10) << 7) | ((opcode & 7) << 8) | fetch8());
}

void Lc86Cpu::push(u8 value)
{
	u8 sp = memory.readData(SP) + 1;
	memory.writeData(SP, sp);
	memory.writeData(sp, value);
}

u8 Lc86Cpu::pop()
{
	u8 sp = memory.readData(SP);
	u8 value = memory.readData(sp);
	memory.writeData(SP, sp - 1);
	return value;
}

void Lc86Cpu::pushPc()
{
	push(programCounter & 0xff);
	push(programCounter >> 8);
}

void Lc86Cpu::popPc()
{
	programCounter = static_cast<u16>(pop()) << 8;
	programCounter |= pop();
}

void Lc86Cpu::setFlag(u8 mask, bool set)
{
	u8 psw = memory.readData(PSW);
	memory.writeData(PSW, set ? psw | mask : psw & ~mask);
}

void Lc86Cpu::add(u8 value, bool withCarry)
{
	constexpr u8 Carry = 0x80;
	constexpr u8 AuxCarry = 0x40;
	constexpr u8 Overflow = 0x04;
	const u8 a = memory.readData(ACC);
	const u8 carry = withCarry && (memory.readData(PSW) & Carry) ? 1 : 0;
	const u16 sum = static_cast<u16>(a) + value + carry;
	const u8 result = sum;
	u8 psw = memory.readData(PSW) & ~(Carry | AuxCarry | Overflow);
	if (sum > 0xff)
		psw |= Carry;
	if ((a & 0xf) + (value & 0xf) + carry > 0xf)
		psw |= AuxCarry;
	if ((~(a ^ value) & (a ^ result) & 0x80) != 0)
		psw |= Overflow;
	memory.writeData(ACC, result);
	memory.writeData(PSW, psw);
}

void Lc86Cpu::subtract(u8 value, bool withCarry)
{
	constexpr u8 Carry = 0x80;
	constexpr u8 AuxCarry = 0x40;
	constexpr u8 Overflow = 0x04;
	const u8 a = memory.readData(ACC);
	const u8 carry = withCarry && (memory.readData(PSW) & Carry) ? 1 : 0;
	const u16 subtrahend = static_cast<u16>(value) + carry;
	const u8 result = static_cast<u8>(a - subtrahend);
	u8 psw = memory.readData(PSW) & ~(Carry | AuxCarry | Overflow);
	if (a < subtrahend)
		psw |= Carry;
	if ((a & 0xf) < (value & 0xf) + carry)
		psw |= AuxCarry;
	if (((a ^ value) & (a ^ result) & 0x80) != 0)
		psw |= Overflow;
	memory.writeData(ACC, result);
	memory.writeData(PSW, psw);
}

void Lc86Cpu::compareBranch(u8 lhs, u8 rhs, bool equal, s8 displacement)
{
	setFlag(0x80, lhs < rhs);
	if ((lhs == rhs) == equal)
		programCounter += displacement;
}

Lc86Cpu::StepResult Lc86Cpu::step()
{
	const u8 opcode = fetch8();
	switch (opcode)
	{
	case 0x00: // NOP
		return { 1, true };
	case 0x01: // BR r8; PC already addresses the following instruction.
		programCounter += static_cast<s8>(fetch8());
		return { 2, true };
	case 0x02:
	case 0x03: // LD d9
		memory.writeData(ACC, memory.readData(direct9(opcode)));
		return { 1, true };
	case 0x04:
	case 0x05:
	case 0x06:
	case 0x07: // LD @Ri
		memory.writeData(ACC, memory.readData(indirect(opcode)));
		return { 1, true };
	case 0x10: // CALLR r16 (little-endian displacement, relative to PC - 1)
	{
		const u16 displacement = fetch8() | static_cast<u16>(fetch8()) << 8;
		pushPc();
		programCounter += displacement - 1;
		return { 4, true };
	}
	case 0x11: // BRF r16
	{
		const u16 displacement = fetch8() | static_cast<u16>(fetch8()) << 8;
		programCounter += displacement - 1;
		return { 4, true };
	}
	case 0x12:
	case 0x13: // ST d9
	{
		const u16 address = direct9(opcode);
		memory.writeData(address, memory.readData(ACC));
		return { 1, true };
	}
	case 0x14:
	case 0x15:
	case 0x16:
	case 0x17: // ST @Ri
	{
		const u16 address = indirect(opcode);
		memory.writeData(address, memory.readData(ACC));
		return { 1, true };
	}
	case 0x20: // CALLF a16 (big-endian absolute address)
	{
		const u16 address = static_cast<u16>(fetch8()) << 8 | fetch8();
		pushPc();
		programCounter = address;
		return { 2, true };
	}
	case 0x21: // JMPF a16
		programCounter = static_cast<u16>(fetch8()) << 8 | fetch8();
		memory.switchProgramBank();
		return { 2, true };
	case 0x22:
	case 0x23: // MOV #i8, d9
	{
		const u16 address = direct9(opcode);
		memory.writeData(address, fetch8());
		return { 1, true };
	}
	case 0x24:
	case 0x25:
	case 0x26:
	case 0x27: // MOV #i8, @Ri
	{
		const u16 address = indirect(opcode);
		memory.writeData(address, fetch8());
		return { 1, true };
	}
	case 0x30: // MUL (ACC:C) * B -> B:ACC:C
	{
		const u32 product = (memory.readData(ACC) << 8 | memory.readData(0x103))
				* memory.readData(0x102);
		memory.writeData(0x103, product);
		memory.writeData(ACC, product >> 8);
		memory.writeData(0x102, product >> 16);
		setFlag(0x80, false);
		setFlag(0x04, product > 0xffff);
		return { 7, true };
	}
	case 0x31: // BE #i8, r8
	{
		const u8 value = fetch8();
		const s8 displacement = static_cast<s8>(fetch8());
		compareBranch(memory.readData(ACC), value, true, displacement);
		return { 2, true };
	}
	case 0x40: // DIV (ACC:C) / B -> quotient ACC:C, remainder B
	{
		const u8 divisor = memory.readData(0x102);
		if (divisor == 0)
			memory.writeData(ACC, 0xff);
		else
		{
			const u16 dividend = memory.readData(ACC) << 8 | memory.readData(0x103);
			const u16 quotient = dividend / divisor;
			memory.writeData(0x102, dividend % divisor);
			memory.writeData(0x103, quotient);
			memory.writeData(ACC, quotient >> 8);
		}
		setFlag(0x80, false);
		setFlag(0x04, divisor == 0);
		return { 7, true };
	}
	case 0x41: // BNE #i8, r8
	{
		const u8 value = fetch8();
		const s8 displacement = static_cast<s8>(fetch8());
		compareBranch(memory.readData(ACC), value, false, displacement);
		return { 2, true };
	}
	case 0x50: // LDF
	{
		const u32 address = (static_cast<u32>(memory.readData(FPR) & 1) << 16)
				| memory.readData(TRH) << 8 | memory.readData(TRL);
		memory.writeData(ACC, memory.readFlash(address));
		return { 2, true };
	}
	case 0x51: // STF
	{
		const u16 address = memory.readData(TRH) << 8 | memory.readData(TRL);
		memory.writeFlashInstruction(address, memory.readData(ACC),
				memory.readData(FPR), memory.readData(EXT));
		return { 2, true };
	}
	case 0x52:
	case 0x53: // DBNZ d9, r8
	{
		const u16 address = direct9(opcode);
		const s8 displacement = static_cast<s8>(fetch8());
		const u8 value = memory.readDataLatch(address) - 1;
		memory.writeDataLatch(address, value);
		if (value != 0)
			programCounter += displacement;
		return { 2, true };
	}
	case 0x54:
	case 0x55:
	case 0x56:
	case 0x57: // DBNZ @Ri, r8
	{
		const u16 address = indirect(opcode);
		const s8 displacement = static_cast<s8>(fetch8());
		const u8 value = memory.readDataLatch(address) - 1;
		memory.writeDataLatch(address, value);
		if (value != 0)
			programCounter += displacement;
		return { 2, true };
	}
	case 0x60:
	case 0x61: // PUSH d9
		push(memory.readData(direct9(opcode)));
		return { 2, true };
	case 0x62:
	case 0x63: // INC d9
	{
		const u16 address = direct9(opcode);
		memory.writeDataLatch(address, memory.readDataLatch(address) + 1);
		return { 1, true };
	}
	case 0x64:
	case 0x65:
	case 0x66:
	case 0x67: // INC @Ri
	{
		const u16 address = indirect(opcode);
		memory.writeDataLatch(address, memory.readDataLatch(address) + 1);
		return { 1, true };
	}
	case 0x70:
	case 0x71: // POP d9
		memory.writeData(direct9(opcode), pop());
		return { 2, true };
	case 0x72:
	case 0x73: // DEC d9
	{
		const u16 address = direct9(opcode);
		memory.writeDataLatch(address, memory.readDataLatch(address) - 1);
		return { 1, true };
	}
	case 0x74:
	case 0x75:
	case 0x76:
	case 0x77: // DEC @Ri
	{
		const u16 address = indirect(opcode);
		memory.writeDataLatch(address, memory.readDataLatch(address) - 1);
		return { 1, true };
	}
	case 0x80: // BZ r8
	{
		const s8 displacement = static_cast<s8>(fetch8());
		if (memory.readData(ACC) == 0)
			programCounter += displacement;
		return { 2, true };
	}
	case 0x81: add(fetch8(), false); return { 1, true }; // ADD #i8
	case 0x90: // BNZ r8
	{
		const s8 displacement = static_cast<s8>(fetch8());
		if (memory.readData(ACC) != 0)
			programCounter += displacement;
		return { 2, true };
	}
	case 0x91: add(fetch8(), true); return { 1, true }; // ADDC #i8
	case 0xa0: // RET
		popPc();
		return { 2, true };
	case 0xa1: subtract(fetch8(), false); return { 1, true }; // SUB #i8
	case 0xb1: subtract(fetch8(), true); return { 1, true }; // SUBC #i8
	case 0xb0: // RETI
		popPc();
		memory.returnFromInterrupt();
		return { 2, true };
	case 0xc0: // ROR
	{
		const u8 value = memory.readData(ACC);
		memory.writeData(ACC, value >> 1 | value << 7);
		return { 1, true };
	}
	case 0xc1: // LDC: program byte at TR + ACC
	{
		const u16 address = (memory.readData(0x105) << 8 | memory.readData(0x104))
				+ memory.readData(ACC);
		memory.writeData(ACC, memory.readProgram(address));
		return { 2, true };
	}
	case 0xd0: // RORC
	{
		const u8 value = memory.readData(ACC);
		const u8 carry = memory.readData(PSW) & 0x80;
		setFlag(0x80, value & 1);
		memory.writeData(ACC, value >> 1 | carry);
		return { 1, true };
	}
	case 0xd1: memory.writeData(ACC, memory.readData(ACC) | fetch8()); return { 1, true };
	case 0xe0: // ROL
	{
		const u8 value = memory.readData(ACC);
		memory.writeData(ACC, value << 1 | value >> 7);
		return { 1, true };
	}
	case 0xe1: memory.writeData(ACC, memory.readData(ACC) & fetch8()); return { 1, true };
	case 0xf0: // ROLC
	{
		const u8 value = memory.readData(ACC);
		const u8 carry = memory.readData(PSW) & 0x80;
		setFlag(0x80, value & 0x80);
		memory.writeData(ACC, value << 1 | carry >> 7);
		return { 1, true };
	}
	case 0xf1: memory.writeData(ACC, memory.readData(ACC) ^ fetch8()); return { 1, true };
	default:
		break;
	}

	if ((opcode & 0xe8) == 0x08) // CALL a12: 08..0f, 18..1f
	{
		const u16 address = absolute12(opcode);
		pushPc();
		programCounter = (programCounter & 0xf000) | address;
		return { 2, true };
	}
	if ((opcode & 0xe8) == 0x28) // JMP a12: 28..2f, 38..3f
	{
		const u16 address = absolute12(opcode);
		programCounter = (programCounter & 0xf000) | address;
		return { 2, true };
	}

	// Direct and indirect compare families.
	if ((opcode & 0xfe) == 0x32 || (opcode & 0xfe) == 0x42)
	{
		const u16 address = direct9(opcode);
		const s8 displacement = static_cast<s8>(fetch8());
		compareBranch(memory.readData(ACC), memory.readData(address), (opcode & 0xf0) == 0x30, displacement);
		return { 2, true };
	}
	if ((opcode & 0xfc) == 0x34 || (opcode & 0xfc) == 0x44)
	{
		const u16 address = indirect(opcode);
		const u8 value = fetch8();
		const s8 displacement = static_cast<s8>(fetch8());
		compareBranch(memory.readData(address), value, (opcode & 0xf0) == 0x30, displacement);
		return { 2, true };
	}

	// Bit-addressed instructions encode address bit 8 in opcode bit 4.
	const u8 bitFamily = opcode & 0xe8;
	if (bitFamily == 0x48 || bitFamily == 0x68 || bitFamily == 0x88
			|| bitFamily == 0xa8 || bitFamily == 0xc8 || bitFamily == 0xe8)
	{
		const u8 bit = opcode & 7;
		const u16 address = bitDirect9(opcode);
		const u8 mask = 1 << bit;
		if (bitFamily == 0x48) // BPC
		{
			const s8 displacement = static_cast<s8>(fetch8());
			const u8 value = memory.readDataLatch(address);
			if (value & mask)
			{
				memory.writeDataLatch(address, value & ~mask);
				programCounter += displacement;
			}
			return { 2, true };
		}
		if (bitFamily == 0x68 || bitFamily == 0x88) // BP / BN
		{
			const s8 displacement = static_cast<s8>(fetch8());
			const bool bitSet = (memory.readData(address) & mask) != 0;
			if (bitSet == (bitFamily == 0x68))
				programCounter += displacement;
			return { 2, true };
		}
		u8 value = memory.readDataLatch(address);
		if (bitFamily == 0xa8)
			value ^= mask;
		else if (bitFamily == 0xc8)
			value &= ~mask;
		else
			value |= mask;
		memory.writeDataLatch(address, value);
		return { 1, true };
	}

	// Direct/indirect arithmetic and logical instruction families.
	auto operand = [&]() -> u8 {
		return (opcode & 4) != 0 ? memory.readData(indirect(opcode))
				: memory.readData(direct9(opcode));
	};
	const u8 family = opcode & 0xf0;
	const bool memoryOperand = (opcode & 7) >= 2;
	if ((opcode & 0xf8) == 0x80 && memoryOperand) { add(operand(), false); return { 1, true }; }
	if ((opcode & 0xf8) == 0x90 && memoryOperand) { add(operand(), true); return { 1, true }; }
	if ((opcode & 0xf8) == 0xa0 && memoryOperand) { subtract(operand(), false); return { 1, true }; }
	if ((opcode & 0xf8) == 0xb0 && memoryOperand) { subtract(operand(), true); return { 1, true }; }
	if (((opcode & 0xf8) == 0xd0 || (opcode & 0xf8) == 0xe0 || (opcode & 0xf8) == 0xf0)
			&& memoryOperand)
	{
		const u8 value = operand();
		const u8 acc = memory.readData(ACC);
		memory.writeData(ACC, family == 0xd0 ? acc | value : family == 0xe0 ? acc & value : acc ^ value);
		return { 1, true };
	}
	if ((opcode & 0xfe) == 0xc2 || (opcode & 0xfc) == 0xc4) // XCH d9/@Ri
	{
		const u16 address = (opcode & 4) != 0 ? indirect(opcode) : direct9(opcode);
		const u8 value = memory.readData(address);
		memory.writeData(address, memory.readData(ACC));
		memory.writeData(ACC, value);
		return { 1, true };
	}

	return { 0, false };
}

} // namespace vmu
