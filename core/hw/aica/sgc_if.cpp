/*
	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.

    Some PLFO and FEG code from Highly_Theoretical lib by Neill Corlett
    (https://gitlab.com/kode54/highly_theoretical)
 */

#include "sgc_if.h"
#include "aica.h"
#include "aica_if.h"
#include "aica_mem.h"
#include "dsp.h"
#include "audio/audiostream.h"
#include "hw/gdrom/gdrom_if.h"
#include "cfg/option.h"
#include "serialize.h"

#include <algorithm>
#include <cstring>
#include <type_traits>

#undef FAR

//#define CLIP_WARN
#define key_printf(...) DEBUG_LOG(AICA, __VA_ARGS__)
#define aeg_printf(...) DEBUG_LOG(AICA, __VA_ARGS__)
#define feg_printf(...) DEBUG_LOG(AICA, __VA_ARGS__)
#define step_printf(...) DEBUG_LOG(AICA, __VA_ARGS__)

#ifdef CLIP_WARN
#define clip_verify(x) verify(x)
#else
#define clip_verify(x)
#endif

namespace aica::sgc
{
//Sound generation, mixin, and channel regs emulation
//x.15
static s32 volume_lut[16];
//255 -> mute
//Converts Send levels to TL-compatible values (DISDL, etc)
static const u32 SendLevel[16] =
{
	255, 14 << 3, 13 << 3, 12 << 3, 11 << 3, 10 << 3, 9 << 3, 8 << 3,
	7 << 3, 6 << 3, 5 << 3, 4 << 3, 3 << 3, 2 << 3, 1 << 3, 0 << 3
};
static s32 tl_lut[256 + 768];	//xx.15 format. >=255 is muted

constexpr int CDDA_SIZE = 2352 / 2;
static s16 cdda_sector[CDDA_SIZE];
static u32 cdda_index = CDDA_SIZE;

//in ms :)
static int PLFO_Scales[8][256];

#define EG_STEP_BITS (16)

static const char* stream_names[]=
{
	"0: 16-bit PCM (two's complement format)",
	"1: 8-bit PCM (two's complement format)",
	"2: 4-bit ADPCM (Yamaha format)",
	"3: prohibited (silent)"
};

static const s32 qtable[32] = {
	 2048,  1536,  1024,   512,
	    0,  -256,  -512,  -768,
	-1024, -1280, -1536, -1792,
	-2048, -2176, -2304, -2432,
	-2560, -2688, -2816, -2944,
	-3072, -3136, -3200, -3264,
	-3328, -3392, -3456, -3520,
	-3584, -3648, -3712, -3776
};

//Fixed point mul w/ rounding :)
template<typename T>
static T FPMul(T a, T b, int bits) {
	const T product = a * b;
	if constexpr (std::is_signed_v<T>)
		return static_cast<T>(arithmeticShiftRight(product, bits));
	else
		return product >> bits;
}

static void VolumePan(SampleType value, u32 vol, u32 pan, SampleType& outl, SampleType& outr)
{
	pan = effectivePan(pan, CommonData->Mono);
	SampleType temp = FPMul(value, outputMixerAttenuation(vol, 0), 15);
	SampleType Sc = FPMul(value, outputMixerAttenuation(vol, pan & 0xf), 15);
	if (pan & 0x10)
	{
		outl += temp;
		outr += Sc;
	}
	else
	{
		outl += Sc;
		outr += temp;
	}
}

class VmuBeep
{
public:
	void init()
	{
		active = false;
		att = 256;
		waveIdx.full = 0;
		waveStep = 0;
		pwmScale = 1024;
	}

	void update(int reload, int transition)
	{
		pwmScale = vmuPwmFundamentalScale(reload, transition);
		if (pwmScale == 0) {
			active = false;
		}
		else
		{
			active = true;
			waveStep = vmuWaveStep(vmuTimerPeriod(reload));
		}
	}

	SampleType getSample()
	{
		if (!active && att >= 256)
			return 0;

		waveIdx.full += waveStep;
		waveIdx.ip %= std::size(wave);
		int nextIdx = (waveIdx.ip + 1) % std::size(wave);
		SampleType s = interpolateSample(wave[waveIdx.ip], wave[nextIdx], waveIdx.fp) * 2;

		s = FPMul(s, pwmScale, 10);
		s = FPMul(s, tl_lut[att], 15);
		if (active)
			att = std::max(att - 2, 0);
		else
			att = std::min(att + 2, 256);
		return s;
	}

	void serialize(Serializer& ser)
	{
		ser << active;
		ser << att;
		ser << waveIdx;
		ser << waveStep;
		ser << pwmScale;
	}

	void deserialize(Deserializer& deser)
	{
		if (deser.version() < Deserializer::V49)
		{
			if (deser.version() >= Deserializer::V22)
			{
				deser.skip<int>();	// beepOn
				deser.skip<int>();	// beepPeriod
				deser.skip<int>();	// beepCounter
			}
			init();
		}
		else
		{
			deser >> active;
			deser >> att;
			deser >> waveIdx;
			deser >> waveStep;
			if (deser.version() >= Deserializer::V106)
				deser >> pwmScale;
			else
				pwmScale = 1024;
		}
	}

private:
	bool active = false;
	int att = 256;
	fp_22_10 waveIdx {};
	int waveStep = 0;
	int pwmScale = 1024;

	// 2698 Hz
	static constexpr SampleType wave[] = {   503, -3519,
		-7540, -8214, -8209, -8214, -8209, -5199, -1172,  2843,
		 6873,  8207,  8214,  8209,  8212,  5866,  1840, -2175,
		-6203, -8210, -8215, -8209, -8214, -6533, -2516,  1507,
		 5526,  8212,  8210,  8212,  8210,  7206,  3187,  -841,
		-4856, -8215, -8209, -8214, -8208, -7882, -3850,   162,
		 4187,  8213,  8208,  8213,  8209,  8211,  4525 };
};
static std::array<VmuBeep, 8> vmuBeeps;

#pragma pack(push, 1)
//All regs are 16b , aligned to 32b (upper bits 0?)
struct ChannelCommonData
{
	//+00 [0]
	//SA is half at reg 0 and the rest at reg 1
	u32 SA_hi:7;
	u32 PCMS:2;
	u32 LPCTL:1;
	u32 SSCTL:1;
	u32 :3;
	u32 KYONB:1;
	u32 KYONEX:1;

	u32 :16;

	//+04 [1]
	//SA (defined above)
	u32 SA_low:16;

	u32 :16;

	//+08 [2]
	u32 LSA:16;

	u32 :16;

	//+0C [3]
	u32 LEA:16;

	u32 :16;

	//+10 [4]
	u32 AR:5;
	u32 :1;
	u32 D1R:5;
	u32 D2R:5;

	u32 :16;

	//+14 [5]
	u32 RR:5;
	u32 DL:5;
	u32 KRS:4;
	u32 LPSLNK:1;
	u32 :1;

	u32 :16;

	//+18[6]
	u32 FNS:11;
	u32 OCT:4;
	u32 :1;

	u32 :16;

	//+1C	RE	LFOF[4:0]	PLFOWS	PLFOS[2:0]	ALFOWS	ALFOS[2:0]
	u32 ALFOS:3;
	u32 ALFOWS:2;

	u32 PLFOS:3;
	u32 PLFOWS:2;

	u32 LFOF:5;
	u32 LFORE:1;

	u32 :16;

	//+20	--	IMXL[3:0]	ISEL[3:0]
	u32 ISEL:4;
	u32 IMXL:4;
	u32 :8;

	u32 :16;

	//+24	--	DISDL[3:0]	--	DIPAN[4:0]
	u32 DIPAN:5;
	u32 :3;
	
	u32 DISDL:4;
	u32 :4;

	u32 :16;
	

	//+28	TL[7:0]	--	Q[4:0]
	u32 Q:5;
	u32 LPOFF:1;		// not documented: 0: LPF enabled, 1: LPF disabled
	u32 SDIR:1;			// sound direct: bypass TL, AEG and ALFO attenuation
	u32 :1;

