#include "aica.h"
#include "aica_if.h"
#include "aica_mem.h"
#include "dsp.h"
#include "sgc_if.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/arm7/arm7.h"
#include "hw/arm7/arm_mem.h"
#include "serialize.h"

namespace aica
{

#define SH4_IRQ_BIT (1 << (holly_SPU_IRQ & 31))

std::deque<u8> midiSendBuffer;
std::deque<u8> midiOutputBuffer;

//Interrupts
//arm side
static u32 GetL(u32 which)
{
	if (which > 7)
		which = 7; //higher bits share bit 7

	u32 bit = 1 << which;
	u32 rv = 0;

	if (CommonData->SCILV0 & bit)
		rv = 1;

	if (CommonData->SCILV1 & bit)
		rv |= 2;
	
	if (CommonData->SCILV2 & bit)
		rv |= 4;

	return rv;
}

static void update_arm_interrupts()
{
	u32 p_ints=SCIEB->full & SCIPD->full;

	u32 Lval=0;
	if (p_ints)
	{
		u32 bit_value=1;
		// Interrupt factors have fixed source priority. SCILV supplies the
		// autovector level for the selected source; level zero disables delivery,
		// so continue looking for the next pending factor in that case.
		for (u32 i=0;i<11;i++)
		{
			if (p_ints & bit_value)
			{
				Lval = GetL(i);
				if (Lval != 0)
					break;
			}
			bit_value<<=1; //next bit
		}
	}

	// Level zero is explicitly disabled even when its pending and enable bits
	// are both set.
	arm::interruptChange(Lval == 0 ? 0 : p_ints,Lval);
}

//sh4 side
static bool UpdateSh4Ints()
{
	u32 p_ints = MCIEB->full & MCIPD->full;
	if (p_ints)
	{
		if ((SB_ISTEXT & SH4_IRQ_BIT) == 0)
			// if no interrupt is already pending then raise one
			asic_RaiseInterrupt(holly_SPU_IRQ);
		return true;
	}
	else
	{
		if ((SB_ISTEXT & SH4_IRQ_BIT) != 0)
			asic_CancelInterrupt(holly_SPU_IRQ);
		return false;
	}
}

AicaTimer timers[3];
int aica_schid = -1;
static SampleClock sampleClock;
int internalDmaSchedId = -1;
int midiOutSchedId = -1;

struct InternalDmaState
{
	bool active = false;
	bool gate = false;
	bool toSoundMemory = false;
	bool soundMemoryAccessAllowed = false;
	u32 soundAddress = 0;
	u32 registerAddress = 0;
	u32 remaining = 0;
	u32 cyclePhase = 0;
};
static InternalDmaState internalDma;
// The AICA memory interface runs at twice the 33.8688 MHz GD clock.
static constexpr u32 AicaMemoryClock = GD_CLOCK * 2;

static int nextInternalDmaWordCycles()
{
	const u64 scaled = (u64)SH4_MAIN_CLOCK + internalDma.cyclePhase;
	internalDma.cyclePhase = scaled % AicaMemoryClock;
	return scaled / AicaMemoryClock;
}

u32 sampleClockPhase()
{
	return sampleClock.phase();
}

void setSampleClockPhase(u32 phase)
{
	sampleClock.setPhase(phase);
}

static int AicaUpdate(int tag, int cycles, int jitter, void *arg)
{
	int remainingJitter = std::max(0, jitter);
	for (;;)
	{
		arm::run(1);
		const int nextCycles = sampleClock.nextCycles();
		if (nextCycles > remainingJitter)
			// handle_cb() subtracts the original jitter from this value.
			return std::max(0, jitter) + nextCycles - remainingJitter;
		remainingJitter -= nextCycles;
	}
}

//Mainloop

void timeStep()
{
	for (auto& timer : timers)
		timer.StepTimer(1);

	SCIPD->SAMPLE_DONE = 1;
	MCIPD->SAMPLE_DONE = 1;

	sgc::AICA_Sample();

	//Make sure sh4/arm interrupt system is up to date
	update_arm_interrupts();
	UpdateSh4Ints();	
}

static void finishInternalDma()
{
	internalDma.active = false;
	CommonData->DEXE = 0;
	MCIPD->DMA_END = 1;
	UpdateSh4Ints();
	SCIPD->DMA_END = 1;
	update_arm_interrupts();
}

static int internalDmaStep(int tag, int cycles, int jitter, void *arg)
{
	if (!internalDma.active)
		return 0;

	auto transferEntry = []()
	{
		// MRWINH is a live bus-owner gate, not a DMA parameter latched by
		// DEXE. Sample it at each memory request boundary.
		internalDma.soundMemoryAccessAllowed =
				mainCpuSoundMemoryAccessAllowed(CommonData->MRWINH);
		if (internalDma.gate)
		{
			if (internalDma.toSoundMemory)
			{
				if (internalDma.soundMemoryAccessAllowed)
					WriteMemArr(&aica_ram[0], internalDma.soundAddress, (u16)0);
			}
			else if (!isInternalDmaParameterRegister(internalDma.registerAddress))
				writeAicaReg(internalDma.registerAddress, (u16)0);
		}
		else if (internalDma.soundMemoryAccessAllowed)
		{
			if (internalDma.toSoundMemory)
				WriteMemArr(&aica_ram[0], internalDma.soundAddress,
						readAicaReg<u16>(internalDma.registerAddress));
			else if (!isInternalDmaParameterRegister(internalDma.registerAddress))
				writeAicaReg(internalDma.registerAddress,
						ReadMemArr<u16>(&aica_ram[0], internalDma.soundAddress));
		}

		// DMA payloads are 16-bit words, while the internal register file is
		// arranged as one entry every four bytes. Both addresses advance by one
		// sparse register-file entry, leaving the other sound-memory halfword alone.
		internalDma.soundAddress += 4;
		internalDma.registerAddress += 4;
		if (--internalDma.remaining == 0)
		{
			finishInternalDma();
			return false;
		}
		return true;
	};

	if (!transferEntry())
		return 0;

	// A scheduler dispatch can arrive several AICA memory clocks late. Apply
	// every transfer whose deadline has already elapsed instead of stretching
	// the DMA over future SH4 instructions. handle_cb() subtracts the original
	// jitter from our return value, so add it back after consuming the elapsed
	// deadlines and return the true time remaining to the next entry.
	int remainingJitter = jitter;
	for (;;)
	{
		const int nextCycles = nextInternalDmaWordCycles();
		if (nextCycles > remainingJitter)
			return jitter + nextCycles - remainingJitter;
		remainingJitter -= nextCycles;
		if (!transferEntry())
			return 0;
	}
}

static void AicaInternalDMA()
{
	if (!CommonData->DEXE || internalDma.active)
		return;

	DEBUG_LOG(AICA, "AICA internal DMA: DGATE %d DDIR %d DLG %x", CommonData->DGATE, CommonData->DDIR, CommonData->DLG);
	internalDma.active = true;
	internalDma.gate = CommonData->DGATE;
	internalDma.toSoundMemory = CommonData->DDIR;
	internalDma.soundMemoryAccessAllowed =
			mainCpuSoundMemoryAccessAllowed(CommonData->MRWINH);
	const InternalDmaGeometry geometry = internalDmaGeometry(CommonData->DMEA_hi,
			CommonData->DMEA_lo, CommonData->DRGA, CommonData->DLG, soundRamMask());
	internalDma.soundAddress = geometry.soundAddress;
	internalDma.registerAddress = geometry.registerAddress;
	internalDma.remaining = geometry.transfers;
	// Bias the phase so each observable deadline is ceil(n * SH4/AICA),
	// avoiding a transfer becoming visible before its memory clock edge.
	internalDma.cyclePhase = AicaMemoryClock - 1;
	if (internalDma.toSoundMemory || !internalDma.gate) {
		internalDma.remaining = std::min(internalDma.remaining, geometry.transfersToRamEnd);
	}
	if (internalDma.remaining == 0) {
		finishInternalDma();
		return;
	}
	sh4_sched_request(internalDmaSchedId, nextInternalDmaWordCycles());
}

void serializeInternalDma(Serializer& ser)
{
	ser << internalDma.active << internalDma.gate << internalDma.toSoundMemory
			<< internalDma.soundMemoryAccessAllowed << internalDma.soundAddress
			<< internalDma.registerAddress << internalDma.remaining
			<< internalDma.cyclePhase;
}

void deserializeInternalDma(Deserializer& deser)
{
	if (deser.version() >= Deserializer::V111)
		deser >> internalDma.active >> internalDma.gate >> internalDma.toSoundMemory
				>> internalDma.soundMemoryAccessAllowed >> internalDma.soundAddress
				>> internalDma.registerAddress >> internalDma.remaining
				>> internalDma.cyclePhase;
	else
		internalDma = {};
}

//Memory i/o
template<typename T>
void writeTimerAndIntReg(u32 reg, T data)
{
	const u32 alignedReg = reg & ~1u;
	u32 writeBits = data;
	u32 writeMask = 0xffffffff;
	if constexpr (sizeof(T) == 1)
	{
		const u32 shift = (reg & 1) * 8;
		writeBits <<= shift;
		writeMask = 0xff << shift;
	}
	else if constexpr (sizeof(T) == 2)
	{
		writeMask = 0xffff;
	}
	auto writeMasked16 = [&](u16 implementedBits)
	{
		u16 value = ReadMemArr<u16>(aica_reg, alignedReg);
		value = (value & ~static_cast<u16>(writeMask))
				| static_cast<u16>(writeBits & writeMask);
		WriteMemArr(aica_reg, alignedReg, static_cast<u16>(value & implementedBits));
	};

	switch (alignedReg)
	{
	case SCIEB_addr:
		SCIEB->full = ((SCIEB->full & ~writeMask) | (writeBits & writeMask)) & 0x7ff;
		update_arm_interrupts();
		break;

	case SCIPD_addr:
		// other bits are read-only
		if (writeBits & (1 << 5))
		{
			SCIPD->SCPU = 1;
			update_arm_interrupts();
		}
		break;

	case SCIRE_addr:
		SCIPD->full &= ~writeBits;
		update_arm_interrupts();
		break;

	case MCIEB_addr:
		MCIEB->full = ((MCIEB->full & ~writeMask) | (writeBits & writeMask)) & 0x7ff;
		if (UpdateSh4Ints())
			arm::avoidRaceCondition();
		break;

	case MCIPD_addr:
		// other bits are read-only
		if (writeBits & (1 << 5))
		{
			MCIPD->SCPU = 1;
			if (UpdateSh4Ints())
				arm::avoidRaceCondition();
		}
		break;

	case MCIRE_addr:
		MCIPD->full &= ~writeBits;
		UpdateSh4Ints();
		break;

	case TIMER_A:
		writeMasked16(0x07ff);
		timers[0].RegisterWrite();
		break;

	case TIMER_B:
		writeMasked16(0x07ff);
		timers[1].RegisterWrite();
		break;

	case TIMER_C:
		writeMasked16(0x07ff);
		timers[2].RegisterWrite();
		break;

	case SCILV0_addr:
	case SCILV1_addr:
	case SCILV2_addr:
		writeMasked16(0x00ff);
		update_arm_interrupts();
		break;

	case 0x2880: // MRWINH, DMEA[22:16]
		{
			const bool oldDspInhibit = (CommonData->MRWINH & 1) != 0;
			writeMasked16(0xfeff);
			// DSP JITs fold the memory-access gate into generated code.
			if (oldDspInhibit != ((CommonData->MRWINH & 1) != 0))
				dsp::state.dirty = true;
			break;
		}

	case 0x2884: // DMEA[15:2]
	case 0x2888: // DGATE, DRGA[14:2]
		writeMasked16(0xfffc);
		break;

	// DEXE, DDIR, DLG
	case 0x288C:
		{
			const u16 current = ReadMemArr<u16>(aica_reg, alignedReg);
			const DmaControlWrite write = mergeDmaControlWrite(current,
					static_cast<u16>(writeBits), static_cast<u16>(writeMask));
			WriteMemArr(aica_reg, alignedReg, write.value);
			if (write.start)
				AicaInternalDMA();
			break;
		}

	default:
		// All assigned common registers are handled above. Writes to holes in
		// the common-data map do not create readable scratch storage.
		break;
	}
}

template void writeTimerAndIntReg<>(u32 reg, u8 data);
template void writeTimerAndIntReg<>(u32 reg, u16 data);
template void writeTimerAndIntReg<>(u32 reg, u32 data);

void midiSend(u8 data)
{
	if (midiSendBuffer.size() >= MidiFifoSize)
	{
		CommonData->MIOVF = 1;
		CommonData->MIFUL = 1;
		return;
	}
	const bool wasEmpty = midiSendBuffer.empty();
	midiSendBuffer.push_back(data);
	CommonData->MIEMP = 0;
	CommonData->MIFUL = midiSendBuffer.size() == MidiFifoSize;
	// MIDI-IN requests on the empty-to-nonempty transition. An SCIRE/MCIRE
	// acknowledgement while bytes remain queued is not retriggered by another
	// byte until software drains the FIFO and a new transition occurs.
	if (wasEmpty)
	{
		SCIPD->MIDI_IN = 1;
		update_arm_interrupts();
		MCIPD->MIDI_IN = 1;
		UpdateSh4Ints();
	}
}

bool midiRead(u8& data)
{
	if (midiSendBuffer.empty())
	{
		CommonData->MIEMP = 1;
		CommonData->MIFUL = 0;
		return false;
	}

	data = midiSendBuffer.front();
	midiSendBuffer.pop_front();
	CommonData->MIEMP = midiSendBuffer.empty();
	CommonData->MIFUL = 0;
	if (midiSendBuffer.empty())
	{
		SCIPD->MIDI_IN = 0;
		MCIPD->MIDI_IN = 0;
		update_arm_interrupts();
		UpdateSh4Ints();
	}
	return true;
}

void refreshMidiOutputStatus()
{
	CommonData->MOEMP = midiOutputBuffer.empty();
	CommonData->MOFUL = midiOutputBuffer.size() == MidiFifoSize;
	// Pending is stored independently because SCIRE/MCIRE can acknowledge the
	// empty transition while the FIFO remains empty.
	update_arm_interrupts();
	UpdateSh4Ints();
}

static void setMidiOutputInterrupt(bool pending)
{
	SCIPD->MIDI_OUT = pending;
	MCIPD->MIDI_OUT = pending;
	update_arm_interrupts();
	UpdateSh4Ints();
}

static int midiOutStep(int tag, int cycles, int jitter, void *arg)
{
	int remainingJitter = jitter;
	for (;;)
	{
		if (!midiOutTransmitByte())
			return 0;
		if ((int)MidiByteCycles > remainingJitter)
			return jitter + MidiByteCycles - remainingJitter;
		remainingJitter -= MidiByteCycles;
	}
}

bool midiOutTransmitByte()
{
	if (midiOutputBuffer.empty())
		return false;
	deliverMidiByte(midiOutputBuffer.front());
	midiOutputBuffer.pop_front();
	refreshMidiOutputStatus();
	if (midiOutputBuffer.empty())
		setMidiOutputInterrupt(true);
	return !midiOutputBuffer.empty();
}

void midiOutWrite(u8 data)
{
	if (midiOutputBuffer.size() == MidiFifoSize)
		return;
	const bool wasEmpty = midiOutputBuffer.empty();
	midiOutputBuffer.push_back(data);
	refreshMidiOutputStatus();
	// Writing the empty FIFO makes it nonempty and automatically cancels the
	// output-empty request. Additional queued bytes do not create an edge.
	if (wasEmpty)
		setMidiOutputInterrupt(false);
	if (wasEmpty)
		sh4_sched_request(midiOutSchedId, MidiByteCycles);
}

void serializeMidiOutput(Serializer& ser)
{
	ser << (u32)midiOutputBuffer.size();
	for (u8 data : midiOutputBuffer)
		ser << data;
}

void deserializeMidiOutput(Deserializer& deser)
{
	midiOutputBuffer.clear();
	// Loading an older state must not retain a transmit deadline from the
	// machine state being replaced. V132+ restores the saved deadline later
	// with the rest of the SH4 scheduler.
	if (midiOutSchedId != -1)
		sh4_sched_request(midiOutSchedId, -1);
	if (deser.version() >= Deserializer::V132)
	{
		u32 size;
		deser >> size;
		for (u32 i = 0; i < size; ++i)
		{
			u8 data;
			deser >> data;
			if (midiOutputBuffer.size() < MidiFifoSize)
				midiOutputBuffer.push_back(data);
		}
	}
}

void init()
{
	initMem();
	initRtc();

	sgc::init();
	if (aica_schid == -1)
		aica_schid = sh4_sched_register(0, &AicaUpdate);
	if (internalDmaSchedId == -1)
		internalDmaSchedId = sh4_sched_register(0, internalDmaStep);
	if (midiOutSchedId == -1)
		midiOutSchedId = sh4_sched_register(0, midiOutStep);
	arm::init();
}

void reset(bool hard)
{
	internalDma = {};
	if (internalDmaSchedId != -1)
		sh4_sched_request(internalDmaSchedId, -1);
	CommonData->DEXE = 0;
	if (hard)
	{
		midiOutputBuffer.clear();
		if (midiOutSchedId != -1)
			sh4_sched_request(midiOutSchedId, -1);
		initMem();
		sgc::term();
		sgc::init();
		sampleClock.reset();
		sh4_sched_request(aica_schid, sampleClock.nextCycles());
	}
	for (std::size_t i = 0; i < std::size(timers); i++)
		timers[i].Init(aica_reg, i);
	resetRtc(hard);
	arm::reset();
}

void term()
{
	arm::term();
	sgc::term();
	termMem();
	sh4_sched_unregister(aica_schid);
	aica_schid = -1;
	sh4_sched_unregister(internalDmaSchedId);
	internalDmaSchedId = -1;
	sh4_sched_unregister(midiOutSchedId);
	midiOutSchedId = -1;
}

} // namespace aica
