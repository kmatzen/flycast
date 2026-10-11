/*
	Highly inefficient and boring interpreter. Nothing special here
*/

#include "types.h"

#include "../sh4_interpreter.h"
#include "../sh4_opcode_list.h"
#include "../sh4_core.h"
#include "../sh4_interrupts.h"
#include "hw/sh4/sh4_mem.h"
#include "../sh4_sched.h"
#include "../sh4_cache.h"
#include "../modules/modules.h"
#include "debug/gdb_server.h"
#include "../sh4_cycles.h"

Sh4ICache icache;
Sh4OCache ocache;
Sh4Interpreter *Sh4Interpreter::Instance;

void Sh4Interpreter::ExecuteOpcode(u16 op, bool chargeIssueCycles,
		bool checkPreExecutionBreak)
{
	const u32 instructionPc = ctx->pc - 2;
	if (!suppressUserBreak && checkPreExecutionBreak
			&& UBC_InstructionBreak(instructionPc, false)
			&& ctx->sr.BL == 0)
		throw SH4ThrownException(instructionPc, Sh4Ex_UserBreak);
	// A delayed branch and its slot are one architectural instruction for UBC
	// purposes. A pre-break addressed to the slot stops before the branch issues
	// or changes state. RTE is the documented exception: its delay slot cannot
	// have a pre-execution break condition.
	if (!suppressUserBreak && (OpDesc[op]->type & Delayslot) != 0
			&& op != 0x002b && UBC_InstructionBreak(ctx->pc, false)
			&& ctx->sr.BL == 0)
		throw SH4ThrownException(instructionPc, Sh4Ex_UserBreak);
	// Operand and post-execution match flags become architectural only when the
	// instruction completes. Preserve the entry state so a later synchronous
	// exception (including a fault on a subsequent operand access) cannot leak
	// a condition match from an instruction that did not retire.
	const u16 entryUbcMatchFlags = UBC_BRCR & 0xc000;
	const bool entryUbcSequenceArmed = UBC_SequenceArmed();
	const bool entryBl = ctx->sr.BL != 0;
	// Issue time precedes decode/execute completion and any externally visible
	// memory or MMIO effect. It is still consumed when a decoded instruction
	// raises instead of retiring normally.
	if (chargeIssueCycles)
	{
		if (dynarecUbcFallback)
		{
			// The dynarec already charged the outer instruction. A delay-slot
			// instruction executed recursively here still needs its native issue
			// cost, without the interpreter's optional underclock multiplier.
			Sh4Cycles nativeCycles;
			nativeCycles.init(ctx);
			nativeCycles.executeCycles(op);
		}
		else
			sh4cycles.executeCycles(op);
	}
	if (ctx->sr.MD == 0 && OpDesc[op]->IsPrivileged(op))
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_IllegalInstr);
	if (ctx->sr.FD == 1 && OpDesc[op]->IsFloatingPoint())
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuDisabled);
	if (OpDesc[op]->IsIllegalFpuMode(op, ctx->fpscr.PR, ctx->fpscr.SZ))
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_IllegalInstr);
	if (OpDesc[op]->ClearsFpuCause(op))
		ctx->fpscr.full &= ~0x3f000u;
	if (instructionDepth == 0)
		deferredOperandBreak = false;
	++instructionDepth;
	try {
		OpPtr[op](ctx, op);
	}
	catch (const SH4ThrownException& ex) {
		// TRAPA is a completion-type exception: a competing post-execution
		// break is suppressed, but its condition flag is still committed. A
		// pre-execution user break raised recursively in a delay slot has likewise
		// already committed its flag. Re-execution exceptions roll back matches
		// tentatively detected by an instruction that did not complete.
		if (ex.expEvn == Sh4Ex_Trap)
			(void)UBC_InstructionBreak(instructionPc, true);
		else if (ex.expEvn != Sh4Ex_UserBreak)
		{
			UBC_BRCR = (UBC_BRCR & ~0xc000) | entryUbcMatchFlags;
			UBC_RestoreSequenceArmed(entryUbcSequenceArmed);
		}
		--instructionDepth;
		if (instructionDepth == 0)
		{
			deferredUserBreak = false;
			deferredOperandBreak = false;
		}
		throw;
	}
	catch (...) {
		--instructionDepth;
		if (instructionDepth == 0)
		{
			deferredUserBreak = false;
			deferredOperandBreak = false;
		}
		throw;
	}
	--instructionDepth;
	const bool postBreak = !suppressUserBreak
			&& UBC_InstructionBreak(instructionPc, true);
	if (instructionDepth != 0)
		deferredUserBreak |= postBreak && !entryBl && ctx->sr.BL == 0;
	else if ((postBreak && !entryBl && ctx->sr.BL == 0) || deferredUserBreak
			|| (deferredOperandBreak && ctx->sr.BL == 0))
	{
		deferredUserBreak = false;
		deferredOperandBreak = false;
		// After-execution breaks save the address of the instruction that would
		// execute next, including a delayed branch's resolved destination.
		throw SH4ThrownException(ctx->pc, Sh4Ex_UserBreak);
	}
}

