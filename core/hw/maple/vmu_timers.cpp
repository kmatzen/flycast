#include "vmu_timers.h"
#include "serialize.h"

namespace vmu
{

void Lc86Timers::reset()
{
	timer0Prescaler = 0;
	previousTimer0Control = 0;
	previousTimer1Control = 0;
	previousBaseTimerControl = 0;
	baseTimerCounter = 0;
	baseTimerRemainder = 0;
}

unsigned Lc86Timers::advance8(u8& value, unsigned ticks, u8 reload)
{
	if (ticks < 0x100u - value)
	{
		value += ticks;
		return 0;
	}
	ticks -= 0x100u - value;
	const unsigned period = 0x100u - reload;
	const unsigned overflows = 1 + ticks / period;
	value = reload + ticks % period;
	return overflows;
}

unsigned Lc86Timers::advanceTimer0(unsigned cpuCycles)
{
	const u8 control = memory.readSfrRaw(0x110);
	const u8 running = control & 0xc0;
	if (!running || (control & 0x10)) // external-clock mode has no CPU-cycle input
	{
		previousTimer0Control = control;
		return 0;
	}
	if (!(previousTimer0Control & 0xc0))
		cpuCycles = cpuCycles > 0 ? cpuCycles - 1 : 0;
	previousTimer0Control = control;
	const unsigned scale = 0x100u - memory.readSfrRaw(0x111);
	timer0Prescaler += cpuCycles;
	const unsigned ticks = timer0Prescaler / scale;
	timer0Prescaler %= scale;
	if (!ticks)
		return 0;

	u8 low = memory.timerCounter(0, false);
	u8 high = memory.timerCounter(0, true);
	if ((control & 0xe0) == 0xe0)
	{
		unsigned value = static_cast<unsigned>(high) << 8 | low;
		const unsigned reload = static_cast<unsigned>(memory.readSfrRaw(0x115)) << 8
				| memory.readSfrRaw(0x113);
		bool overflow = false;
		for (unsigned i = 0; i < ticks; ++i)
			if (++value > 0xffff) { value = reload; overflow = true; }
		memory.setTimerCounter(0, false, value);
		memory.setTimerCounter(0, true, value >> 8);
		if (overflow)
		{
			memory.writeSfrRaw(0x110, control | 0x0a);
			if (control & 0x04)
				pic.raise(Lc86InterruptController::Irq::Timer0High);
		}
		return ticks;
	}
	if (control & 0x40)
	{
		if (advance8(low, ticks, memory.readSfrRaw(0x113)))
		{
			memory.writeSfrRaw(0x110, memory.readSfrRaw(0x110) | 0x02);
			if (control & 0x01)
				pic.raise(Lc86InterruptController::Irq::External2Timer0Low);
		}
		memory.setTimerCounter(0, false, low);
	}
	if (control & 0x80)
	{
		if (advance8(high, ticks, memory.readSfrRaw(0x115)))
		{
			memory.writeSfrRaw(0x110, memory.readSfrRaw(0x110) | 0x08);
			if (control & 0x04)
				pic.raise(Lc86InterruptController::Irq::Timer0High);
		}
		memory.setTimerCounter(0, true, high);
	}
	return ticks;
}

void Lc86Timers::advanceTimer1(unsigned cpuCycles)
{
	const u8 control = memory.readSfrRaw(0x118);
	if (!(control & 0xc0))
	{
		previousTimer1Control = control;
		return;
	}
	if (!(previousTimer1Control & 0xc0))
		cpuCycles = cpuCycles > 0 ? cpuCycles - 1 : 0;
	previousTimer1Control = control;
	if (!cpuCycles)
		return;

	u8 low = memory.timerCounter(1, false);
	u8 high = memory.timerCounter(1, true);
	if ((control & 0xe0) == 0xe0)
	{
		const unsigned lowOverflows = advance8(low, cpuCycles, memory.readSfrRaw(0x11b));
		const unsigned highOverflows = advance8(high, lowOverflows, memory.readSfrRaw(0x11d));
		if (lowOverflows)
			memory.writeSfrRaw(0x118, memory.readSfrRaw(0x118) | 0x02);
		if (highOverflows)
		{
			memory.writeSfrRaw(0x118, memory.readSfrRaw(0x118) | 0x08);
			if (control & 0x04)
				pic.raise(Lc86InterruptController::Irq::Timer1);
		}
	}
	else
	{
		if ((control & 0x40) && advance8(low, cpuCycles, memory.readSfrRaw(0x11b)))
		{
			memory.writeSfrRaw(0x118, memory.readSfrRaw(0x118) | 0x02);
			if (control & 0x01)
				pic.raise(Lc86InterruptController::Irq::Timer1);
		}
		if ((control & 0x80) && advance8(high, cpuCycles, memory.readSfrRaw(0x11d)))
		{
			memory.writeSfrRaw(0x118, memory.readSfrRaw(0x118) | 0x08);
			if (control & 0x04)
				pic.raise(Lc86InterruptController::Irq::Timer1);
		}
	}
	memory.setTimerCounter(1, false, low);
	memory.setTimerCounter(1, true, high);
}

void Lc86Timers::advance(unsigned cpuCycles)
{
	const unsigned timer0Ticks = advanceTimer0(cpuCycles);
	advanceTimer1(cpuCycles);
	advanceBaseTimer(cpuCycles, timer0Ticks);
}

void Lc86Timers::advanceBaseTimer(unsigned cpuCycles, unsigned timer0Ticks)
{
	const u8 control = memory.readSfrRaw(0x17f);
	if (!(control & 0x40))
	{
		baseTimerCounter = 0;
		previousBaseTimerControl = control;
		return;
	}

	unsigned ticks = 0;
	switch (memory.readSfrRaw(0x15f) & 0x30)
	{
	case 0x10: // instruction-cycle clock
		ticks = cpuCycles;
		break;
	case 0x30: // timer 0 prescaler output
		ticks = timer0Ticks;
		break;
	default: // 32.768 kHz quartz clock, scaled to the active CPU clock
	{
		const u8 ocr = memory.readSfrRaw(0x10e);
		const unsigned selector = (ocr >> 4) & 3;
		const u64 systemHz = selector == 2 ? 32768 : selector == 0 ? 600000 : 6000000;
		const unsigned divider = ocr & 0x80 ? 6 : 12;
		baseTimerRemainder += static_cast<u64>(cpuCycles) * 32768 * divider;
		ticks = baseTimerRemainder / systemHz;
		baseTimerRemainder %= systemHz;
		break;
	}
	}
	if (!(previousBaseTimerControl & 0x40) && ticks)
		--ticks;
	previousBaseTimerControl = control;
	if (!ticks)
		return;

	const unsigned int0Rate = control & 0x80 ? 0x40 : 0x4000;
	unsigned int1Rate = 0;
	switch (control & 0xb0)
	{
	case 0x00: case 0x80: int1Rate = 0x20; break;
	case 0x10: case 0x90: int1Rate = 0x80; break;
	case 0x20: int1Rate = 0x200; break;
	case 0x30: int1Rate = 0x800; break;
	case 0xa0: int1Rate = 0x02; break;
	case 0xb0: int1Rate = 0x08; break;
	}
	const unsigned start = baseTimerCounter;
	const unsigned end = start + ticks;
	u8 nextControl = control;
	if (start / int0Rate != end / int0Rate)
	{
		nextControl |= 0x02;
		if (control & 0x01)
			pic.raise(Lc86InterruptController::Irq::External3BaseTimer);
	}
	if (int1Rate && start / int1Rate != end / int1Rate)
	{
		nextControl |= 0x08;
		if (control & 0x04)
			pic.raise(Lc86InterruptController::Irq::External3BaseTimer);
	}
	baseTimerCounter = end & 0x3fff;
	memory.writeSfrRaw(0x17f, nextControl);
}

void Lc86Timers::serialize(Serializer& serializer) const
{
	serializer << timer0Prescaler << previousTimer0Control << previousTimer1Control
			<< previousBaseTimerControl << baseTimerCounter << baseTimerRemainder;
}

void Lc86Timers::deserialize(Deserializer& deserializer)
{
	deserializer >> timer0Prescaler >> previousTimer0Control >> previousTimer1Control
			>> previousBaseTimerControl >> baseTimerCounter >> baseTimerRemainder;
	baseTimerCounter &= 0x3fff;
}

} // namespace vmu
