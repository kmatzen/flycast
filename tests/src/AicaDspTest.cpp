#include "types.h"
#include "hw/aica/aica.h"
#include "hw/aica/aica_if.h"
#include "hw/aica/aica_mem.h"
#include "hw/aica/dsp.h"
#include "hw/aica/sgc_if.h"
#include "hw/sh4/sh4_sched.h"

#include "gtest/gtest.h"

namespace aica::dsp
{

static u8 receivedMidiByte;
static bool midiReceiverSawNonEmpty;
static bool midiReceiverSawClearedInterrupt;

static void captureMidiOutput(u8 data)
{
	receivedMidiByte = data;
	midiReceiverSawNonEmpty = !CommonData->MOEMP;
	midiReceiverSawClearedInterrupt = !SCIPD->MIDI_OUT && !MCIPD->MIDI_OUT;
}

TEST(AicaDspArithmeticTest, AccumulatorWrapsOnSigned26BitBus)
{
	EXPECT_EQ(wrapAccumulator(0x01ffffff), 0x01ffffff);
	EXPECT_EQ(wrapAccumulator(0x02000000), -0x02000000);
	EXPECT_EQ(wrapAccumulator(0x03ffffff), -1);
	EXPECT_EQ(wrapAccumulator(0x04000000), 0);
	EXPECT_EQ(wrapAccumulator(-0x02000001), 0x01ffffff);
}

TEST(AicaDspArithmeticTest, InterpreterUsesExplicitArithmeticShift)
{
	EXPECT_EQ(dsp::arithmeticShiftRight(0x1001, 12), 1);
	EXPECT_EQ(dsp::arithmeticShiftRight(-1, 12), -1);
	EXPECT_EQ(dsp::arithmeticShiftRight(-0x1000, 12), -1);
	EXPECT_EQ(dsp::arithmeticShiftRight(-0x1001, 12), -2);
}

TEST(AicaEnvelopeTest, EffectiveRateIncludesSignedOctaveScaling)
{
	EXPECT_EQ(sgc::effectiveEnvelopeRate(31, 0, 8, 0), 46u);
	EXPECT_EQ(sgc::effectiveEnvelopeRate(0, 0, 8, 0), 0u);
	EXPECT_EQ(sgc::effectiveEnvelopeRate(20, 4, 15, 0x200), 47u);
	EXPECT_EQ(sgc::effectiveEnvelopeRate(31, 14, 7, 0), 0x3cu);
}

TEST(AicaMixerTest, AttenuationUsesHardwareLogLinearMantissa)
{
	EXPECT_EQ(sgc::logLinearAttenuation(0x00), 31 << 10);
	EXPECT_EQ(sgc::logLinearAttenuation(0x01), 30 << 10);
	EXPECT_EQ(sgc::logLinearAttenuation(0x0f), 16 << 10);
	EXPECT_EQ(sgc::logLinearAttenuation(0x10), (31 << 10) >> 1);
	EXPECT_EQ(sgc::logLinearAttenuation(0x20), (31 << 10) >> 2);
	EXPECT_EQ(sgc::logLinearAttenuation(0xe0), (31 << 10) >> 14);
	EXPECT_EQ(sgc::logLinearAttenuation(0xf0), 0);
	EXPECT_EQ(sgc::logLinearAttenuation(0x3ff), 0);
}

TEST(AicaMixerTest, FixedPointProductsUseArithmeticFloorShift)
{
	EXPECT_EQ(sgc::arithmeticShiftRight(17, 4), 1);
	EXPECT_EQ(sgc::arithmeticShiftRight(16, 4), 1);
	EXPECT_EQ(sgc::arithmeticShiftRight(-1, 4), -1);
	EXPECT_EQ(sgc::arithmeticShiftRight(-16, 4), -1);
	EXPECT_EQ(sgc::arithmeticShiftRight(-17, 4), -2);
}

TEST(AicaSampleTest, LinearInterpolationRoundsAfterMultiplyAccumulate)
{
	EXPECT_EQ(sgc::interpolateSample(100, 200, 0), 100);
	EXPECT_EQ(sgc::interpolateSample(100, 200, 0x200), 150);
	EXPECT_EQ(sgc::interpolateSample(100, 200, 0x3ff), 199);
	EXPECT_EQ(sgc::interpolateSample(-1, 1, 0x200), 0);
	EXPECT_EQ(sgc::interpolateSample(-1, 0, 1), -1);
	EXPECT_EQ(sgc::interpolateSample(32767, -32768, 0x200), -1);
}

TEST(AicaMixerTest, FourBitMixerLevelsShareAttenuationDomain)
{
	EXPECT_EQ(sgc::mixerLevelAttenuation(0), 0);
	EXPECT_EQ(sgc::mixerLevelAttenuation(15), sgc::logLinearAttenuation(0));
	EXPECT_EQ(sgc::mixerLevelAttenuation(14), sgc::logLinearAttenuation(8));
	EXPECT_EQ(sgc::mixerLevelAttenuation(1), sgc::logLinearAttenuation(14 << 3));
}

TEST(AicaMixerTest, OutputVolumeAndPanShareOneLogLinearConversion)
{
	EXPECT_EQ(sgc::outputMixerAttenuation(15, 0), sgc::logLinearAttenuation(0));
	EXPECT_EQ(sgc::outputMixerAttenuation(14, 1), sgc::logLinearAttenuation(16));
	EXPECT_EQ(sgc::outputMixerAttenuation(15, 2), sgc::logLinearAttenuation(16));
	EXPECT_EQ(sgc::outputMixerAttenuation(0, 0), 0);
	EXPECT_EQ(sgc::outputMixerAttenuation(15, 15), 0);
}

TEST(AicaEnvelopeTest, EffectiveRateSaturatesAtHardwareMaximum)
{
	EXPECT_EQ(sgc::effectiveEnvelopeRate(30, 0xf, 0, 0), 0x3cu);
	EXPECT_EQ(sgc::effectiveEnvelopeRate(31, 0xf, 0, 0), 0x3cu);
	EXPECT_EQ(sgc::effectiveEnvelopeRate(31, 14, 7, 0x200), 0x3cu);
}

TEST(AicaEnvelopeTest, ClockUsesMeasuredHardwarePeriodPatterns)
{
	EXPECT_EQ(sgc::envelopeClockPeriod(0, 0), 0u);
	EXPECT_EQ(sgc::envelopeClockPeriod(1, 0), 0u);
	EXPECT_EQ(sgc::envelopeClockPeriod(2, 0), 8192u);
	EXPECT_EQ(sgc::envelopeClockPeriod(2, 1), 4096u);
	EXPECT_EQ(sgc::envelopeClockPeriod(2, 2), 4096u);
	EXPECT_EQ(sgc::envelopeClockPeriod(3, 0), 8192u);
	EXPECT_EQ(sgc::envelopeClockPeriod(3, 6), 4096u);
	EXPECT_EQ(sgc::envelopeClockPeriod(4, 0), 4096u);
	EXPECT_EQ(sgc::envelopeClockPeriod(5, 2), 4096u);
	EXPECT_EQ(sgc::envelopeClockPeriod(5, 3), 2048u);
	EXPECT_EQ(sgc::envelopeClockPeriod(0x30, 0), 2u);
	EXPECT_EQ(sgc::envelopeClockPeriod(0x3c, 0), 2u);
}

TEST(AicaEnvelopeTest, HighRatesUseMeasuredFourStepPattern)
{
	EXPECT_EQ(sgc::envelopeDecayStep(0x30, 0), 1u);
	EXPECT_EQ(sgc::envelopeDecayStep(0x31, 0), 2u);
	EXPECT_EQ(sgc::envelopeDecayStep(0x31, 1), 1u);
	EXPECT_EQ(sgc::envelopeDecayStep(0x32, 2), 2u);
	EXPECT_EQ(sgc::envelopeDecayStep(0x32, 3), 1u);
	EXPECT_EQ(sgc::envelopeDecayStep(0x33, 2), 2u);
	EXPECT_EQ(sgc::envelopeDecayStep(0x33, 3), 1u);
	EXPECT_EQ(sgc::envelopeDecayStep(0x34, 0), 2u);
	EXPECT_EQ(sgc::envelopeDecayStep(0x38, 0), 4u);
	EXPECT_EQ(sgc::envelopeDecayStep(0x3c, 0), 8u);
	EXPECT_EQ(sgc::envelopeAttackShift(0x30, 0), 4u);
	EXPECT_EQ(sgc::envelopeAttackShift(0x31, 0), 3u);
	EXPECT_EQ(sgc::envelopeAttackShift(0x3c, 0), 1u);
}

TEST(AicaEnvelopeTest, FilterEnvelopeUsesAegDecayStepSize)
{
	// Hardware trials show that FEG shares the AEG decay clock and step size.
	// Its wider 13-bit cutoff domain therefore takes longer to traverse fully.
	for (u32 rate : { 2u, 0x30u, 0x31u, 0x38u, 0x3cu })
		for (u32 step = 0; step < 8; ++step)
		{
			const u32 delta = sgc::envelopeDecayStep(rate, step);
			EXPECT_EQ(sgc::advanceFilterEnvelope(0, 0x1fff, delta).value, delta);
		}
}

TEST(AicaEnvelopeTest, ClockAdvancesOnlyAtPeriodBoundary)
{
	auto clock = sgc::stepEnvelopeClock(2, 0, 0x30);
	EXPECT_EQ(clock.counter, 1u);
	EXPECT_EQ(clock.step, 0u);
	EXPECT_FALSE(clock.advance);
	clock = sgc::stepEnvelopeClock(clock.counter, clock.step, 0x30);
	EXPECT_EQ(clock.counter, 2u);
	EXPECT_EQ(clock.step, 1u);
	EXPECT_TRUE(clock.advance);
}

TEST(VmuBuzzerTest, PwmFundamentalTracksTimerDutyCycle)
{
	EXPECT_EQ(sgc::vmuTimerPeriod(0xc0), 64u);
	EXPECT_EQ(sgc::vmuTimerPeriod(0), 256u);
	EXPECT_EQ(sgc::vmuPwmFundamentalScale(0xc0, 0xc0), 0u);
	EXPECT_EQ(sgc::vmuPwmFundamentalScale(0xc0, 0xa0), 0u);
	EXPECT_EQ(sgc::vmuPwmFundamentalScale(0xc0, 0xe0), 1024u);
	EXPECT_EQ(sgc::vmuPwmFundamentalScale(0xc0, 0xd0), 768u);
	EXPECT_EQ(sgc::vmuPwmFundamentalScale(0xc0, 0xf0), 768u);
}

TEST(VmuBuzzerTest, WaveStepRetainsFractionalTimerFrequency)
{
	EXPECT_EQ(sgc::vmuWaveStep(0), 0u);
	EXPECT_EQ(sgc::vmuWaveStep(sgc::vmuTimerPeriod(0xc0)), 5930u);
	EXPECT_EQ(sgc::vmuWaveStep(1000), 379u);
}

TEST(AicaEnvelopeTest, KeyRateScaleFDisablesPitchScaling)
{
	EXPECT_EQ(sgc::effectiveEnvelopeRate(20, 0xf, 7, 0x200), 40u);
	EXPECT_EQ(sgc::effectiveEnvelopeRate(20, 0xf, 8, 0), 40u);
}

TEST(AicaAdpcmTest, AddsFullDeltaBeforeClippingPrediction)
{
	const sgc::AdpcmDecodeStep positive = sgc::decodeAdpcmNibble(0x7, -20000, 24576);
	EXPECT_EQ(positive.sample, 26080);
	EXPECT_EQ(positive.quant, 24576);

	const sgc::AdpcmDecodeStep negative = sgc::decodeAdpcmNibble(0xf, 20000, 24576);
	EXPECT_EQ(negative.sample, -26080);
	EXPECT_EQ(negative.quant, 24576);
}

TEST(AicaAdpcmTest, ClipsPredictionAndQuantizerAtDocumentedLimits)
{
	EXPECT_EQ(sgc::decodeAdpcmNibble(0x7, 20000, 24576).sample, 32767);
	EXPECT_EQ(sgc::decodeAdpcmNibble(0xf, -20000, 24576).sample, -32768);

	const sgc::AdpcmDecodeStep minimum = sgc::decodeAdpcmNibble(0, 0, 127);
	EXPECT_EQ(minimum.sample, 15);
	EXPECT_EQ(minimum.quant, 127);
}

TEST(AicaDspRegisterTest, MemsTopByteWriteAlsoUpdatesLowByte)
{
	EXPECT_EQ(mergeDsp24TopByte(0x123456, 0xab, true), static_cast<s32>(0xffab34ab));
	EXPECT_EQ(mergeDsp24HighWord(0x123456, 0xabcd, true), static_cast<s32>(0xffabcdab));
}

TEST(AicaDspRegisterTest, TempTopByteWritePreservesLowByte)
{
	EXPECT_EQ(mergeDsp24TopByte(0x123456, 0xab, false), static_cast<s32>(0xffab3456));
	EXPECT_EQ(mergeDsp24HighWord(0x123456, 0xabcd, false), static_cast<s32>(0xffabcd56));
}

TEST(AicaDmaTest, ExecuteBitStartsOnlyWhenOneIsWritten)
{
	auto write = mergeDmaControlWrite(0x1235, 0, 0x00ff);
	EXPECT_EQ(write.value, 0x1201u);
	EXPECT_FALSE(write.start);

	write = mergeDmaControlWrite(0x0000, 0x3400, 0xff00);
	EXPECT_EQ(write.value, 0x3400u);
	EXPECT_FALSE(write.start);

	write = mergeDmaControlWrite(0x3400, 0x0001, 0x00ff);
	EXPECT_EQ(write.value, 0x3401u);
	EXPECT_TRUE(write.start);

	write = mergeDmaControlWrite(0xffff, 0xffff, 0xffff);
	EXPECT_EQ(write.value, 0xfffdu);
}

TEST(AicaDmaTest, GeometryUsesSparseFourByteRegisterEntries)
{
	const InternalDmaGeometry geometry = internalDmaGeometry(0x12, 0x3456,
			0x234, 0x20, 2_MB - 1);
	EXPECT_EQ(0x12d158u, geometry.soundAddress);
	EXPECT_EQ(0x8d0u, geometry.registerAddress);
	EXPECT_EQ(0x20u, geometry.transfers);
	EXPECT_EQ((2_MB - 0x12d158) / 4, geometry.transfersToRamEnd);

	const InternalDmaGeometry wrapped = internalDmaGeometry(0x7f, 0x3fff,
			0, 1, 2_MB - 1);
	EXPECT_EQ(0x1ffffcu, wrapped.soundAddress);
	EXPECT_EQ(1u, wrapped.transfersToRamEnd);
}

TEST(AicaDmaTest, ParameterRegistersAreProtectedFromInternalDma)
{
	EXPECT_TRUE(isInternalDmaParameterRegister(0x2880));
	EXPECT_TRUE(isInternalDmaParameterRegister(0x2884));
	EXPECT_TRUE(isInternalDmaParameterRegister(0x2888));
	EXPECT_TRUE(isInternalDmaParameterRegister(0x288c));
	EXPECT_TRUE(isInternalDmaParameterRegister(0xa880));
	EXPECT_TRUE(isInternalDmaParameterRegister(0xa88c));
	EXPECT_FALSE(isInternalDmaParameterRegister(0x287c));
	EXPECT_FALSE(isInternalDmaParameterRegister(0x2882));
	EXPECT_FALSE(isInternalDmaParameterRegister(0x2890));
}

TEST(AicaMemoryInhibitTest, SoundCpuGateMatchesMrwinhBitTwo)
{
	EXPECT_TRUE(soundCpuAccessAllowed(0));
	EXPECT_TRUE(soundCpuAccessAllowed(1));
	EXPECT_TRUE(soundCpuAccessAllowed(2));
	EXPECT_FALSE(soundCpuAccessAllowed(4));
	EXPECT_FALSE(soundCpuAccessAllowed(0xf));
}

TEST(AicaEnvelopeTest, MonitorUsesShiftedLevelAndInactiveSentinel)
{
	EXPECT_EQ(sgc::aegMonitorValue(0), 0u);
	EXPECT_EQ(sgc::aegMonitorValue(1), 8u);
	EXPECT_EQ(sgc::aegMonitorValue(0x3bf), 0x1df8u);
	EXPECT_EQ(sgc::aegMonitorValue(0x3c0), 0x1fffu);
	EXPECT_EQ(sgc::aegMonitorValue(0x3ff), 0x1fffu);
}

TEST(AicaEnvelopeTest, UsesHardwareAttackStartAndInactiveBoundary)
{
	EXPECT_EQ(sgc::AegAttackStart, 0x280u);
	EXPECT_FALSE(sgc::aegIsInactive(0x3bf));
	EXPECT_TRUE(sgc::aegIsInactive(0x3c0));
	EXPECT_TRUE(sgc::aegIsInactive(0x3ff));
}

TEST(AicaEnvelopeTest, LoopStartLinkRequiresNonzeroDecayRate)
{
	EXPECT_FALSE(sgc::loopStartLinkTriggered(0, 10, 10));
	EXPECT_FALSE(sgc::loopStartLinkTriggered(4, 9, 10));
	EXPECT_TRUE(sgc::loopStartLinkTriggered(4, 10, 10));
	EXPECT_TRUE(sgc::loopStartLinkTriggered(4, 11, 10));

	// LPSLNK observes the effective envelope rate. A zero D1R field can
	// therefore become active through KRS/OCT/FNS key-rate scaling.
	const u32 scaledZeroD1R = sgc::effectiveEnvelopeRate(0, 2, 0, 0);
	EXPECT_GT(scaledZeroD1R, 0u);
	EXPECT_TRUE(sgc::loopStartLinkTriggered(scaledZeroD1R, 10, 10));
}

TEST(AicaEnvelopeTest, AttackOnlyEntersDecayOneAtANonzeroEffectiveRate)
{
	EXPECT_FALSE(sgc::attackDecay1Triggered(false, 0));
	EXPECT_TRUE(sgc::attackDecay1Triggered(false, 1));
	EXPECT_FALSE(sgc::attackDecay1Triggered(true, 1));
}

TEST(AicaFilterTest, OnlyExactDocumentedThroughSettingBypassesFilter)
{
	EXPECT_FALSE(sgc::filterApplied(false, 4, 0x1fff, 0x1fff, 0x1fff, 0x1fff, 0x1fff));
	EXPECT_TRUE(sgc::filterApplied(false, 4, 0x1ffe, 0x1fff, 0x1fff, 0x1fff, 0x1fff));
	EXPECT_TRUE(sgc::filterApplied(false, 3, 0x1fff, 0x1fff, 0x1fff, 0x1fff, 0x1fff));
}

TEST(AicaFilterTest, LowPassOffOverridesFilterParameters)
{
	EXPECT_FALSE(sgc::filterApplied(true, 0, 0, 0, 0, 0, 0));
}

TEST(AicaFilterTest, RecurrenceUsesSignedArithmetic)
{
	EXPECT_EQ(sgc::filterAccumulator(-100, 20, -30, 7, 4, 5, 6), 673);
	EXPECT_EQ(sgc::filterAccumulator(100, -20, 30, -7, 4, 5, 6), -673);
	EXPECT_EQ(sgc::arithmeticShiftRight(-673, 8), -3);
}

TEST(AicaLfoTest, FrequencyDividerMatchesHardwareTable)
{
	EXPECT_EQ(sgc::lfoStepSamples(0x00), 1020u);
	EXPECT_EQ(sgc::lfoStepSamples(0x10), 60u);
	EXPECT_EQ(sgc::lfoStepSamples(0x1f), 1u);
	EXPECT_NEAR(44100.0 / (256 * sgc::lfoStepSamples(0x00)), 0.17, 0.002);
	EXPECT_NEAR(44100.0 / (256 * sgc::lfoStepSamples(0x10)), 2.87, 0.01);
	EXPECT_NEAR(44100.0 / (256 * sgc::lfoStepSamples(0x1f)), 172.3, 0.1);
	EXPECT_EQ(sgc::effectiveLfoStepSamples(0, 3, 3), 1u);
	EXPECT_EQ(sgc::effectiveLfoStepSamples(0, 3, 2), 1020u);
	EXPECT_EQ(sgc::effectiveLfoStepSamples(0, 2, 3), 1020u);
}

TEST(AicaLfoTest, OscillatorClockAdvancesContinuously)
{
	EXPECT_TRUE(sgc::lfoClockRunsWhileSlotInactive());
	auto step = sgc::stepLfoClock(2, 10);
	EXPECT_EQ(step.counter, 1u);
	EXPECT_FALSE(step.advance);
	step = sgc::stepLfoClock(step.counter, 10);
	EXPECT_EQ(step.counter, 10u);
	EXPECT_TRUE(step.advance);
}

TEST(AicaLfoTest, LiveControlWritesPreserveDividerPhase)
{
	EXPECT_EQ(sgc::retimeLfoCounter(7, 10), 7u);
	EXPECT_EQ(sgc::retimeLfoCounter(7, 4), 4u);
	EXPECT_EQ(sgc::retimeLfoCounter(0, 10), 10u);
}

TEST(AicaLfoTest, ResetBitAppliesOnlyAtPcmLoopBoundary)
{
	EXPECT_FALSE(sgc::resetLfoAtLoop(false, true));
	EXPECT_FALSE(sgc::resetLfoAtLoop(true, false));
	EXPECT_TRUE(sgc::resetLfoAtLoop(true, true));
}

TEST(AicaLfoTest, AmplitudeDepthMatchesDocumentedDbRange)
{
	EXPECT_EQ(sgc::alfoAttenuation(255, 0), 0u);
	EXPECT_EQ(sgc::alfoAttenuation(255, 1), 2u);
	EXPECT_EQ(sgc::alfoAttenuation(255, 3), 8u);
	EXPECT_EQ(sgc::alfoAttenuation(255, 7), 128u);
	EXPECT_EQ(sgc::alfoAttenuation(128, 7), 64u);
}

TEST(AicaLfoTest, PitchDepthLinearlyModulatesPhaseIncrement)
{
	EXPECT_EQ(sgc::plfoScale(0, -128), 1024u);
	EXPECT_EQ(sgc::plfoScale(0, 127), 1024u);
	EXPECT_EQ(sgc::plfoScale(1, -128), 1022u);
	EXPECT_EQ(sgc::plfoScale(1, 127), 1025u);
	EXPECT_EQ(sgc::plfoScale(7, -128), 896u);
	EXPECT_EQ(sgc::plfoScale(7, 127), 1151u);
	EXPECT_EQ(sgc::plfoScale(7, -1), 1023u);
	EXPECT_EQ(sgc::plfoScale(7, 1), 1025u);
}

TEST(AicaLfoTest, WaveformsUseDocumentedResetPhaseAndEncoding)
{
	EXPECT_EQ(sgc::alfoWaveform(0, 0), 0);
	EXPECT_EQ(sgc::alfoWaveform(0, 255), 255);
	EXPECT_EQ(sgc::alfoWaveform(1, 0), 0);
	EXPECT_EQ(sgc::alfoWaveform(1, 127), 0);
	EXPECT_EQ(sgc::alfoWaveform(1, 128), 255);
	EXPECT_EQ(sgc::alfoWaveform(2, 0), 0);
	EXPECT_EQ(sgc::alfoWaveform(2, 128), 255);
	EXPECT_EQ(sgc::alfoWaveform(2, 255), 1);
	EXPECT_EQ(sgc::alfoWaveform(3, 0xa5), 0xa5);

	EXPECT_EQ(sgc::plfoWaveform(0, 0), 0);
	EXPECT_EQ(sgc::plfoWaveform(0, 127), 127);
	EXPECT_EQ(sgc::plfoWaveform(0, 128), -128);
	EXPECT_EQ(sgc::plfoWaveform(1, 0), 127);
	EXPECT_EQ(sgc::plfoWaveform(1, 128), -128);
	EXPECT_EQ(sgc::plfoWaveform(2, 0), 0);
	EXPECT_EQ(sgc::plfoWaveform(2, 64), 127);
	EXPECT_EQ(sgc::plfoWaveform(2, 192), -127);
	EXPECT_EQ(sgc::plfoWaveform(3, 0x7f), 127);
	EXPECT_EQ(sgc::plfoWaveform(3, 0x80), -128);
}

TEST(AicaLfoTest, NoiseWaveformIgnoresProgrammedLfoPeriod)
{
	EXPECT_FALSE(sgc::lfoNoiseUpdatesEverySample(0));
	EXPECT_FALSE(sgc::lfoNoiseUpdatesEverySample(1));
	EXPECT_FALSE(sgc::lfoNoiseUpdatesEverySample(2));
	EXPECT_TRUE(sgc::lfoNoiseUpdatesEverySample(3));
}

TEST(AicaNoiseTest, GeneratorProducesOneSamplePerStateAdvance)
{
	u32 state = sgc::nextNoiseState(0);
	EXPECT_EQ(state, 0x3039u);
	EXPECT_EQ(sgc::noiseSample(state), 0);
	state = sgc::nextNoiseState(state);
	EXPECT_EQ(state, 0xd3dc167eu);
	EXPECT_EQ(sgc::noiseSample(state), static_cast<s16>(0xd3dc));
	EXPECT_NE(sgc::noiseSample(sgc::nextNoiseState(state)), sgc::noiseSample(state));
}

TEST(AicaNoiseTest, SampleFormatStillDecodesRandomSourceData)
{
	constexpr u32 positive = 0x7f123456;
	constexpr u32 negative = 0x80123456;
	EXPECT_EQ(sgc::noiseSample(positive), static_cast<s16>(0x7f12));
	EXPECT_EQ(sgc::noisePcm8Sample(positive), 0x7f00);
	EXPECT_EQ(sgc::noisePcm8Sample(negative), -0x8000);
	EXPECT_EQ(sgc::noiseAdpcmNibble(positive), 7);
	EXPECT_EQ(sgc::noiseAdpcmNibble(negative), 8);

	const sgc::AdpcmDecodeStep decoded = sgc::decodeAdpcmNibble(
			sgc::noiseAdpcmNibble(positive), 0, 127);
	EXPECT_NE(decoded.sample, sgc::noiseSample(positive));
}

TEST(AicaSourceTest, Pcm16WordsUseAicaLittleEndianByteOrder)
{
	EXPECT_EQ(sgc::littleEndianPcm16(0x34, 0x12), 0x1234);
	EXPECT_EQ(sgc::littleEndianPcm16(0x00, 0x80), -32768);
	EXPECT_EQ(sgc::littleEndianPcm16(0xff, 0xff), -1);
}

TEST(AicaSourceTest, Pcm8UsesExplicitTwosComplementConversion)
{
	EXPECT_EQ(sgc::signedPcm8(0x00), 0);
	EXPECT_EQ(sgc::signedPcm8(0x7f), 127);
	EXPECT_EQ(sgc::signedPcm8(0x80), -128);
	EXPECT_EQ(sgc::signedPcm8(0xff), -1);
}

TEST(AicaMixerTest, MonoInvalidatesPanpot)
{
	EXPECT_EQ(sgc::effectivePan(0x00, false), 0x00u);
	EXPECT_EQ(sgc::effectivePan(0x0f, false), 0x0fu);
	EXPECT_EQ(sgc::effectivePan(0x1f, false), 0x1fu);
	EXPECT_EQ(sgc::effectivePan(0x00, true), 0u);
	EXPECT_EQ(sgc::effectivePan(0x0f, true), 0u);
	EXPECT_EQ(sgc::effectivePan(0x1f, true), 0u);
}

TEST(AicaMixerTest, DspInputWrapsOnTwentyBitOverflow)
{
	EXPECT_EQ(sgc::wrapMixs(0x7ffff), 0x7ffff);
	EXPECT_EQ(sgc::wrapMixs(0x80000), -0x80000);
	EXPECT_EQ(sgc::wrapMixs(0xfffff), -1);
	EXPECT_EQ(sgc::wrapMixs(0x100000), 0);
	EXPECT_EQ(sgc::wrapMixs(-0x80001), 0x7ffff);
}

TEST(AicaMixerTest, DacQuantizationUsesUpperTwosComplementBits)
{
	EXPECT_EQ(32767, sgc::quantizeDacOutput(32768, false));
	EXPECT_EQ(-32768, sgc::quantizeDacOutput(-32769, false));
	EXPECT_EQ(32767, sgc::quantizeDacOutput(131071, true));
	EXPECT_EQ(-32768, sgc::quantizeDacOutput(-131072, true));
	EXPECT_EQ(0, sgc::quantizeDacOutput(3, true));
	EXPECT_EQ(-1, sgc::quantizeDacOutput(-1, true));
	EXPECT_EQ(-1, sgc::quantizeDacOutput(-4, true));
	EXPECT_EQ(-2, sgc::quantizeDacOutput(-5, true));
}

TEST(AicaSourceTest, MemoryReadInhibitDoesNotStopNoiseGenerator)
{
	EXPECT_TRUE(sgc::soundMemoryReadAllowed(0, false));
	EXPECT_FALSE(sgc::soundMemoryReadAllowed(2, false));
	EXPECT_TRUE(sgc::soundMemoryReadAllowed(2, true));
	EXPECT_TRUE(sgc::soundMemoryReadAllowed(0xf, true));
}

TEST(AicaSourceTest, ProhibitedFormatGeneratesNoMemoryRead)
{
	EXPECT_TRUE(sgc::sourceReadsSoundMemory(0, false));
	EXPECT_TRUE(sgc::sourceReadsSoundMemory(1, false));
	EXPECT_TRUE(sgc::sourceReadsSoundMemory(2, false));
	EXPECT_FALSE(sgc::sourceReadsSoundMemory(3, false));
	EXPECT_FALSE(sgc::sourceReadsSoundMemory(0, true));
}

TEST(AicaFilterEnvelopeTest, ReachesTargetWithoutAnExtraSampleHold)
{
	auto step = sgc::advanceFilterEnvelope(100, 110, 4);
	EXPECT_EQ(104u, step.value);
	EXPECT_FALSE(step.reached);

	step = sgc::advanceFilterEnvelope(106, 110, 4);
	EXPECT_EQ(110u, step.value);
	EXPECT_TRUE(step.reached);

	step = sgc::advanceFilterEnvelope(114, 110, 8);
	EXPECT_EQ(110u, step.value);
	EXPECT_TRUE(step.reached);

	step = sgc::advanceFilterEnvelope(100, 110, 0);
	EXPECT_EQ(100u, step.value);
	EXPECT_FALSE(step.reached);
}

TEST(AicaPitchTest, AdpcmPositiveOctaveIsLimitedToOne)
{
	EXPECT_EQ(sgc::sampleUpdateRate(0, 7, 0), 1024u << 7);
	EXPECT_EQ(sgc::sampleUpdateRate(1, 7, 0), 1024u << 7);
	EXPECT_EQ(sgc::sampleUpdateRate(2, 7, 0), 1024u << 1);
	EXPECT_EQ(sgc::sampleUpdateRate(3, 2, 0x155), (1024u | 0x155) << 1);
}

TEST(AicaPitchTest, AdpcmNegativeOctaveRetainsFullRange)
{
	EXPECT_EQ(sgc::sampleUpdateRate(2, 15, 0), 1024u >> 1);
	EXPECT_EQ(sgc::sampleUpdateRate(2, 8, 0), 1024u >> 8);
}

TEST(AicaPitchTest, EleventhFnsBitControlsImplicitBaseBit)
{
	EXPECT_EQ(sgc::sampleUpdateRate(0, 0, 0x000), 0x400u);
	EXPECT_EQ(sgc::sampleUpdateRate(0, 0, 0x3ff), 0x7ffu);
	EXPECT_EQ(sgc::sampleUpdateRate(0, 0, 0x400), 0u);
	EXPECT_EQ(sgc::sampleUpdateRate(0, 0, 0x401), 1u);
}

TEST(AicaSourceTest, OnlyPcm16ForcesAnEvenStartAddress)
{
	EXPECT_EQ(sgc::sampleStartAddress(0x12, 0x3457, 0), 0x123456u);
	EXPECT_EQ(sgc::sampleStartAddress(0x12, 0x3457, 1), 0x123457u);
	EXPECT_EQ(sgc::sampleStartAddress(0x12, 0x3457, 2), 0x123457u);
	EXPECT_EQ(sgc::sampleStartAddress(0x12, 0x3457, 3), 0x123457u);
}

TEST(AicaLoopTest, LoopEndIsInclusive)
{
	EXPECT_EQ(sgc::sampleLoopEnd(0, 0xau), 0xbu);
	EXPECT_EQ(sgc::sampleLoopEnd(1, 0xffffu), 0x10000u);
	EXPECT_EQ(sgc::sampleLoopEnd(2, 3u), 4u);
}

TEST(AicaLoopTest, ProhibitedFormatRetainsObservedFourSampleBoundary)
{
	EXPECT_EQ(sgc::sampleLoopEnd(3, 1u), 4u);
	EXPECT_EQ(sgc::sampleLoopEnd(3, 4u), 4u);
}

TEST(AicaLoopTest, InterpolationLookAheadOnlyWrapsWhenLooping)
{
	EXPECT_EQ(sgc::interpolationNextAddress(9, 3, 10, 0, false), 10u);
	EXPECT_EQ(sgc::interpolationNextAddress(10, 3, 10, 0, false), 11u);
	EXPECT_EQ(sgc::interpolationNextAddress(9, 3, 10, 0, true), 10u);
	EXPECT_EQ(sgc::interpolationNextAddress(10, 3, 10, 0, true), 3u);
}

TEST(AicaLoopTest, InvalidLoopRangeDoesNotWrapInterpolation)
{
	EXPECT_EQ(sgc::interpolationNextAddress(10, 12, 10, 0, true), 11u);
}

TEST(AicaLoopTest, ReadsLoopEndBeforeWrapping)
{
	auto step = sgc::advanceSampleAddress(8, 3, 10, 0, true);
	EXPECT_EQ(step.address, 9u);
	EXPECT_FALSE(step.looped);
	step = sgc::advanceSampleAddress(step.address, 3, 10, 0, true);
	EXPECT_EQ(step.address, 10u);
	EXPECT_FALSE(step.looped);
	step = sgc::advanceSampleAddress(step.address, 3, 10, 0, true);
	EXPECT_EQ(step.address, 3u);
	EXPECT_TRUE(step.looped);
	EXPECT_FALSE(step.stopped);
}

TEST(AicaLoopTest, NonLoopingPlaybackStopsAfterLoopEnd)
{
	auto step = sgc::advanceSampleAddress(9, 3, 10, 2, false);
	EXPECT_EQ(step.address, 10u);
	EXPECT_FALSE(step.stopped);
	step = sgc::advanceSampleAddress(step.address, 3, 10, 2, false);
	EXPECT_EQ(step.address, 0u);
	EXPECT_TRUE(step.looped);
	EXPECT_TRUE(step.stopped);
}

TEST(AicaSampleClockTest, ProducesExactly44100SamplesPerSecond)
{
	SampleClock clock;
	u32 cycles = 0;
	u32 longIntervals = 0;
	for (u32 i = 0; i < SampleClock::SampleRate; ++i)
	{
		u32 interval = clock.nextCycles();
		cycles += interval;
		longIntervals += interval == SampleClock::BaseCycles + 1;
		const u64 deadline = ((u64)(i + 1) * SH4_MAIN_CLOCK
				+ SampleClock::SampleRate - 1) / SampleClock::SampleRate;
		EXPECT_EQ(deadline, cycles);
	}

	EXPECT_EQ(cycles, SH4_MAIN_CLOCK);
	EXPECT_EQ(longIntervals, SampleClock::CycleRemainder);
}

TEST(AicaSampleClockTest, RestoredPhaseContinuesTheSameSequence)
{
	SampleClock original;
	for (int i = 0; i < 137; ++i)
		original.nextCycles();

	SampleClock restored;
	restored.setPhase(original.phase());
	for (int i = 0; i < 1000; ++i)
		EXPECT_EQ(original.nextCycles(), restored.nextCycles());
}

class AicaDspTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		settings.platform.aram_size = 2_MB;
		settings.platform.aram_mask = settings.platform.aram_size - 1;
		aica_ram.alloc(ARAM_SIZE);
		aica_ram.zero();
		memset(aica_reg, 0, sizeof(aica_reg));
		midiSendBuffer.clear();
		init();
	}

	void TearDown() override
	{
		term();
		aica_ram.free();
	}

	static void setInstruction(int step, u16 word0, u16 word1, u16 word2, u16 word3 = 0)
	{
		u32 *instruction = &DSPData->MPRO[step * 4];
		instruction[0] = word0;
		instruction[1] = word1;
		instruction[2] = word2;
		instruction[3] = word3;
	}
};