void Sh4Interpreter::RecordUbcOperandAccess(u32 address, u32 size, bool write,
		u64 data, bool hasData)
{
	if (!suppressUserBreak)
		deferredOperandBreak |= UBC_OperandBreak(address, size, write, data, hasData);
}

u32 Sh4Interpreter::ExecuteUbcFallback(Sh4Context *ctx, u16 op, u32 pc)
{
	Sh4Interpreter executor;
	executor.ctx = ctx;
	executor.dynarecUbcFallback = true;
	Sh4Interpreter *previous = Instance;
	Instance = &executor;
	ctx->pc = pc + 2;
	try {
		executor.ExecuteOpcode(op, true, true);
	}
	catch (const SH4ThrownException& ex) {
		Do_Exception(ex.epc, ex.expEvn);
	}
	catch (...) {
		Instance = previous;
		throw;
	}
	Instance = previous;
	return ctx->pc;
}

void Sh4Interpreter::ExecuteFallbackHandler(Sh4Context *ctx, u16 op,
		OpCallFP *handler)
{
	Sh4Interpreter executor;
	executor.ctx = ctx;
	executor.dynarecUbcFallback = true;
	Sh4Interpreter *previous = Instance;
	Instance = &executor;
	try {
		handler(ctx, op);
	}
	catch (...) {
		Instance = previous;
		throw;
	}
	Instance = previous;
}

u16 Sh4Interpreter::ReadNexOp(bool checkPreExecutionBreak)
{
	u32 addr = ctx->pc;
	// A pre-execution user break is decided from the virtual instruction
	// address and has priority over a fetch-side TLB/address exception.
	if (checkPreExecutionBreak && UBC_InstructionBreak(addr, false)
			&& ctx->sr.BL == 0)
		throw SH4ThrownException(addr, Sh4Ex_UserBreak);
	if (sh4InstructionAddressError(addr))
	{
		CCN_TEA = addr;
		CCN_PTEH.VPN = addr >> 10;
		throw SH4ThrownException(addr, Sh4Ex_AddressErrorRead);
	}

	ctx->pc = addr + 2;

	return IReadMem16(addr);
}

void Sh4Interpreter::Run()
{
	Instance = this;
	ctx->restoreHostRoundingMode();

	try {
		do
		{
			try {
				if (ctx->interrupt_pend && ctx->cycle_counter > 0 && UpdateINTC())
					sh4cycles.reset();
				do
				{
					u32 op = ReadNexOp();

					ExecuteOpcode(op);
					// Maskable interrupts are sampled between retired
					// instructions. Delayed branches remain atomic because
					// their slot executes inside ExecuteOpcode().
					// If this instruction also exhausted the CPU budget,
					// UpdateSystem_INTC first dispatches every event at that
					// boundary, then selects the highest-priority interrupt.
					if (ctx->interrupt_pend && ctx->cycle_counter > 0 && UpdateINTC())
						sh4cycles.reset();
				} while (ctx->cycle_counter > 0);
				ctx->cycle_counter += SH4_TIMESLICE;
				if (UpdateSystem_INTC())
					sh4cycles.reset();
			} catch (const SH4ThrownException& ex) {
				Do_Exception(ex.epc, ex.expEvn);
				sh4cycles.reset();
				// Do_Exception charges the native four-cycle entry. Scale only the
				// additional synthetic interpreter-underclock portion here.
				sh4cycles.addCycles(sh4ExceptionEntryCycles(ex.expEvn) * (CPU_RATIO - 1));
			}
		} while (ctx->CpuRunning);
	} catch (const debugger::Stop&) {
	}

	ctx->CpuRunning = false;
	Instance = nullptr;
}

