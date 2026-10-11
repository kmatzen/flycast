//ubc is disabled on dreamcast and can't be used ... but kos-debug uses it !...

#include "hw/hwreg.h"
#include "hw/sh4/sh4_mmr.h"
#include "modules.h"
#include "emulator.h"
#include "serialize.h"

UBCRegisters ubc;
static bool sequenceArmed;

static bool ubcClockRunning()
{
	// STBCR2.MSTP5 stops the clock supplied to the UBC. Its registers retain
	// their values, but the controller cannot observe bus cycles while stopped.
	return (CPG_STBCR2 & 1) == 0;
}

static u32 addressMask(u8 bamr)
{
	// BAMA2 is bit 3 while BAMA1:BAMA0 are bits 1:0.
	switch (((bamr & 8) >> 1) | (bamr & 3))
	{
	case 0: return 0;
	case 1: return 0x000003ff; // 1-Kbyte page offset
	case 2: return 0x00000fff; // 4-Kbyte page offset
	case 4: return 0x0000ffff; // 64-Kbyte page offset
	case 5: return 0x000fffff; // 1-Mbyte page offset
	default: return 0xffffffff; // address excluded from the conditions
	}
}

static bool channelAddressMatches(u32 address, u32 bar, u8 bamr, u8 breakAsid,
		u32 accessMask = 0)
{
	const u32 mask = addressMask(bamr) | accessMask;
	if (((address ^ bar) & ~mask) != 0)
		return false;
	// BASM=1 excludes the ASID from the comparison conditions.
	return (bamr & 4) != 0 || CCN_PTEH.ASID == breakAsid;
}

static bool instructionChannelMatches(u32 address, u32 bar, u8 bamr, u16 bbr,
		u8 breakAsid)
{
	const unsigned access = (bbr >> 4) & 3;
	const unsigned direction = (bbr >> 2) & 3;
	const unsigned size = ((bbr & 0x40) >> 4) | (bbr & 3);
	if ((access & 1) == 0 || (direction & 1) == 0)
		return false; // instruction access and read must both be selected
	if (size != 0 && size != 2)
		return false; // instruction fetches are word accesses

	return channelAddressMatches(address, bar, bamr, breakAsid);
}

static bool operandChannelMatches(u32 address, u32 size, bool write, u64 data,
		bool hasData, u32 bar, u8 bamr, u16 bbr, u8 breakAsid, bool channelB)
{
	const unsigned access = (bbr >> 4) & 3;
	const unsigned direction = (bbr >> 2) & 3;
	const unsigned configuredSize = ((bbr & 0x40) >> 4) | (bbr & 3);
	const unsigned actualSize = size == 1 ? 1 : size == 2 ? 2
			: size == 4 ? 3 : size == 8 ? 4 : 7;
	if ((access & 2) == 0 || (direction & (write ? 2 : 1)) == 0)
		return false;
	if (configuredSize != 0 && configuredSize != actualSize)
		return false;
	if (!channelAddressMatches(address, bar, bamr, breakAsid, size - 1))
		return false;

	if (!channelB || (UBC_BRCR & 0x80) == 0)
		return true;
	// Accesses explicitly documented as having no data cannot satisfy DBEB.
	if (!hasData)
		return false;
	u32 normalizedData;
	u32 comparedBits;
	if (size == 1)
	{
		// The byte must be programmed identically into BDRB[15:8] and
		// BDRB[7:0]. Bits 31:16 do not participate in the comparison.
		normalizedData = (u8)data * 0x0101u;
		comparedBits = 0x0000ffff;
	}
	else if (size == 2)
	{
		normalizedData = (u16)data;
		comparedBits = 0x0000ffff;
	}
	else
	{
		normalizedData = (u32)data;
		comparedBits = 0xffffffff;
	}
	const u32 compareMask = comparedBits & ~UBC_BDMRB;
	const auto matchesData = [compareMask](u32 value) {
		return ((value ^ UBC_BDRB) & compareMask) == 0;
	};
	// A quadword is treated as two independent 32-bit data units. Either unit
	// satisfying the condition produces a channel-B match.
	return matchesData(normalizedData)
			|| (size == 8 && matchesData((u32)(data >> 32)));
}

bool UBC_InstructionBreakEnabled()
{
	return ubcClockRunning()
			&& (((UBC_BBRA >> 4) & 1) != 0 || ((UBC_BBRB >> 4) & 1) != 0);
}

bool UBC_BreakEnabled()
{
	return ubcClockRunning()
			&& (((UBC_BBRA >> 4) & 3) != 0 || ((UBC_BBRB >> 4) & 3) != 0);
}

bool UBC_SequenceArmed()
{
	return sequenceArmed;
}

void UBC_RestoreSequenceArmed(bool armed)
{
	sequenceArmed = armed;
}