TEST_F(AicaDspTest, ResetRingGeometryMatchesCommonRegisters)
{
	EXPECT_EQ(CommonData->RBL, 0u);
	EXPECT_EQ(CommonData->RBP, 0u);
	EXPECT_EQ(state.RBL, 8192u - 1);
	EXPECT_EQ(state.RBP, 0u);
}

TEST_F(AicaDspTest, HardResetClearsPersistentSlotFilterState)
{
	writeRegInternal<u16>(0x002c, 0x1234); // FLV0
	writeRegInternal<u16>(0x0000, 0xc000); // KYONB and KYONEX
	CommonData->AFSEL = 1;
	CommonData->MSLC = 0;
	sgc::ReadCommonReg(0x2810, false);
	EXPECT_EQ(0x1234u, CommonData->EG);

	term();
	memset(aica_reg, 0, sizeof(aica_reg));
	init();
	CommonData->AFSEL = 1;
	CommonData->MSLC = 0;
	sgc::ReadCommonReg(0x2810, false);

	EXPECT_EQ(0u, CommonData->EG);
	EXPECT_EQ(0u, CommonData->SGC);
	EXPECT_EQ(0u, CommonData->LP);
	EXPECT_EQ(0u, CommonData->CA);
}

TEST_F(AicaDspTest, MemorySizeRegisterSelectsAddressWrap)
{
	// The installed two-MiB RAM remains the upper bound even if 64-Mbit mode
	// is requested.
	CommonData->MEM8MB = 0;
	EXPECT_EQ(2_MB - 1, soundRamMask());
	CommonData->MEM8MB = 1;
	EXPECT_EQ(2_MB - 1, soundRamMask());

	settings.platform.aram_size = 8_MB;
	settings.platform.aram_mask = 8_MB - 1;
	EXPECT_EQ(8_MB - 1, soundRamMask());
	writeRegInternal<u16>(0x2804, 0x500);
	EXPECT_EQ(0x280000u, state.RBP);
	state.dirty = false;
	writeRegInternal<u8>(0x2801, 0);
	EXPECT_EQ(2_MB - 1, soundRamMask());
	EXPECT_EQ(0x80000u, state.RBP);
	EXPECT_TRUE(state.dirty);

	// Restore the allocation metadata before fixture teardown.
	settings.platform.aram_size = 2_MB;
	settings.platform.aram_mask = 2_MB - 1;
}