void Sh4Interpreter::Start()
{
	ctx->CpuRunning = true;
}

void Sh4Interpreter::Stop()
{
	ctx->CpuRunning = false;
	ctx->cycle_counter = 0;
}

void Sh4Interpreter::Step()
{
	Instance = this;

	ctx->restoreHostRoundingMode();
	try {
		u32 op = ReadNexOp();
		ExecuteOpcode(op);
		if (ctx->interrupt_pend && UpdateINTC())
			sh4cycles.reset();
	} catch (const SH4ThrownException& ex) {
		Do_Exception(ex.epc, ex.expEvn);
		sh4cycles.reset();
		sh4cycles.addCycles(sh4ExceptionEntryCycles(ex.expEvn) * (CPU_RATIO - 1));
	} catch (const debugger::Stop&) {
	}
	Instance = nullptr;
}

void Sh4Interpreter::Reset(bool hard)
{
	verify(!ctx->CpuRunning);

	if (hard)
	{
		int schedNext = ctx->sh4_sched_next;
		memset(ctx, 0, sizeof(*ctx));
		ctx->sh4_sched_next = schedNext;
	}
	ctx->pc = 0xA0000000;

	memset(ctx->r, 0, sizeof(ctx->r));
	memset(ctx->r_bank, 0, sizeof(ctx->r_bank));

	ctx->gbr = ctx->ssr = ctx->spc = ctx->sgr = ctx->dbr = ctx->vbr = 0;
	ctx->mac.full = ctx->pr = ctx->fpul = 0;

	ctx->sr.setFull(0x700000F0);
	ctx->old_sr.status = ctx->sr.status;
	UpdateSR();

	ctx->fpscr.full = 0x00040001;
	ctx->old_fpscr = ctx->fpscr;
	instructionDepth = 0;
	deferredUserBreak = false;
	deferredOperandBreak = false;
	dynarecUbcFallback = false;
	suppressUserBreak = false;

	icache.Reset(hard);
	ocache.Reset(hard);
	sh4cycles.reset();
	const int initialSlice = std::clamp(ctx->sh4_sched_next, 1, SH4_TIMESLICE);
	sh4_sched_set_cpu_slice(initialSlice);
	ctx->cycle_counter = initialSlice;

	INFO_LOG(INTERPRETER, "Sh4 Reset");
}

bool Sh4Interpreter::IsCpuRunning()
{
	return ctx->CpuRunning;
}

void Sh4Interpreter::ExecuteDelayslot()
{
	try {
		u32 op = ReadNexOp();
		if (OpDesc[op]->IsIllegalDelaySlot(op))
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_IllegalInstr);

		ExecuteOpcode(op);
	} catch (SH4ThrownException& ex) {
		AdjustDelaySlotException(ex);
		throw ex;
	} catch (const debugger::Stop& e) {
		ctx->pc -= 2;	// break on previous instruction
		throw e;
	}
}

void Sh4Interpreter::ExecuteDelayslot(u32 delayedPr)
{
	const u32 oldPr = ctx->pr;
	try {
		u32 op = ReadNexOp();
		if (OpDesc[op]->IsIllegalDelaySlot(op))
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_IllegalInstr);

		// A delayed call's PR'' becomes PR' while its slot executes. STS PR
		// therefore observes the return address, while an LDS PR in the slot
		// supersedes the call's delayed write.
		ctx->pr = delayedPr;
		ExecuteOpcode(op);
	} catch (SH4ThrownException& ex) {
		// A re-execution exception in the slot inhibits the call's PR write.
		ctx->pr = oldPr;
		AdjustDelaySlotException(ex);
		throw ex;
	} catch (const debugger::Stop& e) {
		ctx->pr = oldPr;
		ctx->pc -= 2;
		throw e;
	}
}

