#include "vmu_memory.h"
#include "serialize.h"

#include <algorithm>
#include <cstring>

namespace vmu
{

namespace
{
constexpr u16 ACC = 0x100;
constexpr u16 PSW = 0x101;
constexpr u16 EXT = 0x10d;
constexpr u16 OCR = 0x10e;
constexpr u16 MCR = 0x120;
constexpr u16 XBNK = 0x125;
constexpr u16 VCCR = 0x127;
constexpr u16 P1 = 0x144;
constexpr u16 P1DDR = 0x145;
constexpr u16 P1FCR = 0x146;
constexpr u16 P3 = 0x14c;
constexpr u16 P3DDR = 0x14d;
constexpr u16 P7 = 0x15c;
constexpr u16 VSEL = 0x163;
constexpr u16 VRMAD1 = 0x164;
constexpr u16 VRMAD2 = 0x165;
constexpr u16 VTRBF = 0x166;
}

Lc86MemoryMap::Lc86MemoryMap(const u8 *rom, size_t romSize, u8 *flash, size_t flashSize)
	: rom(rom), romSize(romSize), flash(flash), flashSize(flashSize)
{
	reset();
}

void Lc86MemoryMap::reset()
{
	for (auto& bank : ram)
		bank.fill(0);
	sfr.fill(0);
	for (auto& bank : xram)
		bank.fill(0);
	wram.fill(0);
	sfr[P1FCR - 0x100] = 0xbf;
	sfr[0x4e] = 0xfd; // P3INT
	sfr[0x5f] = 0xc0; // ISL
	sfr[VSEL - 0x100] = 0xfc;
	sfr[0x7f] = 0x41; // Base timer running, interrupt channel 0 enabled.
	buttons = 0;
	flashProgramState = 0;
	flashDirty = false;
	interruptReturn = false;
	programBank = 0;
	timer0Low = timer0High = timer1Low = timer1High = 0;
}

u8 Lc86MemoryMap::readProgram(u16 address)
{
	if (programBank == 0)
		return rom != nullptr && address < romSize ? rom[address] : 0xff;
	const size_t flashAddress = (programBank == 1 ? 0 : 0x10000) + address;
	return flash != nullptr && flashAddress < flashSize ? flash[flashAddress] : 0xff;
}

void Lc86MemoryMap::switchProgramBank()
{
	const u8 externalControl = sfr[EXT - 0x100];
	programBank = externalControl & 1 ? 1 : externalControl & 8 ? 0 : 2;
}

u8 Lc86MemoryMap::readData(u16 address)
{
	if (address < 0x100)
		return ram[ramBank()][address];
	if (address >= 0x180 && address < 0x200)
	{
		if (sfr[VCCR - 0x100] & 0x40)
			return 0xff;
		const unsigned bank = xramBankIndex();
		const unsigned offset = address - 0x180;
		if (bank == 2)
			return offset < 6 ? xram[bank][offset] : 0xff;
		if ((offset & 0x0f) >= 12)
			return 0xff;
		return xram[bank][(offset >> 4) * 12 + (offset & 0x0f)];
	}
	if (address >= 0x180)
		return 0xff;
	if (address == P1DDR || address == P1FCR || address == P3DDR
			|| address == MCR || address == VCCR)
		return 0xff;
	if (address == VTRBF)
	{
		const u16 wramAddress = (sfr[VRMAD2 - 0x100] & 1) << 8 | sfr[VRMAD1 - 0x100];
		const u8 value = wram[wramAddress];
		if (sfr[VSEL - 0x100] & 0x10)
			incrementWramAddress();
		return value;
	}
	if (address == VRMAD2)
		return 0xfe | (sfr[VRMAD2 - 0x100] & 1);
	if (address == 0x112)
		return timer0Low;
	if (address == 0x114)
		return timer0High;
	if (address == 0x11b)
		return timer1Low;
	if (address == 0x11d)
		return timer1High;
	if (address == P1)
		return sfr[P1 - 0x100] & sfr[P1DDR - 0x100];
	if (address == P3)
		return (sfr[P3 - 0x100] & sfr[P3DDR - 0x100])
				| (static_cast<u8>(~buttons) & static_cast<u8>(~sfr[P3DDR - 0x100]));
	if (address == P7)
		return 0xf0 | sfr[P7 - 0x100];
	if (address == OCR)
		return 0x4c | sfr[OCR - 0x100];
	return sfr[address - 0x100];
}

u8 Lc86MemoryMap::readDataLatch(u16 address)
{
	if (address == P1 || address == P3 || address == P7
			|| address == 0x11b || address == 0x11d)
		return sfr[address - 0x100];
	return readData(address);
}

void Lc86MemoryMap::incrementWramAddress()
{
	if (++sfr[VRMAD1 - 0x100] == 0)
		sfr[VRMAD2 - 0x100] ^= 1;
}

void Lc86MemoryMap::writeData(u16 address, u8 value)
{
	if (address < 0x100)
	{
		ram[ramBank()][address] = value;
		return;
	}
	if (address >= 0x180 && address < 0x200)
	{
		if (sfr[VCCR - 0x100] & 0x40)
			return;
		const unsigned bank = xramBankIndex();
		const unsigned offset = address - 0x180;
		if (bank == 2)
		{
			if (offset < 6)
				xram[bank][offset] = value;
		}
		else if ((offset & 0x0f) < 12)
			xram[bank][(offset >> 4) * 12 + (offset & 0x0f)] = value;
		return;
	}
	if (address >= 0x180)
		return;
	if (address == ACC)
	{
		unsigned parity = value;
		parity ^= parity >> 4;
		parity ^= parity >> 2;
		parity ^= parity >> 1;
		sfr[PSW - 0x100] = (sfr[PSW - 0x100] & 0xfe) | (parity & 1);
	}
	if (address == XBNK && value > 2)
		return;
	if (address == VRMAD2)
		value &= 1;
	if (address == VTRBF)
	{
		const u16 wramAddress = (sfr[VRMAD2 - 0x100] & 1) << 8 | sfr[VRMAD1 - 0x100];
		wram[wramAddress] = value;
		if (sfr[VSEL - 0x100] & 0x10)
			incrementWramAddress();
		return;
	}
	if (address == 0x110)
	{
		if (!(value & 0x40))
			timer0Low = sfr[0x13];
		if (!(value & 0x80))
			timer0High = sfr[0x15];
	}
	else if (address == 0x112 && !(sfr[0x10] & 0x40))
		timer0Low = value;
	else if (address == 0x113 && !(sfr[0x10] & 0x40))
		timer0Low = value;
	else if (address == 0x114 && !(sfr[0x10] & 0x80))
		timer0High = value;
	else if (address == 0x115 && !(sfr[0x10] & 0x80))
		timer0High = value;
	else if (address == 0x118)
	{
		if (!(value & 0x40))
			timer1Low = sfr[0x1b];
		if (!(value & 0x80))
			timer1High = sfr[0x1d];
	}
	else if (address == 0x11b && !(sfr[0x18] & 0x40))
		timer1Low = value;
	else if (address == 0x11d && !(sfr[0x18] & 0x80))
		timer1High = value;
	sfr[address - 0x100] = value;
}

u8 Lc86MemoryMap::timerCounter(unsigned timer, bool high) const
{
	return timer == 0 ? (high ? timer0High : timer0Low)
			: (high ? timer1High : timer1Low);
}

void Lc86MemoryMap::setTimerCounter(unsigned timer, bool high, u8 value)
{
	u8 *counter = timer == 0 ? (high ? &timer0High : &timer0Low)
			: (high ? &timer1High : &timer1Low);
	*counter = value;
}

void Lc86MemoryMap::copyLcdFramebuffer(u8 *destination) const
{
	if (!lcdEnabled())
	{
		std::memset(destination, 0, 192);
		return;
	}
	std::memcpy(destination, xram[0].data(), 96);
	std::memcpy(destination + 96, xram[1].data(), 96);
}

void Lc86MemoryMap::serialize(Serializer& serializer) const
{
	serializer << ram << sfr << xram << wram << buttons << flashProgramState
			<< flashDirty << interruptReturn << programBank << timer0Low << timer0High
			<< timer1Low << timer1High;
}

void Lc86MemoryMap::deserialize(Deserializer& deserializer)
{
	deserializer >> ram >> sfr >> xram >> wram >> buttons >> flashProgramState
			>> flashDirty >> interruptReturn >> programBank >> timer0Low >> timer0High
			>> timer1Low >> timer1High;
	if (programBank > 2 || sfr[0x25] > 2 || flashProgramState > 130)
		throw Deserializer::Exception("Invalid VMU memory state");
}

void Lc86MemoryMap::writeDataLatch(u16 address, u8 value)
{
	writeData(address, value);
}

u8 Lc86MemoryMap::readFlash(u32 address)
{
	return flash != nullptr && address < flashSize ? flash[address] : 0xff;
}

void Lc86MemoryMap::writeFlashInstruction(u16 address, u8 value, u8 flashControl,
		u8 externalControl)
{
	if (externalControl & 1)
		return;
	static constexpr u16 unlockAddress[] = { 0x5555, 0x2aaa, 0x5555 };
	static constexpr u8 unlockValue[] = { 0xaa, 0x55, 0xa0 };
	if (flashControl & 2)
	{
		if (flashProgramState < 3 && address == unlockAddress[flashProgramState]
				&& value == unlockValue[flashProgramState])
			++flashProgramState;
		else
			flashProgramState = 0;
		return;
	}
	if (flashProgramState < 3)
	{
		flashProgramState = 0;
		return;
	}
	const size_t flashAddress = (static_cast<size_t>(flashControl & 1) << 16) | address;
	if (flashProgramState == 3 && (flashAddress & 0x7f))
	{
		flashProgramState = 0;
		return;
	}
	if (flash != nullptr && flashAddress < flashSize)
	{
		flash[flashAddress] = value;
		flashDirty = true;
	}
	if (++flashProgramState == 131)
		flashProgramState = 0;
}

} // namespace vmu
