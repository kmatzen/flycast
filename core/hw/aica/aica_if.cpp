/*
	aica interface
		Handles RTC, Display mode reg && arm reset reg !
	arm7 is handled on a separate arm plugin now
*/

#include "aica_if.h"
#include "aica_mem.h"
#include "hw/holly/sb.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "profiler/dc_profiler.h"
#include "hw/sh4/dyna/blockmanager.h"
#include "hw/arm7/arm7.h"
#include "cfg/option.h"

#include "serialize.h"
#include "hw/arm7/arm_mem.h"
#include "dsp.h"
#include "sgc_if.h"
#include "aica.h"

#include <ctime>

namespace aica
{

RamRegion aica_ram;
static u32 VREG;
static u32 ARMRST;
static u32 rtc_EN = 0;
int dma_sched_id = -1;
static bool aicaDmaActive;
static bool aicaDmaToSystem;
static u32 aicaDmaRootAddress;
static u32 aicaDmaG2Address;
static u32 aicaDmaRemaining;
static u32 aicaDmaControl;
struct ExtDmaState {
	bool active;
	bool toSystem;
	u32 rootAddress;
	u32 g2Address;
	u32 remaining;
	u32 control;
};
struct G2DmaConfig {
	u32 rootAddress;
	u32 g2Address;
	u32 length;
	u32 direction;
	u32 trigger;
};
static G2DmaConfig aicaDmaConfig;
static G2DmaConfig extDmaConfig[3];
static ExtDmaState extDma[3];
static int g2BusOwner = -1; // 0: AICA, 1-3: external channels
static u32 RealTimeClock;
int rtc_schid = -1;

static void clearAicaDmaState()
{
	aicaDmaActive = false;
	aicaDmaToSystem = false;
	aicaDmaRootAddress = 0;
	aicaDmaG2Address = 0;
	aicaDmaRemaining = 0;
	aicaDmaControl = 0;
}

static constexpr u32 extStartRegs[] = { SB_E1ST_addr, SB_E2ST_addr, SB_DDST_addr };
static constexpr u32 extEnableRegs[] = { SB_E1EN_addr, SB_E2EN_addr, SB_DDEN_addr };
static constexpr u32 extSourceRegs[] = { SB_E1STAR_addr, SB_E2STAR_addr, SB_DDSTAR_addr };
static constexpr u32 extDestRegs[] = { SB_E1STAG_addr, SB_E2STAG_addr, SB_DDSTAG_addr };
static constexpr u32 extLengthRegs[] = { SB_E1LEN_addr, SB_E2LEN_addr, SB_DDLEN_addr };
static constexpr u32 extTriggerRegs[] = { SB_E1TSEL_addr, SB_E2TSEL_addr, SB_DDTSEL_addr };
static constexpr u32 extSuspendRegs[] = { SB_E1SUSP_addr, SB_E2SUSP_addr, SB_DDSUSP_addr };
static constexpr u32 extG2CounterRegs[] = { SB_E1STAGD_addr, SB_E2STAGD_addr, SB_DDSTAGD_addr };
static constexpr u32 extRootCounterRegs[] = { SB_E1STARD_addr, SB_E2STARD_addr, SB_DDSTARD_addr };
static constexpr u32 extLengthCounterRegs[] = { SB_E1LEND_addr, SB_E2LEND_addr, SB_DDLEND_addr };
static constexpr HollyInterruptID extInterrupts[] = { holly_EXT_DMA1, holly_EXT_DMA2, holly_DEV_DMA };

static void latchAicaDmaConfig()
{
	aicaDmaConfig = { SB_ADSTAR, SB_ADSTAG, SB_ADLEN, SB_ADDIR, SB_ADTSEL };
}

static void latchExtDmaConfig(int index)
{
	extDmaConfig[index] = {
		SB_REGN_32(extSourceRegs[index]), SB_REGN_32(extDestRegs[index]),
		SB_REGN_32(extLengthRegs[index]), SB_REGN_32(extSourceRegs[index] + 8),
		SB_REGN_32(extTriggerRegs[index])
	};
}

static u32 aicaDmaTrigger()
{
	return SB_ADEN ? aicaDmaConfig.trigger : SB_ADTSEL;
}

static u32 extDmaTrigger(int index)
{
	return SB_REGN_32(extEnableRegs[index]) ? extDmaConfig[index].trigger
			: SB_REGN_32(extTriggerRegs[index]);
}

static int g2UnitCycles(u32 remaining)
{
	return std::min(32u, remaining) * (SH4_MAIN_CLOCK / 2 / G2_BUS_CLOCK);
}

static void selectG2BusOwner()
{
	if (g2BusOwner >= 0)
		return;
	if (aicaDmaActive && !((aicaDmaConfig.trigger & 4) && (SB_ADSUSP & 1)))
		g2BusOwner = 0;
	else {
		for (int i = 0; i < 3; ++i)
			if (extDma[i].active && !((extDmaConfig[i].trigger & 4)
					&& (SB_REGN_32(extSuspendRegs[i]) & 1))) {
				g2BusOwner = i + 1;
				break;
			}
	}
}

static void scheduleNextG2Dma()
{
	selectG2BusOwner();
	if (g2BusOwner == 0)
		sh4_sched_request(dma_sched_id, g2UnitCycles(aicaDmaRemaining));
	else if (g2BusOwner > 0)
		sh4_sched_request(dma_sched_id, g2UnitCycles(extDma[g2BusOwner - 1].remaining));
}

u32 GetRTC_now()
{
	// rtc kept static for netplay when savestate is not loaded
	if (config::GGPOEnable)
		// 1/1/70 00:00:00
		return (20 * 365 + 5) * 24 * 60 * 60;

	// The Dreamcast Epoch time is 1/1/50 00:00 but without support for time zone or DST.
	// We compute the TZ/DST current time offset and add it to the result
	// as if we were in the UTC time zone (as well as the DC Epoch)
	time_t rawtime = time(NULL);
	struct tm localtm, gmtm;
	localtm = *localtime(&rawtime);
	gmtm = *gmtime(&rawtime);
	gmtm.tm_isdst = -1;
	time_t time_offset = mktime(&localtm) - mktime(&gmtm);
	// 1/1/50 to 1/1/70 is 20 years and 5 leap days
	return (20 * 365 + 5) * 24 * 60 * 60 + rawtime + time_offset;
}

template<typename T>
T readRtcReg(u32 addr)
{
	switch (addr & 0xFF)
	{
	case 0:
		return (T)(RealTimeClock >> 16);
	case 4:
		return (T)(RealTimeClock & 0xFFFF);
	case 8:
		return 0;
	}

	WARN_LOG(AICA, "readRtcReg: invalid address %x sz %d", addr, (int)sizeof(T));
	return 0;
}
template u8 readRtcReg<>(u32 addr);
template u16 readRtcReg<>(u32 addr);
template u32 readRtcReg<>(u32 addr);

template<typename T>
void writeRtcReg(u32 addr, T data)
{
	switch (addr & 0xFF)
	{
	case 0:
		if (rtc_EN)
		{
			RealTimeClock &= 0xFFFF;
			RealTimeClock |= (data & 0xFFFF) << 16;
			rtc_EN = 0;
		}
		break;
	case 4:
		if (rtc_EN)
		{
			RealTimeClock &= 0xFFFF0000;
			RealTimeClock |= data & 0xFFFF;
		}
		break;
	case 8:
		rtc_EN = data & 1;
		break;

	default:
		WARN_LOG(AICA, "writeRtcReg: invalid address %x sz %d data %x", addr, (int)sizeof(T), data);
		break;
	}
}
template void writeRtcReg<>(u32 addr, u8 data);
template void writeRtcReg<>(u32 addr, u16 data);
template void writeRtcReg<>(u32 addr, u32 data);

template<typename T>
T readAicaReg(u32 addr)
{
	addr &= 0x7FFF;
	if (sizeof(T) == 1)
	{
		switch (addr)
		{
		case 0x2C00:
			return (T)ARMRST;
		case 0x2C01:
			return (T)VREG;
		default:
			break;
		}
	}
	else if (addr == 0x2C00)
		return (T)((VREG << 8) | ARMRST);

	if (sizeof(T) == 4)
		return readRegInternal<u16>(addr);
	else
		return readRegInternal<T>(addr);
}
template u8 readAicaReg<>(u32 addr);
template u16 readAicaReg<>(u32 addr);
template u32 readAicaReg<>(u32 addr);

static void ArmSetRST()
{
	ARMRST &= 1;
	arm::enable(ARMRST == 0);
}

template<typename T>
void writeAicaReg(u32 addr, T data)
{
	addr &= 0x7FFF;
	// The interrupt-completion M register and its RP bit are controlled by
	// ARM7DI only. Main-CPU writes to this register have no effect.
	if (addr == REG_M || addr == REG_M + 1)
		return;

	if (sizeof(T) == 1)
	{
		switch (addr)
		{
		case 0x2C00:
			ARMRST = data;
			INFO_LOG(AICA_ARM, "ARMRST = %02X", ARMRST);
			ArmSetRST();
			return;
		case 0x2C01:
			VREG = data & 3;
			INFO_LOG(AICA_ARM, "VREG = %02X", VREG);
			return;
		default:
			break;
		}
	}
	else if (addr == 0x2C00)
	{
		VREG = (data >> 8) & 3;
		ARMRST = data & 0xFF;
		INFO_LOG(AICA_ARM, "VREG = %02X ARMRST %02X", VREG, ARMRST);
		ArmSetRST();
		return;
	}
	if (sizeof(T) == 4)
		writeRegInternal(addr, (u16)data);
	else
		writeRegInternal(addr, data);
}
template void writeAicaReg<>(u32 addr, u8 data);
template void writeAicaReg<>(u32 addr, u16 data);
template void writeAicaReg<>(u32 addr, u32 data);

static int DreamcastSecond(int tag, int cycles, int jitter, void *arg)
{
	const int elapsedSeconds = 1 + std::max(0, jitter) / SH4_MAIN_CLOCK;
	for (int i = 0; i < elapsedSeconds; ++i)
	{
		RealTimeClock++;
		dc_prof_periodical();

#if FEAT_SHREC != DYNAREC_NONE
		bm_Periodical_1s();
#endif
	}

	return elapsedSeconds * SH4_MAIN_CLOCK;
}

//Init/res/term
void initRtc()
{
	RealTimeClock = GetRTC_now();
	rtc_EN = 0;
	if (rtc_schid == -1)
		rtc_schid = sh4_sched_register(0, &DreamcastSecond);
}

void resetRtc(bool hard)
{
	if (hard)
	{
		initRtc();
		sh4_sched_request(rtc_schid, SH4_MAIN_CLOCK);
	}
	// The RTC write-unlock is a transient control latch, not battery-backed
	// clock state. A system reset must require software to unlock it again.
	rtc_EN = 0;
	VREG = 0;
	ARMRST = 0;
}

void termRtc()
{
	sh4_sched_unregister(rtc_schid);
	rtc_schid = -1;
}

static void transferG2DmaUnit()
{
	if (g2BusOwner > 0)
	{
		const int index = g2BusOwner - 1;
		ExtDmaState& dma = extDma[index];
		const u32 amount = std::min(32u, dma.remaining);
		if (dma.toSystem)
			WriteMemBlock_nommu_dma(dma.rootAddress, dma.g2Address, amount);
		else
			WriteMemBlock_nommu_dma(dma.g2Address, dma.rootAddress, amount);
		dma.rootAddress += amount;
		dma.g2Address += amount;
		dma.remaining -= amount;
		SB_REGN_32(extRootCounterRegs[index]) = dma.rootAddress;
		SB_REGN_32(extG2CounterRegs[index]) = dma.g2Address;
		SB_REGN_32(extLengthCounterRegs[index]) = dma.remaining & 0x01ffffe0;
		if (dma.remaining == 0)
		{
			dma.active = false;
			SB_REGN_32(extLengthCounterRegs[index]) = dma.control & 0x01ffffe0;
			SB_REGN_32(extEnableRegs[index]) = (dma.control & 0x80000000) == 0;
			SB_REGN_32(extStartRegs[index]) = 0;
			if (extDmaConfig[index].trigger & 4)
				SB_REGN_32(extSuspendRegs[index]) |= 0x10;
			else
				SB_REGN_32(extSuspendRegs[index]) &= ~0x10u;
			asic_RaiseInterrupt(extInterrupts[index]);
		}
		// Re-arbitrate at every 32-byte boundary. Channel number is fixed
		// priority, so a newly requested higher channel may preempt.
		g2BusOwner = -1;
		return;
	}
	if (!aicaDmaActive)
	{
		// Before incremental G2 DMA, the payload was copied at start and this
		// event only completed the registers. Preserve pending old save states.
		if (SB_ADST & 1)
		{
			const u32 len = SB_ADLEN & 0x7fffffff;
			SB_ADEN = (SB_ADLEN & 0x80000000) == 0;
			SB_ADSTARD = SB_ADSTAR + len;
			SB_ADSTAGD = SB_ADSTAG + len;
			SB_ADLEND = aicaDmaConfig.length & 0x01ffffe0;
			SB_ADST = 0;
			if (SB_ADTSEL & 4)
				SB_ADSUSP |= 0x10;
			else
				SB_ADSUSP &= ~0x10u;
			asic_RaiseInterrupt(holly_SPU_DMA);
		}
		g2BusOwner = -1;
		return;
	}

	const u32 amount = std::min(32u, aicaDmaRemaining);
	const u32 area0Address = aicaDmaG2Address & 0x01ffffff;
	const bool waveMemory = area0Address >= 0x00800000
			&& area0Address <= 0x00ffffff;
	const bool memoryAccessAllowed = !waveMemory
			|| mainCpuSoundMemoryAccessAllowed(CommonData->MRWINH);
	if (aicaDmaToSystem)
	{
		if (memoryAccessAllowed)
			WriteMemBlock_nommu_dma(aicaDmaRootAddress, aicaDmaG2Address, amount);
	}
	else if (memoryAccessAllowed && (!waveMemory || !CommonData->RP))
		// RP makes the main-CPU/G2 view of sound DRAM read-only. The bus
		// transaction still consumes time and advances the DMA counters.
		WriteMemBlock_nommu_dma(aicaDmaG2Address, aicaDmaRootAddress, amount);
	aicaDmaRootAddress += amount;
	aicaDmaG2Address += amount;
	aicaDmaRemaining -= amount;
	SB_ADSTARD = aicaDmaRootAddress;
	SB_ADSTAGD = aicaDmaG2Address;
	SB_ADLEND = aicaDmaRemaining & 0x01ffffe0;

	if (aicaDmaRemaining == 0)
	{
		aicaDmaActive = false;
		SB_ADLEND = aicaDmaControl & 0x01ffffe0;
		SB_ADEN = (aicaDmaControl & 0x80000000) == 0;
		SB_ADST = 0;
		if (aicaDmaConfig.trigger & 4)
			SB_ADSUSP |= 0x10;
		else
			SB_ADSUSP &= ~0x10u;
		asic_RaiseInterrupt(holly_SPU_DMA);
	}
	g2BusOwner = -1;
}

static int dma_end_sched(int tag, int cycles, int jitter, void *arg)
{
	// A canceled/suspended channel can leave a stale deadline in an old save
	// state. Never let that deadline transfer data for an inactive or paused
	// owner; select only a currently runnable channel.
	if (g2BusOwner < -1 || g2BusOwner > 3)
		g2BusOwner = -1;
	if ((g2BusOwner == 0 && ((!aicaDmaActive && !(SB_ADST & 1))
				|| ((aicaDmaConfig.trigger & 4) && (SB_ADSUSP & 1))))
			|| (g2BusOwner > 0 && (!extDma[g2BusOwner - 1].active
					|| ((extDmaConfig[g2BusOwner - 1].trigger & 4)
							&& (SB_REGN_32(extSuspendRegs[g2BusOwner - 1]) & 1)))))
		g2BusOwner = -1;
	// Old save states copied the AICA payload at start and retained only a
	// scheduled completion with ADST set; they have no incremental DMA owner.
	if (g2BusOwner < 0 && !aicaDmaActive && (SB_ADST & 1))
		g2BusOwner = 0;
	selectG2BusOwner();
	if (g2BusOwner < 0)
		return 0;

	int remainingJitter = jitter;
	for (;;)
	{
		transferG2DmaUnit();
		selectG2BusOwner();
		if (g2BusOwner < 0)
			return 0;

		const u32 remaining = g2BusOwner == 0 ? aicaDmaRemaining
				: extDma[g2BusOwner - 1].remaining;
		const int nextCycles = g2UnitCycles(remaining);
		if (nextCycles > remainingJitter)
			return jitter + nextCycles - remainingJitter;
		remainingJitter -= nextCycles;
	}
}

static bool check_STAG(u32 addr, int channel)
{
	switch (channel) {
	case 0: // AICA registers, wave memory, and their image area
		return (addr >= 0x00700000 && addr <= 0x00707fe0)
				|| (addr >= 0x00800000 && addr <= 0x009fffe0)
				|| (addr >= 0x02700000 && addr <= 0x02ffffe0);
	case 1:
		return addr >= 0x01000000 && addr <= 0x01ffffe0;
	case 2:
		return addr >= 0x03000000 && addr <= 0x03ffffe0;
	case 3:
		return addr >= 0x14000000 && addr <= 0x17ffffe0;
	default:
		return false;
	}
}

static bool check_STAR(u32 addr)
{
	return (addr >= 0x0c000000 && addr <= 0x0cffffe0)
			|| (addr >= 0x04000000 && addr <= 0x04ffffe0)
			|| (addr >= 0x05000000 && addr <= 0x057fffe0);
}

static bool check_STAR_overrun(u32 addr, u32 len)
{
	const u64 end = (u64)addr + len - 1;
	const bool inSystemRam = addr >= 0x0c000000 && end <= 0x0cffffff;
	const bool inTexture64 = addr >= 0x04000000 && end <= 0x04ffffff;
	const bool inTexture32 = addr >= 0x05000000 && end <= 0x057fffff;
	if (!inSystemRam && !inTexture64 && !inTexture32)
		return false;
	// G2APRO constrains G2 DMA involving system memory. Texture-memory
	// transfers use the other legal STAR windows and are not covered by it.
	if (!inSystemRam)
		return true;
	const u32 bottom = (((SB_G2APRO >> 8) & 0x7f) << 20) | 0x08000000;
	const u32 top = ((SB_G2APRO & 0x7f) << 20) | 0x080fffff;

	return addr >= bottom && end <= top;
}

extern const char AICA_TAG[] = "G2-AICA DMA";
extern const char EXT1_TAG[] = "G2-EXT1 DMA";
extern const char EXT2_TAG[] = "G2-EXT2 DMA";
extern const char DDEV_TAG[] = "G2-DDev DMA";

template<u32 ENABLE, u32 START, u32 SRC, u32 DEST, u32 LEN, u32 DIR,
	HollyInterruptID interrupt, HollyInterruptID iainterrupt, HollyInterruptID ovinterrupt,
	const char *LogTag, bool HardwareTrigger = false>
void Write_DmaStart(u32 addr, u32 data)
{
	u32& enableReg = SB_REGN_32(ENABLE);
	u32& startReg = SB_REGN_32(START);
	constexpr int index = START == SB_E1ST_addr ? 0 : START == SB_E2ST_addr ? 1 : 2;
	const G2DmaConfig& config = extDmaConfig[index];
	if (!(data & 1) || enableReg == 0
			|| (((config.trigger & 2) != 0) != HardwareTrigger))
		return;
	u32 src = config.rootAddress;
	u32 dst = config.g2Address;
	u32 len = config.length & 0x7FFFFFFF;
	if (len == 0)
		len = 0x02000000; // Zero encodes the maximum 32 MiB transfer.

	// STAR
	if (!check_STAR(src))
	{
		INFO_LOG(AICA, "%s: Invalid src address %08x", LogTag, src);
		startReg = 0;
		enableReg = 0;
		asic_RaiseInterrupt(iainterrupt);
		return;
	}
	// STAG
	if (!check_STAG(dst, index + 1))
	{
		INFO_LOG(AICA, "%s: Invalid dst address %08x", LogTag, dst);
		startReg = 0;
		enableReg = 0;
		asic_RaiseInterrupt(iainterrupt);
		return;
	}
	// Overrun
	if (!check_STAR_overrun(src, len))
	{
		INFO_LOG(AICA, "%s: Overrun address %08x len %x", LogTag, src, len);
		startReg = 0;
		enableReg = 0;
		asic_RaiseInterrupt(ovinterrupt);
		return;
	}

	ExtDmaState& dma = extDma[index];
	if (dma.active)
		return;
	DEBUG_LOG(AICA, "%s: DMA %s root %X G2 %X %d bytes", LogTag,
			config.direction ? "to root" : "to G2", src, dst, len);
	dma.active = true;
	dma.toSystem = config.direction == 1;
	dma.rootAddress = src;
	dma.g2Address = dst;
	dma.remaining = len;
	dma.control = config.length;
	startReg = 1;
	if ((config.trigger & 4)
			&& (SB_REGN_32(extSuspendRegs[index]) & 1))
		SB_REGN_32(extSuspendRegs[index]) |= 0x10;
	else
		SB_REGN_32(extSuspendRegs[index]) &= ~0x10u;
	SB_REGN_32(extRootCounterRegs[index]) = src;
	SB_REGN_32(extG2CounterRegs[index]) = dst;
	SB_REGN_32(extLengthCounterRegs[index]) = config.length & 0x01ffffe0;
	scheduleNextG2Dma();
}

static void StartAicaDma(u32 data, bool hardwareTrigger)
{
	//0x005F7800	SB_ADSTAG	RW	AICA:G2-DMA G2 start address 
	//0x005F7804	SB_ADSTAR	RW	AICA:G2-DMA system memory start address 
	//0x005F7808	SB_ADLEN	RW	AICA:G2-DMA length 
	//0x005F780C	SB_ADDIR	RW	AICA:G2-DMA direction 
	//0x005F7810	SB_ADTSEL	RW	AICA:G2-DMA trigger select 
	//0x005F7814	SB_ADEN		RW	AICA:G2-DMA enable
	//0x005F7818	SB_ADST		RW	AICA:G2-DMA start
	//0x005F781C	SB_ADSUSP	RW	AICA:G2-DMA suspend 
	
	if ((data & 1) == 1 && (SB_ADST & 1) == 0
			&& (((aicaDmaConfig.trigger & 2) != 0) == hardwareTrigger))
	{
		if (SB_ADEN == 1)
		{
			u32 src = aicaDmaConfig.rootAddress;
			u32 dst = aicaDmaConfig.g2Address;
			u32 len = aicaDmaConfig.length & 0x7FFFFFFF;
			if (len == 0)
				len = 0x02000000; // Zero encodes the maximum 32 MiB transfer.

			if (!check_STAR(src))
			{
				INFO_LOG(AICA, "AICA-DMA : Invalid src address %08x", src);
				SB_ADST = 0;
				SB_ADEN = 0;
				asic_RaiseInterrupt(holly_AICA_ILLADDR);
				return;
			}
			if (!check_STAG(dst, 0))
			{
				INFO_LOG(AICA, "AICA-DMA : Invalid dst address %08x", dst);
				SB_ADST = 0;
				SB_ADEN = 0;
				asic_RaiseInterrupt(holly_AICA_ILLADDR);
				return;
			}
			// Overrun
			if (!check_STAR_overrun(src, len))
			{
				INFO_LOG(AICA, "AICA-DMA : Overrun address %08x len %x", src, len);
				SB_ADST = 0;
				SB_ADEN = 0;
				asic_RaiseInterrupt(holly_AICA_OVERRUN);
				return;
			}

			if (aicaDmaConfig.direction == 1)
				DEBUG_LOG(AICA, "AICA-DMA : SB_ADDIR==1 DMA Read to 0x%X from 0x%X %x bytes", src, dst, SB_ADLEN);
			else
				DEBUG_LOG(AICA, "AICA-DMA : SB_ADDIR==0:DMA Write to 0x%X from 0x%X %x bytes", dst, src, SB_ADLEN);

			// indicate that dma is in progress
			SB_ADST = 1;
			SB_ADSUSP &= ~0x10;
			aicaDmaActive = true;
			aicaDmaToSystem = aicaDmaConfig.direction == 1;
			aicaDmaRootAddress = src;
			aicaDmaG2Address = dst;
			aicaDmaRemaining = len;
			aicaDmaControl = aicaDmaConfig.length;
			SB_ADSTARD = src;
			SB_ADSTAGD = dst;
			SB_ADLEND = aicaDmaConfig.length & 0x01ffffe0;

			// The G2 bus is 16 bits wide at 25 MHz. Commit one 32-byte bus unit
			// at each deadline so data and progress registers are observable in time.
			if ((aicaDmaConfig.trigger & 4) && (SB_ADSUSP & 1))
				SB_ADSUSP |= 0x10;
			else
				scheduleNextG2Dma();
		}
	}
}

static void Write_SB_ADST(u32 addr, u32 data)
{
	StartAicaDma(data, false);
}

static void Write_SB_ADSUSP(u32 addr, u32 data)
{
	if ((aicaDmaTrigger() & 4) == 0) {
		SB_ADSUSP &= ~0x11u;
		return;
	}
	const bool wasStopped = (SB_ADSUSP & 1) != 0;
	const bool stop = (data & 1) != 0;
	SB_ADSUSP = (SB_ADSUSP & ~1u) | (u32)stop;
	if (!aicaDmaActive)
		return;
	if (stop) {
		SB_ADSUSP |= 0x10;
		if (g2BusOwner == 0) {
			sh4_sched_request(dma_sched_id, -1);
			g2BusOwner = -1;
			scheduleNextG2Dma();
		}
	}
	else if (wasStopped) {
		SB_ADSUSP &= ~0x10u;
		scheduleNextG2Dma();
	}
}

template<u32 SUSP>
static u32 Read_SUSP(u32 addr)
{
	// The request bit is write-only. Bit 4 is live status rather than storage:
	// with suspend disabled it reads zero; with suspend enabled it reports an
	// idle DMA or an active DMA held by a suspend request.
	u32 value = SB_REGN_32(SUSP) & 0x20u; // External request input, if modeled.
	if constexpr (SUSP == SB_ADSUSP_addr)
	{
		if ((aicaDmaTrigger() & 4)
				&& (!(SB_ADST & 1) || (SB_ADSUSP & 1)))
			value |= 0x10;
	}
	else
	{
		constexpr int index = SUSP == SB_E1SUSP_addr ? 0
				: SUSP == SB_E2SUSP_addr ? 1 : 2;
		if ((extDmaTrigger(index) & 4)
				&& (!(SB_REGN_32(extStartRegs[index]) & 1)
						|| (SB_REGN_32(SUSP) & 1)))
			value |= 0x10;
	}
	return value;
}

static void Write_SB_ADEN(u32 addr, u32 data)
{
	const bool wasEnabled = SB_ADEN != 0;
	if ((data & 1) == 0)
	{
		SB_ADEN = 0;
		if (SB_ADST & 1)
		{
			if (dma_sched_id != -1 && g2BusOwner == 0)
				sh4_sched_request(dma_sched_id, -1);
			clearAicaDmaState();
			if (g2BusOwner == 0) {
				g2BusOwner = -1;
				scheduleNextG2Dma();
			}
			SB_ADST = 0;
			SB_ADLEND = SB_ADLEN & 0x01ffffe0;
			if (aicaDmaConfig.trigger & 4)
				SB_ADSUSP |= 0x10;
			else
				SB_ADSUSP &= ~0x10u;
		}
		return;
	}
	if (wasEnabled)
		return;

	SB_ADEN = 1;
	if (!check_STAR(SB_ADSTAR) || !check_STAG(SB_ADSTAG, 0))
	{
		SB_ADEN = 0;
		asic_RaiseInterrupt(holly_AICA_ILLADDR);
	}
	else
		latchAicaDmaConfig();
}

template<int index>
static void Write_ExtSUSP(u32 addr, u32 data)
{
	u32& suspend = SB_REGN_32(extSuspendRegs[index]);
	if ((extDmaTrigger(index) & 4) == 0) {
		suspend &= ~0x11u;
		return;
	}
	const bool wasStopped = (suspend & 1) != 0;
	const bool stop = (data & 1) != 0;
	suspend = (suspend & ~1u) | (u32)stop;
	if (!extDma[index].active)
		return;
	if (stop) {
		suspend |= 0x10;
		if (g2BusOwner == index + 1) {
			sh4_sched_request(dma_sched_id, -1);
			g2BusOwner = -1;
			scheduleNextG2Dma();
		}
	}
	else if (wasStopped) {
		suspend &= ~0x10u;
		scheduleNextG2Dma();
	}
}

template<int index, HollyInterruptID illegalAddressInterrupt>
static void Write_ExtEN(u32 addr, u32 data)
{
	u32& enable = SB_REGN_32(extEnableRegs[index]);
	const bool wasEnabled = enable != 0;
	enable = data & 1;
	if (enable == 0 && extDma[index].active) {
		extDma[index].active = false;
		SB_REGN_32(extLengthCounterRegs[index]) = extDmaConfig[index].length & 0x01ffffe0;
		SB_REGN_32(extStartRegs[index]) = 0;
		if (extDmaConfig[index].trigger & 4)
			SB_REGN_32(extSuspendRegs[index]) |= 0x10;
		else
			SB_REGN_32(extSuspendRegs[index]) &= ~0x10u;
		if (g2BusOwner == index + 1) {
			sh4_sched_request(dma_sched_id, -1);
			g2BusOwner = -1;
			scheduleNextG2Dma();
		}
	}
	else if (enable != 0 && !wasEnabled && (!check_STAR(SB_REGN_32(extSourceRegs[index]))
			|| !check_STAG(SB_REGN_32(extDestRegs[index]), index + 1))) {
		enable = 0;
		asic_RaiseInterrupt(illegalAddressInterrupt);
	}
	else if (enable != 0 && !wasEnabled)
		latchExtDmaConfig(index);
}

void sbInterrupt()
{
	if ((aicaDmaConfig.trigger & 2) && SB_ADEN && (SB_ADST & 1) == 0)
		StartAicaDma(1, true);
	if ((extDmaConfig[0].trigger & 2) && SB_E1EN && SB_E1ST == 0)
		Write_DmaStart<SB_E1EN_addr, SB_E1ST_addr, SB_E1STAR_addr, SB_E1STAG_addr, SB_E1LEN_addr,
				SB_E1DIR_addr, holly_EXT_DMA1, holly_EXT1_ILLADDR, holly_EXT1_OVERRUN, EXT1_TAG, true>(SB_E1ST_addr, 1);
	if ((extDmaConfig[1].trigger & 2) && SB_E2EN && SB_E2ST == 0)
		Write_DmaStart<SB_E2EN_addr, SB_E2ST_addr, SB_E2STAR_addr, SB_E2STAG_addr, SB_E2LEN_addr,
				SB_E2DIR_addr, holly_EXT_DMA2, holly_EXT2_ILLADDR, holly_EXT2_OVERRUN, EXT2_TAG, true>(SB_E2ST_addr, 1);
	if ((extDmaConfig[2].trigger & 2) && SB_DDEN && SB_DDST == 0)
		Write_DmaStart<SB_DDEN_addr, SB_DDST_addr, SB_DDSTAR_addr, SB_DDSTAG_addr, SB_DDLEN_addr,
				SB_DDDIR_addr, holly_DEV_DMA, holly_DEV_ILLADDR, holly_DEV_OVERRUN, DDEV_TAG, true>(SB_DDST_addr, 1);
}

template<u32 STAG, HollyInterruptID iainterrupt, const char *LogTag, int Channel>
void Write_SB_STAG(u32 addr, u32 data)
{
	u32& stagReg = sb_regs[(STAG - SB_BASE) / 4];
	stagReg = data & 0x1FFFFFE0;

	if (!check_STAG(data, Channel))
	{
		INFO_LOG(AICA, "%s Write_SB_STAG: Invalid address %08x", LogTag, data);
		asic_RaiseInterrupt(iainterrupt);
	}
}

template<u32 STAR, HollyInterruptID iainterrupt, const char *LogTag>
void Write_SB_STAR(u32 addr, u32 data)
{
	u32& starReg = sb_regs[(STAR - SB_BASE) / 4];
	starReg = data & 0x1FFFFFE0;

	if (!check_STAR(data))
	{
		INFO_LOG(AICA, "%s Write_SB_STAR: Invalid address %08x", LogTag, data);
		asic_RaiseInterrupt(iainterrupt);
	}
}

static void Write_SB_G2APRO(u32 addr, u32 data)
{
	if ((data >> 16) == 0x4659)
		SB_G2APRO = data & 0x00007f7f;
}

void sbInit()
{
	// G2-DMA registers

	// AICA
	hollyRegs.setWriteHandler<SB_ADST_addr>(Write_SB_ADST);
	hollyRegs.setHandlers<SB_ADSUSP_addr>(Read_SUSP<SB_ADSUSP_addr>, Write_SB_ADSUSP);
	hollyRegs.setWriteHandler<SB_ADEN_addr>(Write_SB_ADEN);
	hollyRegs.setWriteHandler<SB_ADSTAR_addr>(Write_SB_STAR<SB_ADSTAR_addr, holly_AICA_ILLADDR, AICA_TAG>);
	hollyRegs.setWriteHandler<SB_ADSTAG_addr>(Write_SB_STAG<SB_ADSTAG_addr, holly_AICA_ILLADDR, AICA_TAG, 0>);

	// G2 Ext device #1
	hollyRegs.setWriteHandler<SB_E1ST_addr>(Write_DmaStart<SB_E1EN_addr, SB_E1ST_addr, SB_E1STAR_addr, SB_E1STAG_addr, SB_E1LEN_addr,
			SB_E1DIR_addr, holly_EXT_DMA1, holly_EXT1_ILLADDR, holly_EXT1_OVERRUN, EXT1_TAG>);
	hollyRegs.setHandlers<SB_E1SUSP_addr>(Read_SUSP<SB_E1SUSP_addr>, Write_ExtSUSP<0>);
	hollyRegs.setWriteHandler<SB_E1EN_addr>(Write_ExtEN<0, holly_EXT1_ILLADDR>);
	hollyRegs.setWriteHandler<SB_E1STAR_addr>(Write_SB_STAR<SB_E1STAR_addr, holly_EXT1_ILLADDR, EXT1_TAG>);
	hollyRegs.setWriteHandler<SB_E1STAG_addr>(Write_SB_STAG<SB_E1STAG_addr, holly_EXT1_ILLADDR, EXT1_TAG, 1>);

	// G2 Ext device #2
	hollyRegs.setWriteHandler<SB_E2ST_addr>(Write_DmaStart<SB_E2EN_addr, SB_E2ST_addr, SB_E2STAR_addr, SB_E2STAG_addr, SB_E2LEN_addr,
			SB_E2DIR_addr, holly_EXT_DMA2, holly_EXT2_ILLADDR, holly_EXT2_OVERRUN, EXT2_TAG>);
	hollyRegs.setHandlers<SB_E2SUSP_addr>(Read_SUSP<SB_E2SUSP_addr>, Write_ExtSUSP<1>);
	hollyRegs.setWriteHandler<SB_E2EN_addr>(Write_ExtEN<1, holly_EXT2_ILLADDR>);
	hollyRegs.setWriteHandler<SB_E2STAR_addr>(Write_SB_STAR<SB_E2STAR_addr, holly_EXT2_ILLADDR, EXT2_TAG>);
	hollyRegs.setWriteHandler<SB_E2STAG_addr>(Write_SB_STAG<SB_E2STAG_addr, holly_EXT2_ILLADDR, EXT2_TAG, 2>);

	// G2 Ext device #3
	hollyRegs.setWriteHandler<SB_DDST_addr>(Write_DmaStart<SB_DDEN_addr, SB_DDST_addr, SB_DDSTAR_addr, SB_DDSTAG_addr, SB_DDLEN_addr,
			SB_DDDIR_addr, holly_DEV_DMA, holly_DEV_ILLADDR, holly_DEV_OVERRUN, DDEV_TAG>);
	hollyRegs.setHandlers<SB_DDSUSP_addr>(Read_SUSP<SB_DDSUSP_addr>, Write_ExtSUSP<2>);
	hollyRegs.setWriteHandler<SB_DDEN_addr>(Write_ExtEN<2, holly_DEV_ILLADDR>);
	hollyRegs.setWriteHandler<SB_DDSTAR_addr>(Write_SB_STAR<SB_DDSTAR_addr, holly_DEV_ILLADDR, DDEV_TAG>);
	hollyRegs.setWriteHandler<SB_DDSTAG_addr>(Write_SB_STAG<SB_DDSTAG_addr, holly_DEV_ILLADDR, DDEV_TAG, 3>);

	hollyRegs.setWriteOnly<SB_G2APRO_addr>(Write_SB_G2APRO);

	dma_sched_id = sh4_sched_register(0, &dma_end_sched);
}

void sbReset(bool hard)
{
	clearAicaDmaState();
	aicaDmaConfig = {};
	for (G2DmaConfig& config : extDmaConfig)
		config = {};
	for (ExtDmaState& dma : extDma)
		dma = {};
	g2BusOwner = -1;
	if (dma_sched_id != -1)
		sh4_sched_request(dma_sched_id, -1);
	SB_ADEN = SB_E1EN = SB_E2EN = SB_DDEN = 0;
	SB_ADST = SB_E1ST = SB_E2ST = SB_DDST = 0;
	SB_ADSUSP = SB_E1SUSP = SB_E2SUSP = SB_DDSUSP = 0;
	SB_G2APRO = 0x7f00;
}

void sbTerm()
{
	sh4_sched_unregister(dma_sched_id);
	dma_sched_id = -1;
}

void serialize(Serializer& ser)
{
	ser << arm::aica_interr;
	ser << arm::aica_reg_L;
	ser << arm::e68k_out;
	ser << arm::e68k_reg_L;
	ser << arm::e68k_reg_M;

	ser.serialize(arm::arm_Reg, arm::RN_ARM_REG_COUNT - 1);	// Too lazy to create a new version and the scratch register is not used between blocks anyway
	ser << arm::armIrqEnable;
	ser << arm::armFiqEnable;
	ser << arm::armMode;
	ser << arm::Arm7Enabled;
	ser << arm::arm7ClockTicks;

	dsp::state.serialize(ser);

	for (const auto& timer : timers)
	{
		ser << timer.c_step;
		ser << timer.m_step;
	}

	if (!ser.rollback())
		aica_ram.serialize(ser);
	ser << VREG;
	ser << ARMRST;
	ser << rtc_EN;
	ser << RealTimeClock;
	ser << sampleClockPhase();
	serializeMidiOutput(ser);

	ser << aica_reg;
	serializeInternalDma(ser);

	sgc::serialize(ser);
	ser << aicaDmaActive << aicaDmaToSystem << aicaDmaRootAddress
			<< aicaDmaG2Address << aicaDmaRemaining << aicaDmaControl;
	ser << g2BusOwner;
	for (const ExtDmaState& dma : extDma)
		ser << dma.active << dma.toSystem << dma.rootAddress << dma.g2Address
				<< dma.remaining << dma.control;
	ser << aicaDmaConfig.rootAddress << aicaDmaConfig.g2Address
			<< aicaDmaConfig.length << aicaDmaConfig.direction << aicaDmaConfig.trigger;
	for (const G2DmaConfig& config : extDmaConfig)
		ser << config.rootAddress << config.g2Address << config.length
				<< config.direction << config.trigger;
}

void deserialize(Deserializer& deser)
{
	deser >> arm::aica_interr;
	deser >> arm::aica_reg_L;
	deser >> arm::e68k_out;
	deser >> arm::e68k_reg_L;
	deser >> arm::e68k_reg_M;

	deser.deserialize(arm::arm_Reg, arm::RN_ARM_REG_COUNT - 1);
	deser >> arm::armIrqEnable;
	deser >> arm::armFiqEnable;
	deser >> arm::armMode;
	deser >> arm::Arm7Enabled;
	if (deser.version() >= Deserializer::V19)
		deser >> arm::arm7ClockTicks;
	else
		arm::arm7ClockTicks = 0;

	dsp::state.deserialize(deser);

	for (int i = 0 ; i < 3 ; i++)
	{
		deser >> timers[i].c_step;
		deser >> timers[i].m_step;
	}

	if (!deser.rollback())
	{
		aica_ram.deserialize(deser);
		if (settings.platform.isAtomiswave())
			deser.skip(6_MB, Deserializer::V30);
	}
	deser >> VREG;
	deser >> ARMRST;
	VREG &= 3;
	ARMRST &= 1;
	deser >> rtc_EN;
	deser >> RealTimeClock;
	if (deser.version() >= Deserializer::V61)
	{
		u32 phase;
		deser >> phase;
		setSampleClockPhase(phase);
	}
	else
		setSampleClockPhase(0);
	deserializeMidiOutput(deser);

	deser >> aica_reg;
	// VER is fixed by the AICA silicon and is not save-state state.
	CommonData->VER = 1;
	// MOEMP/MOFUL are derived from FIFO contents. MIDI_OUT is edge-triggered,
	// however, so retain the serialized pending bit (including SCIRE ack).
	refreshMidiOutputStatus();
	deserializeInternalDma(deser);

	sgc::deserialize(deser);
	if (deser.version() >= Deserializer::V87) {
		deser >> aicaDmaActive >> aicaDmaToSystem >> aicaDmaRootAddress
				>> aicaDmaG2Address >> aicaDmaRemaining >> aicaDmaControl;
	}
	else {
		clearAicaDmaState();
	}
	if (deser.version() >= Deserializer::V91) {
		deser >> g2BusOwner;
		for (ExtDmaState& dma : extDma)
			deser >> dma.active >> dma.toSystem >> dma.rootAddress >> dma.g2Address
					>> dma.remaining >> dma.control;
	}
	else {
		g2BusOwner = aicaDmaActive ? 0 : -1;
		for (ExtDmaState& dma : extDma)
			dma = {};
	}
	if (deser.version() >= Deserializer::V112) {
		deser >> aicaDmaConfig.rootAddress >> aicaDmaConfig.g2Address
				>> aicaDmaConfig.length >> aicaDmaConfig.direction >> aicaDmaConfig.trigger;
		for (G2DmaConfig& config : extDmaConfig)
			deser >> config.rootAddress >> config.g2Address >> config.length
					>> config.direction >> config.trigger;
	}
	else {
		latchAicaDmaConfig();
		for (int i = 0; i < 3; ++i)
			latchExtDmaConfig(i);
	}
}

} // namespace aica