TEST_F(AicaDspTest, MidiOutputInterruptTracksEmptyTransition)
{
	receivedMidiByte = 0;
	midiReceiverSawNonEmpty = false;
	midiReceiverSawClearedInterrupt = false;
	SCIPD->MIDI_OUT = 1;
	MCIPD->MIDI_OUT = 1;
	setMidiReceiver(captureMidiOutput);

	writeRegInternal<u8>(0x280c, 0x5a);

	EXPECT_EQ(0, receivedMidiByte);
	EXPECT_EQ(0u, CommonData->MOEMP);
	EXPECT_EQ(0u, SCIPD->MIDI_OUT);
	EXPECT_EQ(0u, MCIPD->MIDI_OUT);
	EXPECT_TRUE(sh4_sched_is_scheduled(midiOutSchedId));
	EXPECT_EQ((int)MidiByteCycles, sh4_sched_cycles_until(midiOutSchedId));
	midiOutTransmitByte();

	EXPECT_EQ(0x5a, receivedMidiByte);
	EXPECT_TRUE(midiReceiverSawNonEmpty);
	EXPECT_TRUE(midiReceiverSawClearedInterrupt);
	EXPECT_EQ(1u, CommonData->MOEMP);
	EXPECT_EQ(0u, CommonData->MOFUL);
	EXPECT_EQ(1u, SCIPD->MIDI_OUT);
	EXPECT_EQ(1u, MCIPD->MIDI_OUT);
	setMidiReceiver(nullptr);
}

