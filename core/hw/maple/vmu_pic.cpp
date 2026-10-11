#include "vmu_pic.h"
#include "serialize.h"

namespace vmu
{

namespace
{
constexpr u16 IE = 0x108;
constexpr u16 IP = 0x109;
constexpr u16 PCON = 0x107;
constexpr u16 bit(Lc86InterruptController::Irq irq)
{
	return 1u << static_cast<unsigned>(irq);
}
}

void Lc86InterruptController::reset()
{
	pending = 0;
	active.fill(0);
	inhibit = false;
}

void Lc86InterruptController::raise(Irq irq)
{
	if (irq != Irq::Reset && static_cast<unsigned>(irq) < static_cast<unsigned>(Irq::Count))
		pending |= bit(irq);
}

u16 Lc86InterruptController::vector(Irq irq)
{
	static constexpr u16 vectors[] = {
		0x00, 0x03, 0x0b, 0x13, 0x1b, 0x23, 0x2b, 0x33,
		0x3b, 0x43, 0x4b, 0x4f, 0x52, 0x55, 0x5a, 0x5d,
	};
	return vectors[static_cast<unsigned>(irq)];
}

u16 Lc86InterruptController::enabledAtPriority(Priority priority)
{
	const u8 ie = memory.readData(IE);
	const u8 ip = memory.readData(IP);
	u16 mask = 0;
	if (priority == Highest)
	{
		if (!(ie & 0x01))
			mask |= bit(Irq::External0);
		if (!(ie & 0x01) && !(ie & 0x02))
			mask |= bit(Irq::External1);
		return mask;
	}
	if (priority == Low && !(ie & 0x80))
		return 0;

	struct Mapping { Irq irq; u8 mask; };
	static constexpr Mapping mappings[] = {
		{ Irq::External2Timer0Low, 0x01 }, { Irq::External3BaseTimer, 0x02 },
		{ Irq::Timer0High, 0x04 }, { Irq::Timer1, 0x08 },
		{ Irq::Serial0, 0x10 }, { Irq::Serial1, 0x20 },
		{ Irq::MapleReceive, 0x40 }, { Irq::Port3, 0x80 },
	};
	for (const auto& mapping : mappings)
		if (((ip & mapping.mask) != 0) == (priority == High))
			mask |= bit(mapping.irq);
	if (priority == Low)
	{
		if (ie & 0x01)
			mask |= bit(Irq::External0) | bit(Irq::External1);
		else if (ie & 0x02)
			mask |= bit(Irq::External1);
	}
	return mask;
}

int Lc86InterruptController::activePriority() const
{
	for (int priority = Highest; priority >= Low; --priority)
		if (active[priority])
			return priority;
	return -1;
}

bool Lc86InterruptController::poll()
{
	if (inhibit)
	{
		inhibit = false;
		return false;
	}
	const int current = activePriority();
	for (int priority = Highest; priority > current; --priority)
	{
		u16 candidates = pending & enabledAtPriority(static_cast<Priority>(priority));
		if (!candidates)
			continue;
		unsigned irqNumber = 0;
		while (!(candidates & (1u << irqNumber)))
			++irqNumber;
		const u16 request = 1u << irqNumber;
		pending &= ~request;
		active[priority] = request;
		memory.writeData(PCON, memory.readData(PCON) & ~1);
		cpu.acceptInterrupt(vector(static_cast<Irq>(irqNumber)));
		return true;
	}
	return false;
}

void Lc86InterruptController::returned()
{
	const int priority = activePriority();
	if (priority >= 0)
		active[priority] = 0;
	// The LC86 does not accept another interrupt on the instruction boundary
	// immediately following RETI.
	inhibit = true;
}

void Lc86InterruptController::serialize(Serializer& serializer) const
{
	serializer << pending << active << inhibit;
}

void Lc86InterruptController::deserialize(Deserializer& deserializer)
{
	deserializer >> pending >> active >> inhibit;
}

} // namespace vmu
