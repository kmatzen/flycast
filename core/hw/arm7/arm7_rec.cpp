/*
	Copyright 2020 flyinghead

	This file is part of flycast.

    flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with flycast.  If not, see <https://www.gnu.org/licenses/>.
 */

#include "build.h"

#if	FEAT_AREC != DYNAREC_NONE

#include "arm7_rec.h"
#include "arm7.h"
#include "hw/aica/aica_if.h"
#include "oslib/virtmem.h"
#include "arm_mem.h"

#if 0
// for debug
#include <aarch32/disasm-aarch32.h>
#include <iostream>
#include <sstream>
#endif

namespace aica::arm
{

#define arm_printf(...) DEBUG_LOG(AICA_ARM, __VA_ARGS__)

void (*arm_compilecode)();
arm_mainloop_t arm_mainloop;
extern u8 cpuBitsSet[256];

namespace recompiler {

u8* icPtr;
u8* ICache;
void (*EntryPoints[ARAM_SIZE_MAX / 4])();

DECLARE_CODE_CACHE(ARM7_TCB, ICacheSize)

ptrdiff_t rx_offset;

#pragma pack(push,1)
union ArmOpBits
{
	ArmOpBits(u32 opcode) : full(opcode) {}

    struct {
    	// immediate (str/ldr)
    	u32 imm12:12;
        u32 rd:4;
		// common
		u32 rn : 4;
		// data processing
		u32 set_flags : 1;
		u32 op_type : 4;
		//
		u32 imm_op : 1;
		u32 op_group : 2;
		u32 condition : 4;
	};
    // Register
    struct
    {
        u32 rm:4;
        u32 shift_by_reg:1;
        u32 shift_type:2;
        // Shift by immediate
        u32 shift_imm:5;
        u32 :4;
    };
    // Immediate value
    struct
    {
        u32 imm8:8;
        u32 rotate:4;
        u32 :4;
    };
	struct {
		u32 :7;
		// op2 Shift by reg
		u32 _zero:1;
		u32 shift_reg:4;
		u32 :8;