TEST_F(AicaDspTest, MidiOutputUsesFourByteFifo)
{
	receivedMidiByte = 0;
	setMidiReceiver(captureMidiOutput);
	for (u8 data = 1; data <= 5; ++data)
		writeRegInternal<u8>(0x280c, data);

	EXPECT_EQ(4u, midiOutputBuffer.size());
	EXPECT_EQ(1u, CommonData->MOFUL);
	EXPECT_EQ(0u, CommonData->MOEMP);
	EXPECT_EQ(0u, SCIPD->MIDI_OUT);
	EXPECT_EQ(0x1000u, readRegInternal<u16>(0x2808) & 0x1800);

	EXPECT_TRUE(midiOutTransmitByte());
	EXPECT_EQ(1, receivedMidiByte);
	EXPECT_EQ(3u, midiOutputBuffer.size());
	EXPECT_EQ(0u, CommonData->MOFUL);
	EXPECT_EQ(0u, CommonData->MOEMP);

	EXPECT_TRUE(midiOutTransmitByte());
	EXPECT_TRUE(midiOutTransmitByte());
	EXPECT_FALSE(midiOutTransmitByte());
	EXPECT_EQ(1u, CommonData->MOEMP);
	EXPECT_EQ(1u, SCIPD->MIDI_OUT);
	EXPECT_EQ(1u, MCIPD->MIDI_OUT);
	EXPECT_EQ(0x0800u, readRegInternal<u16>(0x2808) & 0x1800);
	setMidiReceiver(nullptr);
}

