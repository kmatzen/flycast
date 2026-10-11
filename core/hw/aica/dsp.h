#pragma once
#include "types.h"
#include "serialize.h"

namespace aica::dsp
{

constexpr s64 arithmeticShiftRight(s64 value, u32 bits)
{
	if (bits == 0)
		return value;
	const u64 divisor = 1ull << bits;
	if (value >= 0)
		return value / static_cast<s64>(divisor);
	const u64 magnitude = static_cast<u64>(-(value + 1)) + 1;
	const u64 quotient = magnitude / divisor;
	return -static_cast<s64>(quotient) - (magnitude % divisor != 0);
}

constexpr s32 wrapAccumulator(s64 value)
{
	// The MAC output is a non-saturating signed 26-bit register.
	u32 bits = static_cast<u32>(value) & 0x03ffffff;
	return (bits & 0x02000000) != 0 ? static_cast<s32>(bits | 0xfc000000) : static_cast<s32>(bits);
}

struct DSPState
{
	// buffered DSP state
	s32 TEMP[128];	// 24 bits
	s32 MEMS[32];	// 24 bits
	s32 MIXS[16];	// 20 bits
	
	// RBL/RBP (decoded, from aica common regs)
	u32 RBP;
	u32 RBL;

	u32 MDEC_CT;

	// volatile dsp regs
	int SHIFTED;	// 24 bit
	int B;			// 26 bit
	int MEMVAL[4];
	u32 MWT_ADDR;
	u32 MWT_DATA;
	u32 MWT_PENDING;
	int FRC_REG;	// 13 bit
	int Y_REG;		// 24 bit
	u32 ADRS_REG;	// 12 bit

	bool stopped;	// DSP program is a no-op
	bool dirty;		// DSP program has changed

	void serialize(Serializer& ser)
	{
		ser << TEMP;
		ser << MEMS;
		ser << MIXS;
		ser << RBP;
		ser << RBL;
		ser << MDEC_CT;
		ser << MEMVAL;
		ser << MWT_ADDR;
		ser << MWT_DATA;
		ser << MWT_PENDING;
	}

	void deserialize(Deserializer& deser)
	{
		deser.skip(4096 * 8, Deserializer::V18);	// DynCode
		deser >> TEMP;
		deser >> MEMS;
		deser >> MIXS;
		deser >> RBP;
		deser >> RBL;
		deser.skip(44, Deserializer::V18);
		deser >> MDEC_CT;
		deser.skip(33596 - 4096 * 8 - sizeof(TEMP) - sizeof(MEMS) - sizeof(MIXS) - 4 * 3 - 44,
				Deserializer::V18);	// other dsp stuff
		if (deser.version() >= Deserializer::V61)
			deser >> MEMVAL;
		else
			memset(MEMVAL, 0, sizeof(MEMVAL));
		if (deser.version() >= Deserializer::V66)
		{
			deser >> MWT_ADDR;
			deser >> MWT_DATA;
			deser >> MWT_PENDING;
		}
		else
		{
			MWT_ADDR = 0;
			MWT_DATA = 0;
			MWT_PENDING = 0;
		}
		if (!deser.rollback())
			dirty = true;
	}
};

extern DSPState state;

void init();
void term();
void step();
void bypassStep();
void writeProg(u32 addr);

void recInit();
void recTerm();
void runStep();
void recompile();

struct Instruction
{
	u8 TRA;
	bool TWT;
	u8 TWA;

	bool XSEL;
	u8 YSEL;
	u8 IRA;
	bool IWT;
	u8 IWA;

	bool EWT;
	u8 EWA;
	bool ADRL;
	bool FRCL;
	u8 SHIFT;
	bool YRL;
	bool NEGB;
	bool ZERO;
	bool BSEL;

	bool NOFL;  //MRQ set
	bool TABLE; //MRQ set
	bool MWT;   //MRQ set
	bool MRD;   //MRQ set
	u8 MASA;    //MRQ set
	bool ADREB; //MRQ set
	bool NXADR; //MRQ set
};

void DecodeInst(const u32 *IPtr, Instruction *i);
u16 DYNACALL PACK(s32 val);
s32 DYNACALL UNPACK(u16 val);
void DYNACALL commitMemoryWrite();
void advanceRingCounter();
bool memoryAccessAllowed();

} // namespace aica::dsp