static u32 instructionMatches(u32 address, bool afterExecution)
{
	const bool matchA = ((UBC_BRCR >> 10) & 1) == afterExecution
			&& instructionChannelMatches(address, UBC_BARA, UBC_BAMRA,
					UBC_BBRA, CCN_BASRA);
	const bool matchB = ((UBC_BRCR >> 6) & 1) == afterExecution
			&& instructionChannelMatches(address, UBC_BARB, UBC_BAMRB,
					UBC_BBRB, CCN_BASRB);
	return (matchA ? 1 : 0) | (matchB ? 2 : 0);
}

bool UBC_InstructionBreakPending(u32 address, bool afterExecution)
{
	if (!ubcClockRunning())
		return false;
	const u32 matches = instructionMatches(address, afterExecution);
	if ((UBC_BRCR & 8) == 0)
		return matches != 0;
	return (matches & 2) != 0 && sequenceArmed;
}

static bool applyMatches(u32 matches)
{
	const bool matchA = (matches & 1) != 0;
	const bool matchB = (matches & 2) != 0;
	if ((UBC_BRCR & 8) == 0)
	{
		sequenceArmed = false;
		if (matchA)
			UBC_BRCR |= 0x8000;
		if (matchB)
			UBC_BRCR |= 0x4000;
		return matchA || matchB;
	}

	// Sequential mode has a private A-matched latch; CMFA remains clear. Any B
	// match reinitializes that latch, and only B following a previous A sets
	// CMFB and requests the user-break exception.
	const bool complete = matchB && sequenceArmed;
	if (matchB)
		sequenceArmed = false;
	else if (matchA)
		sequenceArmed = true;
	if (complete)
		UBC_BRCR |= 0x4000;
	return complete;
}

bool UBC_InstructionBreak(u32 address, bool afterExecution)
{
	if (!ubcClockRunning())
		return false;
	return applyMatches(instructionMatches(address, afterExecution));
}

bool UBC_OperandBreak(u32 address, u32 size, bool write, u64 data, bool hasData)
{
	if (!ubcClockRunning())
		return false;
	const bool matchA = operandChannelMatches(address, size, write, data, hasData,
			UBC_BARA, UBC_BAMRA, UBC_BBRA, CCN_BASRA, false);
	const bool matchB = operandChannelMatches(address, size, write, data, hasData,
			UBC_BARB, UBC_BAMRB, UBC_BBRB, CCN_BASRB, true);
	return applyMatches((matchA ? 1 : 0) | (matchB ? 2 : 0));
}

template<u32 index>
static void writeBbr(u32, u16 data)
{
	UBC[index] = data & 0x007f;
	// Cached blocks compiled while instruction matching was disabled contain no
	// per-instruction UBC boundary. Rebuild them whenever either BBR changes.
	emu.getSh4Executor()->ResetCache();
}

static void writeBrcr(u32, u16 data)
{
	UBC_BRCR = data & 0xc4c9;
	if ((UBC_BRCR & 8) == 0)
		sequenceArmed = false;
}

void UBCRegisters::init()
{
	super::init();

	//UBC BARA 0xFF200000 0x1F200000 32 Undefined Held Held Held Iclk
	setRW<UBC_BARA_addr>();

	//UBC BAMRA 0xFF200004 0x1F200004 8 Undefined Held Held Held Iclk
	setRW<UBC_BAMRA_addr, u8, 0x0f>();

	//UBC BBRA 0xFF200008 0x1F200008 16 0x0000 Held Held Held Iclk
	setWriteHandler<UBC_BBRA_addr, u16>(writeBbr<2>);

	//UBC BARB 0xFF20000C 0x1F20000C 32 Undefined Held Held Held Iclk
	setRW<UBC_BARB_addr>();

	//UBC BAMRB 0xFF200010 0x1F200010 8 Undefined Held Held Held Iclk
	setRW<UBC_BAMRB_addr, u8, 0x0f>();

	//UBC BBRB 0xFF200014 0x1F200014 16 0x0000 Held Held Held Iclk
	setWriteHandler<UBC_BBRB_addr, u16>(writeBbr<5>);

	//UBC BDRB 0xFF200018 0x1F200018 32 Undefined Held Held Held Iclk
	setRW<UBC_BDRB_addr>();

	//UBC BDMRB 0xFF20001C 0x1F20001C 32 Undefined Held Held Held Iclk
	setRW<UBC_BDMRB_addr>();

	//UBC BRCR 0xFF200020 0x1F200020 16 0x0000 Held Held Held Iclk
	setWriteHandler<UBC_BRCR_addr, u16>(writeBrcr);

	reset(true);
}

void UBCRegisters::reset(bool hard)
{
	// Every UBC register is held across manual reset. Internal sequential-match
	// progress is not an architecturally visible register; reset starts a new
	// A-then-B observation sequence.
	if (hard)
		super::reset();
	sequenceArmed = false;
}

void UBCRegisters::serialize(Serializer& ser)
{
	ser << sequenceArmed;
}

void UBCRegisters::deserialize(Deserializer& deser)
{
	if (deser.version() >= Deserializer::V133)
		deser >> sequenceArmed;
	else
		sequenceArmed = false;
}
