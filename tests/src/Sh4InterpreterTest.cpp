/*
	Copyright 2021 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "sh4_ops.h"
#include "emulator.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_opcode_list.h"
#include "hw/sh4/sh4_cycles.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/sh4_cache.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/modules/dmac.h"
#include "hw/holly/sb.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/holly_intc.h"
#include "hw/pvr/pvr_regs.h"
#include "hw/pvr/pvr_mem.h"
#include "hw/pvr/spg.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_if.h"
#include "hw/aica/aica.h"
#include "hw/sh4/dyna/blockmanager.h"

class Sh4InterpreterTest : public Sh4OpTest {
protected:
	void SetUp() override
	{
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		mem_map_default();
		emu.dc_reset(true);
		ctx = &p_sh4rcb->cntx;
		sh4 = Get_Sh4Interpreter();
		sh4->Init();
	}
	void PrepareOp(u16 op, u16 op2 = 0, u16 op3 = 0) override
	{
		ctx->pc = START_PC;
		addrspace::write16(ctx->pc, op);
		if (op2 != 0)
			addrspace::write16(ctx->pc + 2, op2);
		if (op3 != 0)
			addrspace::write16(ctx->pc + 4, op3);
	}
	void RunOp(int numOp = 1) override
	{
		ctx->pc = START_PC;
		for (int i = 0; i < numOp; i++)
			sh4->Step();
	}

	void DelaySlotIllegalInstructionTest()
	{
		// Instructions which modify PC or use PC-relative addressing are
		// forbidden in a delay slot. SLEEP is also explicitly slot-illegal.
		for (u16 op : { u16(0xa000), u16(0x8900), u16(0xc300), u16(0x400e),
				u16(0x9000), u16(0xd000), u16(0xc700), u16(0xfffd), u16(0x001b),
				u16(0x0038) })
			ASSERT_TRUE(OpDesc[op]->IsIllegalDelaySlot(op));
		ASSERT_FALSE(OpDesc[0x0009]->IsIllegalDelaySlot(0x0009));

		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->vbr = 0x8c010000;
		sr().T = 1;
		PrepareOp(0x8f00, 0xc700); // non-taken bf/s followed by mova
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_SlotIllegalInstr);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->pc, ctx->vbr + 0x100);

		ClearRegs();
		ctx->vbr = 0x8c010000;
		PrepareOp(0xa000, 0x001b); // bra 0; sleep
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_SlotIllegalInstr);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->pc, ctx->vbr + 0x100);

		ClearRegs();
		ctx->vbr = 0x8c010000;
		CCN_MMUCR.URC = 0;
		UTLB[0].Data.reg_data = 0x12345678;
		CCN_PTEL.reg_data = 0x87654321;
		PrepareOp(0xa000, 0x0038); // bra 0; ldtlb
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_SlotIllegalInstr);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->pc, ctx->vbr + 0x100);
		ASSERT_EQ(0x12345678u, UTLB[0].Data.reg_data);
	}

	void ConditionalDelaySlotTest()
	{
		// The slot is part of BF/S and BT/S even when the branch is not taken.
		ClearRegs();
		sr().T = 1;
		PrepareOp(0x8f00, 0xe12a); // bf/s 0; mov #42,r1
		RunOp();
		ASSERT_EQ(r(1), 42u);
		ASSERT_EQ(ctx->pc, START_PC + 4);

		ClearRegs();
		sr().T = 0;
		PrepareOp(0x8d00, 0xe22b); // bt/s 0; mov #43,r2
		RunOp();
		ASSERT_EQ(r(2), 43u);
		ASSERT_EQ(ctx->pc, START_PC + 4);

		// The branch condition is sampled before the slot executes.
		ClearRegs();
		sr().T = 0;
		PrepareOp(0x8f02, 0x0018); // bf/s 2; sett
		RunOp();
		ASSERT_EQ(sr().T, 1u);
		ASSERT_EQ(ctx->pc, START_PC + 8);

		ClearRegs();
		sr().T = 1;
		PrepareOp(0x8d02, 0x0008); // bt/s 2; clrt
		RunOp();
		ASSERT_EQ(sr().T, 0u);
		ASSERT_EQ(ctx->pc, START_PC + 8);
	}

	void ConditionalDelaySlotFpuDisabledTest()
	{
		ClearRegs();
		ctx->vbr = 0x8c010000;
		sr().T = 1;
		sr().FD = 1;
		PrepareOp(0x8f00, 0xf04d); // non-taken bf/s followed by fneg fr0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_SlotFpuDisabled);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->pc, ctx->vbr + 0x100);
	}

	void CallDelaySlotPrTest()
	{
		// PR'' is visible to STS in the slot even though the delayed call has
		// not transferred control yet.
		ClearRegs();
		ctx->pr = 0x12345678;
		PrepareOp(0xb000, 0x012a); // bsr 0; sts pr,r1
		RunOp();
		ASSERT_EQ(START_PC + 4, r(1));
		ASSERT_EQ(START_PC + 4, ctx->pr);
		ASSERT_EQ(START_PC + 4, ctx->pc);

		// A non-delayed LDS in the slot supersedes the call's pending PR write.
		ClearRegs();
		r(1) = 0x8c345678;
		PrepareOp(0xb000, 0x412a); // bsr 0; lds r1,pr
		RunOp();
		ASSERT_EQ(0x8c345678u, ctx->pr);
		ASSERT_EQ(START_PC + 4, ctx->pc);

		// A re-execution exception in the slot inhibits the pending PR write.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->pr = 0x87654321;
		r(1) = 0x0c001001;
		PrepareOp(0xb000, 0x6012); // bsr 0; mov.l @r1,r0
		RunOp();
		ASSERT_EQ(Sh4Ex_AddressErrorRead, CCN_EXPEVT);
		ASSERT_EQ(0x87654321u, ctx->pr);
		ASSERT_EQ(START_PC, ctx->spc);
	}

	void DelayedBranchTargetBitTest()
	{
		// Register-derived delayed branch targets always discard bit zero.
		ClearRegs();
		r(1) = 1;
		PrepareOp(0x0123, 0x0009); // braf r1; nop
		RunOp();
		ASSERT_EQ(START_PC + 4, ctx->pc);

		ClearRegs();
		r(1) = 1;
		PrepareOp(0x0103, 0x0009); // bsrf r1; nop
		RunOp();
		ASSERT_EQ(START_PC + 4, ctx->pc);

		ClearRegs();
		r(1) = START_PC + 9;
		PrepareOp(0x412b, 0x0009); // jmp @r1; nop
		RunOp();
		ASSERT_EQ(START_PC + 8, ctx->pc);

		ClearRegs();
		r(1) = START_PC + 9;
		PrepareOp(0x410b, 0x0009); // jsr @r1; nop
		RunOp();
		ASSERT_EQ(START_PC + 8, ctx->pc);

		ClearRegs();
		ctx->pr = START_PC + 9;
		PrepareOp(0x000b, 0x0009); // rts; nop
		RunOp();
		ASSERT_EQ(START_PC + 8, ctx->pc);

		ClearRegs();
		ctx->spc = START_PC + 9;
		ctx->ssr = ctx->sr.status;
		PrepareOp(0x002b, 0x0009); // rte; nop
		RunOp();
		ASSERT_EQ(START_PC + 8, ctx->pc);
	}

	void RteDelaySlotIllegalInstructionTest()
	{
		// RTE restores SR before executing its delay slot. In particular, an
		// RB transition changes which R0-R7 bank the slot observes.
		ClearRegs();
		ctx->old_sr.status = ctx->sr.status;
		ctx->spc = 0x8c020000;
		ctx->ssr = 0; // user mode, RB=0
		ctx->r[0] = 0x11111111;      // active supervisor bank
		ctx->r_bank[0] = 0x22222222; // saved user bank
		ctx->r_bank[1] = REG_MAGIC;
		PrepareOp(0x002b, 0x6103); // rte; mov r0,r1
		RunOp();
		ASSERT_EQ(ctx->pc, 0x8c020000u);
		ASSERT_EQ(ctx->r[0], 0x22222222u);
		ASSERT_EQ(ctx->r[1], 0x22222222u);
		ASSERT_EQ(ctx->r_bank[0], 0x11111111u);

		ClearRegs();
		UpdateSR(); // return the fixture to the supervisor register bank
		ctx->vbr = 0x8c010000;
		ctx->spc = 0x8c020000;
		ctx->ssr = 1u << 30; // supervisor mode after RTE
		sr().MD = 1;
		PrepareOp(0x002b, 0xc700); // rte; mova @(0,pc),r0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_SlotIllegalInstr);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->pc, ctx->vbr + 0x100);

		// The UBC does not accept instruction or operand breaks from an RTE
		// delay slot, before or after the slot instruction executes.
		for (u16 brcr : { u16(0), u16(0x0400) }) {
			ubc.reset(true);
			CCN_BASRA = CCN_BASRB = 0;
			ClearRegs();
			CCN_EXPEVT = 0;
			ctx->spc = 0x8c020000;
			ctx->ssr = sr().getFull();
			UBC_BARA = START_PC + 2;
			UBC_BAMRA = 4;
			UBC_BBRA = 0x14;
			UBC_BRCR = brcr;
			PrepareOp(0x002b, 0xe12a); // rte; mov #42,r1
			RunOp();
			EXPECT_EQ(42u, r(1));
			EXPECT_EQ(0x8c020000u, ctx->pc);
			EXPECT_EQ(0u, CCN_EXPEVT);
			EXPECT_EQ(0u, UBC_BRCR & 0xc000);
		}

		ubc.reset(true);
		CCN_BASRA = CCN_BASRB = 0;
		ClearRegs();
		CCN_EXPEVT = 0;
		ctx->spc = 0x8c020000;
		ctx->ssr = sr().getFull();
		r(1) = 0x0c001000;
		addrspace::write8(r(1), 0x7f);
		UBC_BARA = r(1);
		UBC_BAMRA = 4;
		UBC_BBRA = 0x25; // operand access, read, byte
		PrepareOp(0x002b, 0x6010); // rte; mov.b @r1,r0
		RunOp();
		EXPECT_EQ(0x7fu, r(0));
		EXPECT_EQ(0x8c020000u, ctx->pc);
		EXPECT_EQ(0u, CCN_EXPEVT);
		EXPECT_EQ(0u, UBC_BRCR & 0xc000);
	}

	void PrivilegedInstructionTest()
	{
		constexpr u32 userPc = 0x0c000000;
		for (u16 op : { u16(0x0038), u16(0x002b), u16(0x001b),
				u16(0x0022), u16(0x4023), u16(0x402e), u16(0x4027),
				u16(0x013a), u16(0x01fa), u16(0x4132), u16(0x41f2),
				u16(0x4136), u16(0x41f6), u16(0x413a), u16(0x41fa) })
			ASSERT_TRUE(OpDesc[op]->IsPrivileged(op));
		for (u16 op : { u16(0x0012), u16(0x4013), u16(0x401e), u16(0x4017) })
			ASSERT_FALSE(OpDesc[op]->IsPrivileged(op));

		// GBR is the sole control-register exception and remains accessible in
		// user mode.
		ClearRegs();
		sr().MD = 0;
		gbr() = 0x12345678;
		addrspace::write16(userPc, 0x0112); // stc GBR,r1
		ctx->pc = userPc;
		sh4->Step();
		ASSERT_EQ(r(1), 0x12345678u);

		// Other control-register accesses raise a general illegal-instruction
		// exception before changing architectural state.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		sr().MD = 0;
		addrspace::write16(userPc, 0x0122); // stc VBR,r1
		ctx->pc = userPc;
		sh4->Step();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_IllegalInstr);
		ASSERT_EQ(ctx->spc, userPc);
		ASSERT_EQ(ctx->pc, ctx->vbr + 0x100);
		ASSERT_EQ(r(1), REG_MAGIC);

		// SGR and DBR have special-register encodings, but remain privileged.
		// Reject them before exposing the registers or applying an LDS write.
		for (u16 op : { u16(0x013a), u16(0x01fa), u16(0x413a), u16(0x41fa) })
		{
			ClearRegs();
			ctx->vbr = 0x8c010000;
			sr().MD = 0;
			ctx->sgr = 0x11111111;
			ctx->dbr = 0x22222222;
			r(1) = 0x33333333;
			r(15) = 0x44444444;
			addrspace::write16(userPc, op);
			ctx->pc = userPc;
			sh4->Step();
			ASSERT_EQ(CCN_EXPEVT, Sh4Ex_IllegalInstr);
			ASSERT_EQ(ctx->spc, userPc);
			// Exception entry itself copies R15 to SGR and switches banks.
			ASSERT_EQ(ctx->sgr, 0x44444444u);
			ASSERT_EQ(ctx->dbr, 0x22222222u);
			ASSERT_EQ(ctx->r_bank[1], 0x33333333u);
		}
	}

	void IllegalFpuModeTest()
	{
		ASSERT_TRUE(OpDesc[0xf09d]->IsIllegalFpuMode(0xf09d, true, false));
		ASSERT_TRUE(OpDesc[0xf110]->IsIllegalFpuMode(0xf110, true, false));
		ASSERT_TRUE(OpDesc[0xf14d]->IsIllegalFpuMode(0xf14d, true, false));
		ASSERT_TRUE(OpDesc[0xf15d]->IsIllegalFpuMode(0xf15d, true, false));
		ASSERT_TRUE(OpDesc[0xf2ad]->IsIllegalFpuMode(0xf2ad, false, false));
		ASSERT_TRUE(OpDesc[0xf000]->IsIllegalFpuMode(0xf000, true, true));
		ASSERT_FALSE(OpDesc[0xf00c]->IsIllegalFpuMode(0xf00c, true, true));
		ASSERT_FALSE(OpDesc[0xf000]->IsIllegalFpuMode(0xf000, true, false));
		ASSERT_FALSE(OpDesc[0x406a]->IsIllegalFpuMode(0x406a, true, true));
		ASSERT_FALSE(OpDesc[0xf09d]->IsIllegalFpuMode(0xf09d, false, false));

		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.PR = 1;
		PrepareOp(0xf09d); // fldi1 fr0 is undefined in double-precision mode
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_IllegalInstr);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->pc, ctx->vbr + 0x100);

		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.PR = 1;
		PrepareOp(0xf14d); // fneg with an odd DR encoding is undefined
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_IllegalInstr);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->pc, ctx->vbr + 0x100);

		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.PR = 1;
		PrepareOp(0xa000, 0xf110); // bra; fadd dr1,dr0 (odd DRm)
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_SlotIllegalInstr);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->pc, ctx->vbr + 0x100);
	}

	void FpuCauseLifecycleTest()
	{
		ASSERT_TRUE(OpDesc[0xf000]->ClearsFpuCause(0xf000)); // fadd
		ASSERT_TRUE(OpDesc[0xf03d]->ClearsFpuCause(0xf03d)); // ftrc
		ASSERT_FALSE(OpDesc[0xf04d]->ClearsFpuCause(0xf04d)); // fneg
		ASSERT_FALSE(OpDesc[0xf00c]->ClearsFpuCause(0xf00c)); // fmov
		ASSERT_FALSE(OpDesc[0x406a]->ClearsFpuCause(0x406a)); // lds FPSCR
		ASSERT_FALSE(OpDesc[0xfbfd]->ClearsFpuCause(0xfbfd)); // frchg
		ASSERT_FALSE(OpDesc[0xf3fd]->ClearsFpuCause(0xf3fd)); // fschg

		// Non-operation FPU instructions leave the previous Cause field intact.
		ClearRegs();
		ctx->fpscr.full = 0x3f000;
		fr(0) = 1.f;
		PrepareOp(0xf04d); // fneg fr0
		RunOp();
		ASSERT_EQ(ctx->fpscr.full & 0x3f000u, 0x3f000u);
		PrepareOp(0xf3fd); // fschg
		RunOp();
		ASSERT_EQ(ctx->fpscr.full & 0x3f000u, 0x3f000u);

		// An FPU operation clears Cause before it executes.
		ctx->fpscr.full |= 0x3f000;
		fr(1) = 2.f;
		PrepareOp(0xf010); // fadd fr1,fr0
		RunOp();
		ASSERT_EQ(ctx->fpscr.full & 0x3f000u, 0u);

		// Cause is writable through FPSCR and is not discarded by the load.
		ClearRegs();
		r(1) = 0x0007f000;
		PrepareOp(0x416a); // lds r1,FPSCR
		RunOp();
		ASSERT_EQ(ctx->fpscr.full & 0x3f000u, 0x3f000u);

		// Reserved FPSCR bits ignore writes and always read back as zero.
		ClearRegs();
		r(1) = 0xffc40001;
		PrepareOp(0x416a); // lds r1,FPSCR
		RunOp();
		ASSERT_EQ(ctx->fpscr.full, 0x00040001u);
		PrepareOp(0x026a); // sts FPSCR,r2
		RunOp();
		ASSERT_EQ(r(2), 0x00040001u);

		// An invalid FTRC records Cause.V and sticky Flag.V, then saturates
		// when invalid-operation exceptions are disabled.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		ctx->fpul = 0x12345678;
		ctx->fr_hex(0) = 0x7f800000; // +infinity
		PrepareOp(0xf03d); // ftrc fr0,fpul
		RunOp();
		ASSERT_EQ(ctx->fpul, 0x7fffffffu);
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u); // Cause.V
		ASSERT_NE(ctx->fpscr.full & (1u << 6), 0u);  // Flag.V

		// Enable.V converts the same condition into an FPU exception and the
		// architectural destination remains unchanged.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.einvalidop = 1;
		ctx->fpul = 0x12345678;
		ctx->fr_hex(0) = 0x7fc00000; // SH-4 sNaN (signaling bit is inverted)
		PrepareOp(0xf03d);
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->pc, ctx->vbr + 0x100);
		ASSERT_EQ(ctx->fpul, 0x12345678u);
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 6), 0u);

		// FDIV records division by zero and produces a signed infinity when
		// Enable.Z is clear.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		ctx->fr_hex(0) = 0x3f800000; // 1.0
		ctx->fr_hex(1) = 0x00000000; // 0.0
		PrepareOp(0xf013); // fdiv fr1,fr0
		RunOp();
		ASSERT_EQ(ctx->fr_hex(0), 0x7f800000u);
		ASSERT_NE(ctx->fpscr.full & (1u << 15), 0u); // Cause.Z
		ASSERT_NE(ctx->fpscr.full & (1u << 5), 0u);  // Flag.Z

		// FDIV also has O/U/I possibilities. An enabled possibility traps after
		// an exact division without setting Cause or changing the destination.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.eunderflow = 1;
		ctx->fr_hex(0) = 0x40800000; // 4.0
		ctx->fr_hex(1) = 0x40000000; // 2.0
		PrepareOp(0xf013);
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(0), 0x40800000u);
		ASSERT_EQ(ctx->fpscr.full & 0x7000u, 0u);

		// A finite quotient outside the single-precision range records O and I.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		ctx->fr_hex(0) = 0x7f7fffff;
		ctx->fr_hex(1) = 0x00800000; // smallest normal
		PrepareOp(0xf013);
		RunOp();
		ASSERT_EQ(ctx->fr_hex(0), 0x7f800000u);
		ASSERT_NE(ctx->fpscr.full & (1u << 14), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 12), 0u);

		// Enable.Z traps before changing the destination.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.edivbyzero = 1;
		ctx->fr_hex(0) = 0x3f800000;
		ctx->fr_hex(1) = 0x80000000; // -0.0
		PrepareOp(0xf013);
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(0), 0x3f800000u);
		ASSERT_NE(ctx->fpscr.full & (1u << 15), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 5), 0u);

		// Invalid division emits the SH-4 canonical qNaN, whose quiet/signaling
		// discriminator is opposite to the host convention.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		ctx->fr_hex(0) = 0;
		ctx->fr_hex(1) = 0;
		PrepareOp(0xf013);
		RunOp();
		ASSERT_EQ(ctx->fr_hex(0), 0x7fbfffffu);
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 6), 0u);

		// Basic arithmetic has the same invalid-operation behavior. Opposite
		// infinities cannot be added and produce the SH-4 canonical qNaN.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		ctx->fr_hex(0) = 0x7f800000; // +infinity
		ctx->fr_hex(1) = 0xff800000; // -infinity
		PrepareOp(0xf010); // fadd fr1,fr0
		RunOp();
		ASSERT_EQ(ctx->fr_hex(0), 0x7fbfffffu);
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 6), 0u);

		// Basic arithmetic has O/U/I possibilities regardless of its operands.
		// An enabled possibility traps before writeback with a clear Cause field
		// when the exact operation did not actually generate that condition.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.eoverflow = 1;
		ctx->fr_hex(0) = 0x3f800000; // 1.0
		ctx->fr_hex(1) = 0x40000000; // 2.0
		PrepareOp(0xf010); // fadd fr1,fr0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(0), 0x3f800000u);
		ASSERT_EQ(ctx->fpscr.full & 0x7000u, 0u);

		// Disabled overflow handling writes the default rounded result and marks
		// both overflow and inexact in Cause and sticky Flag.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		ctx->fr_hex(0) = 0x7f7fffff; // largest finite float
		ctx->fr_hex(1) = 0x40000000; // 2.0
		PrepareOp(0xf012); // fmul fr1,fr0
		RunOp();
		ASSERT_EQ(ctx->fr_hex(0), 0x7f800000u);
		ASSERT_NE(ctx->fpscr.full & (1u << 14), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 12), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 4), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 2), 0u);

		// Double arithmetic imports the host operation's IEEE status into FPSCR.
		ClearRegs();
		ctx->fpscr.full = 0x000c0001; // PR=1, DN=1
		ctx->setDR(0, std::numeric_limits<double>::max());
		ctx->setDR(1, 2.0);
		PrepareOp(0xf022); // fmul dr2,dr0
		RunOp();
		ASSERT_TRUE(std::isinf(ctx->getDR(0)));
		ASSERT_NE(ctx->fpscr.full & (1u << 14), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 12), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 4), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 2), 0u);

		// Enable.V suppresses the arithmetic destination write.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.einvalidop = 1;
		ctx->fr_hex(0) = 0x00000000; // zero
		ctx->fr_hex(1) = 0x7f800000; // +infinity
		PrepareOp(0xf012); // fmul fr1,fr0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(0), 0x00000000u);
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 6), 0u);

		// FMAC checks zero-times-infinity as invalid before writing its
		// accumulator. Enabled handling therefore preserves FRn.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.einvalidop = 1;
		ctx->fr_hex(0) = 0x00000000; // zero multiplicand
		ctx->fr_hex(1) = 0x7f800000; // infinite multiplier
		ctx->fr_hex(2) = 0x3f800000; // accumulator
		PrepareOp(0xf21e); // fmac fr0,fr1,fr2
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(2), 0x3f800000u);
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 6), 0u);

		// FMAC has O/U/I possibilities even when its fused result is exact.
		// Enable.U therefore traps with a clear Cause field and preserves FRn.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.eunderflow = 1;
		ctx->fr_hex(0) = 0x3f800000; // 1.0
		ctx->fr_hex(1) = 0x40000000; // 2.0
		ctx->fr_hex(2) = 0x40400000; // 3.0
		PrepareOp(0xf21e);
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(2), 0x40400000u);
		ASSERT_EQ(ctx->fpscr.full & 0x7000u, 0u);

		// FSRRA zero records division by zero and returns signed infinity.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		ctx->fr_hex(0) = 0x80000000; // -0.0
		PrepareOp(0xf07d); // fsrra fr0
		RunOp();
		ASSERT_EQ(ctx->fr_hex(0), 0xff800000u);
		ASSERT_NE(ctx->fpscr.full & (1u << 15), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 5), 0u);

		// A positive normalized input is approximate and always inexact.
		// Enable.I traps before the destination is changed.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.einexact = 1;
		ctx->fr_hex(0) = 0x40800000; // 4.0
		PrepareOp(0xf07d);
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(0), 0x40800000u);
		ASSERT_NE(ctx->fpscr.full & (1u << 12), 0u); // Cause.I
		ASSERT_NE(ctx->fpscr.full & (1u << 2), 0u);  // Flag.I

		// FSCA is also unconditionally approximate. Enabled inexact handling
		// preserves both registers in its destination pair.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.einexact = 1;
		ctx->fpul = 0;
		ctx->fr_hex(0) = 0x12345678;
		ctx->fr_hex(1) = 0x87654321;
		PrepareOp(0xf0fd); // fsca fpul,dr0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(0), 0x12345678u);
		ASSERT_EQ(ctx->fr_hex(1), 0x87654321u);
		ASSERT_NE(ctx->fpscr.full & (1u << 12), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 2), 0u);

		// FCNVSD classifies FPUL using SH-4 NaN encoding. An enabled invalid
		// trap must preserve both words of the destination DR.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x000c0001; // PR=1, DN=1
		ctx->fpscr.einvalidop = 1;
		ctx->fpul = 0x7fc00000; // SH-4 sNaN
		ctx->fr_hex(0) = 0x12345678;
		ctx->fr_hex(1) = 0x87654321;
		PrepareOp(0xf0ad); // fcnvsd fpul,dr0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(0), 0x12345678u);
		ASSERT_EQ(ctx->fr_hex(1), 0x87654321u);
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u);

		// With invalid disabled, qNaN becomes the canonical double qNaN.
		ClearRegs();
		ctx->fpscr.full = 0x000c0001;
		ctx->fpul = 0x7fbfffff; // SH-4 qNaN
		PrepareOp(0xf0ad);
		RunOp();
		ASSERT_EQ(ctx->fr_hex(0), 0x7ff7ffffu);
		ASSERT_EQ(ctx->fr_hex(1), 0xffffffffu);
		ASSERT_EQ(ctx->fpscr.full & (1u << 16), 0u);

		// FCNVDS takes an O/U/I possibility trap whenever any corresponding
		// enable is set, even for an exact conversion whose Cause remains clear.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x000c0001;
		ctx->fpscr.einexact = 1;
		ctx->fpul = 0x12345678;
		ctx->setDR(0, 0.25);
		PrepareOp(0xf0bd); // fcnvds dr0,fpul
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fpul, 0x12345678u);
		ASSERT_EQ(ctx->fpscr.full & 0x7000u, 0u); // no actual O/U/I cause

		// An actual narrowing overflow records both overflow and inexact and,
		// with exceptions disabled, writes the rounded infinity to FPUL.
		ClearRegs();
		ctx->fpscr.full = 0x000c0001;
		ctx->setDR(0, std::numeric_limits<double>::max());
		PrepareOp(0xf0bd);
		RunOp();
		ASSERT_EQ(ctx->fpul, 0x7f800000u);
		ASSERT_NE(ctx->fpscr.full & (1u << 14), 0u); // Cause.O
		ASSERT_NE(ctx->fpscr.full & (1u << 12), 0u); // Cause.I
		ASSERT_NE(ctx->fpscr.full & (1u << 4), 0u);  // Flag.O
		ASSERT_NE(ctx->fpscr.full & (1u << 2), 0u);  // Flag.I

		// FLOAT to single precision has an inexact possibility trap even for
		// exactly representable integers; Cause.I remains clear in that case.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.einexact = 1;
		ctx->fpul = 1;
		ctx->fr_hex(0) = 0x12345678;
		PrepareOp(0xf02d); // float fpul,fr0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(0), 0x12345678u);
		ASSERT_EQ(ctx->fpscr.full & (1u << 12), 0u);

		// With exceptions disabled, a non-representable integer is rounded and
		// records sticky and per-operation inexact state.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		ctx->fpul = 0x01000001; // 2^24 + 1
		PrepareOp(0xf02d);
		RunOp();
		ASSERT_EQ(ctx->fr_hex(0), 0x4b800000u); // 2^24, round-to-nearest-even
		ASSERT_NE(ctx->fpscr.full & (1u << 12), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 2), 0u);

		// Approximate vector operations always record inexact, even for an
		// exactly representable zero inner product.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		for (int i = 0; i < 8; ++i)
			ctx->fr_hex(i) = 0;
		PrepareOp(0xf0ed); // fipr fv0,fv0
		RunOp();
		ASSERT_EQ(ctx->fr_hex(3), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 12), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 2), 0u);

		// FIPR classifies lane-level invalid products and emits canonical qNaN.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		for (int i = 0; i < 8; ++i)
			ctx->fr_hex(i) = 0;
		ctx->fr_hex(4) = 0x7f800000; // 0 * infinity in lane 0
		PrepareOp(0xf1ed); // fipr fv4,fv0
		RunOp();
		ASSERT_EQ(ctx->fr_hex(3), 0x7fbfffffu);
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 6), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 12), 0u);

		// Numeric graphics results aggregate actual overflow across their lanes.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		for (int i = 0; i < 4; ++i)
			ctx->fr_hex(i) = 0x7f7fffff;
		PrepareOp(0xf0ed); // fipr fv0,fv0
		RunOp();
		ASSERT_EQ(ctx->fr_hex(3), 0x7f800000u);
		ASSERT_NE(ctx->fpscr.full & (1u << 14), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 4), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 12), 0u);

		// FTRV conservatively traps for Enable.V/O/U/I before updating any lane.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.einvalidop = 1;
		for (int i = 0; i < 4; ++i)
			ctx->fr_hex(i) = 0x3f800000 + i;
		PrepareOp(0xf1fd); // ftrv xmtrx,fv0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		for (int i = 0; i < 4; ++i)
			ASSERT_EQ(ctx->fr_hex(i), 0x3f800000u + (u32)i);
		ASSERT_EQ(ctx->fpscr.full & (1u << 16), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 12), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 2), 0u);

		// FCMP/EQ treats qNaN as unordered without signaling invalid.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		sr().T = 1;
		ctx->fr_hex(0) = 0x3f800000; // 1.0
		ctx->fr_hex(1) = 0x7fbfffff; // SH-4 qNaN
		PrepareOp(0xf014); // fcmp/eq fr1,fr0
		RunOp();
		ASSERT_EQ(sr().T, 0u);
		ASSERT_EQ(ctx->fpscr.full & (1u << 16), 0u);

		// FCMP/GT treats qNaN as invalid. With Enable.V set, exception
		// delivery suppresses the write to T.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.einvalidop = 1;
		sr().T = 1;
		ctx->fr_hex(0) = 0x3f800000;
		ctx->fr_hex(1) = 0x7fbfffff;
		PrepareOp(0xf015); // fcmp/gt fr1,fr0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ((ctx->ssr >> 0) & 1, 1u); // saved T is unchanged
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 6), 0u);

		// FSQRT of a negative finite value is invalid and produces the SH-4
		// canonical qNaN when Enable.V is clear.
		ClearRegs();
		ctx->fpscr.full = 0x00040001;
		ctx->fr_hex(0) = 0xbf800000; // -1.0
		PrepareOp(0xf06d); // fsqrt fr0
		RunOp();
		ASSERT_EQ(ctx->fr_hex(0), 0x7fbfffffu);
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 6), 0u);

		// Enabled invalid handling leaves the source/destination untouched.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.einvalidop = 1;
		ctx->fr_hex(0) = 0xff800000; // -infinity
		PrepareOp(0xf06d);
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(0), 0xff800000u);
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u);
		ASSERT_NE(ctx->fpscr.full & (1u << 6), 0u);

		// FSQRT has an inexact possibility for every input. Enable.I traps even
		// for an exact square root while leaving Cause.I clear and FRn intact.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00040001;
		ctx->fpscr.einexact = 1;
		ctx->fr_hex(0) = 0x40800000; // 4.0
		PrepareOp(0xf06d);
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(0), 0x40800000u);
		ASSERT_EQ(ctx->fpscr.full & (1u << 12), 0u);

		// With DN clear, a negative denormal is invalid rather than an FPU
		// denormal-input error; the invalid default is canonical qNaN.
		ClearRegs();
		ctx->fpscr.full = 0x00000001;
		ctx->fr_hex(0) = 0x80000001;
		PrepareOp(0xf06d);
		RunOp();
		ASSERT_EQ(ctx->fr_hex(0), 0x7fbfffffu);
		ASSERT_NE(ctx->fpscr.full & (1u << 16), 0u);
		ASSERT_EQ(ctx->fpscr.full & (1u << 17), 0u);

		// With DN clear, a denormalized input raises the unmaskable FPU-error
		// source and also preserves the destination.
		ClearRegs();
		ctx->vbr = 0x8c010000;
		ctx->fpscr.full = 0x00000001; // DN=0
		ctx->fr_hex(0) = 0x00000001; // smallest positive denormal
		PrepareOp(0xf06d);
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_FpuError);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->fr_hex(0), 0x00000001u);
		ASSERT_NE(ctx->fpscr.full & (1u << 17), 0u); // Cause.E
	}

	void DataAlignmentExceptionTest()
	{
		constexpr u32 vbr = 0x8c010000;
		constexpr u32 data = 0x8c020000;

		// Integer loads report the effective address and leave their destination
		// untouched when alignment fails.
		ClearRegs();
		ctx->vbr = vbr;
		r(1) = data + 1;
		PrepareOp(0x6011); // mov.w @r1,r0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_AddressErrorRead);
		ASSERT_EQ(CCN_TEA, data + 1);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->pc, vbr + 0x100);

		// A re-execution exception in a call's delay slot inhibits the call's
		// PR write. This matters in particular to the dynarec, which otherwise
		// could commit PR before executing its faultable slot.
		ClearRegs();
		ctx->vbr = vbr;
		ctx->pr = 0x12345678;
		r(1) = data + 1;
		PrepareOp(0xb000, 0x6011); // bsr 0; mov.w @r1,r0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_AddressErrorRead);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(0x12345678u, ctx->pr);
		ASSERT_EQ(r(0), REG_MAGIC);

		// A rejected store must not perform a partial or rounded-down write.
		ClearRegs();
		ctx->vbr = vbr;
		r(0) = 0x12345678;
		r(1) = data + 2;
		addrspace::write32(data, 0xdeadbeef);
		PrepareOp(0x2102); // mov.l r0,@r1
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_AddressErrorWrite);
		ASSERT_EQ(CCN_TEA, data + 2);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(addrspace::read32(data), 0xdeadbeefu);

		// Pre-decrement is committed only after a successful store.
		ClearRegs();
		ctx->vbr = vbr;
		r(0) = 0x12345678;
		r(1) = data + 6;
		PrepareOp(0x2106); // mov.l r0,@-r1 (effective address data + 2)
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_AddressErrorWrite);
		ASSERT_EQ(CCN_TEA, data + 2);
		ASSERT_EQ(r(1), data + 6);
		ASSERT_EQ(addrspace::read32(data), 0xdeadbeefu);

		// An address error in a delay slot is attributed to the branch.
		ClearRegs();
		ctx->vbr = vbr;
		r(1) = data + 1;
		PrepareOp(0xa000, 0x6011); // bra 0; mov.w @r1,r0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_AddressErrorRead);
		ASSERT_EQ(CCN_TEA, data + 1);
		ASSERT_EQ(ctx->spc, START_PC);
		ASSERT_EQ(ctx->pc, vbr + 0x100);

		// FPU memory transfers obey the same checks. When SZ selects a
		// quadword pair, the operand must be eight-byte aligned.
		ClearRegs();
		ctx->vbr = vbr;
		ctx->fpscr.full = 0;
		r(1) = data + 2;
		PrepareOp(0xf018); // fmov.s @r1,fr0
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_AddressErrorRead);
		ASSERT_EQ(CCN_TEA, data + 2);
		ASSERT_EQ(ctx->spc, START_PC);

		ClearRegs();
		ctx->vbr = vbr;
		ctx->fpscr.full = (1u << 19) | (1u << 20); // PR=1, SZ=1
		r(1) = data + 4;
		PrepareOp(0xf018); // fmov @r1,dr0 ignores PR and follows SZ
		RunOp();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_AddressErrorRead);
		ASSERT_EQ(CCN_TEA, data + 4);
		ASSERT_EQ(ctx->spc, START_PC);

		ClearRegs();
		ctx->vbr = vbr;
		ctx->fpscr.full = (1u << 19) | (1u << 20); // PR=1, SZ=1
		r(1) = data + 8;
		addrspace::write64(data + 8, 0x0123456789abcdefULL);
		PrepareOp(0xf018);
		RunOp();
		ASSERT_EQ(ctx->dr_hex(0), 0x0123456789abcdefULL);
	}

	void AddressSpaceProtectionTest()
	{
		constexpr u32 userPc = 0x0c000000;
		constexpr u32 vbr = 0x8c010000;
		constexpr u32 kernelData = 0x8c020000;

		// With translation disabled, user mode is still restricted to U0.
		ClearRegs();
		ctx->vbr = vbr;
		sr().MD = 0;
		ctx->pc = START_PC;
		sh4->Step();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_AddressErrorRead);
		ASSERT_EQ(CCN_TEA, START_PC);
		ASSERT_EQ(CCN_PTEH.VPN, START_PC >> 10);
		ASSERT_EQ(ctx->spc, START_PC);

		// P4 is not executable even in privileged mode.
		ClearRegs();
		ctx->vbr = vbr;
		ctx->pc = 0xe4000000;
		sh4->Step();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_AddressErrorRead);
		ASSERT_EQ(CCN_TEA, 0xe4000000u);
		ASSERT_EQ(ctx->spc, 0xe4000000u);

		// Operand accesses use the same user/privileged boundary, including
		// byte operations which have no alignment requirement.
		ClearRegs();
		ctx->vbr = vbr;
		sr().MD = 0;
		r(1) = kernelData;
		addrspace::write16(userPc, 0x6010); // mov.b @r1,r0
		ctx->pc = userPc;
		sh4->Step();
		ASSERT_EQ(CCN_EXPEVT, Sh4Ex_AddressErrorRead);
		ASSERT_EQ(CCN_TEA, kernelData);
		ASSERT_EQ(CCN_PTEH.VPN, kernelData >> 10);
		ASSERT_EQ(ctx->spc, userPc);
		ASSERT_EQ(r(0), REG_MAGIC);

		// User store-queue writes are the sole P1-P4 exception when SQMD=0.
		sr().MD = 0;
		ASSERT_TRUE(sh4DataAddressError(0x7ffffffc, 8, false));
		CCN_MMUCR.SQMD = 0;
		ASSERT_FALSE(sh4DataAddressError(0xe0000000, 4, true));
		ASSERT_TRUE(sh4DataAddressError(0xe0000000, 4, false));
		CCN_MMUCR.SQMD = 1;
		ASSERT_TRUE(sh4DataAddressError(0xe0000000, 4, true));
		CCN_MMUCR.SQMD = 0;
	}

	void InterruptIsAcceptedAfterAssertingInstruction()
	{
		ClearRegs();
		ctx->vbr = 0x8c010000;
		// A priority-zero source is pending but disabled. This store raises
		// its priority; let the source reach the INTC first, then verify the
		// priority-changing store retires before the interrupt is taken.
		SetInterruptPend(sh4_TMU0_TUNI0);
		ctx->sh4_sched_next -= sh4InterruptDecisionCycles(Sh4Ex_TMU0);
		sh4_sched_tick(sh4InterruptDecisionCycles(Sh4Ex_TMU0));
		r(0) = 0xf000;
		r(1) = 0xffd00004; // INTC.IPRA
		PrepareOp(0x2101); // mov.w r0,@r1

		RunOp();

		EXPECT_EQ(Sh4Ex_TMU0, CCN_INTEVT);
		EXPECT_EQ(START_PC + 2, ctx->spc);
		EXPECT_EQ(ctx->vbr + 0x600, ctx->pc);
	}

	void TrapUsesCommonExceptionExit()
	{
		ClearRegs();
		ctx->vbr = 0x8c010000;
		PrepareOp(0xc32a); // trapa #0x2a

		RunOp();

		EXPECT_EQ(Sh4Ex_Trap, CCN_EXPEVT);
		EXPECT_EQ(0xa8u, CCN_TRA);
		EXPECT_EQ(START_PC + 2, ctx->spc);
		EXPECT_EQ(ctx->vbr + 0x100, ctx->pc);
	}

	void CacheOperationTranslationExceptions()
	{
		ClearRegs();
		constexpr u32 address = 0x02000000;
		CCN_MMUCR.AT = 1;
		MMU_reset();

		// A miss is deliberately suppressed by PREF.
		EXPECT_NO_THROW(ocache.Prefetch(address));

		// Two matching valid entries are the one translation failure PREF must
		// report architecturally.
		for (u32 i = 0; i < 2; ++i) {
			UTLB[i].Address.VPN = address >> 10;
			UTLB[i].Data.PPN = (0x0c000000 + i * 0x1000) >> 10;
			UTLB[i].Data.SZ0 = 1; // 4 KiB
			UTLB[i].Data.V = 1;
			UTLB[i].Data.PR = 3;
			UTLB[i].Data.D = 1;
			UTLB_Sync(i);
		}
		ctx->pc = START_PC + 2;
		try {
			ocache.Prefetch(address);
			FAIL() << "PREF suppressed a TLB multiple-hit exception";
		}
		catch (const SH4ThrownException& ex) {
			EXPECT_EQ(Sh4Ex_TlbMultiHit, ex.expEvn);
			EXPECT_EQ(START_PC, ex.epc);
			EXPECT_EQ(address, CCN_TEA);
		}

		// OCBP and OCBWB translate as reads, while OCBI translates as a write
		// and therefore observes the dirty bit's initial-page-write rule.
		MMU_reset();
		UTLB[0].Address.VPN = address >> 10;
		UTLB[0].Data.PPN = 0x0c000000 >> 10;
		UTLB[0].Data.SZ0 = 1;
		UTLB[0].Data.V = 1;
		UTLB[0].Data.PR = 3;
		UTLB[0].Data.D = 0;
		UTLB_Sync(0);
		EXPECT_NO_THROW(ocache.WriteBack(address, true, true)); // OCBP
		EXPECT_NO_THROW(ocache.WriteBack(address, true, false)); // OCBWB
		try {
			ocache.WriteBack(address, false, true); // OCBI
			FAIL() << "OCBI ignored the initial-page-write exception";
		}
		catch (const SH4ThrownException& ex) {
			EXPECT_EQ(Sh4Ex_TlbInitPageWrite, ex.expEvn);
			EXPECT_EQ(START_PC, ex.epc);
			EXPECT_EQ(address, CCN_TEA);
		}

		// The opcode path must retain PREF's multiple-hit exception even in the
		// normal fast build where cache contents themselves are not modeled.
		MMU_reset();
		for (u32 i = 0; i < 2; ++i) {
			UTLB[i].Address.VPN = address >> 10;
			UTLB[i].Data.PPN = (0x0c000000 + i * 0x1000) >> 10;
			UTLB[i].Data.SZ0 = 1;
			UTLB[i].Data.V = 1;
			UTLB[i].Data.PR = 3;
			UTLB[i].Data.D = 1;
			UTLB_Sync(i);
		}
		r(1) = address;
		PrepareOp(0x0183); // pref @r1
		RunOp();
		EXPECT_EQ(Sh4Ex_TlbMultiHit, CCN_EXPEVT);
		EXPECT_EQ(address, CCN_TEA);
		EXPECT_EQ(0xa0000000u, ctx->pc);

		CCN_MMUCR.AT = 0;
		MMU_reset();
		SetMemoryHandlers();

		ClearRegs();
		constexpr u32 movcaAddress = 0x0c003000;
		r(0) = 0x89abcdef;
		r(1) = movcaAddress;
		addrspace::write32(movcaAddress, 0);
		PrepareOp(0x01c3); // movca.l r0,@r1
		RunOp();
		EXPECT_EQ(0x89abcdefu, addrspace::read32(movcaAddress));
	}

	void MovcaAllocationDoesNotFetch()
	{
#ifdef STRICT_MODE
		ClearRegs();
		constexpr u32 lineAddress = 0x0c004000;
		constexpr u32 oldLineWord = 0x11223344;
		constexpr u32 memoryWord = 0xaabbccdd;
		constexpr u32 movcaWord = 0x55667788;
		const u32 savedCcr = CCN_CCR.reg_data;
		CCN_MMUCR.AT = 0;
		CCN_CCR.reg_data = 0;
		CCN_CCR.OCE = 1;
		CCN_CCR.WT = 0; // copy-back
		ocache.Reset(true);
		ocache.WriteDataArray(lineAddress, oldLineWord);
		addrspace::write32(lineAddress, memoryWord);

		ocache.Movca(lineAddress + 4, movcaWord);

		EXPECT_EQ(3u, ocache.ReadAddressArray(lineAddress) & 3); // valid, dirty
		EXPECT_EQ(oldLineWord, ocache.ReadDataArray(lineAddress));
		EXPECT_EQ(movcaWord, ocache.ReadDataArray(lineAddress + 4));
		EXPECT_EQ(memoryWord, addrspace::read32(lineAddress));
		CCN_CCR.reg_data = savedCcr;
		ocache.Reset(true);
#endif
	}

	void InterruptEntryLatencyTest()
	{
		EXPECT_EQ(15, sh4InterruptDecisionCycles(Sh4Ex_ExtInterrupt9));
		EXPECT_EQ(15, sh4InterruptDecisionCycles(Sh4Ex_ExtInterruptB));
		EXPECT_EQ(15, sh4InterruptDecisionCycles(Sh4Ex_ExtInterruptD));
		EXPECT_EQ(5, sh4InterruptDecisionCycles(Sh4Ex_TMU0));
		EXPECT_EQ(5, sh4InterruptDecisionCycles(Sh4Ex_SCIF_RXI));
		EXPECT_EQ(4, sh4InterruptEntryCycles(Sh4Ex_ExtInterrupt9));
		EXPECT_EQ(4, sh4InterruptEntryCycles(Sh4Ex_TMU0));
		EXPECT_EQ(4, sh4ExceptionEntryCycles(Sh4Ex_IllegalInstr));
		EXPECT_EQ(4, sh4ExceptionEntryCycles(Sh4Ex_TlbMissRead));

		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->pc = START_PC;
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		Do_Exception(START_PC, Sh4Ex_IllegalInstr);
		EXPECT_EQ(96, ctx->cycle_counter);
		EXPECT_EQ(ctx->vbr + 0x100, ctx->pc);

		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->pc = START_PC;
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		sr().IMASK = 0;
		INTC_IPRA.reg_data = 0xf000;
		SIIDRebuild();
		SetInterruptPend(sh4_TMU0_TUNI0);
		EXPECT_EQ(0u, ctx->interrupt_pend);
		ctx->sh4_sched_next -= 4;
		sh4_sched_tick(4);
		EXPECT_EQ(0u, ctx->interrupt_pend);
		ctx->sh4_sched_next -= 1;
		sh4_sched_tick(1);
		ASSERT_NE(0u, ctx->interrupt_pend);
		UpdateINTC();
		EXPECT_EQ(96, ctx->cycle_counter);
		EXPECT_EQ(ctx->vbr + 0x600, ctx->pc);

		// Sleep/standby is the exception to SR.BL interrupt inhibition. The
		// ordinary pending cache remains empty, but a visible, unmasked request
		// must still be accepted to wake the processor.
		ResetInterruptPend(sh4_TMU0_TUNI0);
		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->pc = START_PC + 2; // SLEEP has completed when the interrupt is taken
		ctx->vbr = 0x8c010000;
		sr().BL = 1;
		sr().IMASK = 15;
		SRdecode();
		SetInterruptPend(sh4_TMU0_TUNI0);
		ctx->sh4_sched_next -= 5;
		sh4_sched_tick(5);
		ASSERT_EQ(0u, ctx->interrupt_pend);
		ASSERT_FALSE(UpdateINTC_Sleep()); // sleep does not bypass SR.IMASK
		sr().IMASK = 0;
		SRdecode();
		ASSERT_EQ(0u, ctx->interrupt_pend); // BL still filters normal execution
		ASSERT_TRUE(UpdateINTC_Sleep());
		EXPECT_EQ(96, ctx->cycle_counter);
		EXPECT_EQ(START_PC + 2, ctx->spc);
		EXPECT_EQ(ctx->vbr + 0x600, ctx->pc);
		ResetInterruptPend(sh4_TMU0_TUNI0);
	}

	void NmiInterruptTest()
	{
		auto tick = [](int cycles) {
			Sh4cntx.sh4_sched_next -= cycles;
			sh4_sched_tick(cycles);
		};

		EXPECT_EQ(9, sh4InterruptDecisionCycles(Sh4Ex_NmiInterrupt));
		interrupts_reset();
		intc.write<u16>(INTC_ICR_addr, 0); // falling-edge NMI
		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->pc = START_PC;
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		sr().IMASK = 15; // NMI is not filtered by IMASK
		SRdecode();
		DMAC_DMAOR.full = 1;
		ASSERT_EQ(1u, INTC_ICR.NMIL);

		SetNmiLevel(false);
		EXPECT_EQ(0u, INTC_ICR.NMIL);
		EXPECT_EQ(1u, DMAC_DMAOR.NMIF);
		EXPECT_EQ(0u, ctx->interrupt_pend);
		tick(8);
		EXPECT_EQ(0u, ctx->interrupt_pend);
		tick(1);
		ASSERT_NE(0u, ctx->interrupt_pend);
		ASSERT_TRUE(UpdateINTC());
		EXPECT_EQ(Sh4Ex_NmiInterrupt, CCN_INTEVT);
		EXPECT_EQ(START_PC, ctx->spc);
		EXPECT_EQ(ctx->vbr + 0x600, ctx->pc);
		EXPECT_EQ(96, ctx->cycle_counter);

		// BL normally holds a recognized NMI. NMIB releases it and is cleared
		// automatically when the NMI is accepted. NMIL remains read-only.
		interrupts_reset();
		intc.write<u16>(INTC_ICR_addr, 0);
		ClearRegs();
		ctx->pc = START_PC;
		ctx->vbr = 0x8c010000;
		sr().BL = 1;
		SRdecode();
		SetNmiLevel(false);
		tick(9);
		EXPECT_EQ(0u, ctx->interrupt_pend);
		EXPECT_FALSE(UpdateINTC());
		intc.write<u16>(INTC_ICR_addr, 0x8200); // NMIB plus attempted NMIL write
		EXPECT_EQ(0u, INTC_ICR.NMIL);
		ASSERT_NE(0u, ctx->interrupt_pend);
		ASSERT_TRUE(UpdateINTC());
		EXPECT_EQ(0u, INTC_ICR.NMIB);

		// Sleep/standby accepts NMI regardless of BL and NMIB.
		interrupts_reset();
		intc.write<u16>(INTC_ICR_addr, 0);
		ClearRegs();
		ctx->pc = START_PC + 2;
		ctx->vbr = 0x8c010000;
		sr().BL = 1;
		SRdecode();
		SetNmiLevel(false);
		tick(9);
		EXPECT_EQ(0u, ctx->interrupt_pend);
		ASSERT_TRUE(UpdateINTC_Sleep());
		EXPECT_EQ(START_PC + 2, ctx->spc);
		EXPECT_EQ(ctx->vbr + 0x600, ctx->pc);

		// MAI masks ordinary interrupts while the NMI pin is low. Selecting
		// rising-edge detection lets this test lower the pin without creating
		// another NMI request.
		interrupts_reset();
		intc.write<u16>(INTC_ICR_addr, 0x0100); // NMIE=1
		ClearRegs();
		sr().BL = 0;
		sr().IMASK = 0;
		INTC_IPRA.reg_data = 0xf000;
		SIIDRebuild();
		SetInterruptPend(sh4_TMU0_TUNI0);
		tick(5);
		ASSERT_NE(0u, ctx->interrupt_pend);
		SetNmiLevel(false);
		intc.write<u16>(INTC_ICR_addr, 0x4100); // MAI=1, NMIE=1
		EXPECT_EQ(0u, ctx->interrupt_pend);
		intc.write<u16>(INTC_ICR_addr, 0x0100);
		EXPECT_NE(0u, ctx->interrupt_pend);
		ResetInterruptPend(sh4_TMU0_TUNI0);
		intc.write<u16>(INTC_ICR_addr, 0); // avoid a rising-edge request on cleanup
		SetNmiLevel(true);
	}

	void UserBreakDebugVectorTest()
	{
		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->vbr = 0x8c010000;
		ctx->dbr = 0x8c020000;
		ctx->r[15] = 0x8c00ff00;
		sr().BL = 1; // user break remains accepted while exceptions are blocked
		sr().MD = 0;
		sr().RB = 0;
		UBC_BRCR = 0;
		Do_Exception(START_PC, Sh4Ex_UserBreak);
		EXPECT_EQ(Sh4Ex_UserBreak, CCN_EXPEVT);
		EXPECT_EQ(START_PC, ctx->spc);
		EXPECT_EQ(0x8c00ff00u, ctx->sgr);
		EXPECT_EQ(ctx->vbr + 0x100, ctx->pc);
		EXPECT_EQ(96, ctx->cycle_counter);

		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->vbr = 0x8c010000;
		ctx->dbr = 0x8c020002;
		ctx->r[15] = 0x8c00fe00;
		sr().BL = 1;
		sr().MD = 0;
		sr().RB = 0;
		const u32 savedSr = sr().getFull();
		UBC_BRCR = 1; // UBDE
		Do_Exception(START_PC + 2, Sh4Ex_UserBreak);
		EXPECT_EQ(savedSr, ctx->ssr);
		EXPECT_EQ(START_PC + 2, ctx->spc);
		EXPECT_EQ(0x8c00fe00u, ctx->sgr);
		EXPECT_EQ(0x8c020002u, ctx->pc);
		EXPECT_EQ(1u, sr().BL);
		EXPECT_EQ(1u, sr().MD);
		EXPECT_EQ(1u, sr().RB);
		EXPECT_EQ(96, ctx->cycle_counter);
		UBC_BRCR = 0;
	}

	void UbcInstructionBreakTest()
	{
		auto clearUbc = [] {
			ubc.reset(true);
			CCN_BASRA = CCN_BASRB = 0;
		};

		// Channel A pre-execution break: the matched instruction does not retire.
		clearUbc();
		ClearRegs();
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		UBC_BARA = START_PC;
		UBC_BAMRA = 4; // exclude ASID from the condition
		UBC_BBRA = 0x14; // instruction access, read, any size
		PrepareOp(0xe12a); // mov #42,r1
		RunOp();
		EXPECT_EQ(0u, r(1));
		EXPECT_EQ(Sh4Ex_UserBreak, CCN_EXPEVT);
		EXPECT_EQ(START_PC, ctx->spc);
		EXPECT_EQ(ctx->vbr + 0x100, ctx->pc);
		EXPECT_NE(0u, UBC_BRCR & 0x8000); // CMFA

		// The virtual-PC pre-check precedes instruction translation, so a match
		// wins over the fetch TLB miss that would otherwise occur.
		clearUbc();
		ClearRegs();
		constexpr u32 missingInstruction = 0x02000000;
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		CCN_MMUCR.AT = 1;
		MMU_reset();
		UBC_BARA = missingInstruction;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x16;
		ctx->pc = missingInstruction;
		sh4->Step();
		EXPECT_EQ(Sh4Ex_UserBreak, CCN_EXPEVT);
		EXPECT_EQ(missingInstruction, ctx->spc);
		CCN_MMUCR.AT = 0;
		MMU_reset();
		SetMemoryHandlers();

		// A pre-break on a normal delay slot is taken before the delayed branch
		// itself issues. Its SPC therefore identifies the branch instruction.
		clearUbc();
		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		ctx->pr = 0x12345678;
		UBC_BARA = START_PC + 2;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x16;
		PrepareOp(0xb001, 0xe12a); // bsr START_PC+6; mov #42,r1
		EXPECT_EQ(ctx->vbr + 0x100,
				Sh4Interpreter::ExecuteUbcFallback(ctx, 0xb001, START_PC));
		EXPECT_EQ(0u, r(1));
		EXPECT_EQ(0x12345678u, ctx->pr);
		EXPECT_EQ(START_PC, ctx->spc);
		EXPECT_EQ(96, ctx->cycle_counter);

		// A precondition still sets its match flag while BL masks acceptance of
		// the user-break exception.
		clearUbc();
		ClearRegs();
		sr().BL = 1;
		UBC_BARA = START_PC;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x16;
		PrepareOp(0xe12a);
		RunOp();
		EXPECT_EQ(42u, r(1));
		EXPECT_EQ(START_PC + 2, ctx->pc);
		EXPECT_NE(0u, UBC_BRCR & 0x8000);

		// PCBA selects a post-execution break and SPC is the next instruction.
		clearUbc();
		ClearRegs();
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		UBC_BARA = START_PC;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x16; // word-sized instruction read
		UBC_BRCR = 0x0400; // PCBA: break after execution
		PrepareOp(0xe12a);
		RunOp();
		EXPECT_EQ(42u, r(1));
		EXPECT_EQ(START_PC + 2, ctx->spc);
		EXPECT_NE(0u, UBC_BRCR & 0x8000);

		// Post-execution acceptance uses BL-before OR BL-after. An instruction
		// that raises BL therefore completes and latches CMFA without trapping.
		clearUbc();
		ClearRegs();
		sr().MD = 1;
		sr().BL = 0;
		r(0) = sr().getFull() | 0x10000000;
		UBC_BARA = START_PC;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x16;
		UBC_BRCR = 0x0400;
		PrepareOp(0x400e); // ldc r0,sr
		RunOp();
		EXPECT_EQ(1u, sr().BL);
		EXPECT_EQ(START_PC + 2, ctx->pc);
		EXPECT_NE(0u, UBC_BRCR & 0x8000);

		// A post break in a delay slot is delivered only after the delayed
		// branch resolves, so SPC contains the branch destination.
		clearUbc();
		ClearRegs();
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		UBC_BARA = START_PC + 2;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x14;
		UBC_BRCR = 0x0400;
		PrepareOp(0xa001, 0xe12a); // bra START_PC+6; mov #42,r1
		RunOp();
		EXPECT_EQ(42u, r(1));
		EXPECT_EQ(START_PC + 6, ctx->spc);
		EXPECT_EQ(ctx->vbr + 0x100, ctx->pc);

		// A synchronous exception has priority over a competing post break,
		// while the channel match flag is still latched.
		clearUbc();
		ClearRegs();
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		UBC_BARA = START_PC;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x14;
		UBC_BRCR = 0x0400;
		PrepareOp(0xc300); // trapa #0
		RunOp();
		EXPECT_EQ(Sh4Ex_Trap, CCN_EXPEVT);
		EXPECT_NE(0u, UBC_BRCR & 0x8000);

		// Address page masks and ASID matching are independent conditions.
		clearUbc();
		UBC_BARA = START_PC;
		UBC_BAMRA = 5; // lower 10 address bits masked, ASID masked
		UBC_BBRA = 0x14;
		EXPECT_FALSE(UBC_InstructionBreak(START_PC + 0x3fe, true));
		EXPECT_TRUE(UBC_InstructionBreak(START_PC + 0x3fe, false));
		UBC_BRCR = 0;
		EXPECT_FALSE(UBC_InstructionBreak(START_PC + 0x400, false));
		UBC_BAMRA = 0;
		CCN_PTEH.ASID = 0x34;
		CCN_BASRA = 0x12;
		EXPECT_FALSE(UBC_InstructionBreak(START_PC, false));
		CCN_BASRA = 0x34;
		EXPECT_TRUE(UBC_InstructionBreak(START_PC, false));

		// SEQ converts channel A into an arm condition; only a later B match
		// requests the exception.
		clearUbc();
		UBC_BARA = START_PC;
		UBC_BARB = START_PC + 2;
		UBC_BAMRA = UBC_BAMRB = 4;
		UBC_BBRA = UBC_BBRB = 0x14;
		UBC_BRCR = 8;
		EXPECT_FALSE(UBC_InstructionBreak(START_PC, false));
		EXPECT_EQ(0u, UBC_BRCR & 0x8000);
		EXPECT_TRUE(UBC_SequenceArmed());
		EXPECT_TRUE(UBC_InstructionBreak(START_PC + 2, false));
		EXPECT_NE(0u, UBC_BRCR & 0x4000);
		EXPECT_FALSE(UBC_SequenceArmed());
		EXPECT_FALSE(UBC_InstructionBreak(START_PC + 2, false));

		// STBCR2.MSTP5 stops the UBC clock. Conditions and the sequential latch
		// are retained, but no instruction or operand bus cycle is observed until
		// the clock is restarted.
		clearUbc();
		UBC_BARA = START_PC;
		UBC_BARB = START_PC + 2;
		UBC_BAMRA = UBC_BAMRB = 4;
		UBC_BBRA = UBC_BBRB = 0x34;
		UBC_BRCR = 8;
		EXPECT_FALSE(UBC_InstructionBreak(START_PC, false));
		EXPECT_TRUE(UBC_SequenceArmed());
		cpg.write<u8>(CPG_STBCR2_addr, 1);
		EXPECT_EQ(1u, cpg.read<u8>(CPG_STBCR2_addr));
		EXPECT_FALSE(UBC_BreakEnabled());
		EXPECT_FALSE(UBC_InstructionBreakEnabled());
		EXPECT_FALSE(UBC_InstructionBreakPending(START_PC + 2, false));
		EXPECT_FALSE(UBC_InstructionBreak(START_PC + 2, false));
		EXPECT_FALSE(UBC_OperandBreak(START_PC + 2, 4, false, 0));
		EXPECT_TRUE(UBC_SequenceArmed());
		cpg.write<u8>(CPG_STBCR2_addr, 0);
		EXPECT_TRUE(UBC_InstructionBreak(START_PC + 2, false));
		EXPECT_FALSE(UBC_SequenceArmed());

		// Operand matches are latched by the memory access but delivered only
		// after the instruction has completed.
		clearUbc();
		ClearRegs();
		constexpr u32 operandAddress = 0x0c001234;
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		r(1) = operandAddress;
		addrspace::write8(operandAddress, 0x82);
		UBC_BARA = operandAddress;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x25; // operand access, read, byte
		PrepareOp(0x6010); // mov.b @r1,r0
		RunOp();
		EXPECT_EQ(0xffffff82u, r(0));
		EXPECT_EQ(Sh4Ex_UserBreak, CCN_EXPEVT);
		EXPECT_EQ(START_PC + 2, ctx->spc);
		EXPECT_NE(0u, UBC_BRCR & 0x8000);

		// Operand acceptance uses BL after the instruction, not the value at
		// the time of the memory access. LDC.L can change BL after its load.
		clearUbc();
		ClearRegs();
		ctx->vbr = 0x8c010000;
		sr().MD = 1;
		sr().BL = 0;
		r(1) = operandAddress;
		addrspace::write32(operandAddress, sr().getFull() | 0x10000000);
		UBC_BARA = operandAddress;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x27;
		PrepareOp(0x4107); // ldc.l @r1+,sr
		RunOp();
		EXPECT_EQ(1u, sr().BL);
		EXPECT_EQ(START_PC + 2, ctx->pc);
		EXPECT_NE(0u, UBC_BRCR & 0x8000);

		clearUbc();
		ClearRegs();
		ctx->vbr = 0x8c010000;
		sr().MD = 1;
		sr().BL = 1;
		r(1) = operandAddress;
		addrspace::write32(operandAddress, sr().getFull() & ~0x10000000);
		UBC_BARA = operandAddress;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x27;
		PrepareOp(0x4107);
		RunOp();
		EXPECT_EQ(Sh4Ex_UserBreak, CCN_EXPEVT);
		EXPECT_EQ(START_PC + 2, ctx->spc);

		// A match on the first access of a multi-access instruction is not
		// committed if a later access raises a synchronous exception.
		clearUbc();
		ClearRegs();
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		r(0) = operandAddress;
		r(1) = operandAddress + 1; // misaligned second word access
		addrspace::write16(operandAddress, 3);
		UBC_BARA = operandAddress;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x26; // operand access, read, word
		UBC_BRCR = 8; // a tentative channel-A match would arm sequential mode
		PrepareOp(0x401f); // mac.w @r1+,@r0+
		RunOp();
		EXPECT_EQ(Sh4Ex_AddressErrorRead, CCN_EXPEVT);
		EXPECT_EQ(0u, UBC_BRCR & 0xc000);
		EXPECT_FALSE(UBC_SequenceArmed());
		EXPECT_EQ(operandAddress, r(0));
		EXPECT_EQ(operandAddress + 1, r(1));

		// Direction, transfer size, and naturally ignored low address bits are
		// separate operand conditions.
		clearUbc();
		UBC_BARA = operandAddress & ~3u;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x27; // operand access, read, longword
		EXPECT_TRUE(UBC_OperandBreak(operandAddress + 1, 4, false, 0x12345678));
		UBC_BRCR = 0;
		EXPECT_FALSE(UBC_OperandBreak(operandAddress, 2, false, 0x5678));
		EXPECT_FALSE(UBC_OperandBreak(operandAddress, 4, true, 0x12345678));

		// Channel B can additionally compare the transferred data. Narrow
		// byte values are duplicated in the low 16 bits; the upper 16 bits are
		// ignored for both byte and word comparisons.
		clearUbc();
		UBC_BARB = operandAddress;
		UBC_BAMRB = 4;
		UBC_BBRB = 0x25;
		UBC_BDRB = 0xa5a55a5a;
		UBC_BRCR = 0x80; // DBEB
		EXPECT_TRUE(UBC_OperandBreak(operandAddress, 1, false, 0x5a));
		UBC_BRCR = 0x80;
		EXPECT_FALSE(UBC_OperandBreak(operandAddress, 1, false, 0x5b));
		UBC_BDMRB = 0x00000101;
		EXPECT_TRUE(UBC_OperandBreak(operandAddress, 1, false, 0x5b));
		UBC_BRCR = 0x80;
		EXPECT_FALSE(UBC_OperandBreak(operandAddress, 1, false, 0, false));

		// MOVCA.L is a longword operand write carrying R0 data (unlike the
		// explicitly data-less PREF/OCB operations), so DBEB participates.
		clearUbc();
		ClearRegs();
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		r(0) = 0x89abcdef;
		r(1) = operandAddress & ~3u;
		UBC_BARB = r(1);
		UBC_BAMRB = 4;
		UBC_BBRB = 0x2b; // operand access, write, longword
		UBC_BDRB = r(0);
		UBC_BRCR = 0x80;
		PrepareOp(0x01c3); // movca.l r0,@r1
		RunOp();
		EXPECT_EQ(Sh4Ex_UserBreak, CCN_EXPEVT);
		EXPECT_EQ(START_PC + 2, ctx->spc);
		EXPECT_NE(0u, UBC_BRCR & 0x4000);

		// Cache-maintenance accesses have architectural UBC directions distinct
		// from whether they ultimately transfer external data: OCBI is a
		// data-less longword write.
		clearUbc();
		ClearRegs();
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		r(1) = operandAddress & ~3u;
		UBC_BARA = r(1);
		UBC_BAMRA = 4;
		UBC_BBRA = 0x2b; // operand access, write, longword
		PrepareOp(0x0193); // ocbi @r1
		RunOp();
		EXPECT_EQ(Sh4Ex_UserBreak, CCN_EXPEVT);
		EXPECT_EQ(START_PC + 2, ctx->spc);
		EXPECT_NE(0u, UBC_BRCR & 0x8000);

		// A quadword is compared as two independent longwords; either half can
		// satisfy the channel-B data condition.
		clearUbc();
		UBC_BARB = operandAddress;
		UBC_BAMRB = 4;
		UBC_BBRB = 0x64; // operand access, read, quadword
		EXPECT_TRUE(UBC_OperandBreak(operandAddress, 8, false, 0x123456789abcdef0));
		UBC_BRCR = 0x80;
		UBC_BDRB = 0x12345678;
		EXPECT_TRUE(UBC_OperandBreak(operandAddress, 8, false,
				0x123456789abcdef0));
		UBC_BRCR = 0x80;
		UBC_BDRB = 0x11111111;
		EXPECT_FALSE(UBC_OperandBreak(operandAddress, 8, false,
				0x123456789abcdef0));

		// Active instruction matching compiles to one precise UBC executor and
		// a dynamic dispatch, so cached dynarec blocks cannot skip boundaries.
		clearUbc();
		UBC_BARA = START_PC + 0x100;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x14;
		PrepareOp(0xe101, 0xe202);
		RuntimeBlockInfo block;
		ASSERT_TRUE(block.Setup(START_PC, ctx->fpscr));
		EXPECT_EQ(1u, block.guest_opcodes);
		EXPECT_TRUE(std::any_of(block.oplist.begin(), block.oplist.end(),
				[](const shil_opcode& op) { return op.op == shop_ubc_exec; }));
		EXPECT_TRUE(std::any_of(block.oplist.begin(), block.oplist.end(),
				[](const shil_opcode& op) { return op.op == shop_jdyn; }));

		// Operand-only matching needs the same one-instruction boundary.
		clearUbc();
		UBC_BARA = operandAddress;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x25;
		PrepareOp(0x6010, 0xe202);
		RuntimeBlockInfo operandBlock;
		ASSERT_TRUE(operandBlock.Setup(START_PC, ctx->fpscr));
		EXPECT_EQ(1u, operandBlock.guest_opcodes);
		EXPECT_TRUE(std::any_of(operandBlock.oplist.begin(), operandBlock.oplist.end(),
				[](const shil_opcode& op) { return op.op == shop_ubc_exec; }));

		// The canonical executor returns the exception destination on a match
		// and suppresses the guest instruction for a pre-execution break.
		clearUbc();
		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		UBC_BARA = START_PC;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x14;
		EXPECT_EQ(ctx->vbr + 0x100,
				Sh4Interpreter::ExecuteUbcFallback(ctx, 0xe12a, START_PC));
		EXPECT_EQ(0u, r(1));
		EXPECT_EQ(Sh4Ex_UserBreak, CCN_EXPEVT);
		EXPECT_EQ(96, ctx->cycle_counter); // entry only; instruction did not issue

		clearUbc();
		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		UBC_BARA = START_PC;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x14;
		UBC_BRCR = 0x0400;
		EXPECT_EQ(ctx->vbr + 0x100,
				Sh4Interpreter::ExecuteUbcFallback(ctx, 0xe12a, START_PC));
		EXPECT_EQ(42u, r(1));
		EXPECT_EQ(START_PC + 2, ctx->spc);
		EXPECT_EQ(95, ctx->cycle_counter); // one issue cycle plus exception entry

		// The canonical executor also commits a matching operand load before
		// taking the post-instruction user-break exception.
		clearUbc();
		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		r(1) = operandAddress;
		addrspace::write8(operandAddress, 0x7f);
		UBC_BARA = operandAddress;
		UBC_BAMRA = 4;
		UBC_BBRA = 0x25;
		EXPECT_EQ(ctx->vbr + 0x100,
				Sh4Interpreter::ExecuteUbcFallback(ctx, 0x6010, START_PC));
		EXPECT_EQ(0x7fu, r(0));
		EXPECT_EQ(START_PC + 2, ctx->spc);
		EXPECT_EQ(95, ctx->cycle_counter);

		clearUbc();
	}

	void DynarecMemoryTimingBoundaryTest()
	{
		ClearRegs();
		// A variable-latency memory operation must be the last architectural
		// instruction charged to a block. Otherwise cache/MMIO cycles discovered
		// while executing it cannot stop the following instruction on time.
		PrepareOp(0xe101, 0x6012, 0xe202); // mov #1,r1; mov.l @r1,r0; mov #2,r2
		RuntimeBlockInfo loadBlock;
		ASSERT_TRUE(loadBlock.Setup(START_PC, ctx->fpscr));
		EXPECT_EQ(2u, loadBlock.guest_opcodes);
		EXPECT_EQ(4u, loadBlock.sh4_code_size);
		EXPECT_EQ(1u, loadBlock.entry_cycles);
		EXPECT_EQ(START_PC + 4, loadBlock.BranchBlock);

		// A memory access in a delay slot remains part of the branch; delayed
		// branches and their slots are one indivisible architectural operation.
		PrepareOp(0xa001, 0x6012, 0xe202); // bra +1; mov.l @r1,r0; mov #2,r2
		RuntimeBlockInfo delayBlock;
		ASSERT_TRUE(delayBlock.Setup(START_PC, ctx->fpscr));
		EXPECT_EQ(2u, delayBlock.guest_opcodes);
		EXPECT_EQ(4u, delayBlock.sh4_code_size);
		EXPECT_EQ(START_PC + 6, delayBlock.BranchBlock);

		// TAS.B must use its precise read-modify-write handler. A split SHIL
		// sequence would update T before a faulting write and leak partial state.
		PrepareOp(0x411b, 0xe202); // tas.b @r1; mov #2,r2
		RuntimeBlockInfo tasBlock;
		ASSERT_TRUE(tasBlock.Setup(START_PC, ctx->fpscr));
		ASSERT_EQ(tasBlock.guest_opcodes, 1u);
		ASSERT_EQ(tasBlock.sh4_code_size, 2u);
		ASSERT_TRUE(std::any_of(tasBlock.oplist.begin(), tasBlock.oplist.end(),
				[](const shil_opcode& op) { return op.op == shop_ifb; }));

		// MAC.W/MAC.L perform two faultable reads before committing either
		// post-increment or the accumulator. Keep them in the shared handler
		// unless SHIL can represent that all-or-nothing architectural update.
		for (u16 macOp : { u16(0x412f), u16(0x012f) }) {
			PrepareOp(macOp, 0xe202); // mac.[wl] @r2+,@r1+; mov #2,r2
			RuntimeBlockInfo macBlock;
			ASSERT_TRUE(macBlock.Setup(START_PC, ctx->fpscr));
			ASSERT_EQ(1u, macBlock.guest_opcodes);
			ASSERT_EQ(2u, macBlock.sh4_code_size);
			ASSERT_TRUE(std::any_of(macBlock.oplist.begin(), macBlock.oplist.end(),
					[](const shil_opcode& op) { return op.op == shop_ifb; }));
		}

		// Calls need pending PR state during their slot: STS sees the pending
		// return address, LDS supersedes it, and a fault inhibits it. Keep the
		// indivisible call/slot pair in the precise shared implementation.
		for (u16 slot : { u16(0x012a), u16(0x412a), u16(0x6012), u16(0xc700) }) {
			PrepareOp(0xb000, slot);
			RuntimeBlockInfo callBlock;
			ASSERT_TRUE(callBlock.Setup(START_PC, ctx->fpscr));
			EXPECT_EQ(1u, callBlock.guest_opcodes);
			EXPECT_EQ(2u, callBlock.sh4_code_size);
			EXPECT_TRUE(std::any_of(callBlock.oplist.begin(), callBlock.oplist.end(),
					[](const shil_opcode& op) { return op.op == shop_ifb; }));
		}

		PrepareOp(0x412b, 0x0009); // jmp @r1; nop
		RuntimeBlockInfo maskedJumpBlock;
		ASSERT_TRUE(maskedJumpBlock.Setup(START_PC, ctx->fpscr));
		// SSA may fold the explicit low-bit mask into the dynamic-jump input,
		// but the decoded block must retain a dynamic terminator.
		EXPECT_TRUE(std::any_of(maskedJumpBlock.oplist.begin(), maskedJumpBlock.oplist.end(),
				[](const shil_opcode& op) { return op.op == shop_jdyn; }));

		// Byte memory operations and cache/store-queue controls have runtime
		// addresses and effects. In particular, cache controls must not silently
		// become NOPs in strict-cache builds.
		for (u16 memoryOp : { u16(0xcc12), u16(0xcd12), u16(0xce12), u16(0xcf12),
				u16(0x0183), u16(0x0193), u16(0x01a3), u16(0x01b3), u16(0x01c3) }) {
			PrepareOp(memoryOp, 0xe202);
			RuntimeBlockInfo memoryBlock;
			ASSERT_TRUE(memoryBlock.Setup(START_PC, ctx->fpscr));
			ASSERT_EQ(1u, memoryBlock.guest_opcodes);
			ASSERT_EQ(2u, memoryBlock.sh4_code_size);
			ASSERT_TRUE(std::any_of(memoryBlock.oplist.begin(), memoryBlock.oplist.end(),
					[](const shil_opcode& op) { return op.op == shop_ifb; }));
		}

		// Accurate FPU operations can trap through FPSCR enables. Likewise, a
		// privileged fallback can trap when a cached block later runs in user
		// mode. Neither block may precharge the following instruction.
		PrepareOp(0xf010, 0xe202); // fadd fr1,fr0; mov #2,r2
		RuntimeBlockInfo faddBlock;
		ASSERT_TRUE(faddBlock.Setup(START_PC, ctx->fpscr));
		EXPECT_EQ(1u, faddBlock.guest_opcodes);
		EXPECT_EQ(2u, faddBlock.sh4_code_size);
		EXPECT_TRUE(faddBlock.matchesFpscr(ctx->fpscr));
		fpscr_t changedFpscr = ctx->fpscr;
		changedFpscr.PR ^= 1;
		EXPECT_FALSE(faddBlock.matchesFpscr(changedFpscr));
		changedFpscr = ctx->fpscr;
		changedFpscr.SZ ^= 1;
		EXPECT_FALSE(faddBlock.matchesFpscr(changedFpscr));
		changedFpscr = ctx->fpscr;
		changedFpscr.RM ^= 1;
		EXPECT_FALSE(faddBlock.matchesFpscr(changedFpscr));
		changedFpscr = ctx->fpscr;
		changedFpscr.DN ^= 1;
		EXPECT_TRUE(faddBlock.matchesFpscr(changedFpscr));
		changedFpscr.PR ^= 1;
		EXPECT_TRUE(loadBlock.matchesFpscr(changedFpscr));

		PrepareOp(0x0122, 0xe202); // stc VBR,r1; mov #2,r2
		RuntimeBlockInfo privilegedBlock;
		ASSERT_TRUE(privilegedBlock.Setup(START_PC, ctx->fpscr));
		EXPECT_EQ(1u, privilegedBlock.guest_opcodes);
		EXPECT_EQ(2u, privilegedBlock.sh4_code_size);

		// A real delay-slot fetch may cross a page, but must use the slot address
		// for translation while preserving the architectural PC during compilation.
		constexpr u32 pageEnd = START_PC + 0xffe;
		addrspace::write16(pageEnd, 0xa000);     // bra 0
		addrspace::write16(pageEnd + 2, 0x0009); // nop delay slot
		ctx->pc = START_PC + 0x100;
		RuntimeBlockInfo crossPageDelayBlock;
		ASSERT_TRUE(crossPageDelayBlock.Setup(pageEnd, ctx->fpscr));
		EXPECT_EQ(2u, crossPageDelayBlock.guest_opcodes);
		EXPECT_EQ(4u, crossPageDelayBlock.sh4_code_size);
		EXPECT_EQ(2u, crossPageDelayBlock.contiguousCodeSize());
		EXPECT_EQ(pageEnd + 2, crossPageDelayBlock.tail_addr);
		EXPECT_EQ(pageEnd + 2, crossPageDelayBlock.tailVaddr());
		EXPECT_EQ(0x0009, crossPageDelayBlock.tail_opcode);
		EXPECT_EQ(START_PC + 0x100, ctx->pc);

		// Adjacent virtual pages need not be adjacent physically. Keep the delay
		// slot's mapping separate so SMC checks and write invalidation watch the
		// physical page that actually supplied it.
		constexpr u32 virtualPage = 0x02000000;
		constexpr u32 firstPhysicalPage = 0x0c000000;
		constexpr u32 slotPhysicalPage = 0x0c010000;
		addrspace::write16(firstPhysicalPage + 0xffe, 0xa000);
		addrspace::write16(slotPhysicalPage, 0x0009);
		CCN_MMUCR.AT = 1;
		MMU_reset();
		for (u32 i = 0; i < 2; ++i) {
			UTLB[i].Address.VPN = (virtualPage + i * 0x1000) >> 10;
			UTLB[i].Data.PPN = (i == 0 ? firstPhysicalPage : slotPhysicalPage) >> 10;
			UTLB[i].Data.SZ0 = 1; // 4 KiB
			UTLB[i].Data.V = 1;
			UTLB[i].Data.PR = 3;
			UTLB[i].Data.D = 1;
			UTLB_Sync(i);
		}
		RuntimeBlockInfo translatedDelayBlock;
		const bool translatedSetup = translatedDelayBlock.Setup(virtualPage + 0xffe, ctx->fpscr);
		EXPECT_TRUE(translatedSetup);
		if (translatedSetup) {
			EXPECT_EQ(firstPhysicalPage + 0xffe, translatedDelayBlock.addr);
			EXPECT_EQ(slotPhysicalPage, translatedDelayBlock.tail_addr);
			EXPECT_EQ(0x0009, translatedDelayBlock.tail_opcode);
		}

		// If the slot page is absent, TEA identifies the failed slot fetch while
		// SPC identifies the delayed branch, as required for exception restart.
		MMU_reset();
		UTLB[0].Address.VPN = virtualPage >> 10;
		UTLB[0].Data.PPN = firstPhysicalPage >> 10;
		UTLB[0].Data.SZ0 = 1;
		UTLB[0].Data.V = 1;
		UTLB[0].Data.PR = 3;
		UTLB[0].Data.D = 1;
		UTLB_Sync(0);
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		addrspace::write16(firstPhysicalPage + 0xffc, 0xe101); // mov #1,r1
		RuntimeBlockInfo beforeMissingSlotBlock;
		const bool beforeMissingSetup = beforeMissingSlotBlock.Setup(virtualPage + 0xffc, ctx->fpscr);
		EXPECT_TRUE(beforeMissingSetup);
		if (beforeMissingSetup) {
			EXPECT_EQ(1u, beforeMissingSlotBlock.guest_opcodes);
			EXPECT_EQ(2u, beforeMissingSlotBlock.sh4_code_size);
		}
		ctx->cycle_counter = 100;
		RuntimeBlockInfo missingSlotBlock;
		EXPECT_FALSE(missingSlotBlock.Setup(virtualPage + 0xffe, ctx->fpscr));
		EXPECT_EQ(Sh4Ex_TlbMissRead, CCN_EXPEVT);
		EXPECT_EQ(virtualPage + 0x1000, CCN_TEA);
		EXPECT_EQ(virtualPage + 0xffe, ctx->spc);
		EXPECT_EQ(95, ctx->cycle_counter); // branch issue plus exception entry
		CCN_MMUCR.AT = 0;
		MMU_reset();
		SetMemoryHandlers();

		// DIV32 recognition is speculative. Even a complete pattern beyond the
		// current page must not be fetched, translated, or aggregated yet.
		addrspace::write16(pageEnd, 0x0019); // div0u
		for (u32 i = 0; i < 32; ++i) {
			addrspace::write16(pageEnd + 2 + i * 4, 0x4124); // rotcl r1
			addrspace::write16(pageEnd + 4 + i * 4, 0x3324); // div1 r2,r3
		}
		RuntimeBlockInfo pageLimitedDivBlock;
		ASSERT_TRUE(pageLimitedDivBlock.Setup(pageEnd, ctx->fpscr));
		EXPECT_EQ(1u, pageLimitedDivBlock.guest_opcodes);
		EXPECT_EQ(2u, pageLimitedDivBlock.sh4_code_size);
		EXPECT_EQ(NullAddress, pageLimitedDivBlock.tail_addr);

		// The final virtual page ends at 2^32. Its boundary must not wrap to
		// zero before comparison and make the block appear empty.
		ctx->pc = START_PC + 0x100;
		sr().MD = 1;
		RuntimeBlockInfo finalPageBlock;
		ASSERT_TRUE(finalPageBlock.Setup(0xfffffffe, ctx->fpscr));
		EXPECT_EQ(1u, finalPageBlock.guest_opcodes);
		EXPECT_EQ(2u, finalPageBlock.sh4_code_size);

		// Compilation can discover FD before host code exists. That path must
		// charge both the faulting instruction and exception entry, just like
		// interpreter execution and a cached block's runtime FD guard.
		ClearRegs();
		ctx->cycle_counter = 100;
		ctx->vbr = 0x8c010000;
		sr().BL = 0;
		sr().FD = 1;
		PrepareOp(0xf000); // fadd fr0,fr0
		RuntimeBlockInfo disabledFpuBlock;
		ASSERT_FALSE(disabledFpuBlock.Setup(START_PC, ctx->fpscr));
		EXPECT_EQ(95, ctx->cycle_counter); // one issue cycle + four entry cycles
		EXPECT_EQ(Sh4Ex_FpuDisabled, CCN_EXPEVT);
		EXPECT_EQ(START_PC, ctx->spc);
	}

	void MacLongSaturationTest()
	{
		constexpr u32 lhs = START_PC + 0x100;
		constexpr u32 rhs = START_PC + 0x104;
		auto runMac = [&](u64 accumulator, u32 lhsValue, u32 rhsValue) {
			ClearRegs();
			sr().S = 1;
			ctx->mac.full = accumulator;
			r(1) = lhs;
			r(2) = rhs;
			addrspace::write32(lhs, lhsValue);
			addrspace::write32(rhs, rhsValue);
			PrepareOp(0x012f); // mac.l @r2+,@r1+
			RunOp();
			EXPECT_EQ(lhs + 4, r(1));
			EXPECT_EQ(rhs + 4, r(2));
		};

		// MACH[31:16] is outside the 48-bit accumulator and remains unchanged.
		runMac(0xabcd7fffffffffffull, 1, 1);
		EXPECT_EQ(0xabcd7fffffffffffull, ctx->mac.full);

		runMac(0xabcd800000000000ull, 0xffffffff, 1);
		EXPECT_EQ(0xabcd800000000000ull, ctx->mac.full);

		// A negative 48-bit value is sign-extended before accumulation.
		runMac(0xabcdffffffffffffull, 1, 1);
		EXPECT_EQ(0xabcd000000000000ull, ctx->mac.full);
	}
};

TEST_F(Sh4InterpreterTest, MovRmRnTest)
{
	Sh4OpTest::MovRmRnTest();
}
TEST_F(Sh4InterpreterTest, MovImmRnTest)
{
	Sh4OpTest::MovImmRnTest();
}
TEST_F(Sh4InterpreterTest, MovMiscTest)
{
	Sh4OpTest::MovMiscTest();
}
TEST_F(Sh4InterpreterTest, LoadTest)
{
	Sh4OpTest::LoadTest();
}
TEST_F(Sh4InterpreterTest, LoadTest2)
{
	Sh4OpTest::LoadTest2();
}
TEST_F(Sh4InterpreterTest, StoreTest)
{
	Sh4OpTest::StoreTest();
}
TEST_F(Sh4InterpreterTest, StoreTest2)
{
	Sh4OpTest::StoreTest2();
}
TEST_F(Sh4InterpreterTest, ArithmeticTest)
{
	Sh4OpTest::ArithmeticTest();
}
TEST_F(Sh4InterpreterTest, ShiftTest)
{
	Sh4OpTest::ShiftTest();
}
TEST_F(Sh4InterpreterTest, MulDivTest)
{
	Sh4OpTest::MulDivTest();
}
TEST_F(Sh4InterpreterTest, CmpTest)
{
	Sh4OpTest::CmpTest();
}
TEST_F(Sh4InterpreterTest, StatusRegTest)
{
	Sh4OpTest::StatusRegTest();
}
TEST_F(Sh4InterpreterTest, FloatingPointTest)
{
	Sh4OpTest::FloatingPointTest();
}
TEST_F(Sh4InterpreterTest, DoubleFloatingPointTest)
{
	Sh4OpTest::DoubleFloatingPointTest();
}
TEST_F(Sh4InterpreterTest, DelaySlotIllegalInstructionTest)
{
	Sh4InterpreterTest::DelaySlotIllegalInstructionTest();
}
TEST_F(Sh4InterpreterTest, ConditionalDelaySlotTest)
{
	Sh4InterpreterTest::ConditionalDelaySlotTest();
}
TEST_F(Sh4InterpreterTest, ConditionalDelaySlotFpuDisabledTest)
{
	Sh4InterpreterTest::ConditionalDelaySlotFpuDisabledTest();
}
TEST_F(Sh4InterpreterTest, PrivilegedInstructionTest)
{
	Sh4InterpreterTest::PrivilegedInstructionTest();
}
TEST_F(Sh4InterpreterTest, IllegalFpuModeTest)
{
	Sh4InterpreterTest::IllegalFpuModeTest();
}
TEST_F(Sh4InterpreterTest, FpuCauseLifecycleTest)
{
	Sh4InterpreterTest::FpuCauseLifecycleTest();
}
TEST_F(Sh4InterpreterTest, DataAlignmentExceptionTest)
{
	Sh4InterpreterTest::DataAlignmentExceptionTest();
}
TEST_F(Sh4InterpreterTest, AddressSpaceProtectionTest)
{
	Sh4InterpreterTest::AddressSpaceProtectionTest();
}
TEST_F(Sh4InterpreterTest, InterruptIsAcceptedAfterAssertingInstruction)
{
	Sh4InterpreterTest::InterruptIsAcceptedAfterAssertingInstruction();
}
TEST_F(Sh4InterpreterTest, TrapUsesCommonExceptionExit)
{
	Sh4InterpreterTest::TrapUsesCommonExceptionExit();
}
TEST_F(Sh4InterpreterTest, CacheOperationTranslationExceptions)
{
	Sh4InterpreterTest::CacheOperationTranslationExceptions();
}
TEST_F(Sh4InterpreterTest, MovcaAllocationDoesNotFetch)
{
	Sh4InterpreterTest::MovcaAllocationDoesNotFetch();
}
TEST_F(Sh4InterpreterTest, InterruptEntryLatencyTest)
{
	Sh4InterpreterTest::InterruptEntryLatencyTest();
}
TEST_F(Sh4InterpreterTest, NmiInterruptTest)
{
	Sh4InterpreterTest::NmiInterruptTest();
}
TEST_F(Sh4InterpreterTest, UserBreakDebugVectorTest)
{
	Sh4InterpreterTest::UserBreakDebugVectorTest();
}
TEST_F(Sh4InterpreterTest, UbcInstructionBreakTest)
{
	Sh4InterpreterTest::UbcInstructionBreakTest();
}
TEST_F(Sh4InterpreterTest, DynarecMemoryTimingBoundaryTest)
{
	Sh4InterpreterTest::DynarecMemoryTimingBoundaryTest();
}
TEST_F(Sh4InterpreterTest, MacLongSaturationTest)
{
	Sh4InterpreterTest::MacLongSaturationTest();
}
TEST_F(Sh4InterpreterTest, RteDelaySlotIllegalInstructionTest)
{
	Sh4InterpreterTest::RteDelaySlotIllegalInstructionTest();
}

TEST_F(Sh4InterpreterTest, CallDelaySlotPrTest)
{
	Sh4InterpreterTest::CallDelaySlotPrTest();
}

TEST_F(Sh4InterpreterTest, DelayedBranchTargetBitTest)
{
	Sh4InterpreterTest::DelayedBranchTargetBitTest();
}

TEST_F(Sh4InterpreterTest, MemoryIssueTimingDoesNotDependOnResetHistory)
{
	Sh4Cycles cycles;
	cycles.init(ctx);
	cycles.reset();

	// Separate each load with a control-group instruction so every sample has
	// the same pairing state. Timing must not change merely because three other
	// memory operations have occurred since reset.
	constexpr u16 load = 0x6012; // mov.l @r1,r0
	constexpr u16 separator = 0x000b; // rts (CO unit)
	const int expected = cycles.countCycles(load);
	for (int i = 0; i < 6; ++i)
	{
		cycles.countCycles(separator);
		EXPECT_EQ(expected, cycles.countCycles(load));
	}
}

TEST_F(Sh4InterpreterTest, LoadUseDependencyStallsParallelIssue)
{
	Sh4Cycles cycles;
	cycles.init(ctx);
	constexpr u16 loadR0 = 0x6012; // mov.l @r1,r0 (LS, latency 2)
	constexpr u16 addR0 = 0x320c;  // add r0,r2 (EX)
	constexpr u16 addR3 = 0x323c;  // add r3,r2 (EX)

	cycles.reset();
	EXPECT_EQ(1, cycles.countCycles(loadR0));
	EXPECT_EQ(2, cycles.countCycles(addR0));

	cycles.reset();
	EXPECT_EQ(1, cycles.countCycles(loadR0));
	EXPECT_EQ(0, cycles.countCycles(addR3));
}

TEST_F(Sh4InterpreterTest, ScifTransmitsByteAtStopBitDeadline)
{
	struct CapturePipe final : SerialPort::Pipe
	{
		void write(u8 data) override
		{
			lastByte = data;
			writeCount++;
		}
		u8 lastByte = 0;
		int writeCount = 0;
	} pipe;
	auto& port = SCIFSerialPort::Instance();
	port.setPipe(&pipe);
	SCIFSerialPort::SCSMR2_write(0, 0); // 8 data bits, no parity, one stop bit
	SCIFSerialPort::SCBRR2_write(0, 0); // 128 SH-4 cycles per bit
	port.SCSCR2_write(0x20);             // transmitter enable
	port.SCFTDR2_write(0x5a);
	port.SCFTDR2_write(0xa5);

	EXPECT_EQ(0, pipe.writeCount);
	Sh4cntx.sh4_sched_next -= 400;
	sh4_sched_tick(400);
	port.postDeserialize(); // baud reconstruction must preserve the pending edge
	Sh4cntx.sh4_sched_next -= 879;
	sh4_sched_tick(879);
	EXPECT_EQ(0, pipe.writeCount);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(1, pipe.writeCount);
	EXPECT_EQ(0x5a, pipe.lastByte);
	EXPECT_EQ(0u, SCIF_SCFSR2.TEND);
	Sh4cntx.sh4_sched_next -= 1279;
	sh4_sched_tick(1279);
	EXPECT_EQ(1, pipe.writeCount);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(2, pipe.writeCount);
	EXPECT_EQ(0xa5, pipe.lastByte);
	EXPECT_EQ(1u, SCIF_SCFSR2.TEND);

	port.SCSCR2_write(0); // disable transmitter
	port.SCFTDR2_write(0x3c);
	Sh4cntx.sh4_sched_next -= 1280;
	sh4_sched_tick(1280);
	EXPECT_EQ(2, pipe.writeCount);
	port.SCSCR2_write(0x20);
	EXPECT_EQ(0u, SCIF_SCFSR2.TEND);
	Sh4cntx.sh4_sched_next -= 1279;
	sh4_sched_tick(1279);
	EXPECT_EQ(2, pipe.writeCount);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(3, pipe.writeCount);
	EXPECT_EQ(0x3c, pipe.lastByte);
	port.setPipe(nullptr);
}

TEST_F(Sh4InterpreterTest, LateScifCallbackConsumesEveryElapsedFrame)
{
	struct CapturePipe final : SerialPort::Pipe
	{
		void write(u8 data) override { bytes.push_back(data); }
		std::vector<u8> bytes;
	} pipe;
	auto& port = SCIFSerialPort::Instance();
	port.setPipe(&pipe);
	SCIFSerialPort::SCSMR2_write(0, 0);
	SCIFSerialPort::SCBRR2_write(0, 0); // ten-bit frame: 1280 SH-4 cycles
	port.SCSCR2_write(0x20);
	port.SCFTDR2_write(0x12);
	port.SCFTDR2_write(0x34);
	port.SCFTDR2_write(0x56);

	Sh4cntx.sh4_sched_next -= 3 * 1280;
	sh4_sched_tick(3 * 1280);
	EXPECT_EQ((std::vector<u8>{ 0x12, 0x34, 0x56 }), pipe.bytes);
	EXPECT_EQ(1u, SCIF_SCFSR2.TEND);
	port.setPipe(nullptr);
}

TEST_F(Sh4InterpreterTest, ScifPipeAttachmentSchedulesReceiveFrame)
{
	struct InputPipe final : SerialPort::Pipe
	{
		int available() override { return pending ? 1 : 0; }
		u8 read() override
		{
			pending = false;
			return 0x69;
		}
		bool pending = true;
	} pipe;
	auto& port = SCIFSerialPort::Instance();
	SCIFSerialPort::SCSMR2_write(0, 0);
	SCIFSerialPort::SCBRR2_write(0, 0); // ten-bit frame: 1280 SH-4 cycles
	port.SCFCR2_write(0x40);             // RDF threshold: four bytes
	port.SCSCR2_write(0x10);             // receiver enable
	port.setPipe(nullptr);               // stop the idle poll first
	port.setPipe(&pipe);

	Sh4cntx.sh4_sched_next -= 1279;
	sh4_sched_tick(1279);
	EXPECT_EQ(0u, port.SCFDR2_read() & 0xff);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(1u, port.SCFDR2_read() & 0xff);
	EXPECT_EQ(0u, port.readStatus() & 3);
	Sh4cntx.sh4_sched_next -= 1919;
	sh4_sched_tick(1919);
	EXPECT_EQ(0u, port.readStatus() & 1);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(1u, port.readStatus() & 1);
	EXPECT_EQ(0x69, port.SCFRDR2_read());
	port.setPipe(nullptr);
}

TEST_F(Sh4InterpreterTest, SystemRamTracksOpenRowsPerBank)
{
	Sh4Cycles cycles;
	constexpr u32 ram = 0x0c001000;
	BSC_MCR.RASD = 1;
	bsc.closeSdramRows();
	// Every scalar or cache-line request is a four-beat SDRAM burst. External
	// bus cycles are converted to the 200 MHz SH-4 clock here.
	EXPECT_EQ(20, cycles.readAccessCycles(ram, 4));       // no row active
	EXPECT_EQ(14, cycles.readAccessCycles(ram + 4, 32)); // row hit
	EXPECT_EQ(24, cycles.readAccessCycles(ram + 0x800, 4)); // row miss
	EXPECT_EQ(20, cycles.readAccessCycles(ram + 0x400000, 4)); // new bank
	EXPECT_EQ(12, cycles.writeAccessCycles(ram + 0x400004, 4)); // row hit
	bsc.closeSdramRows();
	EXPECT_EQ(20, cycles.dmaReadAccessCycles(ram, 32)); // first DMA command
	EXPECT_EQ(8, cycles.dmaWriteAccessCycles(ram + 4, 32)); // pipelined
	EXPECT_EQ(8, cycles.dmaReadAccessCycles(ram + 8, 32)); // pipelined
}

TEST_F(Sh4InterpreterTest, ManualDmaAdvancesOneBusTransferAtATime)
{
	constexpr u32 source = 0x0c001000;
	constexpr u32 destination = 0x0c002000;
	addrspace::write8(source, 0x12);
	addrspace::write8(source + 1, 0x34);
	addrspace::write8(destination, 0);
	addrspace::write8(destination + 1, 0);

	dmac.write<u32>(DMAC_SAR0_addr, source);
	dmac.write<u32>(DMAC_DAR0_addr, destination);
	dmac.write<u32>(DMAC_DMATCR0_addr, 2);
	dmac.write<u32>(DMAC_DMAOR_addr, 1);
	// Auto-request, byte transfers, increment source and destination, enable.
	dmac.write<u32>(DMAC_CHCR0_addr, 0x5411);

	EXPECT_EQ(0, addrspace::read8(destination));
	EXPECT_EQ(2, DMAC_DMATCR(0));
	Sh4cntx.sh4_sched_next -= 33;
	sh4_sched_tick(33);
	EXPECT_EQ(0, addrspace::read8(destination));

	// With RAS-down disabled, RAM read + write takes 20 + 14 SH-4 cycles.
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(0x12, addrspace::read8(destination));
	EXPECT_EQ(1, DMAC_DMATCR(0));
	EXPECT_EQ(0, DMAC_CHCR(0).TE);

	Sh4cntx.sh4_sched_next -= 36;
	sh4_sched_tick(36);
	EXPECT_EQ(0x34, addrspace::read8(destination + 1));
	EXPECT_EQ(0, DMAC_DMATCR(0));
	EXPECT_EQ(1, DMAC_CHCR(0).TE);
}

TEST_F(Sh4InterpreterTest, Channel2DdtAdvancesByTimedBurst)
{
	constexpr u32 source = 0x0c001800;
	for (u32 offset = 0; offset < 64; offset += 4)
		addrspace::write32(source + offset, 0x80000000 | offset);
	DMAC_DMAOR.full = 0x8201;
	DMAC_SAR(2) = source;
	DMAC_DMATCR(2) = 2;
	DMAC_CHCR(2).full = 0x12c1;
	SB_C2DSTAT = 0x11000000; // direct-texture path
	SB_C2DLEN = 64;
	SB_C2DST = 1;
	SB_LMMODE0 = 0;
	SB_ISTNRM &= ~(1u << 19);
	DMAC_Ch2St();

	EXPECT_EQ(64, SB_C2DLEN);
	EXPECT_EQ(0, DMAC_CHCR(2).TE);
	Sh4cntx.sh4_sched_next -= 29;
	sh4_sched_tick(29);
	EXPECT_EQ(64, SB_C2DLEN);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(32, SB_C2DLEN);
	EXPECT_EQ(source + 32, DMAC_SAR(2));
	EXPECT_EQ(1, DMAC_DMATCR(2));
	EXPECT_EQ(0x11000020, SB_C2DSTAT);
	EXPECT_EQ(1, SB_C2DST);
	EXPECT_EQ(0, DMAC_CHCR(2).TE);
	Sh4cntx.sh4_sched_next -= 30;
	sh4_sched_tick(30);
	EXPECT_EQ(0, SB_C2DLEN);
	EXPECT_EQ(source + 64, DMAC_SAR(2));
	EXPECT_EQ(0, DMAC_DMATCR(2));
	EXPECT_EQ(0x11000040, SB_C2DSTAT);
	EXPECT_EQ(0, SB_C2DST);
	EXPECT_EQ(1, DMAC_CHCR(2).TE);
	EXPECT_NE(0u, SB_ISTNRM & (1u << 19));
}

TEST_F(Sh4InterpreterTest, SoftwareResetRestoresHollyDmaControlDefaults)
{
	SB_DBREQM = 1;
	SB_BAVLWC = 7;
	SB_C2DPRYC = 5;
	SB_C2DMAXL = 2;
	SB_LMMODE0 = SB_LMMODE1 = 1;
	SB_RBSPLT = 0x80000000;
	SB_G2DSTO = SB_G2TRTO = 1;
	SB_G2MDMTO = SB_G2MDMW = 0xff;
	SB_G1RRC = SB_G1RWC = SB_G1FRC = SB_G1FWC = 0;
	SB_G1CRC = SB_G1CWC = SB_G1GDRC = SB_G1GDWC = 0;
	SB_G1CRDYC = 0;
	SB_C2DST = SB_SDST = 1;
	SB_PDDIR = SB_PDTSEL = SB_PDEN = SB_PDST = 1;
	SB_ADEN = SB_E1EN = SB_E2EN = SB_DDEN = 1;
	SB_ADST = SB_E1ST = SB_E2ST = SB_DDST = 1;

	emu.dc_reset(false);

	EXPECT_EQ(Sh4Ex_ManualReset, CCN_EXPEVT);
	EXPECT_EQ(0u, SB_DBREQM);
	EXPECT_EQ(0u, SB_BAVLWC);
	EXPECT_EQ(0u, SB_C2DPRYC);
	EXPECT_EQ(1u, SB_C2DMAXL);
	EXPECT_EQ(0u, SB_LMMODE0);
	EXPECT_EQ(0u, SB_LMMODE1);
	EXPECT_EQ(0u, SB_RBSPLT);
	EXPECT_EQ(0x3ffu, SB_G2DSTO);
	EXPECT_EQ(0x3ffu, SB_G2TRTO);
	EXPECT_EQ(0u, SB_G2MDMTO);
	EXPECT_EQ(0u, SB_G2MDMW);
	EXPECT_EQ(0x1ff7u, SB_G1RRC);
	EXPECT_EQ(0x1ff7u, SB_G1RWC);
	EXPECT_EQ(0x1ff7u, SB_G1FRC);
	EXPECT_EQ(0x1ff7u, SB_G1FWC);
	EXPECT_EQ(0x0ff7u, SB_G1CRC);
	EXPECT_EQ(0x0ff7u, SB_G1CWC);
	EXPECT_EQ(0xffffu, SB_G1GDRC);
	EXPECT_EQ(0xffffu, SB_G1GDWC);
	EXPECT_EQ(1u, SB_G1CRDYC);
	EXPECT_EQ(0u, SB_C2DST);
	EXPECT_EQ(0u, SB_SDST);
	EXPECT_EQ(0u, SB_PDDIR);
	EXPECT_EQ(0u, SB_PDTSEL);
	EXPECT_EQ(0u, SB_PDEN);
	EXPECT_EQ(0u, SB_PDST);
	EXPECT_EQ(0xffu, SB_MST);
	EXPECT_EQ(0u, SB_ADEN);
	EXPECT_EQ(0u, SB_E1EN);
	EXPECT_EQ(0u, SB_E2EN);
	EXPECT_EQ(0u, SB_DDEN);
	EXPECT_EQ(0u, SB_ADST);
	EXPECT_EQ(0u, SB_E1ST);
	EXPECT_EQ(0u, SB_E2ST);
	EXPECT_EQ(0u, SB_DDST);
}

TEST_F(Sh4InterpreterTest, SortDmaCompletionWaitsForTerminalLinkFetch)
{
	constexpr u32 linkTable = 0x0c001000;
	addrspace::write32(linkTable, 2); // End-of-list marker.
	bsc.closeSdramRows();
	SB_SDSTAW = 0x08001000;
	SB_SDWLT = 1;
	SB_SDST = 0;
	SB_ISTNRM &= ~(1u << (u8)holly_PVR_SortDMA);

	hollyRegs.write<u32>(SB_SDST_addr, 1);
	ASSERT_EQ(1u, SB_SDST);

	// One cycle reaches the DMA engine. The terminal link itself is still a
	// first SDRAM DMA burst: ACT + CAS + four data beats = 20 SH-4 cycles.
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	ASSERT_EQ(1u, SB_SDST);
	Sh4cntx.sh4_sched_next -= 19;
	sh4_sched_tick(19);
	EXPECT_EQ(1u, SB_SDST);
	EXPECT_EQ(0u, SB_ISTNRM & (1u << (u8)holly_PVR_SortDMA));

	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(0u, SB_SDST);
	EXPECT_NE(0u, SB_ISTNRM & (1u << (u8)holly_PVR_SortDMA));
}

TEST_F(Sh4InterpreterTest, LateSortDmaCallbackConsumesTerminalLinkDeadline)
{
	constexpr u32 linkTable = 0x0c001000;
	addrspace::write32(linkTable, 2);
	bsc.closeSdramRows();
	SB_SDSTAW = 0x08001000;
	SB_SDWLT = 1;
	SB_SDST = 0;
	SB_ISTNRM &= ~(1u << (u8)holly_PVR_SortDMA);

	hollyRegs.write<u32>(SB_SDST_addr, 1);
	// The initial engine cycle and 20-cycle SDRAM link fetch have both elapsed
	// when this single late dispatch runs.
	Sh4cntx.sh4_sched_next -= 21;
	sh4_sched_tick(21);

	EXPECT_EQ(0u, SB_SDST);
	EXPECT_NE(0u, SB_ISTNRM & (1u << (u8)holly_PVR_SortDMA));
}

TEST_F(Sh4InterpreterTest, SortDmaRejectsProhibitedUnshiftedLink)
{
	constexpr u32 linkTable = 0x0c001000;
	addrspace::write32(linkTable, 0x20); // First valid object offset is 0x80.
	bsc.closeSdramRows();
	SB_SDSTAW = 0x08001000;
	SB_SDBAAW = 0x0c002000;
	SB_SDWLT = 1;
	SB_SDLAS = 0;
	SB_ISTNRM &= ~(1u << (u8)holly_PVR_SortDMA);
	SB_ISTERR &= ~(1u << (u8)holly_SORT_DMA_ERROR);

	hollyRegs.write<u32>(SB_SDST_addr, 1);
	Sh4cntx.sh4_sched_next -= 20;
	sh4_sched_tick(20);
	EXPECT_EQ(1u, SB_SDST);
	EXPECT_EQ(0u, SB_ISTERR & (1u << (u8)holly_SORT_DMA_ERROR));

	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(0u, SB_SDST);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_SORT_DMA_ERROR));
	EXPECT_EQ(0u, SB_ISTNRM & (1u << (u8)holly_PVR_SortDMA));
}

TEST_F(Sh4InterpreterTest, SortDmaRejectsReservedCurrentSizeBits)
{
	constexpr u32 linkTable = 0x0c001000;
	constexpr u32 objectBase = 0x0c002000;
	constexpr u32 object = objectBase + 0x80;
	addrspace::write32(linkTable, 0x80);
	addrspace::write32(object, ParamType_Polygon_or_Modifier_Volume << 29);
	addrspace::write32(object + 0x18, 0x01000004); // Only 0x00000004 is valid.
	addrspace::write32(object + 0x1c, 2);
	bsc.closeSdramRows();
	SB_SDSTAW = 0x08001000;
	SB_SDBAAW = objectBase;
	SB_SDWLT = 1;
	SB_SDLAS = 0;
	SB_ISTNRM &= ~(1u << (u8)holly_PVR_SortDMA);
	SB_ISTERR &= ~(1u << (u8)holly_SORT_DMA_ERROR);

	hollyRegs.write<u32>(SB_SDST_addr, 1);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(1u, SB_SDST);
	EXPECT_EQ(0u, SB_ISTERR & (1u << (u8)holly_SORT_DMA_ERROR));

	// The malformed header is not visible to the engine until both the start
	// link and first 32-byte object reads have completed.
	// The first read costs 20 SH-4 cycles. The pipelined read changes rows and
	// costs another 18 (PRE + ACT + CAS + four data beats).
	Sh4cntx.sh4_sched_next -= 37;
	sh4_sched_tick(37);
	EXPECT_EQ(1u, SB_SDST);
	EXPECT_EQ(0u, SB_ISTERR & (1u << (u8)holly_SORT_DMA_ERROR));
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);

	EXPECT_EQ(0u, SB_SDST);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_SORT_DMA_ERROR));
	EXPECT_EQ(0u, SB_ISTNRM & (1u << (u8)holly_PVR_SortDMA));
}

TEST_F(Sh4InterpreterTest, SortDmaFindsGlobalParameterAfterControlParameter)
{
	constexpr u32 linkTable = 0x0c001000;
	constexpr u32 objectBase = 0x0c002000;
	constexpr u32 object = objectBase + 0x80;
	addrspace::write32(linkTable, 0x80);
	addrspace::write32(object, ParamType_User_Tile_Clip << 29);
	addrspace::write32(object + 0x20, ParamType_Polygon_or_Modifier_Volume << 29);
	addrspace::write32(object + 0x20 + 0x18, 4);
	addrspace::write32(object + 0x20 + 0x1c, 2);
	addrspace::write32(object + 0x40, ParamType_Vertex_Parameter << 29);
	addrspace::write32(object + 0x60, ParamType_Vertex_Parameter << 29);
	bsc.closeSdramRows();
	SB_SDSTAW = 0x08001000;
	SB_SDBAAW = objectBase;
	SB_SDWLT = 1;
	SB_SDLAS = 0;
	SB_ISTNRM &= ~(1u << (u8)holly_PVR_SortDMA);
	SB_ISTERR &= ~(1u << (u8)holly_SORT_DMA_ERROR);

	hollyRegs.write<u32>(SB_SDST_addr, 1);
	Sh4cntx.sh4_sched_next -= 100;
	sh4_sched_tick(100);

	EXPECT_EQ(0u, SB_SDST);
	EXPECT_EQ(0u, SB_ISTERR & (1u << (u8)holly_SORT_DMA_ERROR));
	EXPECT_NE(0u, SB_ISTNRM & (1u << (u8)holly_PVR_SortDMA));
}

TEST_F(Sh4InterpreterTest, SpgStatusProjectsBeamBetweenInterruptDeadlines)
{
	FB_R_CTRL.vclk_div = 1; // 27 MHz pixel clock.
	SPG_CONTROL.interlace = 0;
	SPG_LOAD.hcount = 269;  // Exactly 2,000 SH-4 cycles per line.
	SPG_LOAD.vcount = 99;
	SPG_HBLANK_INT.full = 0;
	SPG_HBLANK_INT.hblank_in_interrupt = 100;
	SPG_HBLANK_INT.line_comp_val = 0;
	SPG_VBLANK_INT.vblank_in_interrupt_line_number = 90;
	SPG_VBLANK_INT.vblank_out_interrupt_line_number = 91;
	SPG_VBLANK.vstart = 90;
	SPG_VBLANK.vbend = 95;
	CalculateSync();

	// Service the sole line-zero HBlank event at ceil(2000 * 100 / 270).
	Sh4cntx.sh4_sched_next -= 741;
	sh4_sched_tick(741);
	ASSERT_EQ(0u, spgReadStatus() & 0x3ff);

	// No event is due for many lines. Status must nevertheless expose the
	// continuously running beam at line 5, 500 cycles into that line.
	Sh4cntx.sh4_sched_next -= 9'759;
	sh4_sched_tick(9'759);
	EXPECT_EQ(5u, spgReadStatus() & 0x3ff);
}

TEST_F(Sh4InterpreterTest, FractionalSpgLinesNeverAdvanceBeforePixelBoundary)
{
	FB_R_CTRL.vclk_div = 1; // 27 MHz pixel clock.
	SPG_CONTROL.interlace = 0;
	SPG_LOAD.hcount = 0; // 200 MHz / 27 MHz cycles per line.
	SPG_LOAD.vcount = 99;
	SPG_HBLANK_INT.full = 0;
	SPG_HBLANK_INT.hblank_in_interrupt = 1023;
	SPG_VBLANK_INT.vblank_in_interrupt_line_number = 1;
	SPG_VBLANK_INT.vblank_out_interrupt_line_number = 90;
	SPG_VBLANK.vstart = 90;
	SPG_VBLANK.vbend = 95;
	CalculateSync();

	// ceil(200 / 27) is eight cycles: line one cannot begin at floor=7.
	Sh4cntx.sh4_sched_next -= 7;
	sh4_sched_tick(7);
	EXPECT_EQ(0u, spgReadStatus() & 0x3ff);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(1u, spgReadStatus() & 0x3ff);

	// The second cumulative boundary is ceil(400 / 27)=15, so its interval
	// is seven cycles without accumulating long-term drift.
	Sh4cntx.sh4_sched_next -= 6;
	sh4_sched_tick(6);
	EXPECT_EQ(1u, spgReadStatus() & 0x3ff);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(2u, spgReadStatus() & 0x3ff);
}

TEST_F(Sh4InterpreterTest, HblankRewriteReschedulesFromLiveBeam)
{
	FB_R_CTRL.vclk_div = 1;
	SPG_CONTROL.interlace = 0;
	SPG_LOAD.hcount = 269;
	SPG_LOAD.vcount = 99;
	SPG_HBLANK_INT.full = 0;
	SPG_HBLANK_INT.hblank_in_interrupt = 100;
	SPG_HBLANK_INT.line_comp_val = 0;
	SPG_VBLANK_INT.vblank_in_interrupt_line_number = 90;
	SPG_VBLANK_INT.vblank_out_interrupt_line_number = 91;
	SPG_VBLANK.vstart = 90;
	SPG_VBLANK.vbend = 95;
	CalculateSync();

	Sh4cntx.sh4_sched_next -= 741;
	sh4_sched_tick(741);
	hollyRegs.write<u32>(SB_ISTNRM_addr, 1u << (u8)holly_HBLank);
	Sh4cntx.sh4_sched_next -= 9'759;
	sh4_sched_tick(9'759); // Line 5, cycle 500, without a callback.

	SPG_HBLANK_INT.line_comp_val = 6;
	rescheduleSPG();
	Sh4cntx.sh4_sched_next -= 2'240;
	sh4_sched_tick(2'240);
	EXPECT_EQ(0u, SB_ISTNRM & (1u << (u8)holly_HBLank));
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_NE(0u, SB_ISTNRM & (1u << (u8)holly_HBLank));
}

TEST_F(Sh4InterpreterTest, SpgStatusIncludesCurrentCpuSliceProgress)
{
	FB_R_CTRL.vclk_div = 1;
	SPG_CONTROL.interlace = 0;
	SPG_LOAD.hcount = 269;
	SPG_LOAD.vcount = 99;
	const bool wasRunning = Sh4cntx.CpuRunning;
	const int oldCounter = Sh4cntx.cycle_counter;
	const int oldSlice = sh4_sched_cpu_slice();
	Sh4cntx.CpuRunning = true;
	sh4_sched_set_cpu_slice(4'000);
	Sh4cntx.cycle_counter = 4'000;
	CalculateSync();

	// The scheduler epoch has not advanced, but the CPU has consumed 2,500
	// cycles of this block: one complete line and 500 cycles of the next.
	Sh4cntx.cycle_counter = 1'500;
	EXPECT_EQ(1u, spgReadStatus() & 0x3ff);

	Sh4cntx.CpuRunning = wasRunning;
	Sh4cntx.cycle_counter = oldCounter;
	sh4_sched_set_cpu_slice(oldSlice);
}

TEST_F(Sh4InterpreterTest, VblankRewriteReschedulesFromLiveBeam)
{
	FB_R_CTRL.vclk_div = 1;
	SPG_CONTROL.interlace = 0;
	SPG_LOAD.hcount = 269;
	SPG_LOAD.vcount = 99;
	SPG_HBLANK_INT.full = 0;
	SPG_HBLANK_INT.hblank_in_interrupt = 1023; // Disabled for this line length.
	SPG_VBLANK_INT.vblank_in_interrupt_line_number = 90;
	SPG_VBLANK_INT.vblank_out_interrupt_line_number = 91;
	SPG_VBLANK.vstart = 90;
	SPG_VBLANK.vbend = 95;
	CalculateSync();

	// Advance to line 5, cycle 500 without reaching the old line-90 event.
	Sh4cntx.sh4_sched_next -= 10'500;
	sh4_sched_tick(10'500);
	ASSERT_EQ(5u, spgReadStatus() & 0x3ff);
	SB_ISTNRM &= ~(1u << (u8)holly_SCANINT1);

	SPG_VBLANK_INT_type value = SPG_VBLANK_INT;
	value.vblank_in_interrupt_line_number = 6;
	pvr_WriteReg(SPG_VBLANK_INT_addr, value.full);
	Sh4cntx.sh4_sched_next -= 1'499;
	sh4_sched_tick(1'499);
	EXPECT_EQ(0u, SB_ISTNRM & (1u << (u8)holly_SCANINT1));
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_NE(0u, SB_ISTNRM & (1u << (u8)holly_SCANINT1));
}

TEST_F(Sh4InterpreterTest, VblankRewritePreservesHblankLineCounter)
{
	FB_R_CTRL.vclk_div = 1;
	SPG_CONTROL.interlace = 0;
	SPG_LOAD.hcount = 269;
	SPG_LOAD.vcount = 99;
	SPG_HBLANK_INT.full = 0;
	SPG_HBLANK_INT.hblank_in_interrupt = 100;
	SPG_HBLANK_INT.line_comp_val = 2;
	SPG_HBLANK_INT.hblank_int_mode = 1;
	SPG_VBLANK_INT.vblank_in_interrupt_line_number = 90;
	SPG_VBLANK_INT.vblank_out_interrupt_line_number = 91;
	SPG_VBLANK.vstart = 90;
	SPG_VBLANK.vbend = 95;
	CalculateSync();

	// Mode 1 counts HBlank compares. Lines 0 and 1 advance the hidden count
	// from zero to two without raising the interrupt.
	Sh4cntx.sh4_sched_next -= 741;
	sh4_sched_tick(741);
	Sh4cntx.sh4_sched_next -= 2'000;
	sh4_sched_tick(2'000);
	ASSERT_EQ(0u, SB_ISTNRM & (1u << (u8)holly_HBLank));

	SPG_VBLANK_INT_type value = SPG_VBLANK_INT;
	value.vblank_in_interrupt_line_number = 80;
	pvr_WriteReg(SPG_VBLANK_INT_addr, value.full);
	Sh4cntx.sh4_sched_next -= 2'000;
	sh4_sched_tick(2'000);
	EXPECT_NE(0u, SB_ISTNRM & (1u << (u8)holly_HBLank));
}

TEST_F(Sh4InterpreterTest, LightgunSignalInstallsRasterDeadline)
{
	FB_R_CTRL.vclk_div = 1;
	SPG_CONTROL.interlace = 0;
	SPG_LOAD.hcount = 269;
	SPG_LOAD.vcount = 99;
	SPG_HBLANK_INT.full = 0;
	SPG_HBLANK_INT.hblank_in_interrupt = 1023;
	SPG_VBLANK_INT.vblank_in_interrupt_line_number = 90;
	SPG_VBLANK_INT.vblank_out_interrupt_line_number = 6;
	SPG_VBLANK.vstart = 90;
	SPG_VBLANK.vbend = 95;
	CalculateSync();

	// Service VBlank-Out at line 6, then advance without another event to
	// line 7, cycle 500.
	Sh4cntx.sh4_sched_next -= 12'000;
	sh4_sched_tick(12'000);
	Sh4cntx.sh4_sched_next -= 2'500;
	sh4_sched_tick(2'500);
	ASSERT_EQ(7u, spgReadStatus() & 0x3ff);
	SB_MDST = 1;

	// y=4 targets line 10 relative to VBlank-Out. Installing the gun signal
	// must replace the old line-90 scheduler deadline.
	read_lightgun_position(100, 4);
	Sh4cntx.sh4_sched_next -= 5'499;
	sh4_sched_tick(5'499);
	EXPECT_EQ(1u, SB_MDST);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(0u, SB_MDST);
	EXPECT_EQ(10u, SPG_TRIGGER_POS >> 16);
}

TEST_F(Sh4InterpreterTest, ResetCancelsPendingRenderCompletion)
{
	SB_ISTNRM &= ~((1u << (u8)holly_RENDER_DONE)
			| (1u << (u8)holly_RENDER_DONE_isp)
			| (1u << (u8)holly_RENDER_DONE_vd));
	scheduleRenderDone(nullptr);
	emu.dc_reset(false);

	Sh4cntx.sh4_sched_next -= 4'096;
	sh4_sched_tick(4'096);
	EXPECT_EQ(0u, SB_ISTNRM & 7u);
}

TEST_F(Sh4InterpreterTest, IspSoftResetCancelsOnlyRenderTimeline)
{
	SB_ISTNRM &= ~7u;
	scheduleRenderDone(nullptr);
	pvr_WriteReg(SOFTRESET_addr, 2); // ISP/TSP reset.
	EXPECT_EQ(2u, pvr_ReadReg(SOFTRESET_addr));
	Sh4cntx.sh4_sched_next -= 4'096;
	sh4_sched_tick(4'096);
	EXPECT_EQ(0u, SB_ISTNRM & 7u);

	// The TA-only reset pin does not reset an independent ISP/TSP render.
	scheduleRenderDone(nullptr);
	pvr_WriteReg(SOFTRESET_addr, 1);
	EXPECT_EQ(1u, pvr_ReadReg(SOFTRESET_addr));
	Sh4cntx.sh4_sched_next -= 4'096;
	sh4_sched_tick(4'096);
	EXPECT_EQ(7u, SB_ISTNRM & 7u);
}

TEST_F(Sh4InterpreterTest, AssertedIspResetBlocksRenderStart)
{
	SB_ISTNRM &= ~7u;
	pvr_WriteReg(SOFTRESET_addr, 2);
	pvr_WriteReg(STARTRENDER_addr, 1);
	Sh4cntx.sh4_sched_next -= 1'500'000;
	sh4_sched_tick(1'500'000);
	EXPECT_EQ(0u, SB_ISTNRM & 7u);

	pvr_WriteReg(SOFTRESET_addr, 0);
	EXPECT_EQ(0u, pvr_ReadReg(SOFTRESET_addr));
	pvr_WriteReg(STARTRENDER_addr, 1);
	Sh4cntx.sh4_sched_next -= 4'095;
	sh4_sched_tick(4'095);
	EXPECT_EQ(0u, SB_ISTNRM & 7u);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(7u, SB_ISTNRM & 7u);
}

TEST_F(Sh4InterpreterTest, AssertedTaResetBlocksListInitialization)
{
	TA_NEXT_OPB = 0x1234;
	TA_NEXT_OPB_INIT = 0x5678;
	pvr_WriteReg(SOFTRESET_addr, 1);
	pvr_WriteReg(TA_LIST_INIT_addr, 0x80000000);
	EXPECT_EQ(0x1234u, TA_NEXT_OPB);

	pvr_WriteReg(SOFTRESET_addr, 0);
	pvr_WriteReg(TA_LIST_INIT_addr, 0x80000000);
	EXPECT_EQ(0x5678u, TA_NEXT_OPB);
}

TEST_F(Sh4InterpreterTest, MapleDmaRejectsCachedAddressAliases)
{
	SB_MDAPRO = 0x404f; // Permit physical system RAM only.
	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_MDSTAR_addr, 0x8c001000);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_MAPLE_ILLADDR));

	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_MDSTAR_addr, 0x0c001000);
	EXPECT_EQ(0u, SB_ISTERR & (1u << (u8)holly_MAPLE_ILLADDR));
}

TEST_F(Sh4InterpreterTest, MapleStatusTracksDmaActivity)
{
	constexpr u32 commandTable = 0x0c004000;
	addrspace::write32(commandTable, 0x80000700); // Last descriptor, NOP pattern.
	SB_MDAPRO = 0x404f;
	bsc.closeSdramRows();
	hollyRegs.write<u32>(SB_MDSTAR_addr, commandTable);
	SB_MDTSEL = 0;
	hollyRegs.write<u32>(SB_MDEN_addr, 1);
	hollyRegs.write<u32>(SB_MDST_addr, 1);

	EXPECT_EQ(1u, SB_MDST);
	EXPECT_NE(0u, SB_MST & 0x80000000);
	EXPECT_EQ(commandTable, SB_MTXDAD);

	// A one-word NOP still requires the host controller's first 32-byte SDRAM
	// burst: ACT + CAS + four data beats = 20 SH-4 cycles.
	Sh4cntx.sh4_sched_next -= 19;
	sh4_sched_tick(19);
	EXPECT_EQ(1u, SB_MDST);
	EXPECT_NE(0u, SB_MST & 0x80000000);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(0u, SB_MDST);
	EXPECT_EQ(0u, SB_MST & 0x80000000);
	EXPECT_EQ(0xffu, SB_MST & 0xff);
}

TEST_F(Sh4InterpreterTest, MapleDmaFetchesFrameAtTransmitDeadline)
{
	constexpr u32 commandTable = 0x0c004000;
	constexpr u32 response = 0x0c005000;
	constexpr u32 requestDmaCycles = 20; // ACT + CAS + four data beats.
	constexpr u32 requestWireCycles = sh4CyclesForXfer(7, 2'000'000 / 8);
	constexpr u32 responseBytes = 29 * sizeof(u32); // Header + 28-word status.
	constexpr u32 responseWireCycles = sh4CyclesForXfer(responseBytes + 3,
			740'000 / 8);
	constexpr u32 responseDmaCycles = 4 * 14; // Four closed-row 32-byte bursts.
	auto controller = maple_Create(MDT_SegaController);
	controller->Setup(0, 5);
	MapleDevices[0][5] = controller;

	BSC_MCR.RASD = 0;
	bsc.closeSdramRows();
	addrspace::write32(commandTable, 0x80000000); // Last, START, one frame word.
	addrspace::write32(commandTable + 4, response);
	addrspace::write32(commandTable + 8, MDC_DeviceRequest | (0x10u << 8)); // Empty unit 5.
	addrspace::write32(response, 0xcccccccc);
	addrspace::write32(response + 32, 0xcccccccc);
	addrspace::write32(response + 96, 0xcccccccc);
	SB_MDAPRO = 0x404f;
	hollyRegs.write<u32>(SB_MDSTAR_addr, commandTable);
	SB_MDTSEL = 0;
	hollyRegs.write<u32>(SB_MDEN_addr, 1);
	hollyRegs.write<u32>(SB_MDST_addr, 1);
	ASSERT_EQ(1u, SB_MDST);

	// The descriptor has been accepted, but the frame has not reached the bus.
	// Redirect it to the main controller before the scheduled transmit event.
	addrspace::write32(commandTable + 8, MDC_DeviceRequest | (0x20u << 8));
	Sh4cntx.sh4_sched_next -= requestDmaCycles + requestWireCycles - 1;
	sh4_sched_tick(requestDmaCycles + requestWireCycles - 1);
	EXPECT_EQ(1u, SB_MDST);
	EXPECT_EQ(0xccccccccu, addrspace::read32(response));
	EXPECT_EQ(0u, SB_ISTNRM & (1u << 12));

	// At the request-wire boundary the peripheral has produced its reply, but
	// neither its wire time nor the host controller's SDRAM writeback has elapsed.
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(1u, SB_MDST);
	EXPECT_EQ(0xccccccccu, addrspace::read32(response));
	EXPECT_EQ(0u, SB_ISTNRM & (1u << 12));

	Sh4cntx.sh4_sched_next -= responseWireCycles + 13;
	sh4_sched_tick(responseWireCycles + 13);
	EXPECT_EQ(1u, SB_MDST);
	EXPECT_EQ(0xccccccccu, addrspace::read32(response));
	EXPECT_EQ(0u, SB_ISTNRM & (1u << 12));

	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(MDRS_DeviceStatus, addrspace::read32(response) & 0xff);
	EXPECT_EQ(0xccccccccu, addrspace::read32(response + 32));
	EXPECT_EQ(response + 32, SB_MRXDAD);
	EXPECT_EQ(1u, SB_MDST);
	EXPECT_EQ(0u, SB_ISTNRM & (1u << 12));

	// Each completed 32-byte burst is observable, while DMA completion remains
	// pending through the final partial burst.
	Sh4cntx.sh4_sched_next -= responseDmaCycles - 14 - 1;
	sh4_sched_tick(responseDmaCycles - 14 - 1);
	EXPECT_NE(0xccccccccu, addrspace::read32(response + 32));
	EXPECT_EQ(0xccccccccu, addrspace::read32(response + 96));
	EXPECT_EQ(response + 96, SB_MRXDAD);
	EXPECT_EQ(1u, SB_MDST);
	EXPECT_EQ(0u, SB_ISTNRM & (1u << 12));

	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);

	EXPECT_EQ(0u, SB_MDST);
	EXPECT_EQ(MDRS_DeviceStatus, addrspace::read32(response) & 0xff);
	EXPECT_EQ(response + responseBytes, SB_MRXDAD);
	EXPECT_NE(0u, SB_ISTNRM & (1u << 12));
	MapleDevices[0][5].reset();
}

TEST_F(Sh4InterpreterTest, TransferCyclesRoundUpToCompletionBoundary)
{
	EXPECT_EQ(2u, sh4CyclesForXfer(1, 100'000'000));
	EXPECT_EQ(3u, sh4CyclesForXfer(1, 80'000'000));
	EXPECT_EQ(5u, sh4CyclesForXfer(2, 80'000'000));
}

TEST_F(Sh4InterpreterTest, FixedFrequencyClockPreservesFractionalCycles)
{
	FixedFrequencyClock<60> clock;
	EXPECT_EQ(3'333'334, clock.nextCycles());
	EXPECT_EQ(3'333'333, clock.nextCycles());
	EXPECT_EQ(3'333'333, clock.nextCycles());

	clock.reset();
	u32 cycles = 0;
	for (int i = 0; i < 60; ++i)
		cycles += clock.nextCycles();
	EXPECT_EQ(SH4_MAIN_CLOCK, cycles);
}

TEST_F(Sh4InterpreterTest, G1DmaRejectsCachedAddressAliases)
{
	SB_ISTERR = 0;
	SB_GDEN = 1;
	hollyRegs.write<u32>(SB_GDSTAR_addr, 0x8c001000);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_G1_ILLADDR));
	EXPECT_EQ(0u, SB_GDEN);

	// The register exposes only physical address bits, but the invalid write
	// remains latched until software supplies a genuinely valid address.
	SB_ISTERR = 0;
	SB_GDEN = 1;
	SB_GDDIR = 1;
	hollyRegs.write<u32>(SB_GDST_addr, 1);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_G1_ILLADDR));
	EXPECT_EQ(0u, SB_GDEN);
	EXPECT_EQ(0u, SB_GDST);

	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_GDSTAR_addr, 0x0c001000);
	EXPECT_EQ(0u, SB_ISTERR & (1u << (u8)holly_G1_ILLADDR));
	SB_GDEN = 0;
}

TEST_F(Sh4InterpreterTest, G1TimingRegistersMaskReservedBits)
{
	hollyRegs.write<u32>(SB_G1RRC_addr, 0xffffffff);
	hollyRegs.write<u32>(SB_G1CRC_addr, 0xffffffff);
	hollyRegs.write<u32>(SB_G1GDRC_addr, 0xffffffff);
	hollyRegs.write<u32>(SB_G1CRDYC_addr, 0xffffffff);

	EXPECT_EQ(0x1ff7u, SB_G1RRC);
	EXPECT_EQ(0x0ff7u, SB_G1CRC);
	EXPECT_EQ(0xffffu, SB_G1GDRC);
	EXPECT_EQ(1u, SB_G1CRDYC);
	// They remain write-only to the SH4.
	EXPECT_EQ(0u, hollyRegs.read<u32>(SB_G1RRC_addr));
}

TEST_F(Sh4InterpreterTest, SortDmaAdvancesStartTableByFetchedBlock)
{
	constexpr u32 table = 0x0c004400;
	addrspace::write32(table, 2); // End of DMA.
	SB_SDSTAW = table;
	SB_SDBAAW = 0x0c004800;
	SB_SDWLT = 1;
	SB_SDLAS = 0;
	SB_ISTNRM &= ~(1u << (u8)holly_PVR_SortDMA);
	hollyRegs.write<u32>(SB_SDST_addr, 1);

	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(1u, SB_SDDIV);
	EXPECT_EQ(table + 32, SB_SDSTAW);
	EXPECT_EQ(1u, SB_SDST); // The table-read bus deadline has not completed.

	// Interrupting after the accepted table fetch leaves its address advance
	// visible but does not generate normal completion.
	hollyRegs.write<u32>(SB_SDST_addr, 0);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(0u, SB_SDST);
	EXPECT_EQ(0u, SB_ISTNRM & (1u << (u8)holly_PVR_SortDMA));
}

TEST_F(Sh4InterpreterTest, Channel2DdtCanBeInterruptedThroughStartRegister)
{
	constexpr u32 source = 0x0c001c00;
	DMAC_DMAOR.full = 0x8201;
	DMAC_SAR(2) = source;
	DMAC_DMATCR(2) = 2;
	DMAC_CHCR(2).full = 0x12c1;
	SB_C2DSTAT = 0x11000000;
	SB_C2DLEN = 64;
	SB_C2DST = 0;
	SB_LMMODE0 = 0;
	SB_ISTNRM &= ~(1u << 19);

	hollyRegs.write<u32>(SB_C2DST_addr, 1);
	EXPECT_EQ(1, SB_C2DST);
	Sh4cntx.sh4_sched_next -= 30;
	sh4_sched_tick(30);
	ASSERT_EQ(32, SB_C2DLEN);
	ASSERT_EQ(1, DMAC_DMATCR(2));

	hollyRegs.write<u32>(SB_C2DST_addr, 0);
	EXPECT_EQ(0, SB_C2DST);
	EXPECT_EQ(0, DMAC_CHCR(2).TE);
	Sh4cntx.sh4_sched_next -= 100;
	sh4_sched_tick(100);
	EXPECT_EQ(32, SB_C2DLEN);
	EXPECT_EQ(1, DMAC_DMATCR(2));
	EXPECT_EQ(0, DMAC_CHCR(2).TE);
	EXPECT_EQ(0u, SB_ISTNRM & (1u << 19));
}

TEST_F(Sh4InterpreterTest, Channel2ZeroLengthEncodesSixteenMiB)
{
	constexpr u32 source = 0x0c000000;
	DMAC_DMAOR.full = 0x8201;
	DMAC_SAR(2) = source;
	DMAC_DMATCR(2) = 0x80000;
	DMAC_CHCR(2).full = 0x12c1;
	SB_C2DSTAT = 0x11000000;
	SB_C2DLEN = 0;
	SB_C2DST = 0;
	SB_LMMODE0 = 0;

	hollyRegs.write<u32>(SB_C2DST_addr, 1);
	EXPECT_EQ(1, SB_C2DST);
	Sh4cntx.sh4_sched_next -= 30;
	sh4_sched_tick(30);
	EXPECT_EQ(0x00ffffe0, SB_C2DLEN);
	EXPECT_EQ(0x7ffff, DMAC_DMATCR(2));
	EXPECT_EQ(0x11000020, SB_C2DSTAT);
	hollyRegs.write<u32>(SB_C2DST_addr, 0);
}

TEST_F(Sh4InterpreterTest, ClearingChannel2EnableTerminatesDdtWithoutCompletion)
{
	constexpr u32 source = 0x0c001d00;
	DMAC_DMAOR.full = 0x8201;
	DMAC_SAR(2) = source;
	DMAC_DMATCR(2) = 2;
	DMAC_CHCR(2).full = 0x12c1;
	SB_C2DSTAT = 0x11000000;
	SB_C2DLEN = 64;
	SB_C2DST = 0;
	SB_LMMODE0 = 0;
	SB_ISTNRM &= ~(1u << 19);

	hollyRegs.write<u32>(SB_C2DST_addr, 1);
	dmac.write<u32>(DMAC_CHCR2_addr, DMAC_CHCR(2).full & ~1u);
	EXPECT_EQ(0, SB_C2DST);
	EXPECT_EQ(0, DMAC_CHCR(2).DE);
	EXPECT_EQ(0, DMAC_CHCR(2).TE);
	EXPECT_EQ(64, SB_C2DLEN);
	EXPECT_EQ(2, DMAC_DMATCR(2));
	Sh4cntx.sh4_sched_next -= 100;
	sh4_sched_tick(100);
	EXPECT_EQ(64, SB_C2DLEN);
	EXPECT_EQ(2, DMAC_DMATCR(2));
	EXPECT_EQ(0u, SB_ISTNRM & (1u << 19));
}

TEST_F(Sh4InterpreterTest, ClearingDmacMasterEnableTerminatesChannel2Ddt)
{
	constexpr u32 source = 0x0c001e00;
	// Priority selection is independent of whether DDT is enabled.
	DMAC_DMAOR.full = 0x8001;
	DMAC_SAR(2) = source;
	DMAC_DMATCR(2) = 2;
	DMAC_CHCR(2).full = 0x12c1;
	SB_C2DSTAT = 0x11000000;
	SB_C2DLEN = 64;
	SB_C2DST = 0;
	SB_LMMODE0 = 0;
	SB_ISTNRM &= ~(1u << 19);

	hollyRegs.write<u32>(SB_C2DST_addr, 1);
	dmac.write<u32>(DMAC_DMAOR_addr, 0x8000);
	EXPECT_EQ(0, SB_C2DST);
	EXPECT_EQ(0, DMAC_CHCR(2).TE);
	EXPECT_EQ(64, SB_C2DLEN);
	EXPECT_EQ(2, DMAC_DMATCR(2));
	Sh4cntx.sh4_sched_next -= 100;
	sh4_sched_tick(100);
	EXPECT_EQ(64, SB_C2DLEN);
	EXPECT_EQ(2, DMAC_DMATCR(2));
	EXPECT_EQ(0u, SB_ISTNRM & (1u << 19));
}

TEST_F(Sh4InterpreterTest, Channel2DdtReleasesBusAtConfiguredBurstLimit)
{
	constexpr u32 source = 0x0c001f00;
	DMAC_DMAOR.full = 0x8201;
	DMAC_SAR(2) = source;
	DMAC_DMATCR(2) = 5;
	DMAC_CHCR(2).full = 0x12c1;
	SB_C2DSTAT = 0x11000000;
	SB_C2DLEN = 160;
	SB_C2DST = 0;
	SB_LMMODE0 = 0;
	// Power-on setting 1 limits a CH2 burst to 128 bytes.
	ASSERT_EQ(1u, SB_C2DMAXL);

	hollyRegs.write<u32>(SB_C2DST_addr, 1);
	Sh4cntx.sh4_sched_next -= 120;
	sh4_sched_tick(120);
	ASSERT_EQ(32, SB_C2DLEN);
	ASSERT_EQ(1, DMAC_DMATCR(2));

	// HOLLY returns the bus before accepting the fifth transfer unit.
	Sh4cntx.sh4_sched_next -= 2;
	sh4_sched_tick(2);
	EXPECT_EQ(32, SB_C2DLEN);
	Sh4cntx.sh4_sched_next -= 29;
	sh4_sched_tick(29);
	EXPECT_EQ(32, SB_C2DLEN);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(0, SB_C2DLEN);
	EXPECT_EQ(1, DMAC_CHCR(2).TE);
}

TEST_F(Sh4InterpreterTest, PvrDmaAdvancesSharedBusCountersByBurst)
{
	constexpr u32 source = 0x0c001a00;
	constexpr u32 destination = 0x04000200;
	for (u32 offset = 0; offset < 64; offset += 4)
		addrspace::write32(source + offset, 0x40000000 | offset);
	DMAC_DMAOR.full = 0x8201;
	SB_PDST = 1;
	bsc.closeSdramRows();
	Sh4Cycles cycles;
	const int burstCycles = cycles.dmaReadAccessCycles(source, 32)
			+ cycles.dmaWriteAccessCycles(destination, 32);
	bsc.closeSdramRows();
	DMAC_StartPvrDma(source, destination, 64, false);

	EXPECT_EQ(source, SB_PDSTARD);
	EXPECT_EQ(destination, SB_PDSTAPD);
	EXPECT_EQ(0, SB_PDLEND);
	EXPECT_EQ(1, SB_PDST);
	Sh4cntx.sh4_sched_next -= burstCycles - 1;
	sh4_sched_tick(burstCycles - 1);
	EXPECT_EQ(0, SB_PDLEND);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(source + 32, SB_PDSTARD);
	EXPECT_EQ(destination + 32, SB_PDSTAPD);
	EXPECT_EQ(32, SB_PDLEND);
	EXPECT_EQ(1, SB_PDST);

	// The second system-RAM command is a pipelined row hit.
	const int secondBurstCycles = cycles.dmaReadAccessCycles(source + 32, 32)
			+ cycles.dmaWriteAccessCycles(destination + 32, 32);
	Sh4cntx.sh4_sched_next -= secondBurstCycles;
	sh4_sched_tick(secondBurstCycles);
	EXPECT_EQ(64, SB_PDLEND);
	EXPECT_EQ(0, SB_PDST);
	EXPECT_EQ(1, DMAC_CHCR(0).TE);
	EXPECT_EQ(source + 64, DMAC_SAR(0));
}

TEST_F(Sh4InterpreterTest, AicaDmaCommitsDataAndCountersAtG2BusDeadlines)
{
	constexpr u32 source = 0x0c002000;
	constexpr u32 destination = 0x00800000;
	for (u32 offset = 0; offset < 64; offset += 4) {
		addrspace::write32(source + offset, 0x60000000 | offset);
		addrspace::write32(destination + offset, 0);
	}
	SB_G2APRO = 0x404f; // Permit the 0x0c000000-0x0cffffff root-bus range.
	SB_ADSTAR = source;
	SB_ADSTAG = destination;
	SB_ADLEN = 64;
	SB_ADDIR = 0;
	SB_ADTSEL = 4; // Enable the suspend request/status function.
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	hollyRegs.write<u32>(SB_ADST_addr, 1);

	EXPECT_EQ(1, SB_ADST);
	EXPECT_EQ(64, SB_ADLEND);
	EXPECT_EQ(0u, addrspace::read32(destination));
	constexpr int burstCycles = 32 * (SH4_MAIN_CLOCK / 2 / G2_BUS_CLOCK);
	Sh4cntx.sh4_sched_next -= burstCycles - 1;
	sh4_sched_tick(burstCycles - 1);
	EXPECT_EQ(64, SB_ADLEND);
	EXPECT_EQ(0u, addrspace::read32(destination));
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(32, SB_ADLEND);
	EXPECT_EQ(source + 32, SB_ADSTARD);
	EXPECT_EQ(destination + 32, SB_ADSTAGD);
	EXPECT_EQ(0x60000000u, addrspace::read32(destination));
	EXPECT_EQ(1, SB_ADST);

	hollyRegs.write<u32>(SB_ADSUSP_addr, 1);
	EXPECT_NE(0u, SB_ADSUSP & 0x10);
	Sh4cntx.sh4_sched_next -= burstCycles * 2;
	sh4_sched_tick(burstCycles * 2);
	EXPECT_EQ(32, SB_ADLEND);
	EXPECT_EQ(1, SB_ADST);
	hollyRegs.write<u32>(SB_ADSUSP_addr, 0);
	EXPECT_EQ(0u, SB_ADSUSP & 0x10);

	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(64, SB_ADLEND);
	EXPECT_EQ(0, SB_ADST);
	ASSERT_NE(0u, SB_ISTNRM & (1u << (u8)holly_SPU_DMA));
	// Completion status is independent of interrupt acknowledgement. A pending
	// completion interrupt must not fabricate another busy read.
	EXPECT_EQ(0u, hollyRegs.read<u32>(SB_ADST_addr));
	EXPECT_EQ(0u, hollyRegs.read<u32>(SB_ADST_addr));
	EXPECT_EQ(0u, SB_ADST);
	EXPECT_EQ(64, SB_ADLEN);
	EXPECT_EQ(source, SB_ADSTAR);
	EXPECT_EQ(destination, SB_ADSTAG);
}

TEST_F(Sh4InterpreterTest, LateAicaDmaDispatchCatchesUpWholeG2Units)
{
	constexpr u32 source = 0x0c002200;
	constexpr u32 destination = 0x00800200;
	for (u32 offset = 0; offset < 96; offset += 4) {
		addrspace::write32(source + offset, 0x70000000 | offset);
		addrspace::write32(destination + offset, 0);
	}
	SB_G2APRO = 0x404f;
	SB_ADSTAR = source;
	SB_ADSTAG = destination;
	SB_ADLEN = 96;
	SB_ADDIR = 0;
	SB_ADTSEL = 0;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	hollyRegs.write<u32>(SB_ADST_addr, 1);

	constexpr int burstCycles = 32 * (SH4_MAIN_CLOCK / 2 / G2_BUS_CLOCK);
	static_assert(burstCycles == 128);
	// At cycle 300, the units due at 128 and 256 are visible; the third unit
	// must retain its remaining 84 cycles rather than being exposed early.
	Sh4cntx.sh4_sched_next -= 300;
	sh4_sched_tick(300);
	EXPECT_EQ(32u, SB_ADLEND);
	EXPECT_EQ(source + 64, SB_ADSTARD);
	EXPECT_EQ(destination + 64, SB_ADSTAGD);
	EXPECT_EQ(0x70000020u, addrspace::read32(destination + 32));
	EXPECT_EQ(0u, addrspace::read32(destination + 64));
	EXPECT_EQ(1u, SB_ADST);

	Sh4cntx.sh4_sched_next -= 83;
	sh4_sched_tick(83);
	EXPECT_EQ(0u, addrspace::read32(destination + 64));
	EXPECT_EQ(1u, SB_ADST);
	Sh4cntx.sh4_sched_next -= 1;
	sh4_sched_tick(1);
	EXPECT_EQ(0x70000040u, addrspace::read32(destination + 64));
	EXPECT_EQ(96u, SB_ADLEND);
	EXPECT_EQ(0u, SB_ADST);
}

TEST_F(Sh4InterpreterTest, AicaReadProtectBlocksG2DmaWritesButNotReads)
{
	constexpr u32 systemAddress = 0x0c002600;
	constexpr u32 soundAddress = 0x00800600;
	constexpr int burstCycles = 32 * (SH4_MAIN_CLOCK / 2 / G2_BUS_CLOCK);
	addrspace::write32(systemAddress, 0x11223344);
	aica::CommonData->RP = 0;
	addrspace::write32(soundAddress, 0xaabbccdd);
	aica::CommonData->RP = 1;
	SB_G2APRO = 0x404f;
	SB_ADSTAR = systemAddress;
	SB_ADSTAG = soundAddress;
	SB_ADLEN = 32;
	SB_ADDIR = 0;
	SB_ADTSEL = 0;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	hollyRegs.write<u32>(SB_ADST_addr, 1);

	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(0xaabbccddu, addrspace::read32(soundAddress));
	EXPECT_EQ(0u, SB_ADST);
	EXPECT_EQ(32u, SB_ADLEND);

	// RP permits main-CPU reads, including a G2 DMA from sound DRAM.
	addrspace::write32(systemAddress, 0);
	hollyRegs.write<u32>(SB_ADEN_addr, 0);
	SB_ADDIR = 1;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	hollyRegs.write<u32>(SB_ADST_addr, 1);
	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(0xaabbccddu, addrspace::read32(systemAddress));
	EXPECT_EQ(0u, SB_ADST);
	aica::CommonData->RP = 0;
}

TEST_F(Sh4InterpreterTest, MainCpuMemoryInhibitBlocksBothG2DmaDirections)
{
	constexpr u32 systemAddress = 0x0c002700;
	constexpr u32 soundAddress = 0x00800700;
	constexpr int burstCycles = 32 * (SH4_MAIN_CLOCK / 2 / G2_BUS_CLOCK);
	aica::CommonData->MRWINH = 0;
	addrspace::write32(systemAddress, 0x11223344);
	addrspace::write32(soundAddress, 0xaabbccdd);
	aica::CommonData->MRWINH = 8;
	SB_G2APRO = 0x404f;
	SB_ADSTAR = systemAddress;
	SB_ADSTAG = soundAddress;
	SB_ADLEN = 32;
	SB_ADDIR = 0;
	SB_ADTSEL = 0;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	hollyRegs.write<u32>(SB_ADST_addr, 1);
	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	aica::CommonData->MRWINH = 0;
	EXPECT_EQ(0xaabbccddu, addrspace::read32(soundAddress));

	// The same inhibit also suppresses sound-RAM reads by the G2 master.
	hollyRegs.write<u32>(SB_ADEN_addr, 0);
	addrspace::write32(systemAddress, 0x55667788);
	aica::CommonData->MRWINH = 8;
	SB_ADDIR = 1;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	hollyRegs.write<u32>(SB_ADST_addr, 1);
	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(0x55667788u, addrspace::read32(systemAddress));
	EXPECT_EQ(0u, SB_ADST);
	aica::CommonData->MRWINH = 0;
}

TEST_F(Sh4InterpreterTest, SoundMemoryProtectionDoesNotBlockAicaRegisterDma)
{
	constexpr u32 systemAddress = 0x0c002780;
	constexpr u32 registerAddress = 0x00703000; // DSP COEF0
	constexpr int burstCycles = 32 * (SH4_MAIN_CLOCK / 2 / G2_BUS_CLOCK);
	aica::CommonData->RP = 1;
	aica::CommonData->MRWINH = 8;
	addrspace::write32(systemAddress, 0x00001238);
	SB_G2APRO = 0x404f;
	SB_ADSTAR = systemAddress;
	SB_ADSTAG = registerAddress;
	SB_ADLEN = 32;
	SB_ADDIR = 0;
	SB_ADTSEL = 0;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	hollyRegs.write<u32>(SB_ADST_addr, 1);
	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(0x1238u, addrspace::read32(registerAddress));

	// Register reads through the same DMA channel are not DRAM reads either.
	hollyRegs.write<u32>(SB_ADEN_addr, 0);
	addrspace::write32(systemAddress, 0);
	SB_ADDIR = 1;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	hollyRegs.write<u32>(SB_ADST_addr, 1);
	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(0x1238u, addrspace::read32(systemAddress));
	EXPECT_EQ(0u, SB_ADST);
	aica::CommonData->RP = 0;
	aica::CommonData->MRWINH = 0;
}

TEST_F(Sh4InterpreterTest, G2DmaChannelsSerializeOnTheSharedBus)
{
	constexpr int burstCycles = 32 * (SH4_MAIN_CLOCK / 2 / G2_BUS_CLOCK);
	SB_G2APRO = 0x404f;
	SB_ADSTAR = 0x0c002400;
	SB_ADSTAG = 0x00800400;
	SB_ADLEN = 64;
	SB_ADDIR = 0;
	SB_ADTSEL = 0;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	hollyRegs.write<u32>(SB_ADST_addr, 1);

	SB_E1STAR = 0x0c002800;
	SB_E1STAG = 0x01000000;
	SB_E1LEN = 32;
	SB_E1DIR = 0;
	SB_E1TSEL = 0;
	hollyRegs.write<u32>(SB_E1EN_addr, 1);
	hollyRegs.write<u32>(SB_E1ST_addr, 1);
	ASSERT_EQ(1, SB_ADST);
	ASSERT_EQ(1, SB_E1ST);
	ASSERT_EQ(64, SB_ADLEND);
	ASSERT_EQ(32, SB_E1LEND);

	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(32, SB_ADLEND);
	EXPECT_EQ(32, SB_E1LEND);

	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(64, SB_ADLEND);
	EXPECT_EQ(0, SB_ADST);
	EXPECT_EQ(32, SB_E1LEND);
	EXPECT_EQ(1, SB_E1ST);

	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(32, SB_E1LEND);
	EXPECT_EQ(0, SB_E1ST);
	EXPECT_EQ(32, SB_E1LEN);
	EXPECT_EQ(0x0c002800u, SB_E1STAR);
	EXPECT_EQ(0x01000000u, SB_E1STAG);
}

TEST_F(Sh4InterpreterTest, G2DmaConfigurationIsLatchedWhenEnabled)
{
	constexpr u32 source = 0x0c003800;
	constexpr u32 replacementSource = 0x0c003c00;
	constexpr u32 destination = 0x00801000;
	constexpr u32 replacementDestination = 0x00801400;
	constexpr int burstCycles = 32 * (SH4_MAIN_CLOCK / 2 / G2_BUS_CLOCK);
	addrspace::write32(source, 0x12345678);
	addrspace::write32(replacementSource, 0x87654321);
	addrspace::write32(destination, 0);
	addrspace::write32(replacementDestination, 0);
	SB_G2APRO = 0x404f;
	SB_ADSTAR = source;
	SB_ADSTAG = destination;
	SB_ADLEN = 32;
	SB_ADDIR = 0;
	SB_ADTSEL = 0;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);

	// These registers remain writable, but do not update the DMA engine's
	// internal configuration until it is disabled again.
	hollyRegs.write<u32>(SB_ADSTAR_addr, replacementSource);
	hollyRegs.write<u32>(SB_ADSTAG_addr, replacementDestination);
	hollyRegs.write<u32>(SB_ADLEN_addr, 64);
	hollyRegs.write<u32>(SB_ADTSEL_addr, 2);
	hollyRegs.write<u32>(SB_ADEN_addr, 1); // Rewriting 1 does not relatch.
	hollyRegs.write<u32>(SB_ADST_addr, 1);

	ASSERT_EQ(1u, SB_ADST);
	EXPECT_EQ(32u, SB_ADLEND);
	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(0x12345678u, addrspace::read32(destination));
	EXPECT_EQ(0u, addrspace::read32(replacementDestination));
	EXPECT_EQ(0u, SB_ADST);
}

TEST_F(Sh4InterpreterTest, HigherPriorityG2DmaPreemptsAtUnitBoundary)
{
	constexpr int burstCycles = 32 * (SH4_MAIN_CLOCK / 2 / G2_BUS_CLOCK);
	SB_G2APRO = 0x404f;
	SB_E1STAR = 0x0c002c00;
	SB_E1STAG = 0x01000000;
	SB_E1LEN = 64;
	SB_E1DIR = 0;
	SB_E1TSEL = 0;
	hollyRegs.write<u32>(SB_E1EN_addr, 1);
	hollyRegs.write<u32>(SB_E1ST_addr, 1);

	// Channel 1 has already acquired its first unit when channel 0 requests.
	SB_ADSTAR = 0x0c003000;
	SB_ADSTAG = 0x00800800;
	SB_ADLEN = 32;
	SB_ADDIR = 0;
	SB_ADTSEL = 0;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	hollyRegs.write<u32>(SB_ADST_addr, 1);

	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(32, SB_E1LEND);
	EXPECT_EQ(32, SB_ADLEND);

	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(32, SB_E1LEND);
	EXPECT_EQ(32, SB_ADLEND);
	EXPECT_EQ(0, SB_ADST);
	EXPECT_EQ(1, SB_E1ST);

	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(64, SB_E1LEND);
	EXPECT_EQ(0, SB_E1ST);
}

TEST_F(Sh4InterpreterTest, ZeroLengthG2DmaCounterWrapsAfterFirstUnit)
{
	constexpr int burstCycles = 32 * (SH4_MAIN_CLOCK / 2 / G2_BUS_CLOCK);
	SB_G2APRO = 0x404f;
	SB_ADSTAR = 0x0c003400;
	SB_ADSTAG = 0x00800c00;
	SB_ADLEN = 0;
	SB_ADDIR = 0;
	SB_ADTSEL = 0;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	hollyRegs.write<u32>(SB_ADST_addr, 1);

	EXPECT_EQ(0u, SB_ADLEND); // Zero encodes 32 MiB.
	Sh4cntx.sh4_sched_next -= burstCycles;
	sh4_sched_tick(burstCycles);
	EXPECT_EQ(0x01ffffe0u, SB_ADLEND);
	EXPECT_EQ(0x0c003420u, SB_ADSTARD);
	EXPECT_EQ(0x00800c20u, SB_ADSTAGD);

	hollyRegs.write<u32>(SB_ADEN_addr, 0);
	EXPECT_EQ(0u, SB_ADLEND);
	EXPECT_EQ(0u, SB_ADST);
}

TEST_F(Sh4InterpreterTest, ManualDmaStopCompletesAcceptedTransferUnit)
{
	constexpr u32 source = 0x0c001040;
	constexpr u32 destination = 0x0c002040;
	addrspace::write8(source, 0x41);
	addrspace::write8(source + 1, 0x42);
	addrspace::write8(destination, 0);
	addrspace::write8(destination + 1, 0);
	dmac.write<u32>(DMAC_SAR0_addr, source);
	dmac.write<u32>(DMAC_DAR0_addr, destination);
	dmac.write<u32>(DMAC_DMATCR0_addr, 2);
	dmac.write<u32>(DMAC_DMAOR_addr, 1);
	dmac.write<u32>(DMAC_CHCR0_addr, 0x5411);

	// DME is a stop request, but the already accepted dual-address transfer
	// remains indivisible and reaches its transfer-unit boundary.
	dmac.write<u32>(DMAC_DMAOR_addr, 0);
	Sh4cntx.sh4_sched_next -= 34;
	sh4_sched_tick(34);
	EXPECT_EQ(0x41, addrspace::read8(destination));
	EXPECT_EQ(0, addrspace::read8(destination + 1));
	EXPECT_EQ(1, DMAC_DMATCR(0));
	EXPECT_EQ(0, DMAC_CHCR(0).TE);
}

TEST_F(Sh4InterpreterTest, SdramRefreshWaitsForAcceptedDmaUnit)
{
	constexpr u32 source = 0x0c001000;
	constexpr u32 destination = 0x0c002000;
	addrspace::write8(source, 0x71);
	addrspace::write8(destination, 0);
	// Auto-refresh, RAS-down, TRAS=1 and TRC=1.
	bsc.write<u32>(BSC_MCR_addr, 0x88000404);
	bsc.write<u16>(BSC_RTCOR_addr, 0xa501);
	bsc.write<u16>(BSC_RTCSR_addr, 0xa508); // request after 8 SH-4 cycles
	dmac.write<u32>(DMAC_SAR0_addr, source);
	dmac.write<u32>(DMAC_DAR0_addr, destination);
	dmac.write<u32>(DMAC_DMATCR0_addr, 1);
	dmac.write<u32>(DMAC_DMAOR_addr, 1);
	dmac.write<u32>(DMAC_CHCR0_addr, 0x5411);

	// The request at cycle 8 cannot split the 38-cycle DMA read/write pair.
	Sh4cntx.sh4_sched_next -= 38;
	sh4_sched_tick(38);
	ASSERT_EQ(0x71, addrspace::read8(destination));

	Sh4Cycles cycles;
	// Refresh starts at the DMA boundary: 26 SH-4 cycles, then a 20-cycle
	// no-row read because REF closed every active SDRAM bank.
	EXPECT_EQ(46, cycles.readAccessCycles(source, 4));
}

TEST_F(Sh4InterpreterTest, CpuExternalAccessWaitsForCycleStealDmaUnit)
{
	constexpr u32 source = 0x0c001080;
	constexpr u32 destination = 0x0c002080;
	addrspace::write8(source, 0x81);
	addrspace::write8(source + 1, 0x82);
	addrspace::write8(destination, 0);
	addrspace::write8(destination + 1, 0);

	const bool wasRunning = Sh4cntx.CpuRunning;
	Sh4cntx.CpuRunning = true;
	sh4_sched_set_cpu_slice(100);
	Sh4cntx.cycle_counter = 100;
	dmac.write<u32>(DMAC_SAR0_addr, source);
	dmac.write<u32>(DMAC_DAR0_addr, destination);
	dmac.write<u32>(DMAC_DMATCR0_addr, 2);
	dmac.write<u32>(DMAC_DMAOR_addr, 1);
	dmac.write<u32>(DMAC_CHCR0_addr, 0x5411); // cycle steal

	Sh4Cycles cycles;
	cycles.init(&Sh4cntx);
	cycles.addReadAccessCycles(source, 1);
	// One 34-cycle DMA unit finishes before the CPU's 20-cycle RAM read.
	EXPECT_EQ(46, Sh4cntx.cycle_counter);
	EXPECT_EQ(0x81, addrspace::read8(destination));
	EXPECT_EQ(0, addrspace::read8(destination + 1));
	EXPECT_EQ(1, DMAC_DMATCR(0));
	Sh4cntx.CpuRunning = wasRunning;
}

TEST_F(Sh4InterpreterTest, CpuExternalAccessWinsCycleStealGrantWindow)
{
	constexpr u32 source = 0x0c0010a0;
	constexpr u32 destination = 0x0c0020a0;
	addrspace::write8(source, 0xa1);
	addrspace::write8(source + 1, 0xa2);
	addrspace::write8(destination, 0);
	addrspace::write8(destination + 1, 0);
	dmac.write<u32>(DMAC_SAR0_addr, source);
	dmac.write<u32>(DMAC_DAR0_addr, destination);
	dmac.write<u32>(DMAC_DMATCR0_addr, 2);
	dmac.write<u32>(DMAC_DMAOR_addr, 1);
	dmac.write<u32>(DMAC_CHCR0_addr, 0x5411);
	Sh4cntx.sh4_sched_next -= 34;
	sh4_sched_tick(34);
	ASSERT_EQ(0xa1, addrspace::read8(destination));
	ASSERT_EQ(1, DMAC_DMATCR(0));

	const bool wasRunning = Sh4cntx.CpuRunning;
	Sh4cntx.CpuRunning = true;
	sh4_sched_set_cpu_slice(100);
	Sh4cntx.cycle_counter = 100;
	Sh4Cycles cycles;
	cycles.init(&Sh4cntx);
	cycles.addReadAccessCycles(source, 1);
	// The DMA unit has released the bus, so only the CPU's 20-cycle access is
	// charged. The second DMA unit remains pending behind a fresh grant window.
	EXPECT_EQ(80, Sh4cntx.cycle_counter);
	EXPECT_EQ(0, addrspace::read8(destination + 1));
	EXPECT_EQ(1, DMAC_DMATCR(0));
	Sh4cntx.CpuRunning = wasRunning;
}

TEST_F(Sh4InterpreterTest, CpuExternalAccessWaitsForEntireDmaBurst)
{
	constexpr u32 source = 0x0c0010c0;
	constexpr u32 destination = 0x0c0020c0;
	addrspace::write8(source, 0x91);
	addrspace::write8(source + 1, 0x92);
	addrspace::write8(destination, 0);
	addrspace::write8(destination + 1, 0);

	const bool wasRunning = Sh4cntx.CpuRunning;
	Sh4cntx.CpuRunning = true;
	sh4_sched_set_cpu_slice(100);
	Sh4cntx.cycle_counter = 100;
	dmac.write<u32>(DMAC_SAR0_addr, source);
	dmac.write<u32>(DMAC_DAR0_addr, destination);
	dmac.write<u32>(DMAC_DMATCR0_addr, 2);
	dmac.write<u32>(DMAC_DMAOR_addr, 1);
	dmac.write<u32>(DMAC_CHCR0_addr, 0x5491); // burst

	Sh4Cycles cycles;
	cycles.init(&Sh4cntx);
	cycles.addReadAccessCycles(source, 1);
	// The first unit takes 34 cycles; the pipelined second takes 28, followed
	// by the CPU's 20-cycle read.
	EXPECT_EQ(18, Sh4cntx.cycle_counter);
	EXPECT_EQ(0x91, addrspace::read8(destination));
	EXPECT_EQ(0x92, addrspace::read8(destination + 1));
	EXPECT_EQ(0, DMAC_DMATCR(0));
	EXPECT_EQ(1, DMAC_CHCR(0).TE);
	Sh4cntx.CpuRunning = wasRunning;
}

TEST_F(Sh4InterpreterTest, ManualDmaRoundRobinArbitratesSingleBus)
{
	constexpr u32 source0 = 0x0c001100;
	constexpr u32 source1 = 0x0c001200;
	constexpr u32 destination0 = 0x0c002100;
	constexpr u32 destination1 = 0x0c002200;
	addrspace::write8(source0, 0x10);
	addrspace::write8(source0 + 1, 0x11);
	addrspace::write8(source1, 0x20);
	addrspace::write8(source1 + 1, 0x21);
	addrspace::write8(destination0, 0);
	addrspace::write8(destination0 + 1, 0);
	addrspace::write8(destination1, 0);
	addrspace::write8(destination1 + 1, 0);

	dmac.write<u32>(DMAC_SAR0_addr, source0);
	dmac.write<u32>(DMAC_DAR0_addr, destination0);
	dmac.write<u32>(DMAC_DMATCR0_addr, 2);
	dmac.write<u32>(DMAC_SAR1_addr, source1);
	dmac.write<u32>(DMAC_DAR1_addr, destination1);
	dmac.write<u32>(DMAC_DMATCR1_addr, 2);
	dmac.write<u32>(DMAC_DMAOR_addr, 0x301); // DME, round robin
	dmac.write<u32>(DMAC_CHCR0_addr, 0x5411);
	dmac.write<u32>(DMAC_CHCR1_addr, 0x5411);

	auto tick = [](int cycles) {
		Sh4cntx.sh4_sched_next -= cycles;
		sh4_sched_tick(cycles);
	};
	tick(34);
	EXPECT_EQ(0x10, addrspace::read8(destination0));
	EXPECT_EQ(0, addrspace::read8(destination1));
	tick(36);
	EXPECT_EQ(0x20, addrspace::read8(destination1));
	EXPECT_EQ(0, addrspace::read8(destination0 + 1));
	tick(36);
	EXPECT_EQ(0x11, addrspace::read8(destination0 + 1));
	EXPECT_EQ(0, addrspace::read8(destination1 + 1));
	tick(36);
	EXPECT_EQ(0x21, addrspace::read8(destination1 + 1));
	EXPECT_EQ(1, DMAC_CHCR(0).TE);
	EXPECT_EQ(1, DMAC_CHCR(1).TE);
}

TEST_F(Sh4InterpreterTest, ManualDmaFixedPriorityPreemptsAtTransferBoundary)
{
	constexpr u32 source0 = 0x0c001300;
	constexpr u32 source1 = 0x0c001400;
	constexpr u32 destination0 = 0x0c002300;
	constexpr u32 destination1 = 0x0c002400;
	addrspace::write8(source0, 0x30);
	addrspace::write8(source0 + 1, 0x31);
	addrspace::write8(source1, 0x40);
	addrspace::write8(destination0, 0);
	addrspace::write8(destination0 + 1, 0);
	addrspace::write8(destination1, 0);

	// Start the lower-priority request first, then make channel 0 ready before
	// the first bus transfer completes. PR=00 must choose channel 0 at every
	// transfer-unit boundary until its request is exhausted.
	dmac.write<u32>(DMAC_SAR1_addr, source1);
	dmac.write<u32>(DMAC_DAR1_addr, destination1);
	dmac.write<u32>(DMAC_DMATCR1_addr, 1);
	dmac.write<u32>(DMAC_DMAOR_addr, 1);
	dmac.write<u32>(DMAC_CHCR1_addr, 0x5411);
	dmac.write<u32>(DMAC_SAR0_addr, source0);
	dmac.write<u32>(DMAC_DAR0_addr, destination0);
	dmac.write<u32>(DMAC_DMATCR0_addr, 2);
	dmac.write<u32>(DMAC_CHCR0_addr, 0x5411);

	auto tick = [](int cycles) {
		Sh4cntx.sh4_sched_next -= cycles;
		sh4_sched_tick(cycles);
	};
	tick(34);
	EXPECT_EQ(0x40, addrspace::read8(destination1));
	EXPECT_EQ(0, addrspace::read8(destination0));
	tick(36);
	EXPECT_EQ(0x30, addrspace::read8(destination0));
	tick(36);
	EXPECT_EQ(0x31, addrspace::read8(destination0 + 1));
	EXPECT_EQ(1, DMAC_CHCR(0).TE);
	EXPECT_EQ(1, DMAC_CHCR(1).TE);
}

TEST_F(Sh4InterpreterTest, ManualDmaRoundRobinPriorityResetsWhenIdle)
{
	constexpr u32 source0 = 0x0c001500;
	constexpr u32 source1 = 0x0c001600;
	constexpr u32 destination0 = 0x0c002500;
	constexpr u32 destination1 = 0x0c002600;
	addrspace::write8(source0, 0x50);
	addrspace::write8(source0 + 1, 0x51);
	addrspace::write8(source1, 0x60);
	addrspace::write8(destination0, 0);
	addrspace::write8(destination0 + 1, 0);
	addrspace::write8(destination1, 0);

	auto tick = [](int cycles) {
		Sh4cntx.sh4_sched_next -= cycles;
		sh4_sched_tick(cycles);
	};
	dmac.write<u32>(DMAC_DMAOR_addr, 0x301); // DME, round robin
	dmac.write<u32>(DMAC_SAR0_addr, source0);
	dmac.write<u32>(DMAC_DAR0_addr, destination0);
	dmac.write<u32>(DMAC_DMATCR0_addr, 1);
	dmac.write<u32>(DMAC_CHCR0_addr, 0x5411);
	tick(34);
	EXPECT_EQ(0x50, addrspace::read8(destination0));

	// Channel 0 was just moved to the bottom, but the intervening idle state
	// restores the reset order. Simultaneous new requests therefore start on 0.
	dmac.write<u32>(DMAC_DMAOR_addr, 0x300);
	dmac.write<u32>(DMAC_SAR0_addr, source0 + 1);
	dmac.write<u32>(DMAC_DAR0_addr, destination0 + 1);
	dmac.write<u32>(DMAC_DMATCR0_addr, 1);
	dmac.write<u32>(DMAC_CHCR0_addr, 0x5411);
	dmac.write<u32>(DMAC_SAR1_addr, source1);
	dmac.write<u32>(DMAC_DAR1_addr, destination1);
	dmac.write<u32>(DMAC_DMATCR1_addr, 1);
	dmac.write<u32>(DMAC_CHCR1_addr, 0x5411);
	dmac.write<u32>(DMAC_DMAOR_addr, 0x301);
	tick(34);
	EXPECT_EQ(0x51, addrspace::read8(destination0 + 1));
	EXPECT_EQ(0, addrspace::read8(destination1));
	tick(36);
	EXPECT_EQ(0x60, addrspace::read8(destination1));
}