		// LDR/STR
		u32 load:1;
		u32 write_back:1;
		u32 byte:1;
		u32 up:1;
		u32 pre_index:1;
		u32 :7;
	};
	u32 full;
};
#pragma pack(pop)
static_assert(sizeof(ArmOpBits) == sizeof(u32), "sizeof(ArmOpBits) == sizeof(u32)");

static std::vector<ArmOp> block_ops;

static u32 decodeRotatedImmediate(u32 imm8, u32 rotate)
{
	return rotate == 0 ? imm8 : (imm8 >> rotate) | (imm8 << (32 - rotate));
}

static s32 decodeBranchOffset(u32 opcode)
{
	s64 offset = opcode & 0x00ffffffu;
	if (opcode & 0x00800000u)
		offset -= 0x01000000;
	return (s32)(offset * 4);
}

//findfirstset -- used in LDM/STM handling
#ifdef _MSC_VER
#include <intrin.h>

u32 findfirstset(u32 v)
{
	unsigned long rv;
	_BitScanForward(&rv,v);
	return rv+1;
}
#else
#define findfirstset __builtin_ffs
#endif

static ArmOp decodeArmOp(u32 opcode, u32 arm_pc)
{
	ArmOpBits bits(opcode);

	ArmOp op;
	op.condition = (ArmOp::Condition)bits.condition;
	if (op.condition == ArmOp::UC)
	{
		//NV condition means VFP on newer cores, let interpreter handle it...
		op.op_type = ArmOp::FALLBACK;
		op.arg[0] = ArmOp::Operand(opcode);
		op.cycles = 0;
		return op;
	}
	if (op.condition != ArmOp::AL)
		op.flags |= ArmOp::OP_READS_FLAGS;

	switch (bits.op_group)
	{
	case 0:	// data processing, Multiply, Swap
		// disambiguate
		if ((opcode & 0x01f0f000) == 0x0130f000)
		{
			// ARM7DI gives TEQP special 32-bit-mode semantics: privileged
			// execution restores CPSR from SPSR and User execution is a no-op.
			// Keep that mode-changing compatibility operation on the reference path.
			op.op_type = ArmOp::FALLBACK;
			op.arg[0] = ArmOp::Operand(opcode);
			op.cycles = 0;
			return op;
		}
		if (bits.imm_op == 0 && bits.shift_by_reg && bits._zero != 0)
		{
			// Multiply, swap, and later extra load/store encodings share bits
			// 7 and 4. The reference core classifies the ARM7DI-valid subset.
			op.op_type = ArmOp::FALLBACK;
			op.arg[0] = ArmOp::Operand(opcode);
			op.cycles = 0;
			bool validMultiply = (opcode & 0x0FC000F0) == 0x00000090
					|| (opcode & 0x0FC000F0) == 0x000000D0;
			bool validSwap = (opcode & 0x0FB00FF0) == 0x01000090;
			if (!validMultiply && !validSwap)
				op.flags |= ArmOp::OP_SETS_PC;
			return op;
		}
		if (bits.op_type >= (u32)ArmOp::TST && bits.op_type <= ArmOp::CMN && bits.set_flags == 0)
		{
			// MSR, MRS
			op.spsr = bits.op_type & 2;
			if ((bits.full & 0x0FBF0FFF) == 0x010F0000)
			{
				if (bits.rd == 15)
				{
					// ARM7DI forbids R15 as an MRS destination. Do not let a
					// malformed guest instruction trip a host-side assertion;
					// preserve the interpreter's implementation-defined behavior.
					op.op_type = ArmOp::FALLBACK;
					op.arg[0] = ArmOp::Operand(opcode);
					op.cycles = 0;
					return op;
				}
				op.op_type = ArmOp::MRS;
				op.rd = ArmOp::Operand((Arm7Reg)bits.rd);
			}
			else if ((bits.full & 0x0FBFFFF0) == 0x0129F000)
			{
				op.op_type = ArmOp::MSR;
				op.psrMask = 0xF;
				op.arg[0] = ArmOp::Operand((Arm7Reg)bits.rm);
			}
			else if ((bits.full & 0x0FBFFFF0) == 0x0128F000)
			{
				op.op_type = ArmOp::MSR;
				op.psrMask = 8;
				op.arg[0] = ArmOp::Operand((Arm7Reg)bits.rm);
			}
			else if ((bits.full & 0x0DBFF000) == 0x0128F000)
			{
				op.op_type = ArmOp::MSR;
				op.psrMask = 8;
				u32 rotate = bits.rotate * 2;
				op.arg[0] = ArmOp::Operand(decodeRotatedImmediate(bits.imm8, rotate));
			}
			else
			{
				op.op_type = ArmOp::FALLBACK;
				op.arg[0] = ArmOp::Operand(opcode);
				op.cycles = 0;
				op.flags |= ArmOp::OP_SETS_PC;
			}
			return op;
		}
		{
			op.op_type = (ArmOp::OpType)bits.op_type;
			// Register-controlled shifts use only Rs[7:0] and have several
			// architecturally significant carry cases at 0, 32, and >32.
			// The native backends don't implement those cases consistently.
			// Keep them on the reference path until they can all preserve the
			// exact ARM7 shifter semantics.
			if (!bits.imm_op && bits.shift_by_reg)
			{
				const bool writesPc = !op.isCompOp() && bits.rd == 15;
				op.op_type = ArmOp::FALLBACK;
				op.arg[0] = ArmOp::Operand(opcode);
				op.cycles = 0;
				// interpret() owns condition and NZCV handling. Preserve only the
				// block-ending property needed to keep its PC result intact.
				op.flags = writesPc ? ArmOp::OP_SETS_PC : 0;
				return op;
			}
			if (!op.isCompOp())
				op.rd = ArmOp::Operand((Arm7Reg)bits.rd);
			int argidx = 0;
			if (op.op_type != ArmOp::MOV && op.op_type != ArmOp::MVN)
			{
				op.arg[0] = ArmOp::Operand((Arm7Reg)bits.rn);
				if (op.arg[0].getReg().armreg == RN_PC)
					op.arg[0] = ArmOp::Operand(arm_pc + (!bits.imm_op && bits.shift_by_reg ? 12 : 8));
				argidx++;
			}
			if (bits.set_flags)
				op.flags |= ArmOp::OP_SETS_FLAGS;
			if (bits.imm_op)
			{
				u32 rotate = bits.rotate * 2;
				u32 value = decodeRotatedImmediate(bits.imm8, rotate);
				op.arg[argidx] = ArmOp::Operand(value);
				if (rotate != 0)
				{
					op.immediateCarryValid = true;
					op.immediateCarry = value >> 31;
				}
			}
			else
			{
				op.arg[argidx] = ArmOp::Operand((Arm7Reg)bits.rm);
				op.arg[argidx].shift_type = (ArmOp::ShiftOp)bits.shift_type;
				op.arg[argidx].shift_imm = bits.shift_by_reg == 0;
				if (op.arg[argidx].shift_imm)
				{
					op.arg[argidx].shift_value = bits.shift_imm;
					if (op.arg[argidx].shift_type == ArmOp::RRX && op.arg[argidx].shift_value == 0)
						op.flags |= ArmOp::OP_READS_FLAGS;
				}
				else
				{
					op.arg[argidx].shift_reg = ArmOp::Register((Arm7Reg)bits.shift_reg);
					op.cycles++;
				}
				// Compute pc-relative addresses
				if (op.arg[argidx].getReg().armreg == RN_PC)
				{
					if (op.arg[argidx].shift_imm && !((op.flags & ArmOp::OP_SETS_FLAGS) && op.isLogicalOp()) && op.arg[argidx].shift_type != ArmOp::ROR)
					{

						const u32 next_pc = arm_pc + 8;
						switch (op.arg[argidx].shift_type)
						{
						case ArmOp::LSL:
							op.arg[argidx] = ArmOp::Operand(next_pc << op.arg[argidx].shift_value);
							break;
						case ArmOp::LSR:
							// An immediate zero field encodes LSR #32, not LSR #0.
							op.arg[argidx] = ArmOp::Operand(op.arg[argidx].shift_value == 0
									? 0 : next_pc >> op.arg[argidx].shift_value);
							break;
						case ArmOp::ASR:
							// Likewise, a zero field is ASR #32. PC+8 is positive in
							// ARM7 address space, so its sign-filled result is zero.
							op.arg[argidx] = ArmOp::Operand(op.arg[argidx].shift_value == 0
									? 0 : (u32)((s32)next_pc >> op.arg[argidx].shift_value));
							break;
						default:
							break;
						}
					}
					else
					{
						const bool writesPc = !op.isCompOp() && bits.rd == 15;
						// Keeping R15 as a shifted native operand would require materializing
						// its instruction-relative value, while changing it to an immediate
						// discards the shift metadata. This includes logical flag-setting
						// shifts and all ROR/RRX forms. Execute them on the reference path.
						op.op_type = ArmOp::FALLBACK;
						op.arg = {};
						op.arg[0] = ArmOp::Operand(opcode);
						op.rd = ArmOp::Operand();
						op.cycles = 0;
						// interpret() performs condition evaluation and all flag I/O itself.
						op.flags = writesPc ? ArmOp::OP_SETS_PC : 0;
						return op;
					}
				}
			}
			if (op.rd.isReg() && op.rd.getReg().armreg == RN_PC)
			{
				if (op.op_type == ArmOp::MOV && op.arg[0].isReg() && !bits.set_flags && !op.arg[0].isShifted())
				{
					// MOVcc pc, rn -> B
					op.op_type = ArmOp::B;
					op.flags |= ArmOp::OP_SETS_PC;
					op.rd = ArmOp::Operand();
					op.cycles += 3;
					return op;
				}
				// All other data-processing writes to pc need ARM-state alignment,
				// pipeline-refill timing, and (with S) SPSR restoration. The native
				// ALU paths do not implement that complete combination.
				op.op_type = ArmOp::FALLBACK;
				op.arg[0] = ArmOp::Operand(opcode);
				op.arg[1] = ArmOp::Operand();
				op.arg[2] = ArmOp::Operand();
				op.rd = ArmOp::Operand();
				op.cycles = 0;
				op.flags |= ArmOp::OP_SETS_PC;
			}
			if (op.op_type == ArmOp::ADC || op.op_type == ArmOp::SBC || op.op_type == ArmOp::RSC)
				op.flags |= ArmOp::OP_READS_FLAGS;
		}
		break;

	case 1: // LDR/STR
		{
			if (bits.imm_op && (opcode & 0x10))
			{
				// ARM7DI reserves the 011...1 encoding as Undefined; bit 4
				// must be zero for a single-transfer register offset.
				op.op_type = ArmOp::FALLBACK;
				op.arg[0] = ArmOp::Operand(opcode);
				op.cycles = 0;
				op.flags |= ArmOp::OP_SETS_PC;
				return op;
			}
			op.add_offset = bits.up;
			op.byte_xfer = bits.byte;
			op.pre_index = bits.pre_index;
			// LDR with Rd == Rn and write-back has an architecturally ambiguous
			// collision, so preserve the existing no-write-back choice. STR has no
			// such collision: it stores the old base and then updates Rn.
			op.write_back = (bits.write_back || !bits.pre_index)
					&& (!bits.load || bits.rd != bits.rn)
					&& (bits.imm_op != 0 || bits.imm12 != 0);
			if (bits.load)
			{
				op.op_type = ArmOp::LDR;
				op.rd = ArmOp::Operand((Arm7Reg)bits.rd);
				if (op.rd.getReg().armreg == RN_PC)			// LDR w/ rd=pc
				{
					op.flags |= ArmOp::OP_SETS_PC;
					// The native memory paths don't apply ARM-state PC alignment,
					// and LDR pc also has pipeline-refill timing. Keep all forms on
					// the reference path so both behaviors remain identical.
					op.op_type = ArmOp::FALLBACK;
					op.arg[0] = ArmOp::Operand(opcode);
					op.cycles = 0;
					return op;
				}
				op.cycles += 4;
			}
			else
			{
				op.op_type = ArmOp::STR;
				op.arg[2] = ArmOp::Operand((Arm7Reg)bits.rd);
				if (op.arg[2].getReg().armreg == RN_PC)
					op.arg[2] = ArmOp::Operand(arm_pc + 12);
				op.cycles += 3;
			}
			op.arg[0] = ArmOp::Operand((Arm7Reg)bits.rn);
			if (!bits.load && bits.rd == bits.rn && bits.pre_index && op.write_back)
			{
				// ARM7DI forbids a write-back store whose source is also the base,
				// leaving its result implementation-defined. Preserve the reference
				// core's pre-index ordering (updated base is stored) instead of letting
				// the SSA split choose a different result.
				op.op_type = ArmOp::FALLBACK;
				op.arg[0] = ArmOp::Operand(opcode);
				op.arg[1] = ArmOp::Operand();
				op.arg[2] = ArmOp::Operand();
				op.cycles = 0;
				return op;
			}
			if (op.arg[0].getReg().armreg == RN_PC && op.write_back)
			{
				// LDR/STR w pc-based offset and write-back
				op.flags |= ArmOp::OP_SETS_PC;
				// Invalid: Write-back must not be specified if R15 is used as the base register
				op.op_type = ArmOp::FALLBACK;
				op.arg[0] = ArmOp::Operand(opcode);
				op.arg[1] = ArmOp::Operand();
				op.arg[2] = ArmOp::Operand();
				op.cycles = 0;
				return op;
			}
			if (bits.imm_op == 0)
			{
				// Immediate offset
				if (op.arg[0].getReg().armreg == RN_PC)
					// Compute pc-relative address
					op.arg[0] = ArmOp::Operand(arm_pc + 8 + (op.add_offset ? (int)bits.imm12 : -(int)bits.imm12));
				else
					op.arg[1] = ArmOp::Operand(bits.imm12);
			}
			else
			{
				// Offset by register, optionally shifted
				if (op.arg[0].getReg().armreg == RN_PC)
					op.arg[0] = ArmOp::Operand(arm_pc + 8);
				op.arg[1] = ArmOp::Operand((Arm7Reg)bits.rm);
				op.arg[1].shift_type = (ArmOp::ShiftOp)bits.shift_type;
				op.arg[1].shift_imm = true;
				op.arg[1].shift_value = bits.shift_imm;
				if (op.arg[1].getReg().armreg == RN_PC)
				{
					// Invalid: r15 cannot be used as the offset register
					op.op_type = ArmOp::FALLBACK;
					op.arg[0] = ArmOp::Operand(opcode);
					op.arg[1] = ArmOp::Operand();
					op.arg[2] = ArmOp::Operand();
					op.cycles = 0;
					return op;
				}
				if (op.arg[1].shift_type == ArmOp::RRX && op.arg[1].shift_value == 0)
					op.flags |= ArmOp::OP_READS_FLAGS;
			}
		}
		break;

	case 2:	// LDM/STM and B,BL
		if (bits.imm_op)
		{
			// B, BL
			op.op_type = bits.op_type & 8 ? ArmOp::BL : ArmOp::B;	// L bit
			op.arg[0] = ArmOp::Operand(arm_pc + 8 + decodeBranchOffset(opcode));
			op.flags |= ArmOp::OP_SETS_PC;
			op.cycles += 3;
		}
		else
		{
			// LDM/STM
			u32 reg_list = opcode & 0xffff;
			// one register selected and no PSR
			if (!(opcode & (1 << 22))
					&& cpuBitsSet[reg_list & 255] + cpuBitsSet[(reg_list >> 8) & 255] == 1
					&& (bits.pre_index || bits.write_back)
					&& (!bits.write_back || !(reg_list & (1u << bits.rn))))
			{
				if (opcode & (1 << 20))
				{
					// LDM
					//One register xfered
					//Can be rewriten as normal mem opcode ..
					ArmOpBits newbits(0x04000000 | (opcode & 0xf0000000));

					//Imm offset
					//opcd |= 0<<25;
					//Post incr
					newbits.pre_index = bits.pre_index;
					//Up/Dn
					newbits.up = bits.up;
					//Word/Byte
					//newbits.byte = 0;
					//Write back (post-indexed no-writeback forms use fallback)
					newbits.write_back = bits.write_back;
					//Load
					newbits.load = 1;

					//Rn
					newbits.rn = bits.rn;

					//Rd
					newbits.rd = findfirstset(reg_list) - 1;

					//Offset
					newbits.full |= 4;

					arm_printf("ARM: MEM TFX R %08X -> %08X", opcode, newbits.full);

					op = decodeArmOp(newbits.full, arm_pc);
					return op;
				}
				//STM common case
				else
				{
					ArmOpBits newbits(0x04000000 | (opcode & 0xf0000000));

					//Imm offset
					//opcd |= 0<<25;
					//Post incr
					newbits.pre_index = bits.pre_index;
					//Up/Dn
					newbits.up = bits.up;
					//Word/Byte
					//newbits.byte = 0;
					//Write back (post-indexed no-writeback forms use fallback)
					newbits.write_back = bits.write_back;
					//Load
					newbits.load = 0;

					//Rn
					newbits.rn = bits.rn;

					//Rd
					newbits.rd = findfirstset(reg_list) - 1;

					//Offset
					newbits.full |= 4;

					arm_printf("ARM: MEM TFX W %08X -> %08X", opcode, newbits.full);

					op = decodeArmOp(newbits.full, arm_pc);
					return op;
				}
			}
			op.op_type = ArmOp::FALLBACK;
			op.arg[0] = ArmOp::Operand(opcode);
			op.cycles = 0;

			if ((opcode & 0x8000) && bits.load)	// LDM w/ pc
				op.flags |= ArmOp::OP_SETS_PC;
		}
		break;

	case 3: // coproc, SWI
		op.op_type = ArmOp::FALLBACK;
		op.arg[0] = ArmOp::Operand(opcode);
		op.cycles = 0;
		// AICA has no coprocessor, so both coprocessor instructions and SWI
		// leave sequential execution through an exception.
		op.flags |= ArmOp::OP_SETS_PC;
		break;
	}

	return op;
}

static void block_ssa_pass()
{
	std::array<u32, RN_ARM_REG_COUNT> versions{};
	for (auto it = block_ops.begin(); it != block_ops.end(); it++)
	{
		if (it->op_type == ArmOp::FALLBACK)
			for (auto& v : versions)
				v++;
		else
		{
			if ((it->op_type == ArmOp::STR || it->op_type == ArmOp::LDR) && it->write_back)
			{
				// Extract add/sub operation from STR/LDR
				if (it->op_type == ArmOp::LDR && !it->pre_index && it->arg[1].isReg()
						&& it->arg[1].getReg().armreg == it->rd.getReg().armreg)
				{
					// Edge case where the offset reg is the target register but its value before the op
					// must be used in post-increment/decrement
					// Thus we save its value in a scratch register.
					ArmOp newop(ArmOp::MOV, it->condition);
					newop.rd = ArmOp::Operand(RN_SCRATCH);
					newop.arg[0] = ArmOp::Operand(it->rd);
					// Insert before
					it = block_ops.insert(it, newop);
					it++;
					ArmOp newop2(it->add_offset ? ArmOp::ADD : ArmOp::SUB, it->condition);
					newop2.rd = ArmOp::Operand(it->arg[0]);
					newop2.arg[0] = newop2.rd;
					newop2.arg[1] = it->arg[1];
					newop2.arg[1].setReg(RN_SCRATCH);
					if (it->arg[1].shift_type == ArmOp::RRX && it->arg[1].shift_value == 0)
						newop2.flags |= ArmOp::OP_READS_FLAGS;
					it->flags &= ~ArmOp::OP_READS_FLAGS;
					it->write_back = false;
					it->arg[1] = ArmOp::Operand();
					// Insert after
					it = block_ops.insert(it + 1, newop2);
					it--;
					it--;
				}
				else
				{
					ArmOp newop(it->add_offset ? ArmOp::ADD : ArmOp::SUB, it->condition);
					newop.rd = ArmOp::Operand(it->arg[0]);
					newop.arg[0] = newop.rd;
					newop.arg[1] = it->arg[1];
					if (it->arg[1].shift_type == ArmOp::RRX && it->arg[1].shift_value == 0)
						newop.flags |= ArmOp::OP_READS_FLAGS;
					if (it->condition == ArmOp::AL)
						it->flags &= ~ArmOp::OP_READS_FLAGS;
					it->write_back = false;
					it->arg[1] = ArmOp::Operand();
					if (it->pre_index)
						// Insert before
						it = block_ops.insert(it, newop);
					else
					{
						// Insert after
						it = block_ops.insert(it + 1, newop);
						it--;
					}
				}
			}
			// Set versions
			for (auto& arg : it->arg)
			{
				if (arg.isReg())
					arg.getReg().version = versions[(size_t)arg.getReg().armreg];
				if (!arg.shift_imm)
					arg.shift_reg.version = versions[(size_t)arg.shift_reg.armreg];
			}
			if (it->rd.isReg())
				it->rd.getReg().version = ++versions[(size_t)it->rd.getReg().armreg];
		}
	}
}

void compile()
{
	// The dispatcher is the cycle-budget and FIQ sampling boundary. Keeping a
	// translated block to one architectural instruction prevents execution
	// from running up to 31 instructions past either boundary.
	constexpr u32 MaxBlockInstructions = 1;
	//Get the code ptr
	void* rv = icPtr;

	//setup local pc counter
	u32 pc = arm_Reg[R15_ARM_NEXT].I;

	//update the block table
	// Note that we mask with the max aica size (8 MB), which is
	// also the size of the EntryPoints table. This way the dynarec
	// main loop doesn't have to worry about the actual aica
	// ram size. The aica ram always wraps to 8 MB anyway.
	EntryPoints[(pc & (ARAM_SIZE_MAX - 1)) / 4] = (void (*)())writeToExec(rv);

	block_ops.clear();

	u32 cycles = 0;

	arm_printf("ARM7 Block %x", pc);
	for (u32 ops = 0; ops < MaxBlockInstructions; ops++)
	{
		//Read opcode ...
		u32 opcd = *(u32*)&aica_ram[pc & soundRamMask()];

#if 0
		std::ostringstream ostr;
		vixl::aarch32::Disassembler disassembler(ostr, pc);
		disassembler.DecodeA32(opcd);
		arm_printf("%s", ostr.str().c_str());
#endif

		ArmOp last_op = decodeArmOp(opcd, pc);
		if (last_op.condition != ArmOp::AL && last_op.op_type != ArmOp::FALLBACK)
		{
			// Block cycle charges are unconditional, but ARM instruction-specific
			// cycles are incurred only when the condition passes. Keep conditional
			// instructions on the reference path until cycle subtraction is emitted
			// inside each backend's condition guard.
			const bool msr = last_op.op_type == ArmOp::MSR;
			const u8 flags = last_op.flags;
			last_op.op_type = ArmOp::FALLBACK;
			last_op.arg = {};
			last_op.arg[0] = ArmOp::Operand(opcd);
			last_op.cycles = 0;
			// MSR must still end the block so a newly unmasked FIQ is sampled at
			// the next instruction boundary. OP_SETS_PC provides that boundary;
			// the sequential PC is installed by the conditional-block path below.
			last_op.flags = flags | (msr ? ArmOp::OP_SETS_PC : 0);
		}
		cycles += last_op.cycles;

		//Goto next opcode
		pc += 4;

		// 0 is ANDEQ r0,r0,r0 and is side-effect free in either condition
		// outcome. Do not special-case ARAM's "SEGA" fill word: it is a real
		// conditional encoding and must trap or execute if control reaches it.
		if (opcd != 0)
		{

			if (last_op.op_type == ArmOp::FALLBACK)
			{
				// Interpreter needs pc + 8 in r15
				ArmOp armop(ArmOp::MOV, ArmOp::AL);
				armop.rd = ArmOp::Operand(RN_PC);
				armop.arg[0] = ArmOp::Operand(pc + 4);
				block_ops.push_back(armop);
			}
			//Branch ?
			if (last_op.flags & ArmOp::OP_SETS_PC)
			{
				if (last_op.condition != ArmOp::AL)
				{
					// insert a "mov armNextPC, pc + 4" before the jump if not taken
					ArmOp armop(ArmOp::MOV, ArmOp::AL);
					armop.rd = ArmOp::Operand(R15_ARM_NEXT);
					armop.arg[0] = ArmOp::Operand(pc);
					block_ops.push_back(armop);
				}
				if (last_op.op_type == ArmOp::BL)
				{
					// Save pc+4 into r14
					ArmOp armop(ArmOp::MOV, last_op.condition);
					armop.rd = ArmOp::Operand(RN_LR);
					armop.arg[0] = ArmOp::Operand(pc);
					block_ops.push_back(armop);
				}
				block_ops.push_back(last_op);
				arm_printf("ARM: %06X: Block End %d", pc, ops);
				break;
			}
			block_ops.push_back(last_op);
			if (last_op.op_type == ArmOp::MSR)
			{
				// A CPSR write can unmask a pending FIQ. Return to the dispatcher
				// so it is recognized at the next instruction boundary.
				ArmOp armop(ArmOp::MOV, ArmOp::AL);
				armop.rd = ArmOp::Operand(R15_ARM_NEXT);
				armop.arg[0] = ArmOp::Operand(pc);
				block_ops.push_back(armop);
				break;
			}
		}

		//block size limit ?
		if (ops + 1 == MaxBlockInstructions)
		{
			// Update armNextPC
			ArmOp armop(ArmOp::MOV, ArmOp::AL);
			armop.rd = ArmOp::Operand(R15_ARM_NEXT);
			armop.arg[0] = ArmOp::Operand(pc);
			block_ops.push_back(armop);
			arm_printf("ARM: %06X: Block split", pc);
		}
	}

	block_ssa_pass();

	arm7backend_compile(block_ops, cycles);

	arm_printf("arm7rec_compile done: %p,%p", rv, icPtr);
}

void flush()
{
	icPtr = ICache;
	arm7backend_flush();
	verify(arm_compilecode != nullptr);
	for (u32 i = 0; i < std::size(EntryPoints); i++)
		EntryPoints[i] = arm_compilecode;
}

void init()
{
#ifdef FEAT_NO_RWX_PAGES
	bool rc = virtmem::prepare_jit_block(ARM7_TCB, ICacheSize, (void**)&ICache, &rx_offset);
#else
	bool rc = virtmem::prepare_jit_block(ARM7_TCB, ICacheSize, (void**)&ICache);
#endif
	verify(rc);

	flush();
}

void term()
{
#ifdef FEAT_NO_RWX_PAGES
	if (ICache != nullptr)
		virtmem::release_jit_block(ARM7_TCB, ICache, ICacheSize);
#else
	if (ICache != nullptr && ICache != ARM7_TCB)
		virtmem::release_jit_block(ICache, ICacheSize);
#endif
	ICache = nullptr;
}

template <bool Load, bool Byte>
u32 DYNACALL DoMemOp(u32 addr,u32 data)
{
	u32 rv=0;

	if (Load)
	{
		if (Byte)
			rv = readMem<u8>(addr);
		else
			rv = readMem<u32>(addr);
	}
	else
	{
		if (Byte)
			writeMem<u8>(addr,data);
		else
			writeMem<u32>(addr,data);
	}

	return rv;
}

void *getMemOp(bool Load, bool Byte)
{
	if (Load)
	{
		if (Byte)
			return (void*)(u32(DYNACALL*)(u32,u32))&DoMemOp<true,true>;
		else
			return (void*)(u32(DYNACALL*)(u32,u32))&DoMemOp<true,false>;
	}
	else
	{
		if (Byte)
			return (void*)(u32(DYNACALL*)(u32,u32))&DoMemOp<false,true>;
		else
			return (void*)(u32(DYNACALL*)(u32,u32))&DoMemOp<false,false>;
	}
}

} // namespace recompiler

// Run a timeslice of arm7

void run(u32 samples)
{
	for (u32 i = 0; i < samples; i++)
	{
		if (Arm7Enabled && soundCpuAccessAllowed(CommonData->MRWINH))
		{
			arm_Reg[CYCL_CNT].I += ARM_CYCLES_PER_SAMPLE;
			arm_mainloop(arm_Reg, recompiler::EntryPoints);
		}
		timeStep();
	}
}

void avoidRaceCondition()
{
	arm_Reg[CYCL_CNT].I = std::max((int)arm_Reg[CYCL_CNT].I, 50);
}

} // namespace aica::arm
#endif // FEAT_AREC != DYNAREC_NONE
