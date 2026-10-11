/*
	Interrupt list caching and handling

	SH4 has a very flexible interrupt controller. In order to handle it efficiently, a sorted
	interrupt bitfield is build from the set interrupt priorities. Higher priorities get allocated
	into higher bits, and a simple mask is kept. In order to check for pending interrupts a simple
	!=0 test works, and to identify the pending interrupt bsr(pend) will give the sorted id. As
	this is a single cycle operation on most platforms, the interrupt checking/identification
	is very fast !
*/

#include "types.h"
#include "sh4_interrupts.h"
#include "sh4_core.h"
#include "sh4_mmr.h"
#include "sh4_sched.h"
#include "modules/dmac.h"
#include "oslib/oslib.h"
#include "debug/gdb_server.h"
#include "serialize.h"
#include <cassert>

//these are fixed
const u16 IRLPriority = 0x0246;
#define IRLP9 &IRLPriority,0
#define IRLP11 &IRLPriority,4
#define IRLP13 &IRLPriority,8

#define GIPA(p) &INTC_IPRA.reg_data,4*p
#define GIPB(p) &INTC_IPRB.reg_data,4*p
#define GIPC(p) &INTC_IPRC.reg_data,4*p

struct InterptSourceList_Entry
{
	const u16* PrioReg;
	u32 Shift;
	Sh4ExceptionCode IntEvnCode;

	int GetPrLvl() const { return (*PrioReg >> Shift) & 0xF; }
};

static const InterptSourceList_Entry InterruptSourceList[sh4_INT_ID_COUNT] =
{
	//IRL
	{ IRLP9, Sh4Ex_ExtInterrupt9 },		//sh4_IRL_9
	{ IRLP11, Sh4Ex_ExtInterruptB },	//sh4_IRL_11
	{ IRLP13, Sh4Ex_ExtInterruptD },	//sh4_IRL_13

	//HUDI
	{ GIPC(0), Sh4Ex_HUDI },			//sh4_HUDI_HUDI

	//GPIO (missing on dc ?)
	{ GIPC(3), Sh4Ex_GPIO },			//sh4_GPIO_GPIOI

	//DMAC
	{ GIPC(2), Sh4Ex_DMAC_DTME0 },		//sh4_DMAC_DMTE0
	{ GIPC(2), Sh4Ex_DMAC_DTME1 },		//sh4_DMAC_DMTE1
	{ GIPC(2), Sh4Ex_DMAC_DTME2 },		//sh4_DMAC_DMTE2
	{ GIPC(2), Sh4Ex_DMAC_DTME3 },		//sh4_DMAC_DMTE3
	{ GIPC(2), Sh4Ex_DMAC_DMAE },		//sh4_DMAC_DMAE

	//TMU
	{ GIPA(3), Sh4Ex_TMU0 },			//sh4_TMU0_TUNI0
	{ GIPA(2), Sh4Ex_TMU1 },			//sh4_TMU1_TUNI1
	{ GIPA(1), Sh4Ex_TMU2 },			//sh4_TMU2_TUNI2
	{ GIPA(1), Sh4Ex_TMU2_TICPI2 },		//sh4_TMU2_TICPI2

	//RTC
	{ GIPA(0), Sh4Ex_RTC_ATI },			//sh4_RTC_ATI
	{ GIPA(0), Sh4Ex_RTC_PRI },			//sh4_RTC_PRI
	{ GIPA(0), Sh4Ex_RTC_CUI },			//sh4_RTC_CUI

	//SCI
	{ GIPB(1), Sh4Ex_SCI_ERI },			//sh4_SCI1_ERI
	{ GIPB(1), Sh4Ex_SCI_RXI },			//sh4_SCI1_RXI
	{ GIPB(1), Sh4Ex_SCI_TXI },			//sh4_SCI1_TXI
	{ GIPB(1), Sh4Ex_SCI_TEI },			//sh4_SCI1_TEI

	//SCIF
	{ GIPC(1), Sh4Ex_SCIF_ERI },		//sh4_SCIF_ERI
	{ GIPC(1), Sh4Ex_SCIF_RXI },		//sh4_SCIF_RXI
	{ GIPC(1), Sh4Ex_SCIF_BRI },		//sh4_SCIF_BRI
	{ GIPC(1), Sh4Ex_SCIF_TXI },		//sh4_SCIF_TXI

	//WDT
	{ GIPB(3), Sh4Ex_WDT },				//sh4_WDT_ITI

	//REF
	{ GIPB(2), Sh4Ex_REF_RCMI },		//sh4_REF_RCMI
	{ GIPA(2), Sh4Ex_REF_ROVI },		//sh4_REF_ROVI
};