	u32 TL:8;

	u32 :16;

	//+2C	--	FLV0[12:0]
	u32 FLV0:13;
	u32 :3;

	u32 :16;

	//+30	--	FLV1[12:0]
	u32 FLV1:13;
	u32 :3;
	
	u32 :16;

	//+34	--	FLV2[12:0]
	u32 FLV2:13;
	u32 :3;
	
	u32 :16;

	//+38	--	FLV3[12:0]
	u32 FLV3:13;
	u32 :3;
	
	u32 :16;

	//+3C	--	FLV4[12:0]
	u32 FLV4:13;
	u32 :3;
	
	u32 :16;
	
	//+40	--	FAR[4:0]	--	FD1R[4:0]
	u32 FD1R:5;
	u32 :3;
	u32 FAR:5;
	u32 :3;

	u32 :16;

	//+44	--	FD2R[4:0]	--	FRR[4:0]
	u32 FRR:5;
	u32 :3;
	u32 FD2R:5;
	u32 :3;

	u32 :16;
};
#pragma pack(pop)


enum EGState
{
	EG_Attack = 0,
	EG_Decay1 = 1,
	EG_Decay2 = 2,
	EG_Release = 3
};

enum class LFOType
{
	Sawtooth,
	Square,
	Triangle,
	Random
};

enum PCMSType
{
	PCM16 = 0,
	PCM8 = 1,
	ADPCM = 2,
	SILENCE = 3,
	NOISE_PCM16 = 4,
	NOISE_PCM8 = 5,
	NOISE_ADPCM = 6,
};

constexpr PCMSType baseSampleFormat(PCMSType format)
{
	return format == NOISE_PCM16 ? PCM16
			: format == NOISE_PCM8 ? PCM8
			: format == NOISE_ADPCM ? ADPCM : format;
}

constexpr bool isNoiseFormat(PCMSType format)
{
	return format == NOISE_PCM16 || format == NOISE_PCM8 || format == NOISE_ADPCM;
}

/*
	KEY_OFF->KEY_ON : Resets everything, and starts playback (EG: A)
	KEY_ON->KEY_ON  : nothing
	KEY_ON->KEY_OFF : Switches to RELEASE state (does not disable channel)

*/

struct ChannelEx
{
	static ChannelEx Chans[64];

	ChannelCommonData* ccd;

	u32 SA;
	u32 CA;
	fp_22_10 step;
	u32 update_rate;

	SampleType s0, s1;

	struct
	{
		u32 LSA;
		u32 LEA;

		u8 looped;
	} loop;
	
	struct
	{
		//used in adpcm decoding
		s32 last_quant;
		// Saved quantization and previous sample used in PCMS mode 2 (yamaha ADPCM)
		s32 loopstart_quant;
		SampleType loopstart_prev_sample;
		bool in_loop;

		void Reset(ChannelEx* ch)
		{
			last_quant = 127;
			loopstart_quant = 0;
			loopstart_prev_sample = 0;
			in_loop = false;

			ch->s0 = 0;
			ch->s1 = 0;
		}
	} adpcm;

	u32 noise_state;//for Noise generator
	u8 noise_lookahead;

	struct
	{
		u32 DirectAtt;
		u32 DLAtt;
		u32 DRAtt;
		u32 DSPAtt;
		SampleType* DSPOut;
	} VolMix;
	
	void (ChannelEx::*StepAEG)();
	void (ChannelEx::*StepFEG)();
	void (ChannelEx::*StepStream)();
	void (ChannelEx::*StepStreamInitial)();
	
	struct
	{
		s32 val;
		s32 GetValue() { return val >> EG_STEP_BITS;}
		void SetValue(u32 aegb) { val = aegb << EG_STEP_BITS; }

		EGState state = EG_Attack;

		u32 AttackRate;
		u32 Decay1Rate;
		u32 Decay2Value;
		u32 Decay2Rate;
		u32 ReleaseRate;
		u32 counter;
		u32 step;
	} AEG;
	
	struct
	{
		u32 value;
		u32 GetValue() { return value >> EG_STEP_BITS;}
		void SetValue(u32 fegb) { value = fegb << EG_STEP_BITS; }

		EGState state = EG_Attack;

		SampleType prev1;
		SampleType prev2;
		s32 fractSave;
		s32 q;
		u32 AttackRate;
		u32 Decay1Rate;
		u32 Decay2Rate;
		u32 ReleaseRate;
		u32 counter;
		u32 step;
		// Application bypass only; the FEG itself keeps running while bypassed.
		bool applied = false;
	} FEG;
	
	struct Lfo
	{
		u32 counter;
		u32 start_value;
		u8 state;
		u32 noise_state;
		u8 noise_value;
		u8 alfo;
		u8 alfo_scale;
		fp_22_10 plfo_step;
		int *plfo_scale;
		LFOType alfo_type;
		LFOType plfo_type;
		void (Lfo::*alfo_calc)();
		void (Lfo::*plfo_calc)();

		template<LFOType Type>
		void CalcAlfo();
		template<LFOType Type>
		void CalcPlfo();

		void Step()
		{
			const LfoClockStep clock = stepLfoClock(counter, start_value);
			counter = clock.counter;
			const bool randomAlfo = alfo_type == LFOType::Random;
			const bool randomPlfo = plfo_type == LFOType::Random;
			if (randomAlfo || randomPlfo)
			{
				// Noise LFO ignores LFOF and obtains a new value at the sample rate.
				// Both modulation paths share the slot's noise source.
				noise_state = nextNoiseState(noise_state);
				noise_value = noise_state >> 24;
			}
			if (clock.advance)
				state++;
			if (clock.advance || randomAlfo)
				(this->*alfo_calc)();
			if (clock.advance || randomPlfo)
				(this->*plfo_calc)();
		}
		void Reset()
		{
			state = 0;
			counter = start_value;
			(this->*alfo_calc)();
			(this->*plfo_calc)();
		}
		void SetStartValue(u32 nv) {
			start_value = nv;
		}

		static void (Lfo::*ALFOWS_CALC[4])();
		static void (Lfo::*PLFOWS_CALC[4])();
	} lfo;

	bool enabled;	//set to false to 'freeze' the channel
	bool quiet;
	int ChannelNumber;

	static void (ChannelEx::*STREAM_STEP_LUT[7][2][2])();
	static void (ChannelEx::*STREAM_INITAL_STEP_LUT[7])();
	static void (ChannelEx::*AEG_STEP_LUT[4])();
	static void (ChannelEx::*FEG_STEP_LUT[4])();

	template<PCMSType PCMS, bool Last>
	void StepDecodeSample(u32 CA);

	template<PCMSType PCMS>
	void StepDecodeSampleInitial() {
		if constexpr (PCMS == NOISE_PCM16 || PCMS == NOISE_PCM8)
		{
			noise_state = nextNoiseState(noise_state);
			s0 = PCMS == NOISE_PCM16 ? noiseSample(noise_state) : noisePcm8Sample(noise_state);
			noise_state = nextNoiseState(noise_state);
			s1 = PCMS == NOISE_PCM16 ? noiseSample(noise_state) : noisePcm8Sample(noise_state);
		}
		else if constexpr (PCMS == NOISE_ADPCM)
		{
			noise_state = nextNoiseState(noise_state);
			const u8 first = noiseAdpcmNibble(noise_state);
			noise_state = nextNoiseState(noise_state);
			noise_lookahead = noiseAdpcmNibble(noise_state);
			s32 q = adpcm.last_quant;
			AdpcmDecodeStep decoded = decodeAdpcmNibble(first, 0, q);
			s0 = decoded.sample;
			q = decoded.quant;
			adpcm.last_quant = q;
			s1 = decodeAdpcmNibble(noise_lookahead, s0, q).sample;
		}
		else
			StepDecodeSample<PCMS, true>(0);
	}
	template<PCMSType PCMS, bool LPCTL, bool LPSLNK>
	void StreamStep();

	template<EGState state>
	void AegStep();
	template<EGState state>
	void FegStep();

