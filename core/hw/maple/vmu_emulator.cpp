#include "vmu_emulator.h"
#include "serialize.h"

#include <algorithm>

namespace vmu
{

void Emulator::reset()
{
	memoryMap.reset();
	cpuCore.reset();
	interruptController.reset();
	timers.reset();
	timeCreditPicoseconds = 0;
}

u32 Emulator::clockHz() const
{
	const u8 oscillatorControl = memoryMap.readSfrRaw(0x10e);
	const unsigned selector = (oscillatorControl >> 4) & 3;
	const u32 oscillatorHz = selector == 2 ? 32768 : selector == 0 ? 600000 : 6000000;
	return oscillatorHz / (oscillatorControl & 0x80 ? 6 : 12);
}

u64 Emulator::runForNanoseconds(u64 nanoseconds)
{
	constexpr s64 PicosecondsPerSecond = 1'000'000'000'000;
	constexpr s64 MaximumCredit = PicosecondsPerSecond;
	const u64 added = nanoseconds > static_cast<u64>(MaximumCredit / 1000)
			? MaximumCredit : nanoseconds * 1000;
	timeCreditPicoseconds = std::min<s64>(MaximumCredit, timeCreditPicoseconds + added);
	u64 instructions = 0;
	while (timeCreditPicoseconds > 0)
	{
		const u8 oscillatorControl = memoryMap.readSfrRaw(0x10e);
		const unsigned selector = (oscillatorControl >> 4) & 3;
		const u32 oscillatorHz = selector == 2 ? 32768 : selector == 0 ? 600000 : 6000000;
		const unsigned divider = oscillatorControl & 0x80 ? 6 : 12;
		const auto result = step();
		if (!result.valid || result.cycles == 0)
		{
			// HOLD stops the oscillator; elapsed suspended time must not become
			// executable credit after an input wakes the VMU.
			timeCreditPicoseconds = 0;
			break;
		}
		timeCreditPicoseconds -= static_cast<s64>(result.cycles) * PicosecondsPerSecond
				* divider / oscillatorHz;
		++instructions;
	}
	return instructions;
}

void Emulator::setButtons(u8 pressed)
{
	memoryMap.setButtons(pressed);
	const u8 interruptControl = memoryMap.readSfrRaw(0x14e);
	const u8 inputMask = static_cast<u8>(~memoryMap.readSfrRaw(0x14d));
	const u8 latch = memoryMap.readSfrRaw(0x14c);
	if ((interruptControl & 0x04) && (pressed & inputMask & latch))
	{
		memoryMap.writeSfrRaw(0x14e, interruptControl | 0x02);
		memoryMap.writeSfrRaw(0x107, memoryMap.readSfrRaw(0x107) & ~0x02);
		if (interruptControl & 0x01)
			interruptController.raise(Lc86InterruptController::Irq::Port3);
	}
}

bool Emulator::buzzerOutput(u8& period, u8& duty) const
{
	const u8 timerControl = memoryMap.readSfrRaw(0x118);
	const bool enabled = (memoryMap.readSfrRaw(0x145) & 0x80)
			&& (memoryMap.readSfrRaw(0x146) & 0x80)
			&& !(memoryMap.readSfrRaw(0x144) & 0x80)
			&& (timerControl & 0x40) && !(timerControl & 0x20);
	period = enabled ? memoryMap.readSfrRaw(0x11b) : 0;
	duty = enabled ? memoryMap.readSfrRaw(0x11a) : 0;
	return enabled;
}

void Emulator::serialize(Serializer& serializer) const
{
	memoryMap.serialize(serializer);
	cpuCore.serialize(serializer);
	interruptController.serialize(serializer);
	timers.serialize(serializer);
	serializer << timeCreditPicoseconds;
}

void Emulator::deserialize(Deserializer& deserializer)
{
	memoryMap.deserialize(deserializer);
	cpuCore.deserialize(deserializer);
	interruptController.deserialize(deserializer);
	timers.deserialize(deserializer);
	deserializer >> timeCreditPicoseconds;
}

Lc86Cpu::StepResult Emulator::step()
{
	interruptController.poll();
	const u8 powerControl = memoryMap.readSfrRaw(0x107);
	if (powerControl & 0x02) // HOLD stops the CPU, clocks, and timers.
		return { 0, true };
	if (powerControl & 0x01) // HALT leaves peripherals running.
	{
		timers.advance(1);
		return { 1, true };
	}

	const auto result = cpuCore.step();
	if (!result.valid)
		return result;
	timers.advance(result.cycles);
	if (memoryMap.takeInterruptReturn())
		interruptController.returned();
	return result;
}

} // namespace vmu