//Maps siid -> EventID
alignas(64) static Sh4ExceptionCode InterruptEnvId[32];
//Maps piid -> 1<<siid
alignas(64) static u32 InterruptBit[32];
//Maps sh4 interrupt level to inclusive bitfield
alignas(64) static u32 InterruptLevelBit[16];

static void Do_Interrupt(Sh4ExceptionCode intEvn);

static u32 interrupt_vpend; // Vector of pending interrupts
static u32 interrupt_visible_vpend; // Pending interrupts after INTC decision latency
static u32 interrupt_vmask; // Vector of masked interrupts             (-1 inhibits all interrupts)
static u32 decoded_srimask; // Vector of interrupts allowed by SR.IMSK (-1 inhibits all interrupts)
static int interrupt_sched[sh4_INT_ID_COUNT];
static int nmi_sched;
static bool interrupts_initialized;
static bool nmi_pending;
static bool nmi_visible;
static bool nmi_level = true;

static constexpr u32 NMI_PENDING_BIT = 0x80000000u;

//bit 0 ~ 27 : interrupt source 27:0. 0 = lowest level, 27 = highest level.
static void recalc_pending_itrs()
{
	const bool maskAll = INTC_ICR.MAI && !nmi_level;
	Sh4cntx.interrupt_pend = maskAll ? 0
			: interrupt_visible_vpend & interrupt_vmask & decoded_srimask;
	if (nmi_visible && (!Sh4cntx.sr.BL || INTC_ICR.NMIB))
		Sh4cntx.interrupt_pend |= NMI_PENDING_BIT;
}

//Rebuild sorted interrupt id table (priorities were updated)
void SIIDRebuild()
{
	int cnt = 0;
	u32 vpend = interrupt_vpend;
	u32 visible_vpend = interrupt_visible_vpend;
	u32 vmask = interrupt_vmask;
	interrupt_vpend = 0;
	interrupt_visible_vpend = 0;
	interrupt_vmask = 0;
	//rebuild interrupt table
	for (int ilevel = 0; ilevel < 16; ilevel++)
	{
		for (int isrc = 0; isrc < sh4_INT_ID_COUNT; isrc++)
		{
			if (InterruptSourceList[isrc].GetPrLvl() == ilevel)
			{
				InterruptEnvId[cnt] = InterruptSourceList[isrc].IntEvnCode;
				u32 p = InterruptBit[isrc] & vpend;
				u32 visible = InterruptBit[isrc] & visible_vpend;
				u32 m = InterruptBit[isrc] & vmask;
				InterruptBit[isrc] = 1 << cnt;
				if (p)
					interrupt_vpend |= InterruptBit[isrc];
				if (visible)
					interrupt_visible_vpend |= InterruptBit[isrc];
				if (m)
					interrupt_vmask |= InterruptBit[isrc];
				cnt++;
			}
		}
		InterruptLevelBit[ilevel] = (1 << cnt) - 1;
	}
	SRdecode();
}