TEST_F(AicaDspTest, RefreshingMidiOutputStatusPreservesAcknowledgedInterrupt)
{
	midiOutputBuffer.clear();
	CommonData->MOEMP = 0;
	CommonData->MOFUL = 1;
	SCIPD->MIDI_OUT = 0;
	MCIPD->MIDI_OUT = 0;

	refreshMidiOutputStatus();

	EXPECT_EQ(1u, CommonData->MOEMP);
	EXPECT_EQ(0u, CommonData->MOFUL);
	EXPECT_EQ(0u, SCIPD->MIDI_OUT);
	EXPECT_EQ(0u, MCIPD->MIDI_OUT);
}

TEST_F(AicaDspTest, CommonMonitorAndMidiInputRegistersAreReadOnly)
{
	CommonData->MIBUF = 0x12;
	CommonData->MIFUL = 1;
	CommonData->MIOVF = 1;
	CommonData->MOEMP = 1;
	CommonData->MOFUL = 0;
	CommonData->EG = 0x123;
	CommonData->SGC = 2;
	CommonData->LP = 1;
	CommonData->CA = 0x4567;

	writeRegInternal<u16>(0x2808, 0xffff);
	writeRegInternal<u16>(0x2810, 0xffff);
	writeRegInternal<u16>(0x2814, 0xffff);
	writeRegInternal<u16>(0x2802, 0xffff);

	EXPECT_EQ(0x12u, CommonData->MIBUF);
	EXPECT_EQ(1u, CommonData->MIFUL);
	EXPECT_EQ(1u, CommonData->MIOVF);
	EXPECT_EQ(1u, CommonData->MOEMP);
	EXPECT_EQ(0u, CommonData->MOFUL);
	EXPECT_EQ(0x123u, CommonData->EG);
	EXPECT_EQ(2u, CommonData->SGC);
	EXPECT_EQ(1u, CommonData->LP);
	EXPECT_EQ(0x4567u, CommonData->CA);
	EXPECT_EQ(0, aica_reg[0x2802]);
	EXPECT_EQ(0, aica_reg[0x2803]);
}