void Sh4Interpreter::ExecuteDelayslot_RTE()
{
	const bool previousSuppressUserBreak = suppressUserBreak;
	suppressUserBreak = true;
	try {
		// In an RTE delay slot, status register (SR) bits are referenced as follows.
		// In instruction access, the MD bit is used before modification, and in data access,
		// the MD bit is accessed after modification.
		// The other bits—S, T, M, Q, FD, BL, and RB—after modification are used for delay slot
		// instruction execution. The STC and STC.L SR instructions access all SR bits after modification.
		u32 op = ReadNexOp(false);
		// Now restore all SR bits
		ctx->sr.setFull(ctx->ssr);
		// RB and MD take effect for delay-slot execution (only the slot's
		// instruction fetch used the pre-RTE MD bit). Apply the bank switch now;
		// interrupt recognition remains deferred until RTE completes.
		UpdateSR();
		if (OpDesc[op]->IsIllegalDelaySlot(op))
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_IllegalInstr);
		// And execute
		ExecuteOpcode(op, true, false);
	} catch (SH4ThrownException& ex) {
		suppressUserBreak = previousSuppressUserBreak;
		AdjustDelaySlotException(ex);
		throw ex;
	} catch (const debugger::Stop& e) {
		suppressUserBreak = previousSuppressUserBreak;
		ctx->pc -= 2;	// break on previous instruction
		throw e;
	} catch (...) {
		suppressUserBreak = previousSuppressUserBreak;
		throw;
	}
	suppressUserBreak = previousSuppressUserBreak;
}

static int updateSystemIntc(int elapsed)
{
	Sh4cntx.sh4_sched_next -= elapsed;
	if (Sh4cntx.sh4_sched_next <= 0)
		sh4_sched_tick(elapsed);
	if (Sh4cntx.interrupt_pend)
		return UpdateINTC();
	else
		return 0;
}

int UpdateSystem_INTC()
{
	// Execution backends add the legacy fixed slice before entering here. If
	// the final instruction crossed the deadline, the replenished counter is
	// smaller than SH4_TIMESLICE by exactly that overrun. Advance peripherals
	// through the whole instruction rather than carrying the excess into a new
	// slice, which would postpone crossed scheduler events by up to one slice.
	const int overrun = std::max(0, SH4_TIMESLICE - Sh4cntx.cycle_counter);
	const int elapsed = sh4_sched_cpu_slice() + overrun;
	const int replenishedCounter = Sh4cntx.cycle_counter;
	const int interrupt = updateSystemIntc(elapsed);
	const int interruptEntryCycles = replenishedCounter - Sh4cntx.cycle_counter;
	const int nextSlice = std::clamp(Sh4cntx.sh4_sched_next, 1, SH4_TIMESLICE);
	Sh4cntx.cycle_counter = nextSlice - interruptEntryCycles;
	sh4_sched_set_cpu_slice(nextSlice);
	return interrupt;
}

int UpdateSystem_INTC_Sleep()
{
	// A sleeping CPU can skip idle time, but must not jump past the next device
	// deadline. The timeslice cap periodically returns control when no event is
	// pending without introducing callback jitter.
	const int elapsed = std::clamp(Sh4cntx.sh4_sched_next, 0, SH4_TIMESLICE);
	const int previousCounter = Sh4cntx.cycle_counter;
	Sh4cntx.sh4_sched_next -= elapsed;
	if (Sh4cntx.sh4_sched_next <= 0)
		sh4_sched_tick(elapsed);
	const int interrupt = UpdateINTC_Sleep();
	if (interrupt)
	{
		const int interruptEntryCycles = previousCounter - Sh4cntx.cycle_counter;
		const int nextSlice = std::clamp(Sh4cntx.sh4_sched_next, 1, SH4_TIMESLICE);
		Sh4cntx.cycle_counter = nextSlice - interruptEntryCycles;
		sh4_sched_set_cpu_slice(nextSlice);
	}
	return interrupt;
}

void Sh4Interpreter::Init()
{
	ctx = &p_sh4rcb->cntx;
	memset(ctx, 0, sizeof(*ctx));
	sh4cycles.init(ctx);
	icache.init(ctx);
	ocache.init(ctx);
}

void Sh4Interpreter::Term()
{
	Stop();
	INFO_LOG(INTERPRETER, "Sh4 Term");
}

Sh4Executor *Get_Sh4Interpreter()
{
	return new Sh4Interpreter();
}