	void Init(int cn, u8* ccd_raw)
	{
		ccd = (ChannelCommonData*)&ccd_raw[cn * 0x80];
		ChannelNumber = cn;
		// Channel objects persist across a hard reset, unlike the physical
		// generators. Restore their power-on phases before deriving register
		// state below.
		enabled = false;
		SA = 0;
		CA = 0;
		step.full = 0;
		s0 = 0;
		s1 = 0;
		loop = {};
		noise_state = 0;
		noise_lookahead = 0;
		AEG = {};
		AEG.state = EG_Release;
		AEG.SetValue(0x3ff);
		FEG = {};
		lfo.state = 0;
		lfo.counter = 0;
		lfo.noise_state = 0;
		lfo.noise_value = 0;
		adpcm.Reset(this);
		quiet = true;
		for (u32 i = 0; i < 0x80; i += 2)
			RegWrite(i, 2);
		quiet = false;
		disable();
	}

	void disable()
	{
		enabled = false;
		SetAegState(EG_Release);
		AEG.SetValue(0x3FF);
		CA = 0;
	}

	void enable()
	{
		enabled = true;
	}

	SampleType InterpolateSample()
	{
		return interpolateSample(s0, s1, step.fp);
	}

	SampleType lowPassFilter(SampleType sample)
	{
		if (!FEG.applied)
			return sample;
		constexpr u32 COEF_BITS = 30;
		const u32 fv = FEG.GetValue();
		const u32 exp = fv >> 9;
		const u32 mant = (fv & 0x1FF) | 0x200;
		u64 a0Bits = ((u64)mant << 30) >> ((15 - exp) * 2);
		a0Bits *= (mant - 1) / 8;
		a0Bits >>= (47 - COEF_BITS);
		const s64 a0 = static_cast<s64>(a0Bits);
		s64 f = ((s64)mant << exp) << (COEF_BITS - 25);
		f += (s64)FEG.q * f / 4096;
		const s64 b1 = 128ll * 1024 * (1 << (COEF_BITS - 16)) - (f + a0);
		const s64 b2 = 64ll * 1024 * (1 << (COEF_BITS - 16)) - f;
		if (exp == 0)
			// avoid residual signal
			FEG.fractSave = 0;

		const s64 mac = filterAccumulator(sample, FEG.prev1, FEG.prev2,
				FEG.fractSave, a0, b1, b2);	// 20+COEF_BITS bits
		// Coefficient and history bounds keep the shifted result within the
		// signed working sample range; make the narrowing explicit.
		sample = static_cast<SampleType>(arithmeticShiftRight(mac, COEF_BITS));
		FEG.fractSave = static_cast<s32>(sample * (1ll << COEF_BITS) - mac);
		FEG.prev2 = FEG.prev1;
		sample = std::clamp(sample, -512 * 1024, 512 * 1024 - 1);
		FEG.prev1 = sample;
		return sample;
	}

	bool Step(SampleType& oLeft, SampleType& oRight, SampleType& oDsp)
	{
		if (!enabled)
		{
			// The per-slot LFO is free-running. Key-on does not reset its phase,
			// so time spent inactive must still advance both periodic and noise
			// waveforms.
			lfo.Step();
			oLeft = oRight = oDsp = 0;
			return false;
		}

		SampleType sample = InterpolateSample() * 16;
		sample = lowPassFilter(sample);


		//Volume & Mixer processing
		//All attenuations are added together then applied and mixed :)

		//offset is up to 511
		//*Att is up to 511
		//logtable handles up to 1024, anything >=255 is mute

		u32 ofsatt;
		if (ccd->SDIR == 1)
		{
			ofsatt = 0;
		}
		else
		{
			ofsatt = lfo.alfo + (AEG.GetValue() >> 2);
			ofsatt = std::min(ofsatt, (u32)255); // make sure it never gets more 255 -- it can happen with some alfo/aeg combinations
		}
		u32 const max_att = ((16 << 4) - 1) - ofsatt;
		
		s32* logtable = ofsatt + tl_lut;

		u32 dl = std::min(CommonData->Mono ? VolMix.DirectAtt : VolMix.DLAtt, max_att);
		u32 dr = std::min(CommonData->Mono ? VolMix.DirectAtt : VolMix.DRAtt, max_att);
		u32 ds = std::min(VolMix.DSPAtt, max_att);

		oLeft = FPMul<s64>(sample, logtable[dl], 19);	// 16 bits
		oRight = FPMul<s64>(sample, logtable[dr], 19);	// 16 bits
		oDsp = FPMul<s64>(sample, logtable[ds], 15);	// 20 bits

		clip_verify((s16)oLeft == oLeft);
		clip_verify((s16)oRight == oRight);
		clip_verify(oDsp >= -(1 << 19) && oDsp < (1 << 19));
		clip_verify((s64)sample * oLeft >= 0);
		clip_verify((s64)sample * oRight >= 0);
		clip_verify((s64)sample * oDsp >= 0);

		(this->*StepAEG)();
		if (enabled)
		{
			(this->*StepFEG)();
			(this->*StepStream)();
			lfo.Step();
		}
		return true;
	}

	void Step(SampleType& mixl, SampleType& mixr)
	{
		SampleType oLeft, oRight, oDsp;

		Step(oLeft, oRight, oDsp);

		*VolMix.DSPOut = wrapMixs(*VolMix.DSPOut + oDsp);

		mixl += oLeft;
		mixr += oRight;
	}

	static void StepAll(SampleType& mixl, SampleType& mixr)
	{
		for (ChannelEx& channel : Chans)
			channel.Step(mixl, mixr);
	}

	void SetAegState(EGState newstate)
	{
		StepAEG = AEG_STEP_LUT[newstate];
		AEG.state = newstate;
		if (enabled)
		{
			u32 rate = newstate == EG_Attack ? AEG.AttackRate
					: newstate == EG_Decay1 ? AEG.Decay1Rate
					: newstate == EG_Decay2 ? AEG.Decay2Rate
					: AEG.ReleaseRate;
			const u32 period = envelopeClockPeriod(rate, AEG.step);
			if (period == 0 || AEG.counter == 0 || AEG.counter > period)
				AEG.counter = period;
		}
		if (newstate == EG_Release)
			ccd->KYONB = 0;
	}

	void SetFegState(EGState newstate, bool resetAttack = true)
	{
		StepFEG = FEG_STEP_LUT[newstate];
		FEG.state = newstate;
		if (newstate == EG_Attack && resetAttack)
		{
			FEG.SetValue(ccd->FLV0);
			FEG.prev1 = 0;
			FEG.prev2 = 0;
			FEG.fractSave = 0;
		}
		if (enabled)
		{
			u32 rate = newstate == EG_Attack ? FEG.AttackRate
					: newstate == EG_Decay1 ? FEG.Decay1Rate
					: newstate == EG_Decay2 ? FEG.Decay2Rate
					: FEG.ReleaseRate;
			const u32 period = envelopeClockPeriod(rate, FEG.step);
			if (period == 0 || FEG.counter == 0 || FEG.counter > period)
				FEG.counter = period;
		}
	}

	void KEY_ON()
	{
		if (AEG.state != EG_Release)
			return;

		enable();

		// reset AEG
		SetAegState(EG_Attack);
		AEG.SetValue(AegAttackStart);
		AEG.step = 0;
		AEG.counter = envelopeClockPeriod(AEG.AttackRate, 0);

		// reset FEG
		SetFegState(EG_Attack);
		FEG.step = 0;
		FEG.counter = envelopeClockPeriod(FEG.AttackRate, 0);

		// reset sampling state
		CA = 0;
		step.full = 0;

		loop.looped = false;
		adpcm.Reset(this);

		(this->*StepStreamInitial)();
		key_printf("[%d] KEY_ON %s @ %g Hz, loop %d - AEG AR %d DC1R %d DC2V %d DC2R %d RR %d - KRS %d OCT %d FNS %d - SA %x LSA %x LEA %x",
				ChannelNumber, stream_names[ccd->PCMS], (44100.0 * update_rate) / 1024, ccd->LPCTL,
				ccd->AR, ccd->D1R, ccd->DL << 5, ccd->D2R, ccd->RR,
				ccd->KRS, ccd->OCT, ccd->FNS,
				SA, ccd->LSA, ccd->LEA);
	}