//Decode SR.IMSK into a interrupt mask, update and return the interrupt state
bool SRdecode()
{
	if (Sh4cntx.sr.BL)
		decoded_srimask=~0xFFFFFFFF;
	else
		decoded_srimask=~InterruptLevelBit[Sh4cntx.sr.IMASK];

	recalc_pending_itrs();
	return Sh4cntx.interrupt_pend;
}

int UpdateINTC()
{
	if (!Sh4cntx.interrupt_pend)
		return 0;
	if (nmi_visible && (!Sh4cntx.sr.BL || INTC_ICR.NMIB))
	{
		nmi_pending = false;
		nmi_visible = false;
		INTC_ICR.NMIB = 0;
		Do_Interrupt(Sh4Ex_NmiInterrupt);
		return 1;
	}

	Do_Interrupt(InterruptEnvId[bitscanrev(Sh4cntx.interrupt_pend & ~NMI_PENDING_BIT)]);
	return 1;
}

int UpdateINTC_Sleep()
{
	if (nmi_visible)
	{
		nmi_pending = false;
		nmi_visible = false;
		INTC_ICR.NMIB = 0;
		Do_Interrupt(Sh4Ex_NmiInterrupt);
		return 1;
	}

	// Unlike normal execution, sleep and standby accept an interrupt even
	// while SR.BL is set. SR.IMASK and the interrupt-controller source masks
	// still apply, so select from the unblocked visible requests here rather
	// than changing the ordinary cached pending vector.
	const u32 sleepPending = interrupt_visible_vpend & interrupt_vmask
			& ~InterruptLevelBit[Sh4cntx.sr.IMASK];
	if (!sleepPending)
		return 0;

	Do_Interrupt(InterruptEnvId[bitscanrev(sleepPending)]);
	return 1;
}

int sh4InterruptDecisionCycles(Sh4ExceptionCode event)
{
	if (event == Sh4Ex_NmiInterrupt)
		return 9; // 1 Icyc + 4 Bcyc, with Bcyc = 2 Icyc on Dreamcast
	if (event >= Sh4Ex_ExtInterrupt0 && event <= Sh4Ex_ExtInterruptE)
		return 15; // 1 Icyc + 7 Bcyc, with Bcyc = 2 Icyc on Dreamcast
	return 5; // 1 Icyc + 2 Bcyc for on-chip peripheral sources
}

int sh4InterruptEntryCycles(Sh4ExceptionCode event)
{
	(void)event;
	return 4;
}

int sh4ExceptionEntryCycles(Sh4ExceptionCode event)
{
	(void)event;
	return 4;
}

static int interruptDecisionCallback(int tag, int, int, void *)
{
	const InterruptID intr = static_cast<InterruptID>(tag);
	if (interrupt_vpend & InterruptBit[intr])
	{
		interrupt_visible_vpend |= InterruptBit[intr];
		recalc_pending_itrs();
	}
	return 0;
}

static int nmiDecisionCallback(int, int, int, void *)
{
	if (nmi_pending)
	{
		nmi_visible = true;
		recalc_pending_itrs();
	}
	return 0;
}

void SetNmiLevel(bool high)
{
	if (high == nmi_level)
		return;
	const bool rising = !nmi_level && high;
	nmi_level = high;
	INTC_ICR.NMIL = high;
	recalc_pending_itrs();

	if (rising != (INTC_ICR.NMIE != 0) || nmi_pending)
		return;
	nmi_pending = true;
	nmi_visible = false;
	DMAC_Nmi();
	if (interrupts_initialized)
		sh4_sched_request(nmi_sched, sh4InterruptDecisionCycles(Sh4Ex_NmiInterrupt));
	else
	{
		nmi_visible = true;
		recalc_pending_itrs();
	}
}

