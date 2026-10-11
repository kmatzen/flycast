#pragma once
#include "types.h"

namespace aica::sgc
{

constexpr u32 effectiveEnvelopeRate(u32 rate, u32 keyRateScale, u32 octaveBits, u32 fns)
{
	int effectiveRate = rate * 2;
	if (keyRateScale < 0xf)
	{
		int octave = octaveBits;
		if (octave >= 8)
			octave -= 16;
		effectiveRate += (static_cast<int>(keyRateScale) + octave) * 2;
		effectiveRate += (fns >> 9) & 1;
	}
	// Hardware truncates the effective envelope rate at 0x3c. The nominal
	// 6-bit rate space has three encodings above that, but they do not select
	// faster AEG/FEG timing.
	return effectiveRate < 0 ? 0 : effectiveRate > 0x3c ? 0x3c : effectiveRate;
}

void AICA_Sample();

void WriteChannelReg(u32 channel, u32 reg, int size);

void init();
void term();

union fp_22_10
{
	struct
	{
		u32 fp:10;
		u32 ip:22;
	};
	u32 full;
};
union fp_s_22_10
{
	struct
	{
		u32 fp:10;
		s32 ip:22;
	};
	s32 full;
};
union fp_20_12
{
	struct
	{
		u32 fp:12;
		u32 ip:20;
	};
	u32 full;
};

typedef s32 SampleType;

constexpr s64 arithmeticShiftRight(s64 value, u32 bits)
{
	if (bits == 0)
		return value;
	const u64 divisor = 1ull << bits;
	if (value >= 0)
		return value / static_cast<s64>(divisor);
	// C++17 does not define signed right shift as arithmetic. Reproduce the
	// AICA's floor division without negating INT64_MIN.
	const u64 magnitude = static_cast<u64>(-(value + 1)) + 1;
	const u64 quotient = magnitude / divisor;
	return -static_cast<s64>(quotient) - (magnitude % divisor != 0);
}

constexpr u32 effectivePan(u32 pan, bool mono)
{
	return mono ? 0 : pan;
}

constexpr s32 logLinearAttenuation(u32 attenuation)
{
	// TL, AEG, ALFO, pan and send levels are combined in units where 0x10
	// is one attenuation octave. AICA converts that logarithmic value with
	// a four-bit linear mantissa rather than an ideal exponential curve.
	const u32 exponent = attenuation >> 4;
	if (exponent >= 15)
		return 0;
	const u32 mantissa = (~attenuation) & 0xf;
	return static_cast<s32>(((0x10 + mantissa) << 10) >> exponent); // Q15
}

constexpr s32 mixerLevelAttenuation(u32 level)
{
	level &= 0xf;
	return level == 0 ? 0 : logLinearAttenuation((0xf - level) << 3);
}

constexpr s32 outputMixerAttenuation(u32 level, u32 panSteps)
{
	level &= 0xf;
	panSteps &= 0xf;
	if (level == 0 || panSteps == 0xf)
		return 0;
	const u32 attenuation = ((0xf - level) << 3) + (panSteps << 3);
	return logLinearAttenuation(attenuation);
}

constexpr s32 interpolateSample(s32 current, s32 next, u32 fraction)
{
	// The sample interpolator is a single fixed-point multiply-accumulate.
	// Keep the full products until their sum is formed so negative products
	// are rounded only once at the output of the interpolator.
	fraction &= 0x3ff;
	const s64 accumulator = static_cast<s64>(current) * (0x400 - fraction)
			+ static_cast<s64>(next) * fraction;
	return static_cast<s32>(arithmeticShiftRight(accumulator, 10));
}

constexpr s32 wrapMixs(s32 value)
{
	u32 bits = static_cast<u32>(value) & 0xfffff;
	return (bits & 0x80000) != 0 ? static_cast<s32>(bits | 0xfff00000) : static_cast<s32>(bits);
}

constexpr s64 filterAccumulator(s32 sample, s32 previous1, s32 previous2,
		s32 fraction, s64 a0, s64 b1, s64 b2)
{
	return -a0 * sample + b1 * previous1 - b2 * previous2 - fraction;
}

constexpr u32 aegMonitorValue(u32 attenuation)
{
	// Active AEG levels occupy the 13-bit EG field with three zero LSBs;
	// hardware reports the all-ones sentinel once attenuation is inactive.
	return attenuation > 0x3bf ? 0x1fff : attenuation << 3;
}

constexpr u32 AegAttackStart = 0x280;
constexpr u32 AegInactiveThreshold = 0x3bf;

constexpr bool aegIsInactive(u32 attenuation)
{
	return attenuation > AegInactiveThreshold;
}

constexpr u32 envelopeClockPeriod(u32 rate, u32 step)
{
	rate = rate > 0x3c ? 0x3c : rate;
	if (rate < 2)
		return 0;
	if (rate >= 0x30)
		return 2;

	const u32 group = rate >> 2;
	u32 period = 8192u >> group;
	switch (rate & 3)
	{
	case 0:
		break;
	case 1:
		// Three long periods followed by two periods at twice the rate.
		if (step % 5 >= 3)
			period >>= 1;
		break;
	case 2:
		// One long period followed by two periods at twice the rate.
		if (step % 3 != 0)
			period >>= 1;
		break;
	case 3:
		// One long period followed by six periods at twice the rate.
		if (step % 7 != 0)
			period >>= 1;
		break;
	}
	return period < 2 ? 2 : period;
}

constexpr u32 envelopeDecayStep(u32 rate, u32 step)
{
	rate = rate > 0x3c ? 0x3c : rate;
	if (rate <= 0x30)
		return 1;

	const u32 scale = (rate - 0x31) >> 2;
	const u32 high = 2u << scale;
	const u32 low = 1u << scale;
	switch ((rate - 0x31) & 3)
	{
	case 0: return step % 4 == 0 ? high : low;
	case 1: return (step & 1) == 0 ? high : low;
	case 2: return step % 4 == 3 ? low : high;
	default: return high;
	}
}

constexpr u32 envelopeAttackShift(u32 rate, u32 step)
{
	u32 size = envelopeDecayStep(rate, step);
	u32 shift = 4;
	while (size > 1 && shift > 1)
	{
		size >>= 1;
		shift--;
	}
	return shift;
}

struct EnvelopeClockStep
{
	u32 counter;
	u32 step;
	bool advance;
};

constexpr EnvelopeClockStep stepEnvelopeClock(u32 counter, u32 step, u32 rate)
{
	const u32 period = envelopeClockPeriod(rate, step);
	if (period == 0)
		return { 0, step, false };
	if (counter > 1)
		return { counter - 1, step, false };
	return { envelopeClockPeriod(rate, step + 1), step + 1, true };
}

struct FilterEnvelopeStep
{
	u32 value;
	bool reached;
};

constexpr FilterEnvelopeStep advanceFilterEnvelope(u32 value, u32 target, u32 rate)
{
	if (value == target)
		return { value, true };
	if (rate == 0)
		return { value, false };
	if (value < target)
	{
		const u32 distance = target - value;
		return rate >= distance ? FilterEnvelopeStep { target, true }
				: FilterEnvelopeStep { value + rate, false };
	}
	const u32 distance = value - target;
	return rate >= distance ? FilterEnvelopeStep { target, true }
			: FilterEnvelopeStep { value - rate, false };
}

constexpr bool attackDecay1Triggered(bool loopStartLink, u32 decay1Rate)
{
	return !loopStartLink && decay1Rate != 0;
}

constexpr bool filterApplied(bool lowPassOff, u32 q, u32 flv0, u32 flv1,
		u32 flv2, u32 flv3, u32 flv4)
{
	// The documented through setting is exact: Q=4 and all cutoff levels=1fff.
	return !lowPassOff && (q != 4 || flv0 != 0x1fff || flv1 != 0x1fff
			|| flv2 != 0x1fff || flv3 != 0x1fff || flv4 != 0x1fff);
}

constexpr u32 lfoStepSamples(u32 frequency)
{
	u32 n = frequency & 0x1f;
	u32 scale = n >> 2;
	u32 fine = (~n) & 3;
	u32 group = 128 >> scale;
	return (group - 1) * 4 + group * (fine + 1);
}

constexpr u32 effectiveLfoStepSamples(u32 frequency, u32 alfoWaveform, u32 plfoWaveform)
{
	// With both outputs selecting noise there is no periodic consumer, and
	// hardware ignores LFOF: the shared state advances at the sample rate.
	return (alfoWaveform & 3) == 3 && (plfoWaveform & 3) == 3
			? 1 : lfoStepSamples(frequency);
}

struct LfoClockStep
{
	u32 counter;
	bool advance;
};

constexpr LfoClockStep stepLfoClock(u32 counter, u32 period)
{
	return counter <= 1 ? LfoClockStep { period, true }
			: LfoClockStep { counter - 1, false };
}

constexpr u32 retimeLfoCounter(u32 counter, u32 period)
{
	// Register writes do not reset LFO phase. If a faster frequency is
	// selected, the remaining countdown cannot exceed its new period.
	return counter == 0 || counter > period ? period : counter;
}

constexpr bool resetLfoAtLoop(bool resetEnabled, bool looped)
{
	// Hardware tests show that LFORE resets at each actual sample-loop start,
	// never at key-on. SSCTL replaces the waveform source with random data;
	// it does not bypass the slot's loop or LFO controls.
	return resetEnabled && looped;
}

constexpr u32 alfoAttenuation(u32 waveform, u32 scale)
{
	if ((scale & 7) == 0)
		return 0;
	// Measured ALFO depths are 0x008..0x200 in the AEG's 10-bit
	// attenuation domain. The mixer uses that domain shifted right by two,
	// yielding maxima of 2..128 attenuation units for depths 1..7.
	const u32 depth = 1u << (scale & 7);
	return ((waveform & 0xff) * depth + 127) / 255;
}

constexpr u32 plfoScale(u32 scale, int waveform)
{
	const u32 depth = scale & 7;
	if (depth == 0)
		return 1024;
	// Hardware modulates the phase increment linearly. At depth 7 the signed
	// waveform contributes one eighth of the base increment; each lower depth
	// halves that contribution. Arithmetic division rounds negative values
	// toward minus infinity on the chip, so spell that out portably.
	const int numerator = waveform * (1 << depth);
	const int adjustment = numerator >= 0 ? numerator / 128
			: -((-numerator + 127) / 128);
	return 1024 + adjustment;
}

constexpr u8 alfoWaveform(u32 waveform, u8 phase)
{
	switch (waveform & 3)
	{
	case 0: // sawtooth
		return phase;
	case 1: // square
		return phase < 128 ? 0 : 255;
	case 2: // triangle
		return phase < 128 ? phase * 2 : 511 - phase * 2;
	default: // noise generator value supplied in place of phase
		return phase;
	}
}

constexpr int plfoWaveform(u32 waveform, u8 phase)
{
	switch (waveform & 3)
	{
	case 0: // sawtooth, encoded as signed two's complement
		return phase < 128 ? phase : static_cast<int>(phase) - 256;
	case 1: // square
		return phase < 128 ? 127 : -128;
	case 2: // triangle
		if (phase < 64)
			return phase * 2;
		if (phase < 128)
			return 255 - phase * 2;
		if (phase < 192)
			return 256 - phase * 2;
		return phase * 2 - 511;
	default: // noise generator value supplied in place of phase
		return (phase & 0x80) != 0 ? static_cast<int>(phase) - 256 : phase;
	}
}

constexpr bool lfoNoiseUpdatesEverySample(u32 waveform)
{
	return (waveform & 3) == 3;
}

constexpr bool lfoClockRunsWhileSlotInactive()
{
	// The oscillator is independent of key-on; only its use by the slot is
	// gated. This also makes the observed key-on phase depend on elapsed time.
	return true;
}

constexpr u32 nextNoiseState(u32 state)
{
	return state * 0x41c64e6d + 0x3039;
}

constexpr s16 signedPcm16(u16 bits)
{
	return static_cast<s16>((bits & 0x8000) != 0 ? static_cast<s32>(bits) - 0x10000 : bits);
}

constexpr s16 signedPcm8(u8 bits)
{
	return static_cast<s16>((bits & 0x80) != 0 ? static_cast<s32>(bits) - 0x100 : bits);
}

constexpr s16 littleEndianPcm16(u8 low, u8 high)
{
	return signedPcm16(static_cast<u16>(low) | (static_cast<u16>(high) << 8));
}

constexpr bool soundMemoryReadAllowed(u32 mrwinh, bool noiseSource)
{
	// MRWINH bit 1 gates waveform RAM reads. The internally generated noise
	// source does not consume a sound-memory cycle.
	return noiseSource || (mrwinh & 2) == 0;
}

constexpr bool sourceReadsSoundMemory(u32 pcms, bool noiseSource)
{
	// SSCTL noise and the prohibited PCMS=3 encoding generate no waveform-RAM
	// read. Hardware experiments observe zero source data for PCMS=3.
	return !noiseSource && (pcms & 3) != 3;
}

constexpr s16 quantizeDacOutput(s32 sample, bool dac18Bit)
{
	if (dac18Bit)
	{
		// The 18-bit interface selects the upper 16 two's-complement bits.
		// Spell out an arithmetic shift so negative quantization is portable.
		if (sample < 0)
			sample = -static_cast<s32>((-static_cast<s64>(sample) + 3) / 4);
		else
			sample /= 4;
	}
	return static_cast<s16>(sample < -32768 ? -32768 : sample > 32767 ? 32767 : sample);
}

struct AdpcmDecodeStep
{
	s32 sample;
	s32 quant;
};

constexpr AdpcmDecodeStep decodeAdpcmNibble(u32 nibble, s32 previous, s32 quant)
{
	constexpr s32 quantFactors[8] = {
		0x0e6, 0x0e6, 0x0e6, 0x0e6, 0x133, 0x199, 0x200, 0x266
	};
	const u32 magnitude = nibble & 7;
	const s32 delta = (quant * static_cast<s32>(magnitude * 2 + 1)) >> 3;
	const s32 predicted = (nibble & 8) != 0 ? previous - delta : previous + delta;
	const s32 sample = predicted < -32768 ? -32768 : predicted > 32767 ? 32767 : predicted;
	const s32 scaledQuant = (quant * quantFactors[magnitude]) >> 8;
	const s32 nextQuant = scaledQuant < 127 ? 127 : scaledQuant > 24576 ? 24576 : scaledQuant;
	return { sample, nextQuant };
}

constexpr s16 noiseSample(u32 state)
{
	return signedPcm16(state >> 16);
}

constexpr s16 noisePcm8Sample(u32 state)
{
	return signedPcm8(state >> 24) * 256;
}

constexpr u8 noiseAdpcmNibble(u32 state)
{
	return (state >> 28) & 0xf;
}

constexpr bool loopStartLinkTriggered(u32 decay1Rate, u32 address, u32 loopStart)
{
	return decay1Rate != 0 && address >= loopStart;
}

constexpr u32 sampleUpdateRate(u32 pcms, u32 octaveBits, u32 fns)
{
	u32 octave = octaveBits & 0xf;
	// Both encodings that select the ADPCM-side source path are limited to
	// +1 octave, even though PCMS=3 yields zero sample data.
	if (pcms >= 2 && octave < 8 && octave > 1)
		octave = 1;
	// The normally-zero eleventh FNS bit is physically wired. The generator's
	// implicit 0x400 base is an XOR, as observed on hardware, rather than OR.
	u32 rate = (fns & 0x7ff) ^ 0x400;
	return (octave & 8) != 0 ? rate >> (16 - octave) : rate << octave;
}

constexpr u32 sampleStartAddress(u32 high, u32 low, u32 pcms)
{
	u32 address = ((high & 0x7f) << 16) | (low & 0xffff);
	// The 16-bit PCM data bus cannot address an odd byte.
	return (pcms & 3) == 0 ? address & ~1u : address;
}

constexpr u32 sampleLoopEnd(u32 pcms, u32 loopEnd)
{
	// LEA names the last sample read. The prohibited PCMS=3 encoding retains
	// the observed four-sample address-generator boundary despite being silent.
	return pcms == 3 ? (loopEnd + 3) & ~3u : loopEnd + 1;
}

constexpr u32 interpolationNextAddress(u32 current, u32 loopStart, u32 loopEnd,
		u32 pcms, bool loopEnabled)
{
	u32 next = current + 1;
	const u32 end = sampleLoopEnd(pcms, loopEnd);
	return loopEnabled && loopStart < end && next >= end ? loopStart : next;
}

struct SampleAddressStep
{
	u32 address;
	bool stopped;
	bool looped;
};

constexpr SampleAddressStep advanceSampleAddress(u32 current, u32 loopStart, u32 loopEnd,
		u32 pcms, bool loopEnabled)
{
	u32 next = current + 1;
	const u32 invalidLoopBoundary = pcms == 3 ? sampleLoopEnd(pcms, loopEnd) : loopEnd;
	if (loopStart > invalidLoopBoundary)
		return next >= loopStart ? SampleAddressStep { 0, true, true }
				: SampleAddressStep { next, false, false };
	if (next < sampleLoopEnd(pcms, loopEnd))
		return { next, false, false };
	return loopEnabled ? SampleAddressStep { loopStart, false, true }
			: SampleAddressStep { 0, true, true };
}

constexpr u32 vmuTimerPeriod(u32 reload)
{
	return 0x100 - (reload & 0xff);
}

// Timer1 counts upward from the reload value through 0xff. The PWM transition
// register selects a point within that interval; it is not a duty-cycle count.
// Approximate the fundamental strength (normalized to a 50% duty cycle) while
// retaining the measured first-party buzzer waveform used by the mixer.
constexpr u32 vmuPwmFundamentalScale(u32 reload, u32 transition)
{
	reload &= 0xff;
	transition &= 0xff;
	if (transition <= reload)
		return 0;
	const u64 period = vmuTimerPeriod(reload);
	const u64 firstPhase = transition - reload;
	const u64 secondPhase = 0x100 - transition;
	return static_cast<u32>(firstPhase * secondPhase * 4096
			/ (static_cast<u64>(period) * period));
}

constexpr u32 vmuWaveStep(u32 period)
{
	// Timer 1 is clocked at 6 MHz / 6. Convert its exact frequency directly
	// to the Q10 phase increment of the measured 2698 Hz waveform. Dividing
	// the frequency first loses a phase unit for otherwise valid periods.
	return period == 0 ? 0 : static_cast<u32>(1000000ull * 1024 / (period * 2698ull));
}

u16 ReadCommonReg(u32 reg, bool byte);
void serialize(Serializer& ctx);
void deserialize(Deserializer& ctx);
void vmuBeep(int bus, int port, int period, int timerLow);

} // namespace aica::sgc
