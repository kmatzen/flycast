//
// Audio Overload SDK
//
// Copyright (c) 2007-2009 R. Belmont and Richard Bannister, and others.
// All rights reserved.
//

#include "build.h"

#if FEAT_DSPREC != DYNAREC_JIT

#include "dsp.h"
#include "aica.h"
#include "aica_if.h"
#include "hw/hwreg.h"

namespace aica
{

namespace dsp
{

void runStep()
{
	if (state.stopped)
		return;

	s32 ACC = 0;		//26 bit
	s32 SHIFTED = 0;	//24 bit
	s32 X = 0;			//24 bit
	s32 Y = 0;			//13 bit
	s32 B = 0;			//26 bit
	s32 INPUTS = 0;		//24 bit
	s32 FRC_REG = 0;	//13 bit
	s32 Y_REG = 0;		//24 bit
	u32 ADRS_REG = 0;	// 12 bit

	for (int step = 0; step < 128; ++step)
	{
		// MWT executes in the sound-memory cycle following the request step.
		commitMemoryWrite();

		u32 *IPtr = DSPData->MPRO + step * 4;

		if (IPtr[0] == 0 && IPtr[1] == 0 && IPtr[2] == 0 && IPtr[3] == 0)
		{
			// Empty instruction shortcut
			X = state.TEMP[state.MDEC_CT & 0x7F];
			Y = FRC_REG;

			ACC = wrapAccumulator(arithmeticShiftRight((s64)X * (s64)Y, 12) + X);

			continue;
		}

		u32 TRA = (IPtr[0] >> 9) & 0x7F;
		bool TWT = IPtr[0] & 0x100;

		bool XSEL = IPtr[1] & 0x8000;
		u32 YSEL = (IPtr[1] >> 13) & 3;
		u32 IRA = (IPtr[1] >> 7) & 0x3F;
		bool IWT = IPtr[1] & 0x40;

		bool EWT = IPtr[2] & 0x1000;
		bool ADRL = IPtr[2] & 0x80;
		bool FRCL = IPtr[2] & 0x40;
		u32 SHIFT = (IPtr[2] >> 4) & 3;
		bool YRL = IPtr[2] & 8;
		bool NEGB = IPtr[2] & 4;
		bool ZERO = IPtr[2] & 2;
		bool BSEL = IPtr[2] & 1;

		u32 COEF = step;

		// operations are done at 24 bit precision

		// INPUTS RW
		if (IRA <= 0x1f)
			INPUTS = state.MEMS[IRA];
		else if (IRA <= 0x2F)
			INPUTS = state.MIXS[IRA - 0x20] * 16;		// MIXS is 20 bit
		else if (IRA <= 0x31)
			INPUTS = static_cast<s16>(DSPData->EXTS[IRA - 0x30]) * 256;	// EXTS is 16 bits
		else
			INPUTS = 0;

		if (IWT)
		{
			u32 IWA = (IPtr[1] >> 1) & 0x1F;
			state.MEMS[IWA] = state.MEMVAL[step & 3];	// MEMVAL was selected in previous MRD
		}

		// Operand sel
		// B
		if (!ZERO)
		{
			if (BSEL)
				B = ACC;
			else
				B = state.TEMP[(TRA + state.MDEC_CT) & 0x7F];
			if (NEGB)
				B = -B;
		}
		else
		{
			B = 0;
		}

		// X
		if (XSEL)
			X = INPUTS;
		else
			X = state.TEMP[(TRA + state.MDEC_CT) & 0x7F];

		// Y
		if (YSEL == 0)
			Y = FRC_REG;
		else if (YSEL == 1)
			Y = static_cast<s32>(arithmeticShiftRight((s16)DSPData->COEF[COEF], 3));	//COEF is 16 bits
		else if (YSEL == 2)
			Y = static_cast<s32>(arithmeticShiftRight(Y_REG, 11));
		else if (YSEL == 3)
			Y = static_cast<s32>(arithmeticShiftRight(Y_REG, 4)) & 0x0FFF;

		if (YRL)
			Y_REG = INPUTS;

		// Shifter
		// There's a 1-step delay at the output of the X*Y + B adder. So we use the ACC value from the previous step.
		if (SHIFT == 0 || SHIFT == 3)
			SHIFTED = ACC;
		else
			SHIFTED = ACC * 2;		// x2 scale

		if (SHIFT < 2)
			SHIFTED = std::min(std::max(SHIFTED, -0x00800000), 0x007FFFFF);
		else
		{
			// Non-saturating modes still have a signed 24-bit output bus.
			SHIFTED &= 0x00FFFFFF;
			if (SHIFTED & 0x00800000)
				SHIFTED |= 0xFF000000;
		}

		// ACCUM
		ACC = wrapAccumulator(arithmeticShiftRight((s64)X * (s64)Y, 12) + B);

		if (TWT)
		{
			u32 TWA = (IPtr[0] >> 1) & 0x7F;
			state.TEMP[(TWA + state.MDEC_CT) & 0x7F] = SHIFTED;
		}

		if (FRCL)
		{
			if (SHIFT == 3)
				FRC_REG = SHIFTED & 0x0FFF;
			else
				FRC_REG = static_cast<s32>(arithmeticShiftRight(SHIFTED, 11));
		}

		if ((step & 1) && memoryAccessAllowed())
		{
			bool MWT = IPtr[2] & 0x4000;
			bool MRD = IPtr[2] & 0x2000;

			if (MRD || MWT)
			{
				bool TABLE = IPtr[2] & 0x8000;

				bool NOFL = IPtr[3] & 0x8000;
				u32 MASA = (IPtr[3] >> 9) & 0x3f;
				bool ADREB = IPtr[3] & 0x100;
				bool NXADR = IPtr[3] & 0x80;

				u32 ADDR = DSPData->MADRS[MASA];
				if (ADREB)
					ADDR += ADRS_REG & 0x0FFF;
				if (NXADR)
					ADDR++;
				if (!TABLE)
				{
					ADDR += state.MDEC_CT;
					ADDR &= state.RBL;		// RBL is ring buffer length - 1
				}
				else
					ADDR &= 0xFFFF;

				ADDR <<= 1;					// Word -> byte address
				ADDR += state.RBP;			// RBP is already a byte address
				if (MRD)			// memory only allowed on odd. DoA inserts NOPs on even
				{
					const u16 memoryValue = ReadMemArr<u16>(&aica_ram[0],
							ADDR & soundRamMask());
					if (NOFL)
						state.MEMVAL[(step + 2) & 3]
								= static_cast<s16>(memoryValue) * 256;
					else
						state.MEMVAL[(step + 2) & 3] = UNPACK(memoryValue);
				}
				if (MWT)
				{
					state.MWT_ADDR = ADDR;
					state.MWT_DATA = NOFL ? static_cast<u16>(arithmeticShiftRight(SHIFTED, 8)) : PACK(SHIFTED);
					state.MWT_PENDING = 1;
				}
			}
		}

		if (ADRL)
		{
			if (SHIFT == 3)
				ADRS_REG = static_cast<u32>(arithmeticShiftRight(SHIFTED, 12));
			else
				ADRS_REG = static_cast<u32>(arithmeticShiftRight(INPUTS, 16));
		}

		if (EWT)
		{
			u32 EWA = (IPtr[2] >> 8) & 0x0F;
			DSPData->EFREG[EWA] = static_cast<u32>(arithmeticShiftRight(SHIFTED, 8));
		}

	}
	advanceRingCounter();
}

} // namespace dsp
} // namespace aica
#endif
