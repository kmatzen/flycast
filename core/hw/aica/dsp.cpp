#include "dsp.h"
#include "aica.h"
#include "aica_if.h"
#include "hw/hwreg.h"
/*
	DSP rec_v1

	Tries to emulate a guesstimation of the aica dsp, by directly emitting x86 opcodes.

	This was my first dsp implementation, as implemented for nullDC 1.0.3. 
	
	This was derived from a schematic I drew for the dsp, based on 
	liberal interpretation of known specs, the saturn dsp, digital 
	electronics assumptions, as well as "best-fitted" my typical 
	test game suite.


	Initial code by skmp, now part of the reicast project.
	See LICENSE & COPYRIGHT files further details
*/
namespace aica::dsp
{

DSPState state;

//float format is ?
u16 DYNACALL PACK(s32 val)
{
	u32 bits = static_cast<u32>(val);
	int sign = (bits >> 23) & 0x1;
	u32 temp = (bits ^ (bits << 1)) & 0xFFFFFF;
	int exponent = 0;
	for (int k = 0; k < 12; k++)
	{
		if (temp & 0x800000)
			break;
		temp <<= 1;
		exponent += 1;
	}
	if (exponent < 12)
		bits = (bits << exponent) & 0x3FFFFF;
	else
		bits <<= 11;
	bits = (bits >> 11) & 0x7FF;
	bits |= sign << 15;
	bits |= exponent << 11;

	return (u16)bits;
}

s32 DYNACALL UNPACK(u16 val)
{
	int sign = (val >> 15) & 0x1;
	int exponent = (val >> 11) & 0xF;
	int mantissa = val & 0x7FF;
	u32 uval = mantissa << 11;
	if (exponent > 11)
	{
		exponent = 11;		// cap exponent to 11 for denormals
		uval |= sign << 22;
	}
	else
		uval |= (sign ^ 1) << 22;
	uval |= sign << 23;		// actual sign bit
	s32 signedValue = (uval & 0x800000) != 0
			? static_cast<s32>(uval | 0xFF000000)
			: static_cast<s32>(uval);

	return static_cast<s32>(arithmeticShiftRight(signedValue, exponent));
}

void DYNACALL commitMemoryWrite()
{
	if (state.MWT_PENDING != 0)
	{
		if (memoryAccessAllowed())
			WriteMemArr(&aica_ram[0], state.MWT_ADDR & soundRamMask(),
					static_cast<u16>(state.MWT_DATA));
		state.MWT_PENDING = 0;
	}
}

bool memoryAccessAllowed()
{
	return (CommonData->MRWINH & 1) == 0;
}

void advanceRingCounter()
{
	--state.MDEC_CT;
	if (state.MDEC_CT == 0)
		state.MDEC_CT = state.RBL + 1;
}

void DecodeInst(const u32 *IPtr, Instruction *i)
{
	i->TRA = (IPtr[0] >> 9) & 0x7F;
	i->TWT = IPtr[0] & 0x100;
	i->TWA = (IPtr[0] >> 1) & 0x7F;

	i->XSEL = IPtr[1] & 0x8000;
	i->YSEL = (IPtr[1] >> 13) & 3;
	i->IRA = (IPtr[1] >> 7) & 0x3F;
	i->IWT = IPtr[1] & 0x40;
	i->IWA = (IPtr[1] >> 1) & 0x1F;

	i->TABLE = IPtr[2] & 0x8000;
	// MRWINH bit 0 removes DSP sound-memory requests from the bus. JIT
	// backends decode these flags while compiling, and writes to MRWINH mark
	// the program dirty so a changed gate is reflected on the next sample.
	i->MWT = (IPtr[2] & 0x4000) && memoryAccessAllowed();
	i->MRD = (IPtr[2] & 0x2000) && memoryAccessAllowed();
	i->EWT = IPtr[2] & 0x1000;
	i->EWA = (IPtr[2] >> 8) & 0x0F;
	i->ADRL = IPtr[2] & 0x80;
	i->FRCL = IPtr[2] & 0x40;
	i->SHIFT = (IPtr[2] >> 4) & 3;
	i->YRL = IPtr[2] & 8;
	i->NEGB = IPtr[2] & 4;
	i->ZERO = IPtr[2] & 2;
	i->BSEL = IPtr[2] & 1;

	i->NOFL = IPtr[3] & 0x8000;
	i->MASA = (IPtr[3] >> 9) & 0x3f;
	i->ADREB = IPtr[3] & 0x100;
	i->NXADR = IPtr[3] & 0x80;
}

#if FEAT_DSPREC == DYNAREC_NONE
void recInit() {
}

void recTerm() {
}

void recompile() {
}
#endif

void init()
{
	memset(&state, 0, sizeof(state));
	// Keep the decoded address generator in sync with the visible reset
	// registers. RBL=0 selects an 8-Kword ring; the historical 32-Kword
	// default addressed the wrong quarter of the ring until the guest wrote it.
	state.RBL = (8192 << CommonData->RBL) - 1;
	state.RBP = (CommonData->RBP * 2048) & soundRamMask();
	state.MDEC_CT = 1;
	state.dirty = true;

	recInit();
}

void writeProg(u32 addr)
{
	if (addr >= 0x3400 && addr < 0x3C00)
		state.dirty = true;
}

void term()
{
	state.stopped = true;
	recTerm();
}

void step()
{
	if (state.dirty)
	{
		state.dirty = false;
		state.stopped = true;
		for (u32 instr : DSPData->MPRO)
			if (instr != 0)
			{
				state.stopped = false;
				break;
			}
		if (!state.stopped)
			recompile();
	}
	// Effect registers accumulate contributions within one DSP sample only.
	memset(DSPData->EFREG, 0, sizeof(DSPData->EFREG));
	// Complete a step-127 memory write even if the program was stopped or
	// replaced before the next sample begins.
	commitMemoryWrite();
	if (state.stopped)
	{
		// The DSP clock and ring pointer keep running even when every MPRO
		// instruction is zero; only execution can be skipped.
		advanceRingCounter();
		return;
	}
	runStep();
}

void bypassStep()
{
	// Disabling DSP effects is an emulator-side output shortcut, not a
	// hardware clock gate. Preserve sample-boundary state and outstanding
	// sound-memory timing while omitting microprogram execution.
	memset(DSPData->EFREG, 0, sizeof(DSPData->EFREG));
	commitMemoryWrite();
	advanceRingCounter();
}

} // namespace aica::dsp