TEST_F(AicaDspTest, InternalDmaRegistersEnforceFourByteGranularity)
{
	writeRegInternal<u16>(0x2884, 0xffff);
	writeRegInternal<u16>(0x2888, 0xffff);
	// Leave DEXE clear so this register-mask test does not start a transfer.
	writeRegInternal<u16>(0x288c, 0xfffe);

	EXPECT_EQ(0xfffcu, *(u16 *)&aica_reg[0x2884]);
	EXPECT_EQ(0xfffcu, *(u16 *)&aica_reg[0x2888]);
	EXPECT_EQ(0xfffcu, *(u16 *)&aica_reg[0x288c]);
}

TEST_F(AicaDspTest, ExternalInputRegistersAreReadOnly)
{
	DSPData->EXTS[0] = 0x1234;
	DSPData->EXTS[1] = 0x5678;

	writeRegInternal<u16>(0x45c0, 0xabcd);
	writeRegInternal<u8>(0x45c4, 0xef);

	EXPECT_EQ(DSPData->EXTS[0], 0x1234u);
	EXPECT_EQ(DSPData->EXTS[1], 0x5678u);
}

TEST_F(AicaDspTest, UndefinedDspRangesAndUpperHalfwordsDoNotLatch)
{
	writeRegInternal<u16>(0x3300, 0xffff);
	writeRegInternal<u16>(0x3c00, 0xffff);
	writeRegInternal<u16>(0x45c8, 0xffff);
	writeRegInternal<u16>(0x4582, 0xffff);

	EXPECT_EQ(0, readRegInternal<u16>(0x3300));
	EXPECT_EQ(0, readRegInternal<u16>(0x3c00));
	EXPECT_EQ(0, readRegInternal<u16>(0x45c8));
	EXPECT_EQ(0, readRegInternal<u16>(0x4582));
	EXPECT_EQ(0, *(u16 *)&aica_reg[0x3300]);
	EXPECT_EQ(0, *(u16 *)&aica_reg[0x3c00]);
	EXPECT_EQ(0, *(u16 *)&aica_reg[0x45c8]);
}

TEST_F(AicaDspTest, ChannelRegistersMatchHardwareReadbackAndIgnoreUpperHalfwords)
{
	*(u16 *)&aica_reg[0] = 0xffff;
	EXPECT_EQ(0x47ff, readRegInternal<u16>(0));

	writeRegInternal<u16>(0x0010, 0xffff);
	writeRegInternal<u16>(0x0014, 0xffff);
	writeRegInternal<u16>(0x0018, 0xffff);
	writeRegInternal<u16>(0x0020, 0xffff);
	writeRegInternal<u16>(0x0024, 0xffff);
	writeRegInternal<u16>(0x0028, 0xffff);
	writeRegInternal<u16>(0x002c, 0xffff);
	writeRegInternal<u16>(0x0040, 0xffff);

	EXPECT_EQ(0xffdf, readRegInternal<u16>(0x0010));
	EXPECT_EQ(0x7fff, readRegInternal<u16>(0x0014));
	EXPECT_EQ(0xffff, readRegInternal<u16>(0x0018));
	EXPECT_EQ(0x00ff, readRegInternal<u16>(0x0020));
	EXPECT_EQ(0xffff, readRegInternal<u16>(0x0024));
	EXPECT_EQ(0xffff, readRegInternal<u16>(0x0028));
	EXPECT_EQ(0x1fff, readRegInternal<u16>(0x002c));
	EXPECT_EQ(0x1f1f, readRegInternal<u16>(0x0040));

	writeRegInternal<u16>(0x0022, 0xffff);
	EXPECT_EQ(0, readRegInternal<u16>(0x0022));
	EXPECT_EQ(0x00ff, readRegInternal<u16>(0x0020));
}

TEST_F(AicaDspTest, EffectOutputRegistersExposeOnlyImplementedFields)
{
	writeRegInternal<u16>(0x2000, 0xffff);
	writeRegInternal<u16>(0x2044, 0xffff);

	EXPECT_EQ(0x0f1f, readRegInternal<u16>(0x2000));
	EXPECT_EQ(0x0f1f, readRegInternal<u16>(0x2044));
	EXPECT_EQ(0x1f, readRegInternal<u8>(0x2000));
	EXPECT_EQ(0x0f, readRegInternal<u8>(0x2001));

	writeRegInternal<u16>(0x2002, 0xffff);
	writeRegInternal<u16>(0x2048, 0xffff);
	EXPECT_EQ(0, readRegInternal<u16>(0x2002));
	EXPECT_EQ(0, readRegInternal<u16>(0x2048));
	EXPECT_EQ(0x0f1f, readRegInternal<u16>(0x2000));
}

TEST_F(AicaDspTest, CoefficientUndefinedLowBitsReadAsZero)
{
	writeRegInternal<u16>(0x3000, 0xffff);
	EXPECT_EQ(DSPData->COEF[0], 0xfff8u);
	EXPECT_EQ(readRegInternal<u16>(0x3000), 0xfff8u);

	writeRegInternal<u8>(0x3000, 0x07);
	EXPECT_EQ(DSPData->COEF[0], 0xff00u);
	EXPECT_EQ(readRegInternal<u8>(0x3000), 0u);
}

TEST_F(AicaDspTest, DspWordRegistersIgnoreReservedUpperHalfwords)
{
	writeRegInternal<u32>(0x3000, 0xabcfffff);
	writeRegInternal<u32>(0x3200, 0xdef01234);
	writeRegInternal<u32>(0x3400, 0x56789abc);
	writeRegInternal<u32>(0x4580, 0x13572468);

	EXPECT_EQ(0xfff8u, readRegInternal<u32>(0x3000));
	EXPECT_EQ(0x1234u, readRegInternal<u32>(0x3200));
	EXPECT_EQ(0x9abcu, readRegInternal<u32>(0x3400));
	EXPECT_EQ(0x2468u, readRegInternal<u32>(0x4580));
	EXPECT_EQ(0u, readRegInternal<u16>(0x3002));
	EXPECT_EQ(0u, readRegInternal<u16>(0x3202));
	EXPECT_EQ(0u, readRegInternal<u16>(0x3402));
	EXPECT_EQ(0u, readRegInternal<u16>(0x4582));
}

TEST_F(AicaDspTest, CommonControlAcceptsBothBytesOfWordSizedWrites)
{
	writeRegInternal<u32>(0x2800, 0xdead0f0a);

	EXPECT_EQ(0xau, CommonData->MVOL);
	EXPECT_EQ(1u, CommonData->DAC18B);
	EXPECT_EQ(1u, CommonData->MEM8MB);
	EXPECT_EQ(1u, CommonData->Mono);
	EXPECT_EQ(0u, aica_reg[0x2802]);
	EXPECT_EQ(0u, aica_reg[0x2803]);
}

TEST_F(AicaDspTest, WriteOnlyCommonControlsDoNotReadBack)
{
	writeRegInternal<u16>(0x2804, 0x6abc); // RBL=3, RBP=0xabc
	writeRegInternal<u16>(0x280c, 0x7f5a); // AFSEL=1, MSLC=63, MIDI=0x5a

	EXPECT_EQ(3u, CommonData->RBL);
	EXPECT_EQ(0xabcu, CommonData->RBP);
	EXPECT_EQ(1u, CommonData->AFSEL);
	EXPECT_EQ(63u, CommonData->MSLC);
	ASSERT_EQ(1u, midiOutputBuffer.size());
	EXPECT_EQ(0x5a, midiOutputBuffer.front());

	EXPECT_EQ(0u, readRegInternal<u8>(0x2804));
	EXPECT_EQ(0u, readRegInternal<u8>(0x2805));
	EXPECT_EQ(0u, readRegInternal<u16>(0x2804));
	EXPECT_EQ(0u, readRegInternal<u8>(0x280c));
	EXPECT_EQ(0u, readRegInternal<u8>(0x280d));
	EXPECT_EQ(0u, readRegInternal<u16>(0x280c));
}