	void KEY_OFF()
	{
		if (AEG.state == EG_Release)
			return;
		key_printf("[%d] KEY_OFF -> Release", ChannelNumber);
		SetAegState(EG_Release);
		SetFegState(EG_Release);
	}

	//PCMS,SSCTL,LPCTL,LPSLNK
	void UpdateStreamStep()
	{
		PCMSType fmt = (PCMSType)ccd->PCMS;
		if (ccd->SSCTL)
			fmt = ccd->PCMS == PCM16 ? NOISE_PCM16
					: ccd->PCMS == PCM8 ? NOISE_PCM8
					: ccd->PCMS == ADPCM ? NOISE_ADPCM : SILENCE;

		StepStream = STREAM_STEP_LUT[fmt][ccd->LPCTL][ccd->LPSLNK];
		StepStreamInitial = STREAM_INITAL_STEP_LUT[fmt];
	}
	//SA,PCMS
	void UpdateSA()
	{
		SA = sampleStartAddress(ccd->SA_hi, ccd->SA_low, ccd->PCMS);
	}
	//LSA,LEA
	void UpdateLoop()
	{
		loop.LSA = ccd->LSA;
		loop.LEA = ccd->LEA;
	}

	u32 EG_EffRate(u32 rate)
	{
		return effectiveEnvelopeRate(rate, ccd->KRS, ccd->OCT, ccd->FNS);
	}

	//D2R,D1R,AR,DL,RR,KRS, [OCT,FNS] for now
	void UpdateAEG()
	{
		AEG.AttackRate = EG_EffRate(ccd->AR);
		AEG.Decay1Rate = EG_EffRate(ccd->D1R);
		AEG.Decay2Value = ccd->DL<<5;
		AEG.Decay2Rate = EG_EffRate(ccd->D2R);
		AEG.ReleaseRate = EG_EffRate(ccd->RR);
		if (enabled)
		{
			u32 rate = AEG.state == EG_Attack ? AEG.AttackRate
					: AEG.state == EG_Decay1 ? AEG.Decay1Rate
					: AEG.state == EG_Decay2 ? AEG.Decay2Rate
					: AEG.ReleaseRate;
			const u32 period = envelopeClockPeriod(rate, AEG.step);
			if (period == 0 || AEG.counter == 0 || AEG.counter > period)
				AEG.counter = period;
		}
	}
	//OCT,FNS
	void UpdatePitch()
	{
		this->update_rate = sampleUpdateRate(ccd->PCMS, ccd->OCT, ccd->FNS);
	}

	//LFORE,LFOF,PLFOWS,PLFOS,ALFOWS,ALFOS
	void UpdateLFO(bool derivedState)
	{
		lfo.alfo_type = static_cast<LFOType>(ccd->ALFOWS);
		lfo.plfo_type = static_cast<LFOType>(ccd->PLFOWS);
		const u32 stepSamples = effectiveLfoStepSamples(
				ccd->LFOF, ccd->ALFOWS, ccd->PLFOWS);
		lfo.SetStartValue(stepSamples);
		if (!derivedState)
			lfo.counter = retimeLfoCounter(lfo.counter, stepSamples);

		lfo.alfo_scale = ccd->ALFOS;

		lfo.alfo_calc = lfo.ALFOWS_CALC[ccd->ALFOWS];
		lfo.plfo_calc = lfo.PLFOWS_CALC[ccd->PLFOWS];
		lfo.plfo_scale = PLFO_Scales[ccd->PLFOS];
		(lfo.*lfo.alfo_calc)();
		(lfo.*lfo.plfo_calc)();
		if (ccd->ALFOS != 0)
			aeg_printf("[%d] ALFO LFOF %d wave %d scale %d", ChannelNumber, ccd->LFOF, ccd->ALFOWS, ccd->ALFOS);
		if (ccd->PLFOS != 0)
			feg_printf("[%d] PLFO LFOF %d wave %d scale %d", ChannelNumber, ccd->LFOF, ccd->PLFOWS, ccd->PLFOS);
	}

	//ISEL
	void UpdateDSPMIX()
	{
		VolMix.DSPOut = &dsp::state.MIXS[ccd->ISEL];
	}
	//TL,DISDL,DIPAN,IMXL
	void UpdateAtts()
	{
		u32 total_level = ccd->SDIR ? 0 : ccd->TL;
		u32 attFull = total_level + SendLevel[ccd->DISDL];
		VolMix.DirectAtt = attFull;
		u32 attPan = attFull + SendLevel[(~ccd->DIPAN) & 0xF];

		//0x1* -> R decreases
		if (ccd->DIPAN & 0x10)
		{
			VolMix.DLAtt = attFull;
			VolMix.DRAtt = attPan;
		}
		else //0x0* -> L decreases
		{
			VolMix.DLAtt = attPan;
			VolMix.DRAtt = attFull;
		}

		VolMix.DSPAtt = total_level + SendLevel[ccd->IMXL];
	}

	//Q,FLV0,FLV1,FLV2,FLV3,FLV4,FAR,FD1R,FD2R,FRR, LPOFF
	void UpdateFEG()
	{
		FEG.applied = filterApplied(ccd->LPOFF, ccd->Q, ccd->FLV0, ccd->FLV1,
				ccd->FLV2, ccd->FLV3, ccd->FLV4);
		if (FEG.applied && !quiet)
			feg_printf("FEG active channel %d Q %d FLV: %04d %04d %04d %04d %04d AR %02d FD1R %02d FD2R %02d FRR %02d",
					ChannelNumber, ccd->Q,
					ccd->FLV0, ccd->FLV1, ccd->FLV2, ccd->FLV3, ccd->FLV4,
					ccd->FAR, ccd->FD1R, ccd->FD2R, ccd->FRR);
		FEG.q = qtable[ccd->Q];
		FEG.AttackRate = EG_EffRate(ccd->FAR);
		FEG.Decay1Rate = EG_EffRate(ccd->FD1R);
		FEG.Decay2Rate = EG_EffRate(ccd->FD2R);
		FEG.ReleaseRate = EG_EffRate(ccd->FRR);
		if (enabled)
		{
			u32 rate = FEG.state == EG_Attack ? FEG.AttackRate
					: FEG.state == EG_Decay1 ? FEG.Decay1Rate
					: FEG.state == EG_Decay2 ? FEG.Decay2Rate
					: FEG.ReleaseRate;
			const u32 period = envelopeClockPeriod(rate, FEG.step);
			if (period == 0 || FEG.counter == 0 || FEG.counter > period)
				FEG.counter = period;
		}
	}
	
	void RegWrite(u32 offset, int size)
	{
		switch (offset)
		{
		case 0x00: // PCMS, SA
		case 0x01: // KYONEX, KYONB, SSCTL, LPCTL, PCMS
			UpdateStreamStep();
			// PCMS shares the high byte with the key/loop controls. It changes
			// both ADPCM octave limiting and PCM16 SA alignment immediately.
			UpdatePitch();
			UpdateSA();
			if ((offset == 1 || size == 2) && ccd->KYONEX)
			{
				ccd->KYONEX=0;
				for (ChannelEx& channel : Chans)
				{
					if (channel.ccd->KYONB)
						channel.KEY_ON();
					else
						channel.KEY_OFF();
				}
			}
			break;

		case 0x04: // SA
		case 0x05: // SA
			UpdateSA();
			break;

		case 0x08://LSA
		case 0x09://LSA
		case 0x0C://LEA
		case 0x0D://LEA
			UpdateLoop();
			break;

		case 0x10://D1R,AR
		case 0x11://D2R,D1R
			UpdateAEG();
			break;

		case 0x14://RR,DL
		case 0x15://DL,KRS,LPSLINK
			UpdateStreamStep();
			UpdateAEG();
			break;

		case 0x18://FNS
		case 0x19://FNS,OCT
			UpdatePitch();
			UpdateAEG();
			UpdateFEG();
			break;

		case 0x1C://ALFOS,ALFOWS,PLFOS
		case 0x1D://PLFOWS,LFOF,LFORE
			UpdateLFO(false);
			break;

		case 0x20://ISEL,IMXL
		//case 0x21://nothing here !
			UpdateDSPMIX();
			UpdateAtts();
			break;

		case 0x24://DIPAN
		case 0x25://DISDL
			UpdateAtts();
			break;

		case 0x28://Q, LPOFF, SDIR
		case 0x29://TL
			if (size == 2 || offset == 0x28)
				UpdateFEG();
			UpdateAtts();
			break;

		case 0x2C: //FLV0
		case 0x2D: //FLV0
		case 0x30: //FLV1
		case 0x31: //FLV1
		case 0x34: //FLV2
		case 0x35: //FLV2
		case 0x38: //FLV3
		case 0x39: //FLV3
		case 0x3C: //FLV4
		case 0x3D: //FLV4
		case 0x40: //FD1R
		case 0x41: //FAR
		case 0x44: //FRR
		case 0x45: //FD2R
			UpdateFEG();
			break;

		}
	} 

