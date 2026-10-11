#pragma once

#include "vmu_memory.h"
#include "vmu_pic.h"

namespace vmu
{

class Lc86Timers
{
public:
	Lc86Timers(Lc86MemoryMap& memory, Lc86InterruptController& pic)
		: memory(memory), pic(pic) {}

	void reset();
	void advance(unsigned cpuCycles);
	void serialize(Serializer& serializer) const;
	void deserialize(Deserializer& deserializer);

private:
	static unsigned advance8(u8& value, unsigned ticks, u8 reload);
	unsigned advanceTimer0(unsigned cpuCycles);
	void advanceTimer1(unsigned cpuCycles);
	void advanceBaseTimer(unsigned cpuCycles, unsigned timer0Ticks);

	Lc86MemoryMap& memory;
	Lc86InterruptController& pic;
	unsigned timer0Prescaler = 0;
	u8 previousTimer0Control = 0;
	u8 previousTimer1Control = 0;
	u8 previousBaseTimerControl = 0;
	u16 baseTimerCounter = 0;
	u64 baseTimerRemainder = 0;
};

} // namespace vmu
