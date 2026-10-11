#pragma once

#include "vmu_cpu.h"

#include <array>
#include <cstddef>

namespace vmu
{

// LC86K87 data/program bus and the VMU's on-chip memories. Peripheral timing
// is intentionally kept outside this class; register storage and documented
// access side effects live here so the CPU can run independently.
class Lc86MemoryMap final : public Lc86Memory
{
public:
	static constexpr size_t RomSize = 64 * 1024;
	static constexpr size_t FlashSize = 128 * 1024;

	Lc86MemoryMap(const u8 *rom, size_t romSize, u8 *flash, size_t flashSize);

	void reset();
	u8 readProgram(u16 address) override;
	void switchProgramBank() override;
	u8 readData(u16 address) override;
	u8 readDataLatch(u16 address) override;
	void writeData(u16 address, u8 value) override;
	void writeDataLatch(u16 address, u8 value) override;
	u8 readFlash(u32 address) override;
	void writeFlashInstruction(u16 address, u8 value, u8 flashControl,
			u8 externalControl) override;
	void returnFromInterrupt() override { interruptReturn = true; }

	void setButtons(u8 pressed) { buttons = pressed; }
	bool takeInterruptReturn()
	{
		const bool value = interruptReturn;
		interruptReturn = false;
		return value;
	}
	bool takeFlashDirty()
	{
		const bool value = flashDirty;
		flashDirty = false;
		return value;
	}
	u8 *workRam() { return wram.data(); }
	const u8 *xramBank(unsigned bank) const { return xram[bank].data(); }
	bool lcdEnabled() const { return (sfr[0x20] & 0x08) && (sfr[0x27] & 0x80); }
	void copyLcdFramebuffer(u8 *destination) const;
	u8 lcdIcon(unsigned icon) const { return icon < 6 ? xram[2][icon] : 0; }
	u8 readSfrRaw(u16 address) const { return sfr[address - 0x100]; }
	void writeSfrRaw(u16 address, u8 value) { sfr[address - 0x100] = value; }
	u8 timerCounter(unsigned timer, bool high) const;
	void setTimerCounter(unsigned timer, bool high, u8 value);
	void serialize(Serializer& serializer) const;
	void deserialize(Deserializer& deserializer);

private:
	void incrementWramAddress();
	unsigned ramBank() const { return (sfr[1] >> 1) & 1; }
	unsigned xramBankIndex() const { return sfr[0x25] <= 2 ? sfr[0x25] : 0; }

	const u8 *rom;
	size_t romSize;
	u8 *flash;
	size_t flashSize;
	std::array<std::array<u8, 256>, 2> ram {};
	std::array<u8, 128> sfr {};
	std::array<std::array<u8, 96>, 3> xram {};
	std::array<u8, 512> wram {};
	u8 buttons = 0;
	u8 flashProgramState = 0;
	bool flashDirty = false;
	bool interruptReturn = false;
	u8 programBank = 0;
	u8 timer0Low = 0;
	u8 timer0High = 0;
	u8 timer1Low = 0;
	u8 timer1High = 0;
};

} // namespace vmu