	static void initAll() {
		for (std::size_t i = 0; i < std::size(Chans); i++)
			Chans[i].Init(i, aica_reg);
	}
};

void (ChannelEx::*ChannelEx::STREAM_STEP_LUT[7][2][2])();
void (ChannelEx::*ChannelEx::STREAM_INITAL_STEP_LUT[7])();
void (ChannelEx::*ChannelEx::AEG_STEP_LUT[4])();
void (ChannelEx::*ChannelEx::FEG_STEP_LUT[4])();
void (ChannelEx::Lfo::*ChannelEx::Lfo::ALFOWS_CALC[4])();
void (ChannelEx::Lfo::*ChannelEx::Lfo::PLFOWS_CALC[4])();

static SampleType DecodeADPCM(u32 sample, s32 prev, s32& quant)
{
	const AdpcmDecodeStep result = decodeAdpcmNibble(sample, prev, quant);
	quant = result.quant;
	return result.sample;
}

template<typename T>
static T readAicaRam(u32 addr) {
	static_assert(sizeof(T) == 1 || sizeof(T) == 2);
	const u32 aligned = addr & soundRamMask() & ~(sizeof(T) - 1);
	if constexpr (sizeof(T) == 1)
		return static_cast<T>(aica_ram[aligned]);
	else
		// Sound RAM is little-endian regardless of host byte order.
		return static_cast<T>(littleEndianPcm16(aica_ram[aligned], aica_ram[aligned + 1]));
}

template<PCMSType PCMS, bool Last>
void ChannelEx::StepDecodeSample(u32 CA)
{
	// PCM lookups can skip intermediate source positions. ADPCM and noise have
	// decoder/generator state that must advance once per traversed sample.
	if constexpr (!Last && (PCMS == PCM16 || PCMS == PCM8))
		return;
	if (sourceReadsSoundMemory(ccd->PCMS, ccd->SSCTL)
			&& !soundMemoryReadAllowed(CommonData->MRWINH, false))
		// A prohibited memory cycle leaves the source and ADPCM decoder
		// latches unchanged. Address generation continues independently.
		return;

	const u32 next_addr = interpolationNextAddress(CA, loop.LSA, loop.LEA,
			baseSampleFormat(PCMS), ccd->LPCTL);

	SampleType s0, s1;
	switch(PCMS)
	{
	case NOISE_PCM16:
		s0 = this->s1;
		noise_state = nextNoiseState(noise_state);
		s1 = noiseSample(noise_state);
		break;
	case NOISE_PCM8:
		s0 = this->s1;
		noise_state = nextNoiseState(noise_state);
		s1 = noisePcm8Sample(noise_state);
		break;

	case PCM16:
		s0 = readAicaRam<s16>(SA + CA * 2);
		s1 = readAicaRam<s16>(SA + next_addr * 2);
		break;

	case PCM8:
		s0 = signedPcm8(readAicaRam<u8>(SA + CA)) * 256;
		s1 = signedPcm8(readAicaRam<u8>(SA + next_addr)) * 256;
		break;

	case SILENCE:
		s0 = 0;
		s1 = 0;
		break;

	case ADPCM:
	case NOISE_ADPCM:
		{
			u8 ad1;
			u8 ad2;
			if constexpr (PCMS == NOISE_ADPCM)
			{
				ad1 = noise_lookahead;
				noise_state = nextNoiseState(noise_state);
				ad2 = noiseAdpcmNibble(noise_state);
				noise_lookahead = ad2;
			}
			else
			{
				ad1 = readAicaRam<u8>(SA + (CA >> 1));
				ad2 = readAicaRam<u8>(SA + (next_addr >> 1));
				ad1 = (ad1 >> ((CA & 1) * 4)) & 0xf;
				ad2 = (ad2 >> ((next_addr & 1) * 4)) & 0xf;
			}

			s32 q = adpcm.last_quant;
			if (CA == loop.LSA)
			{
				if (!adpcm.in_loop)
				{
					adpcm.in_loop = true;
					adpcm.loopstart_quant = q;
					adpcm.loopstart_prev_sample = this->s0;
				}
				else
				{
					q = adpcm.loopstart_quant;
					this->s0 = adpcm.loopstart_prev_sample;
				}
			}
			s0 = DecodeADPCM(ad1, this->s0, q);
			adpcm.last_quant = q;
			if constexpr (Last)
			{
				SampleType prev = s0;
				if (baseSampleFormat(PCMS) == ADPCM && next_addr == loop.LSA && adpcm.in_loop)
				{
					q = adpcm.loopstart_quant;
					prev = adpcm.loopstart_prev_sample;
				}
				s1 = DecodeADPCM(ad2, prev, q);
			}
			else {
				s1 = 0;
			}
		}
		break;
	}
	
	this->s0 = s0;
	this->s1 = s1;
}

template<PCMSType PCMS, bool LPCTL, bool LPSLNK>
void ChannelEx::StreamStep()
{
	step.full += FPMul(update_rate, lfo.plfo_step.full, 10);
	fp_22_10 sp = step;
	step.ip = 0;

	while (sp.ip > 0)
	{
		sp.ip--;

		u32 CA = this->CA + 1;

		if constexpr (LPSLNK)
		{
			if (AEG.state == EG_Attack && loopStartLinkTriggered(AEG.Decay1Rate, CA, loop.LSA))
			{
				step_printf("[%d]LPSLNK : Switching to EG_Decay1 %X", ChannelNumber, AEG.GetValue());
				SetAegState(EG_Decay1);
			}
		}

		const SampleAddressStep addressStep = advanceSampleAddress(
				this->CA, loop.LSA, loop.LEA, baseSampleFormat(PCMS), LPCTL);
		CA = addressStep.address;
		if (addressStep.looped)
			loop.looped = 1;
		if (resetLfoAtLoop(ccd->LFORE, addressStep.looped && !addressStep.stopped))
			lfo.Reset();
		if (addressStep.stopped)
		{
			disable();
			this->CA = CA;
			break;
		}
		if (addressStep.looped)
			key_printf("[%d]LPCTL : Looping LSA %x LEA %x AEG %x", ChannelNumber, loop.LSA, loop.LEA, AEG.GetValue());

		this->CA = CA;

		//keep adpcm up to date
		if (sp.ip == 0)
			StepDecodeSample<PCMS, true>(CA);
		else
			StepDecodeSample<PCMS, false>(CA);
	}
}

template<LFOType Type>
void ChannelEx::Lfo::CalcAlfo()
{
	const u32 rv = alfoWaveform(static_cast<u32>(Type),
			Type == LFOType::Random ? noise_value : state);
	alfo = alfoAttenuation(rv, alfo_scale);
}

template<LFOType Type>
void ChannelEx::Lfo::CalcPlfo()
{
	const int rv = plfoWaveform(static_cast<u32>(Type),
			Type == LFOType::Random ? noise_value : state);
	plfo_step.full = plfo_scale[rv + 128];
}

template<EGState state>
void ChannelEx::AegStep()
{
	u32 rate;
	switch (state)
	{
	case EG_Attack: rate = AEG.AttackRate; break;
	case EG_Decay1: rate = AEG.Decay1Rate; break;
	case EG_Decay2: rate = AEG.Decay2Rate; break;
	case EG_Release: rate = AEG.ReleaseRate; break;
	}
	const EnvelopeClockStep clock = stepEnvelopeClock(AEG.counter, AEG.step, rate);
	AEG.counter = clock.counter;
	AEG.step = clock.step;
	if (!clock.advance)
		return;

	switch(state)
	{
	case EG_Attack:
		if (AEG.GetValue() != 0)
		{
			const u32 decrement = (static_cast<u32>(AEG.GetValue())
					>> envelopeAttackShift(rate, AEG.step - 1)) + 1;
			AEG.SetValue(decrement >= static_cast<u32>(AEG.GetValue())
					? 0 : static_cast<u32>(AEG.GetValue()) - decrement);
			if (AEG.GetValue() <= 0)
			{
				if (attackDecay1Triggered(ccd->LPSLNK, AEG.Decay1Rate))
				{
					aeg_printf("[%d]AEG_step : Switching to EG_Decay1", ChannelNumber);
					SetAegState(EG_Decay1);
				}
				AEG.SetValue(0);
			}
		}
		break;
	case EG_Decay1:
		AEG.SetValue(std::min(0x3ffu, static_cast<u32>(AEG.GetValue())
				+ envelopeDecayStep(rate, AEG.step - 1)));
		if (aegIsInactive(AEG.GetValue()))
		{
			disable();
			break;
		}
		if ((u32)AEG.GetValue() >= AEG.Decay2Value)
		{
			aeg_printf("[%d]AEG_step : Switching to EG_Decay2", ChannelNumber);
			SetAegState(EG_Decay2);
		}
		break;
	case EG_Decay2:
		AEG.SetValue(std::min(0x3ffu, static_cast<u32>(AEG.GetValue())
				+ envelopeDecayStep(rate, AEG.step - 1)));
		if (aegIsInactive(AEG.GetValue()))
			disable();
		break;
	case EG_Release: //only on key_off
		AEG.SetValue(std::min(0x3ffu, static_cast<u32>(AEG.GetValue())
				+ envelopeDecayStep(rate, AEG.step - 1)));
		if (aegIsInactive(AEG.GetValue()))
		{
			aeg_printf("[%d]AEG_step : EG_Release End @ %x", ChannelNumber, AEG.GetValue());
			disable();
		}
		break;
	}
}
template<EGState state>
void ChannelEx::FegStep()
{
	u32 rate;
	u32 target;
	switch(state)
	{
	case EG_Attack:
		rate = FEG.AttackRate;
		target = ccd->FLV1;
		break;
	case EG_Decay1:
		rate = FEG.Decay1Rate;
		target = ccd->FLV2;
		break;
	case EG_Decay2:
		rate = FEG.Decay2Rate;
		target = ccd->FLV3;
		break;
	case EG_Release:
		rate = FEG.ReleaseRate;
		target = ccd->FLV4;
		break;
	}
	const EnvelopeClockStep clock = stepEnvelopeClock(FEG.counter, FEG.step, rate);
	FEG.counter = clock.counter;
	FEG.step = clock.step;
	if (!clock.advance)
		return;

	const FilterEnvelopeStep envelope = advanceFilterEnvelope(FEG.GetValue(), target,
			envelopeDecayStep(rate, FEG.step - 1));
	FEG.SetValue(envelope.value);
	if (envelope.reached && FEG.state < EG_Decay2)
	{
		feg_printf("[%d]FEG_step : Switching to next state: %d Freq %d", ChannelNumber, (int)FEG.state + 1, target);
		SetFegState((EGState)((int)FEG.state + 1));
	}
}
static void staticinitialise()
{
	ChannelEx::STREAM_STEP_LUT[PCM16][0][0] = &ChannelEx::StreamStep<PCM16, false, false>;
	ChannelEx::STREAM_STEP_LUT[PCM8][0][0] = &ChannelEx::StreamStep<PCM8, false, false>;
	ChannelEx::STREAM_STEP_LUT[ADPCM][0][0] = &ChannelEx::StreamStep<ADPCM, false, false>;
	ChannelEx::STREAM_STEP_LUT[SILENCE][0][0] = &ChannelEx::StreamStep<SILENCE, false, false>;
	ChannelEx::STREAM_STEP_LUT[NOISE_PCM16][0][0] = &ChannelEx::StreamStep<NOISE_PCM16, false, false>;
	ChannelEx::STREAM_STEP_LUT[NOISE_PCM8][0][0] = &ChannelEx::StreamStep<NOISE_PCM8, false, false>;
	ChannelEx::STREAM_STEP_LUT[NOISE_ADPCM][0][0] = &ChannelEx::StreamStep<NOISE_ADPCM, false, false>;

	ChannelEx::STREAM_STEP_LUT[PCM16][0][1] = &ChannelEx::StreamStep<PCM16, false, true>;
	ChannelEx::STREAM_STEP_LUT[PCM8][0][1] = &ChannelEx::StreamStep<PCM8, false, true>;
	ChannelEx::STREAM_STEP_LUT[ADPCM][0][1] = &ChannelEx::StreamStep<ADPCM, false, true>;
	ChannelEx::STREAM_STEP_LUT[SILENCE][0][1] = &ChannelEx::StreamStep<SILENCE, false, true>;
	ChannelEx::STREAM_STEP_LUT[NOISE_PCM16][0][1] = &ChannelEx::StreamStep<NOISE_PCM16, false, true>;
	ChannelEx::STREAM_STEP_LUT[NOISE_PCM8][0][1] = &ChannelEx::StreamStep<NOISE_PCM8, false, true>;
	ChannelEx::STREAM_STEP_LUT[NOISE_ADPCM][0][1] = &ChannelEx::StreamStep<NOISE_ADPCM, false, true>;

	ChannelEx::STREAM_STEP_LUT[PCM16][1][0] = &ChannelEx::StreamStep<PCM16, true, false>;
	ChannelEx::STREAM_STEP_LUT[PCM8][1][0] = &ChannelEx::StreamStep<PCM8, true, false>;
	ChannelEx::STREAM_STEP_LUT[ADPCM][1][0] = &ChannelEx::StreamStep<ADPCM, true, false>;
	ChannelEx::STREAM_STEP_LUT[SILENCE][1][0] = &ChannelEx::StreamStep<SILENCE, true, false>;
	ChannelEx::STREAM_STEP_LUT[NOISE_PCM16][1][0] = &ChannelEx::StreamStep<NOISE_PCM16, true, false>;
	ChannelEx::STREAM_STEP_LUT[NOISE_PCM8][1][0] = &ChannelEx::StreamStep<NOISE_PCM8, true, false>;
	ChannelEx::STREAM_STEP_LUT[NOISE_ADPCM][1][0] = &ChannelEx::StreamStep<NOISE_ADPCM, true, false>;

	ChannelEx::STREAM_STEP_LUT[PCM16][1][1] = &ChannelEx::StreamStep<PCM16, true, true>;
	ChannelEx::STREAM_STEP_LUT[PCM8][1][1] = &ChannelEx::StreamStep<PCM8, true, true>;
	ChannelEx::STREAM_STEP_LUT[ADPCM][1][1] = &ChannelEx::StreamStep<ADPCM, true, true>;
	ChannelEx::STREAM_STEP_LUT[SILENCE][1][1] = &ChannelEx::StreamStep<SILENCE, true, true>;
	ChannelEx::STREAM_STEP_LUT[NOISE_PCM16][1][1] = &ChannelEx::StreamStep<NOISE_PCM16, true, true>;
	ChannelEx::STREAM_STEP_LUT[NOISE_PCM8][1][1] = &ChannelEx::StreamStep<NOISE_PCM8, true, true>;
	ChannelEx::STREAM_STEP_LUT[NOISE_ADPCM][1][1] = &ChannelEx::StreamStep<NOISE_ADPCM, true, true>;

	ChannelEx::STREAM_INITAL_STEP_LUT[PCM16] = &ChannelEx::StepDecodeSampleInitial<PCM16>;
	ChannelEx::STREAM_INITAL_STEP_LUT[PCM8] = &ChannelEx::StepDecodeSampleInitial<PCM8>;
	ChannelEx::STREAM_INITAL_STEP_LUT[ADPCM] = &ChannelEx::StepDecodeSampleInitial<ADPCM>;
	ChannelEx::STREAM_INITAL_STEP_LUT[SILENCE] = &ChannelEx::StepDecodeSampleInitial<SILENCE>;
	ChannelEx::STREAM_INITAL_STEP_LUT[NOISE_PCM16] = &ChannelEx::StepDecodeSampleInitial<NOISE_PCM16>;
	ChannelEx::STREAM_INITAL_STEP_LUT[NOISE_PCM8] = &ChannelEx::StepDecodeSampleInitial<NOISE_PCM8>;
	ChannelEx::STREAM_INITAL_STEP_LUT[NOISE_ADPCM] = &ChannelEx::StepDecodeSampleInitial<NOISE_ADPCM>;

	ChannelEx::AEG_STEP_LUT[EG_Attack] = &ChannelEx::AegStep<EG_Attack>;
	ChannelEx::AEG_STEP_LUT[EG_Decay1] = &ChannelEx::AegStep<EG_Decay1>;
	ChannelEx::AEG_STEP_LUT[EG_Decay2] = &ChannelEx::AegStep<EG_Decay2>;
	ChannelEx::AEG_STEP_LUT[EG_Release] = &ChannelEx::AegStep<EG_Release>;

	ChannelEx::FEG_STEP_LUT[EG_Attack] = &ChannelEx::FegStep<EG_Attack>;
	ChannelEx::FEG_STEP_LUT[EG_Decay1] = &ChannelEx::FegStep<EG_Decay1>;
	ChannelEx::FEG_STEP_LUT[EG_Decay2] = &ChannelEx::FegStep<EG_Decay2>;
	ChannelEx::FEG_STEP_LUT[EG_Release] = &ChannelEx::FegStep<EG_Release>;

	ChannelEx::Lfo::ALFOWS_CALC[(int)LFOType::Sawtooth] = &ChannelEx::Lfo::CalcAlfo<LFOType::Sawtooth>;
	ChannelEx::Lfo::ALFOWS_CALC[(int)LFOType::Square] = &ChannelEx::Lfo::CalcAlfo<LFOType::Square>;
	ChannelEx::Lfo::ALFOWS_CALC[(int)LFOType::Triangle] = &ChannelEx::Lfo::CalcAlfo<LFOType::Triangle>;
	ChannelEx::Lfo::ALFOWS_CALC[(int)LFOType::Random] = &ChannelEx::Lfo::CalcAlfo<LFOType::Random>;

	ChannelEx::Lfo::PLFOWS_CALC[(int)LFOType::Sawtooth] = &ChannelEx::Lfo::CalcPlfo<LFOType::Sawtooth>;
	ChannelEx::Lfo::PLFOWS_CALC[(int)LFOType::Square] = &ChannelEx::Lfo::CalcPlfo<LFOType::Square>;
	ChannelEx::Lfo::PLFOWS_CALC[(int)LFOType::Triangle] = &ChannelEx::Lfo::CalcPlfo<LFOType::Triangle>;
	ChannelEx::Lfo::PLFOWS_CALC[(int)LFOType::Random] = &ChannelEx::Lfo::CalcPlfo<LFOType::Random>;

	for (std::size_t i = 1; i < std::size(volume_lut); i++)
		volume_lut[i] = mixerLevelAttenuation(i);

	for (int i = 0; i < 256; i++)
		tl_lut[i] = logLinearAttenuation(i);
	//tl entries 256 to 1023 are 0

	for (int s = 0; s < 8; s++)
	{
		for (int i = -128; i < 128; i++)
			PLFO_Scales[s][i + 128] = plfoScale(s, i);
	}
}
static OnLoad staticInit(staticinitialise);

ChannelEx ChannelEx::Chans[64];

#define Chans ChannelEx::Chans

void init()
{
	ChannelEx::initAll();
	for (VmuBeep& beep : vmuBeeps)
		beep.init();
	cdda_index = CDDA_SIZE;
	memset(cdda_sector, 0, sizeof(cdda_sector));
	dsp::init();
}

void term()
{
	dsp::term();
}

void WriteChannelReg(u32 channel, u32 reg, int size)
{
	Chans[channel].RegWrite(reg, size);
}

u16 ReadCommonReg(u32 reg,bool byte)
{
	u16 readFlags = 0;
	switch(reg)
	{
	case 0x2808:
	case 0x2809:
		CommonData->MIEMP = midiSendBuffer.empty();
		CommonData->MIFUL = midiSendBuffer.size() >= MidiFifoSize;
		CommonData->MOEMP = midiOutputBuffer.empty();
		CommonData->MOFUL = midiOutputBuffer.size() == MidiFifoSize;
		if (!byte || reg == 0x2808)
		{
			readFlags = (CommonData->MIEMP << 8) | (CommonData->MIFUL << 9) | (CommonData->MIOVF << 10)
					| (CommonData->MOEMP << 11) | (CommonData->MOFUL << 12);
			u8 data;
			if (midiRead(data))
				CommonData->MIBUF = data;
			CommonData->MIOVF = 0;
		}
		break;
	case 0x2810: // EG, SGC, LP
	case 0x2811:
		{
			ChannelEx& channel = Chans[CommonData->MSLC];
			
			CommonData->LP = channel.loop.looped;
			if (CommonData->AFSEL)
			{
				CommonData->EG = channel.FEG.GetValue();
				CommonData->SGC = channel.FEG.state;
			}
			else
			{
				CommonData->EG = aegMonitorValue(channel.AEG.GetValue());
				CommonData->SGC = channel.AEG.state;
			}

			if (!byte || reg == 0x2811)
				channel.loop.looped = 0;
		}
		break;
	case 0x2814: //CA
	case 0x2815: //CA
		{
			const ChannelEx& channel = Chans[CommonData->MSLC];
			CommonData->CA = channel.CA;
		}
		break;
	}
	return readFlags;
}

void vmuBeep(int bus, int port, int reload, int transition)
{
	if (bus < 0 || bus >= 4 || port < 0 || port >= 2)
		return;
	vmuBeeps[bus * 2 + port].update(reload, transition);
}

void AICA_Sample()
{
	SampleType mixl,mixr;
	mixl = 0;
	mixr = 0;
	// MIXS has two generations: slots accumulate the new sample while the DSP
	// reads the complete generation retained from the preceding sample.
	s32 dspInputs[16];
	memcpy(dspInputs, dsp::state.MIXS, sizeof(dspInputs));
	memset(dsp::state.MIXS, 0, sizeof(dsp::state.MIXS));

	ChannelEx::StepAll(mixl,mixr);
	s32 nextDspInputs[16];
	memcpy(nextDspInputs, dsp::state.MIXS, sizeof(nextDspInputs));
	memcpy(dsp::state.MIXS, dspInputs, sizeof(dspInputs));
	
	//OK , generated all Channels  , now DSP/ect + final mix ;p
	//CDDA EXTS input
	
	if (cdda_index >= CDDA_SIZE)
	{
		cdda_index = 0;
		libCore_CDDA_Sector(cdda_sector);
	}
	s32 EXTS0L = cdda_sector[cdda_index];
	s32 EXTS0R = cdda_sector[cdda_index+1];
	cdda_index += 2;

	//Final MIX ..
	//Add CDDA / DSP effect(s)

	//CDDA
	VolumePan(EXTS0L, dsp_out_vol[16].EFSDL, dsp_out_vol[16].EFPAN, mixl, mixr);
	VolumePan(EXTS0R, dsp_out_vol[17].EFSDL, dsp_out_vol[17].EFPAN, mixl, mixr);

	DSPData->EXTS[0] = EXTS0L;
	DSPData->EXTS[1] = EXTS0R;

	if (config::DSPEnabled)
	{
		// The output mixer reads EFREG before this sample's DSP writes, so the
		// newly generated effects remain pending until the next sample.
		u16 effectOutputs[16];
		for (int i = 0; i < 16; ++i)
			effectOutputs[i] = DSPData->EFREG[i];
		dsp::step();
		memcpy(dsp::state.MIXS, nextDspInputs, sizeof(nextDspInputs));

		for (int i = 0; i < 16; i++)
			VolumePan(signedPcm16(effectOutputs[i]), dsp_out_vol[i].EFSDL, dsp_out_vol[i].EFPAN, mixl, mixr);
	}
	else
	{
		memcpy(dsp::state.MIXS, nextDspInputs, sizeof(nextDspInputs));
		dsp::bypassStep();
	}
	// VMU PWM generators continue running with emulated time even when their
	// optional host mix is disabled or host audio delivery is muted.
	SampleType b = 0;
	for (VmuBeep& beep : vmuBeeps)
		b += beep.getSample();
	if (config::VmuSound)
	{
		mixl += b;
		mixr += b;
	}

#ifdef LIBRETRO
	if (settings.aica.muteAudio)
#else
	if (settings.input.fastForwardMode || settings.aica.muteAudio)
#endif
		return;

	//MVOL !
	//we want to make sure mix* is *At least* 23 bits wide here, so 64 bit mul !
	u32 mvol = CommonData->MVOL;
	s32 val = volume_lut[mvol];
	mixl = (s32)FPMul<s64>(mixl, val, 15);
	mixr = (s32)FPMul<s64>(mixr, val, 15);

	//Sample is ready ! clip/saturate and store :}

#ifdef CLIP_WARN
	if ((s16)mixl != mixl || (s16)mixr != mixr)
		printf("Clipped mixl %d mixr %d\n", mixl, mixr);
#endif

	mixl = quantizeDacOutput(mixl, CommonData->DAC18B);
	mixr = quantizeDacOutput(mixr, CommonData->DAC18B);

	WriteSample(mixr, mixl);
}

void serialize(Serializer& ser)
{
	for (const ChannelEx& channel : Chans)
	{
		ser << channel.SA;
		ser << channel.CA;
		ser << channel.step;
		ser << channel.s0;
		ser << channel.s1;
		ser << channel.loop.looped;
		ser << channel.adpcm.last_quant;
		ser << channel.adpcm.loopstart_quant;
		ser << channel.adpcm.loopstart_prev_sample;
		ser << channel.adpcm.in_loop;
		ser << channel.noise_state;
		ser << channel.noise_lookahead;

		ser << channel.AEG.val;
		ser << channel.AEG.state;
		ser << channel.AEG.counter;
		ser << channel.AEG.step;
		ser << channel.FEG.value;
		ser << channel.FEG.state;
		ser << channel.FEG.prev1;
		ser << channel.FEG.prev2;
		ser << channel.FEG.fractSave;
		ser << channel.FEG.counter;
		ser << channel.FEG.step;

		ser << channel.lfo.counter;
		ser << channel.lfo.state;
		ser << channel.lfo.noise_state;
		ser << channel.lfo.noise_value;
		ser << channel.enabled;
	}
	for (VmuBeep& beep : vmuBeeps)
		beep.serialize(ser);
	ser << cdda_sector;
	ser << cdda_index;
	ser << (u32)midiSendBuffer.size();
	for (u8 b : midiSendBuffer)
		ser << b;
}

void deserialize(Deserializer& deser)
{
	for (ChannelEx& channel : Chans)
	{
		channel.quiet = true;
		deser >> channel.SA;
		deser >> channel.CA;
		deser >> channel.step;
		channel.UpdatePitch();
		deser >> channel.s0;
		deser >> channel.s1;
		deser >> channel.loop.looped;
		channel.UpdateLoop();
		deser >> channel.adpcm.last_quant;
		deser >> channel.adpcm.loopstart_quant;
		deser >> channel.adpcm.loopstart_prev_sample;
		deser >> channel.adpcm.in_loop;
		deser >> channel.noise_state;
		if (deser.version() >= Deserializer::V123)
			deser >> channel.noise_lookahead;
		else
			channel.noise_lookahead = noiseAdpcmNibble(channel.noise_state);
		channel.UpdateAtts();
		channel.UpdateDSPMIX();

		deser >> channel.AEG.val;
		deser >> channel.AEG.state;
		channel.SetAegState(channel.AEG.state);
		channel.UpdateAEG();
		if (deser.version() >= Deserializer::V122)
		{
			deser >> channel.AEG.counter;
			deser >> channel.AEG.step;
		}
		else
		{
			channel.AEG.step = 0;
			u32 rate = channel.AEG.state == EG_Attack ? channel.AEG.AttackRate
					: channel.AEG.state == EG_Decay1 ? channel.AEG.Decay1Rate
					: channel.AEG.state == EG_Decay2 ? channel.AEG.Decay2Rate
					: channel.AEG.ReleaseRate;
			channel.AEG.counter = envelopeClockPeriod(rate, 0);
		}
		deser >> channel.FEG.value;
		deser >> channel.FEG.state;
		deser >> channel.FEG.prev1;
		deser >> channel.FEG.prev2;
		if (deser.version() >= Deserializer::V56) {
			deser >> channel.FEG.fractSave;
		}
		else
		{
			channel.FEG.fractSave = 0;
			channel.FEG.prev1 = 0;
			channel.FEG.prev2 = 0;
		}
		channel.UpdateFEG();
		if (deser.version() >= Deserializer::V126)
		{
			deser >> channel.FEG.counter;
			deser >> channel.FEG.step;
		}
		else
		{
			channel.FEG.step = 0;
			u32 rate = channel.FEG.state == EG_Attack ? channel.FEG.AttackRate
					: channel.FEG.state == EG_Decay1 ? channel.FEG.Decay1Rate
					: channel.FEG.state == EG_Decay2 ? channel.FEG.Decay2Rate
					: channel.FEG.ReleaseRate;
			channel.FEG.counter = envelopeClockPeriod(rate, 0);
		}
		// Preserve the serialized attack level; resetting it to FLV0 here
		// causes an audible filter discontinuity after loading a state.
		channel.SetFegState(channel.FEG.state, false);
		channel.UpdateStreamStep();

		deser >> channel.lfo.counter;
		deser >> channel.lfo.state;
		if (deser.version() >= Deserializer::V134)
		{
			deser >> channel.lfo.noise_state;
			deser >> channel.lfo.noise_value;
		}
		else
		{
			channel.lfo.noise_state = 0;
			channel.lfo.noise_value = 0;
		}
		channel.UpdateLFO(true);
		deser >> channel.enabled;
		channel.quiet = false;
	}
	if (deser.version() >= Deserializer::V103)
	{
		for (VmuBeep& beep : vmuBeeps)
			beep.deserialize(deser);
	}
	else
	{
		vmuBeeps[0].deserialize(deser);
		for (size_t i = 1; i < vmuBeeps.size(); i++)
			vmuBeeps[i].init();
	}
	deser >> cdda_sector;
	deser >> cdda_index;
	midiSendBuffer.clear();
	if (deser.version() >= Deserializer::V28)
	{
		u32 size;
		deser >> size;
		const MidiRestoreState restore = midiRestoreState(size);
		for (u32 i = 0; i < size; i++)
		{
			u8 b;
			deser >> b;
			if (midiSendBuffer.size() < restore.retained)
				midiSendBuffer.push_back(b);
		}
		if (restore.overflow)
			CommonData->MIOVF = 1;
	}
	CommonData->MIEMP = midiSendBuffer.empty();
	CommonData->MIFUL = midiSendBuffer.size() == MidiFifoSize;
	// MIDI_IN is an edge-triggered latch and may have been acknowledged while
	// bytes remain queued. Its serialized SCIPD/MCIPD bits are authoritative;
	// only the level-derived FIFO status flags are reconstructed here.
}

} // namespace aica::sgc
