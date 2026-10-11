#pragma once

#include "vmu_cpu.h"

#include <array>

namespace vmu
{

class Lc86InterruptController
{
public:
	enum class Irq : u8
	{
		Reset,
		External0,
		External1,
		External2Timer0Low,
		External3BaseTimer,
		Timer0High,
		Timer1,
		Serial0,
		Serial1,
		MapleReceive,
		Port3,
		Count = 16,
	};

	Lc86InterruptController(Lc86Cpu& cpu, Lc86Memory& memory)
		: cpu(cpu), memory(memory) {}

	void reset();
	void raise(Irq irq);
	bool poll();
	void returned();
	void serialize(Serializer& serializer) const;
	void deserialize(Deserializer& deserializer);
	u16 pendingMask() const { return pending; }

	static u16 vector(Irq irq);

private:
	enum Priority : u8 { Low, High, Highest, PriorityCount };
	u16 enabledAtPriority(Priority priority);
	int activePriority() const;

	Lc86Cpu& cpu;
	Lc86Memory& memory;
	u16 pending = 0;
	std::array<u16, PriorityCount> active {};
	bool inhibit = false;
};

} // namespace vmu