TEST_F(AicaDspTest, UndefinedCommonRegisterRangesDoNotLatch)
{
	for (u32 addr : { 0x2818u, 0x287cu, 0x28c0u, 0x2bfcu, 0x2c04u,
			0x2cfcu, 0x2d08u, 0x2ffcu })
	{
		writeRegInternal<u32>(addr, 0xffffffff);
		EXPECT_EQ(0u, readRegInternal<u32>(addr)) << std::hex << addr;
		EXPECT_EQ(0u, *reinterpret_cast<u32 *>(&aica_reg[addr])) << std::hex << addr;
	}
}

TEST_F(AicaDspTest, MidiInputInterruptTracksFifoOccupancy)
{
	EXPECT_TRUE(CommonData->MIEMP);
	midiSend(0x12);
	midiSend(0x34);
	EXPECT_FALSE(CommonData->MIEMP);
	EXPECT_TRUE(SCIPD->MIDI_IN);
	EXPECT_TRUE(MCIPD->MIDI_IN);

	u8 data = 0;
	ASSERT_TRUE(midiRead(data));
	EXPECT_EQ(data, 0x12);
	EXPECT_TRUE(SCIPD->MIDI_IN);
	EXPECT_TRUE(MCIPD->MIDI_IN);

	ASSERT_TRUE(midiRead(data));
	EXPECT_EQ(data, 0x34);
	EXPECT_FALSE(SCIPD->MIDI_IN);
	EXPECT_FALSE(MCIPD->MIDI_IN);
	EXPECT_TRUE(CommonData->MIEMP);
}

TEST_F(AicaDspTest, MidiInputEmptyStatusReportsPreReadOccupancy)
{
	midiSend(0x5a);

	const u16 nonempty = readRegInternal<u16>(0x2808);
	EXPECT_EQ(0, nonempty & 0x0100);
	EXPECT_EQ(0x5a, nonempty & 0xff);
	EXPECT_TRUE(CommonData->MIEMP);

	const u16 empty = readRegInternal<u16>(0x2808);
	EXPECT_NE(0u, empty & 0x0100);
	EXPECT_EQ(0x5a, empty & 0xff); // the data latch retains its last value
}

TEST_F(AicaDspTest, MidiInputFifoReportsPreReadStatusAtFourBytes)
{
	for (u8 i = 0; i < 4; ++i)
		midiSend(i);
	EXPECT_EQ(midiSendBuffer.size(), 4u);
	EXPECT_TRUE(CommonData->MIFUL);
	EXPECT_FALSE(CommonData->MIOVF);

	midiSend(0xff);
	EXPECT_EQ(midiSendBuffer.size(), 4u);
	EXPECT_TRUE(CommonData->MIFUL);
	EXPECT_TRUE(CommonData->MIOVF);

	const u16 value = readRegInternal<u16>(0x2808);
	EXPECT_EQ(value & 0xff, 0u);
	EXPECT_NE(value & 0x0200, 0u);
	EXPECT_NE(value & 0x0400, 0u);
	EXPECT_EQ(midiSendBuffer.size(), 3u);
	EXPECT_FALSE(CommonData->MIFUL);
	EXPECT_FALSE(CommonData->MIOVF);
}

TEST_F(AicaDspTest, MidiStatusByteReadDoesNotConsumeInput)
{
	for (u8 i = 0; i < 4; ++i)
		midiSend(i);
	midiSend(0xff);

	const u8 status = readRegInternal<u8>(0x2809);
	EXPECT_NE(status & 0x02, 0u); // MIFUL
	EXPECT_NE(status & 0x04, 0u); // MIOVF
	EXPECT_EQ(midiSendBuffer.size(), 4u);
	EXPECT_TRUE(CommonData->MIOVF);

	EXPECT_EQ(readRegInternal<u8>(0x2808), 0u);
	EXPECT_EQ(midiSendBuffer.size(), 3u);
	EXPECT_FALSE(CommonData->MIOVF);
}

TEST(AicaMidiTest, RestoreClampsLegacyQueueToHardwareCapacity)
{
	EXPECT_EQ(midiRestoreState(0).retained, 0u);
	EXPECT_FALSE(midiRestoreState(0).overflow);
	EXPECT_EQ(midiRestoreState(MidiFifoSize).retained, MidiFifoSize);
	EXPECT_FALSE(midiRestoreState(MidiFifoSize).overflow);
	EXPECT_EQ(midiRestoreState(MidiFifoSize + 2).retained, MidiFifoSize);
	EXPECT_TRUE(midiRestoreState(MidiFifoSize + 2).overflow);
}

TEST(AicaDspFormatTest, PacksAndUnpacksHardwareFloatFormat)
{
	EXPECT_EQ(PACK(0), 0x6000);
	EXPECT_EQ(PACK(0x7fffff), 0x07ff);
	EXPECT_EQ(PACK(-0x800000), 0x8000);
	EXPECT_EQ(UNPACK(0x6000), 0);
	EXPECT_EQ(UNPACK(0x07ff), 0x7ff800);
	EXPECT_EQ(UNPACK(0x8000), -0x800000);
}

TEST_F(AicaDspTest, EffectOutputWrapsTo24BitsAndLastWriteWins)
{
	state.TEMP[1] = 0x7fffff;
	state.MIXS[0] = 0x7ffff;
	DSPData->COEF[0] = 0x7fff;
	DSPData->COEF[1] = 0x7fff;

	// ACC = MIXS0 * COEF + TEMP[DEC]. The second and third steps send
	// two copies of the preceding ACC through non-saturating SHIFT=3.
	setInstruction(0, 0, 0xb000, 0);
	setInstruction(1, 0, 0xb000, 0x1030);
	setInstruction(2, 0, 0, 0x1032);
	state.dirty = true;

	step();

	// ACC is 0xfff7ef. The DSP's signed 24-bit shifter wraps it to
	// -0x811, and the later EWT replaces the earlier value with -9.
	EXPECT_EQ((s16)DSPData->EFREG[0], -9);
}

TEST_F(AicaDspTest, EffectOutputIsClearedBeforeEachSample)
{
	DSPData->EFREG[3] = 0x1234;
	// A non-empty no-op program makes the DSP run without writing EFREG.
	setInstruction(0, 0, 0, 2);
	state.dirty = true;

	step();

	EXPECT_EQ(DSPData->EFREG[3], 0u);
}

TEST_F(AicaDspTest, EffectOutputIsClearedWhenProgramStops)
{
	DSPData->EFREG[3] = 0x1234;
	// An empty program takes the stopped fast path but still represents a new
	// DSP sample, so output from the preceding sample must not survive.
	state.dirty = true;

	step();

	EXPECT_TRUE(state.stopped);
	EXPECT_EQ(DSPData->EFREG[3], 0u);
}

TEST_F(AicaDspTest, EmptyProgramStillAdvancesRingCounter)
{
	memset(DSPData->MPRO, 0, sizeof(DSPData->MPRO));
	dsp::state.RBL = 7;
	dsp::state.MDEC_CT = 3;
	dsp::state.dirty = true;

	dsp::step();
	EXPECT_TRUE(dsp::state.stopped);
	EXPECT_EQ(2u, dsp::state.MDEC_CT);
	dsp::step();
	EXPECT_EQ(1u, dsp::state.MDEC_CT);
	dsp::step();
	EXPECT_EQ(8u, dsp::state.MDEC_CT);
}

TEST_F(AicaDspTest, BypassPreservesSampleBoundaryHardwareState)
{
	dsp::state.RBL = 7;
	dsp::state.MDEC_CT = 3;
	dsp::state.MWT_ADDR = 0x120;
	dsp::state.MWT_DATA = 0xcafe;
	dsp::state.MWT_PENDING = 1;
	DSPData->EFREG[2] = 0x1234;
	*(u16 *)&aica_ram[0x120] = 0;

	dsp::bypassStep();

	EXPECT_EQ(2u, dsp::state.MDEC_CT);
	EXPECT_EQ(0u, dsp::state.MWT_PENDING);
	EXPECT_EQ(0xcafe, *(u16 *)&aica_ram[0x120]);
	EXPECT_EQ(0, DSPData->EFREG[2]);
}