void SetInterruptPend(InterruptID intr)
{
	if (interrupt_vpend & InterruptBit[intr])
		return;
	interrupt_vpend |= InterruptBit[intr];
	if (interrupts_initialized)
		sh4_sched_request(interrupt_sched[intr],
				sh4InterruptDecisionCycles(InterruptSourceList[intr].IntEvnCode));
	else
	{
		interrupt_visible_vpend |= InterruptBit[intr];
		recalc_pending_itrs();
	}
}
void ResetInterruptPend(InterruptID intr)
{
	interrupt_vpend &= ~InterruptBit[intr];
	interrupt_visible_vpend &= ~InterruptBit[intr];
	if (interrupts_initialized)
		sh4_sched_request(interrupt_sched[intr], -1);
	recalc_pending_itrs();
}

void SetInterruptMask(InterruptID intr)
{
	interrupt_vmask |= InterruptBit[intr];
	recalc_pending_itrs();
}
void ResetInterruptMask(InterruptID intr)
{
	interrupt_vmask &= ~InterruptBit[intr];
	recalc_pending_itrs();
}

static void Do_Interrupt(Sh4ExceptionCode intEvn)
{
	Sh4cntx.cycle_counter -= sh4InterruptEntryCycles(intEvn);
	CCN_INTEVT = intEvn;

	Sh4cntx.ssr = Sh4cntx.sr.getFull();
	Sh4cntx.spc = Sh4cntx.pc;
	Sh4cntx.sgr = Sh4cntx.r[15];
	Sh4cntx.sr.BL = 1;
	Sh4cntx.sr.MD = 1;
	Sh4cntx.sr.RB = 1;
	UpdateSR();
	Sh4cntx.pc = Sh4cntx.vbr + 0x600;
	debugger::subroutineCall();
}

static void enterResetException(Sh4ExceptionCode expEvn)
{
	// Reset exceptions initialize the on-chip modules as a manual reset does.
	// This deliberately stops short of the platform-wide soft-reset path: RAM
	// and external Dreamcast/Naomi devices are outside the SH-4 reset domain.
	sh4_mmr_reset(false);

	Sh4cntx.cycle_counter -= sh4ExceptionEntryCycles(expEvn);
	CCN_EXPEVT = expEvn;
	Sh4cntx.vbr = 0;
	// T, S, Q, and M are undefined after reset; preserve their previous values
	// rather than inventing a deterministic result for them.
	Sh4cntx.sr.MD = 1;
	Sh4cntx.sr.RB = 1;
	Sh4cntx.sr.BL = 1;
	Sh4cntx.sr.IMASK = 15;
	Sh4cntx.sr.FD = 0;
	UpdateSR();
	Sh4cntx.fpscr.full = 0x00040001;
	Sh4Context::UpdateFPSCR(&Sh4cntx);
	Sh4cntx.pc = 0xa0000000;
	debugger::subroutineCall();
}

void Do_Exception(u32 epc, Sh4ExceptionCode expEvn)
{
	assert((expEvn >= Sh4Ex_TlbMissRead && expEvn <= Sh4Ex_SlotIllegalInstr)
			|| expEvn == Sh4Ex_FpuDisabled || expEvn == Sh4Ex_SlotFpuDisabled || expEvn == Sh4Ex_UserBreak);

	if (expEvn == Sh4Ex_TlbMultiHit)
	{
		// Instruction and operand TLB multiple hits are reset-class
		// exceptions. Unlike ordinary exceptions they do not save SPC/SSR/SGR
		// or use VBR; execution restarts at the fixed reset vector.
		enterResetException(expEvn);
		return;
	}
	if (Sh4cntx.sr.BL != 0 && expEvn != Sh4Ex_UserBreak)
	{
		// A general exception while exceptions are blocked causes a manual
		// reset. SPC and SSR become undefined; keep their previous values rather
		// than inventing deterministic replacement data.
		enterResetException(Sh4Ex_ManualReset);
		return;
	}
	Sh4cntx.cycle_counter -= sh4ExceptionEntryCycles(expEvn);
	CCN_EXPEVT = expEvn;

	Sh4cntx.ssr = Sh4cntx.sr.getFull();
	Sh4cntx.spc = epc;
	Sh4cntx.sgr = Sh4cntx.r[15];
	Sh4cntx.sr.BL = 1;
	Sh4cntx.sr.MD = 1;
	Sh4cntx.sr.RB = 1;
	UpdateSR();

	if (expEvn == Sh4Ex_UserBreak && (UBC_BRCR & 1) != 0)
		// BRCR.UBDE selects the dedicated debug handler address directly; DBR
		// is not a vector base and therefore receives no exception offset.
		Sh4cntx.pc = Sh4cntx.dbr;
	else
		Sh4cntx.pc = Sh4cntx.vbr
				+ (expEvn == Sh4Ex_TlbMissRead || expEvn == Sh4Ex_TlbMissWrite
						? 0x400 : 0x100);
	debugger::subroutineCall();

	//printf("RaiseException: from pc %08x to %08x, event %x\n", epc, next_pc, expEvn);
}


