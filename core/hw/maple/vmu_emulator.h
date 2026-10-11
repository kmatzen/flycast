#pragma once

#include "vmu_timers.h"

namespace vmu
{

class Emulator
{
public:
	Emulator(const u8 *rom, size_t romSize, u8 *flash, size_t flashSize)
		: memoryMap(rom, romSize, flash, flashSize), cpuCore(memoryMap),
		  interruptController(cpuCore, memoryMap), timers(memoryMap, interruptController) {}

	void reset();
	Lc86Cpu::StepResult step();
	u64 runForNanoseconds(u64 nanoseconds);
	u32 clockHz() const;
	void raiseInterrupt(Lc86InterruptController::Irq irq) { interruptController.raise(irq); }
	void setButtons(u8 pressed);
	bool buzzerOutput(u8& period, u8& duty) const;
	void serialize(Serializer& serializer) const;
	void deserialize(Deserializer& deserializer);

	Lc86MemoryMap& memory() { return memoryMap; }
	const Lc86MemoryMap& memory() const { return memoryMap; }
	Lc86Cpu& cpu() { return cpuCore; }
	Lc86InterruptController& pic() { return interruptController; }

private:
	Lc86MemoryMap memoryMap;
	Lc86Cpu cpuCore;
	Lc86InterruptController interruptController;
	Lc86Timers timers;
	s64 timeCreditPicoseconds = 0;
};

} // namespace vmu