TEST_F(AicaDspTest, ExternalInputIsSignExtendedBeforeScaling)
{
	DSPData->EXTS[0] = static_cast<u16>(-0x100);
	DSPData->COEF[0] = 0x1000;
	// Multiply signed EXTS0 (-0x100, promoted to 24 bits) by 0.5, then
	// expose the preceding accumulator through non-saturating SHIFT=3.
	setInstruction(0, 0, 0xb800, 0);
	setInstruction(1, 0, 0, 0x1030);
	state.dirty = true;

	step();

	EXPECT_EQ((s16)DSPData->EFREG[0], -32);
}

TEST_F(AicaDspTest, NoFloatMemoryWriteStoresRawPcm)
{
	state.TEMP[1] = -0x123400;
	// Step 0 loads ACC. Step 1 writes the preceding ACC as raw 16-bit PCM.
	setInstruction(0, 0, 0, 0);
	setInstruction(1, 0, 0, 0xc030, 0x8000);
	state.dirty = true;

	step();

	EXPECT_EQ(*(s16 *)&aica_ram[0], -0x1234);
}

TEST_F(AicaDspTest, NoFloatMemoryReadLoadsRawPcm)
{
	*(s16 *)&aica_ram[0] = -0x1234;
	// An MRD result is available to IWT two steps later.
	setInstruction(1, 0, 0, 0xa000, 0x8000);
	setInstruction(3, 0, 0x40, 0);
	state.dirty = true;

	step();

	EXPECT_EQ(state.MEMS[0], -0x123400);
}

TEST_F(AicaDspTest, NonInterpolationAddressLatchUsesInputsBitsTwentyTwoThroughSixteen)
{
	state.MEMS[0] = 0x020000;
	*(s16 *)&aica_ram[2] = -0x1111;
	*(s16 *)&aica_ram[4] = -0x2222;
	// Step 0 sign-extends INPUTS[22:16], producing word address 2. Step 1
	// performs a table read through ADRS_REG; step 3 receives the result.
	setInstruction(0, 0, 0, 0x0080);
	setInstruction(1, 0, 0, 0xa000, 0x8100);
	setInstruction(3, 0, 0x0040, 0);
	state.dirty = true;

	step();

	EXPECT_EQ(-0x222200, state.MEMS[0]);
}

TEST_F(AicaDspTest, InterpolationAddressLatchUsesShiftedIntegerBits)
{
	state.TEMP[1] = 0x003000;
	*(s16 *)&aica_ram[0] = -0x1111;
	*(s16 *)&aica_ram[6] = -0x3333;
	// The zero instruction loads ACC from TEMP[MDEC_CT]. With SHFT=3, step 1
	// latches SHIFTED[23:12] (3), rather than INPUTS, into ADRS_REG. Step 3
	// performs a table read through that address and step 5 receives it.
	setInstruction(1, 0, 0, 0x00b0);
	setInstruction(3, 0, 0, 0xa000, 0x8100);
	setInstruction(5, 0, 0x0040, 0);
	state.dirty = true;

	step();

	EXPECT_EQ(-0x333300, state.MEMS[0]);
}

TEST_F(AicaDspTest, MemoryReadInhibitSuppressesDspRead)
{
	*(s16 *)&aica_ram[0] = -0x1234;
	state.MEMVAL[3] = 0x345600;
	CommonData->MRWINH = 1;
	// Without inhibition, step 1 replaces MEMVAL3 and step 3 transfers the
	// read result to MEMS0. A prohibited access leaves the pipeline latch.
	setInstruction(1, 0, 0, 0xa000, 0x8000);
	setInstruction(3, 0, 0x40, 0);
	state.dirty = true;

	step();

	EXPECT_EQ(state.MEMS[0], 0x345600);
}

TEST_F(AicaDspTest, MemoryWriteInhibitCancelsPendingDspWrite)
{
	state.MWT_ADDR = 0;
	state.MWT_DATA = 0x4567;
	state.MWT_PENDING = 1;
	CommonData->MRWINH = 1;

	step();

	EXPECT_EQ(*(u16 *)&aica_ram[0], 0);
	EXPECT_EQ(state.MWT_PENDING, 0u);
}

TEST_F(AicaDspTest, MemoryWriteOccursAfterSameStepInputRead)
{
	*(s16 *)&aica_ram[0] = -0x1234;
	DSPData->COEF[3] = 0x7fff;
	// Step 1 requests raw PCM. At step 3 the result reaches MEMS0 while
	// IRA reads MEMS0 in the same instruction. Hardware reads INPUTS before
	// writing MEMS, so X receives the old zero value.
	setInstruction(1, 0, 0, 0xa000, 0x8000);
	setInstruction(3, 0, 0xa040, 2);
	setInstruction(4, 0, 0, 0x1032);
	state.dirty = true;

	step();

	EXPECT_EQ((s16)DSPData->EFREG[0], 0);
	EXPECT_EQ(state.MEMS[0], -0x123400);
}

TEST_F(AicaDspTest, SimultaneousMemoryReadSamplesBeforeWrite)
{
	*(s16 *)&aica_ram[0] = -0x1234;
	state.TEMP[1] = 0x234500;
	// Step 1 reads and writes the same word. The read pipeline samples the
	// old word before the write is committed, then delivers it at step 3.
	setInstruction(0, 0, 0, 0);
	setInstruction(1, 0, 0, 0xe030, 0x8000);
	setInstruction(3, 0, 0x40, 0);
	state.dirty = true;

	step();

	EXPECT_EQ(*(s16 *)&aica_ram[0], 0x2345);
	EXPECT_EQ(state.MEMS[0], -0x123400);
}

TEST_F(AicaDspTest, MemoryReadPipelineWrapsAcrossSampleBoundary)
{
	*(s16 *)&aica_ram[0] = -0x1234;
	// A read in the final odd step becomes available two steps later, which
	// is step 1 of the next 128-step sample program.
	setInstruction(1, 0, 0x40, 0);
	setInstruction(127, 0, 0, 0xa000, 0x8000);
	state.dirty = true;

	step();
	EXPECT_EQ(state.MEMS[0], 0);

	step();
	EXPECT_EQ(state.MEMS[0], -0x123400);
}

TEST_F(AicaDspTest, MemoryWritePipelineWrapsAcrossSampleBoundary)
{
	state.TEMP[1] = 0x234500;
	// Step 126 loads ACC and step 127 requests a raw write. Its memory cycle
	// is step 0 of the next sample, not the end of the current sample.
	setInstruction(126, 0, 0, 0);
	setInstruction(127, 0, 0, 0xc030, 0x8000);
	state.dirty = true;

	step();
	EXPECT_EQ(*(s16 *)&aica_ram[0], 0);
	EXPECT_EQ(state.MWT_PENDING, 1u);

	setInstruction(127, 0, 0, 0);
	state.dirty = true;
	step();
	EXPECT_EQ(*(s16 *)&aica_ram[0], 0x2345);
	EXPECT_EQ(state.MWT_PENDING, 0u);
}

TEST_F(AicaDspTest, SaveStatePreservesMemoryReadPipeline)
{
	state.MEMVAL[0] = -0x123400;
	state.MEMVAL[1] = 0x234500;
	state.MEMVAL[2] = -0x345600;
	state.MEMVAL[3] = 0x456700;

	std::array<u8, 4096> buffer {};
	Serializer serializer(buffer.data(), buffer.size());
	state.serialize(serializer);
	memset(state.MEMVAL, 0, sizeof(state.MEMVAL));

	Deserializer deserializer(buffer.data(), serializer.size());
	state.deserialize(deserializer);

	EXPECT_EQ(state.MEMVAL[0], -0x123400);
	EXPECT_EQ(state.MEMVAL[1], 0x234500);
	EXPECT_EQ(state.MEMVAL[2], -0x345600);
	EXPECT_EQ(state.MEMVAL[3], 0x456700);
}

TEST_F(AicaDspTest, SaveStatePreservesMemoryWritePipeline)
{
	state.MWT_ADDR = 0x1234;
	state.MWT_DATA = 0xabcd;
	state.MWT_PENDING = 1;

	std::array<u8, 4096> buffer {};
	Serializer serializer(buffer.data(), buffer.size());
	state.serialize(serializer);
	state.MWT_ADDR = state.MWT_DATA = state.MWT_PENDING = 0;

	Deserializer deserializer(buffer.data(), serializer.size());
	state.deserialize(deserializer);

	EXPECT_EQ(state.MWT_ADDR, 0x1234u);
	EXPECT_EQ(state.MWT_DATA, 0xabcdu);
	EXPECT_EQ(state.MWT_PENDING, 1u);
}

} // namespace aica::dsp