//Init/Res/Term
void interrupts_init()
{
	for (int i = 0; i < sh4_INT_ID_COUNT; i++)
		interrupt_sched[i] = sh4_sched_register(i, interruptDecisionCallback);
	nmi_sched = sh4_sched_register(0, nmiDecisionCallback);
	interrupts_initialized = true;
}

void interrupts_reset()
{
	if (interrupts_initialized)
		for (int sched : interrupt_sched)
			sh4_sched_request(sched, -1);
	if (interrupts_initialized)
		sh4_sched_request(nmi_sched, -1);

	//reset interrupts cache
	interrupt_vpend = 0;
	interrupt_visible_vpend = 0;
	interrupt_vmask = 0xFFFFFFFF;
	decoded_srimask = 0;
	nmi_pending = false;
	nmi_visible = false;
	nmi_level = true;
	INTC_ICR.NMIL = 1;

	for (u32 i = 0; i < sh4_INT_ID_COUNT; i++)
		InterruptBit[i] = 1 << i;

	//rebuild the interrupts table
	SIIDRebuild();
}

void interrupts_term()
{
	interrupts_initialized = false;
	for (int& sched : interrupt_sched)
	{
		sh4_sched_unregister(sched);
		sched = -1;
	}
	sh4_sched_unregister(nmi_sched);
	nmi_sched = -1;
}

void interrupts_serialize(Serializer& ser)
{
	ser << InterruptEnvId;
	ser << InterruptBit;
	ser << InterruptLevelBit;
	ser << interrupt_vpend;
	ser << interrupt_visible_vpend;
	ser << interrupt_vmask;
	ser << decoded_srimask;
	for (int sched : interrupt_sched)
		sh4_sched_serialize(ser, sched);
	ser << nmi_pending;
	ser << nmi_visible;
	ser << nmi_level;
	sh4_sched_serialize(ser, nmi_sched);
}

void interrupts_deserialize(Deserializer& deser)
{
	deser >> InterruptEnvId;
	deser >> InterruptBit;
	deser >> InterruptLevelBit;
	deser >> interrupt_vpend;
	if (deser.version() >= Deserializer::V67)
		deser >> interrupt_visible_vpend;
	else
		interrupt_visible_vpend = interrupt_vpend;
	deser >> interrupt_vmask;
	deser >> decoded_srimask;
	if (deser.version() >= Deserializer::V67)
		for (int sched : interrupt_sched)
			sh4_sched_deserialize(deser, sched);
	else
		for (int sched : interrupt_sched)
			sh4_sched_request(sched, -1);
	if (deser.version() >= Deserializer::V125)
	{
		deser >> nmi_pending;
		deser >> nmi_visible;
		deser >> nmi_level;
		sh4_sched_deserialize(deser, nmi_sched);
	}
	else
	{
		nmi_pending = false;
		nmi_visible = false;
		nmi_level = INTC_ICR.NMIL;
		sh4_sched_request(nmi_sched, -1);
	}
	recalc_pending_itrs();
}
