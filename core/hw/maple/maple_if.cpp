#include "maple_if.h"
#include "maple_cfg.h"
#include "maple_helper.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_cycles.h"
#include "hw/sh4/modules/modules.h"
#include "network/ggpo.h"
#include "hw/naomi/card_reader.h"

#include <memory>

enum MaplePattern
{
	MP_Start,
	MP_SDCKBOccupy = 2,
	MP_Reset,
	MP_SDCKBOccupyCancel,
	MP_NOP = 7
};

std::shared_ptr<maple_device> MapleDevices[MAPLE_PORTS][6];

int maple_schid = -1;
bool maple_dma_pending_start;

/*
	Maple host controller
	Direct processing, async interrupt handling
	Device code is on maple_devs.cpp/h, config&management is on maple_cfg.cpp/h

	This code is missing many of the hardware details, like proper trigger handling,
	DMA continuation on suspect, etc ...
*/

static void maple_DoDma();
static void maple_handle_reconnect();
static int maple_schd(int tag, int cycles, int jitter, void *arg);

static u32 mapleRequestReadCycles(u32 address, u32 size)
{
	Sh4Cycles busCycles;
	u32 cycles = 0;
	for (u32 offset = 0; offset < size; offset += 32)
		cycles += busCycles.dmaReadAccessCycles(address + offset,
				std::min(32u, size - offset));
	return cycles;
}

static u32 mapleResponseWriteBurstCycles(u32 address, u32 size)
{
	Sh4Cycles busCycles;
	return busCycles.dmaWriteAccessCycles(address, size);
}

//really hackish
//misses delay , and stop/start implementation
//ddt/etc are just hacked for wince to work
//now with proper maple delayed DMA maybe its time to look into it ?
bool maple_ddt_pending_reset;
// pending DMA xfers
std::vector<std::pair<u32, std::vector<u32>>> mapleDmaOut;
std::vector<u32> mapleDmaDelays;
std::vector<MapleDmaRequest> mapleDmaRequests;
u32 mapleDmaIndex;
u32 mapleDmaTailCycles;
bool mapleDmaResponsePending;
u32 mapleDmaResponseOffset;
bool mapleDmaResponseBurstPending;
bool mapleDmaResponseLegacyAtomic;
u32 mapleDmaJitterCycles;
bool SDCKBOccupied;

static void scheduleMapleDmaStage(u32 cycles)
{
	cycles = std::max(1u, cycles);
	if (mapleDmaJitterCycles >= cycles) {
		mapleDmaJitterCycles -= cycles;
		cycles = 0;
	}
	else {
		cycles -= mapleDmaJitterCycles;
		mapleDmaJitterCycles = 0;
	}
	sh4_sched_request(maple_schid, cycles);
}

static void clearMapleDmaState()
{
	maple_dma_pending_start = false;
	if (maple_schid >= 0)
		sh4_sched_request(maple_schid, -1);
	mapleDmaOut.clear();
	mapleDmaDelays.clear();
	mapleDmaRequests.clear();
	mapleDmaIndex = 0;
	mapleDmaTailCycles = 0;
	mapleDmaResponsePending = false;
	mapleDmaResponseOffset = 0;
	mapleDmaResponseBurstPending = false;
	mapleDmaResponseLegacyAtomic = false;
	mapleDmaJitterCycles = 0;
	SB_MST &= 0x7fffffffu;
}

static void abortMapleDma(HollyInterruptID interrupt)
{
	bsc.endSdramDmaPipeline();
	clearMapleDmaState();
	SB_MDST = 0;
	asic_RaiseInterrupt(interrupt);
}

void maple_vblank()
{
	// An SDCKB occupation lasts through the scan interval and completes at
	// VBlank regardless of whether the command list used software or hardware
	// triggering. Complete it before accepting this VBlank's next trigger.
	if (SDCKBOccupied)
	{
		SDCKBOccupied = false;
		// The occupy command itself has already completed. Resume the command
		// stream at the next descriptor's wire deadline instead of executing it
		// synchronously at the VBlank edge.
		if (mapleDmaIndex < mapleDmaRequests.size())
			sh4_sched_request(maple_schid,
					std::max(1u, mapleDmaRequests[mapleDmaIndex].hostCycles));
		else if (mapleDmaTailCycles != 0) {
			const u32 tail = mapleDmaTailCycles;
			mapleDmaTailCycles = 0;
			sh4_sched_request(maple_schid, tail);
		}
		else
			maple_schd(0, 0, 0, nullptr);
	}

	if (SB_MDEN & 1)
	{
		if (SB_MDTSEL == 1)
		{
			// Hardware trigger on vblank
			if (maple_ddt_pending_reset) {
				DEBUG_LOG(MAPLE, "DDT vblank ; reset pending");
			}
			else if (SB_MDST == 0)
			{
				SB_MDST = 1;
				mapleDmaJitterCycles = 0;
				// if trigger reset is manual, mark it as pending
				if ((SB_MSYS >> 12) & 1)
					maple_ddt_pending_reset = true;

				// MSYS bits 3:0 delay a hardware trigger by 1.3 ms per
				// step. Values above 11 are reserved by the host controller.
				const u32 delay = std::min(SB_MSYS & 0xf, 11u);
				if (delay == 0)
					maple_DoDma();
				else
				{
					maple_dma_pending_start = true;
					sh4_sched_request(maple_schid,
							delay * (SH4_MAIN_CLOCK / 10'000 * 13));
				}
			}
		}
		else
			maple_ddt_pending_reset = false;
	}
	if (settings.platform.isConsole())
		maple_handle_reconnect();
}

static void maple_SB_MSHTCL_Write(u32 addr, u32 data)
{
	if ((data & 1) && SB_MDTSEL == 1 && (SB_MSYS & (1u << 12)))
		maple_ddt_pending_reset = false;
}

static void maple_SB_MDST_Write(u32 addr, u32 data)
{
	// A software start is accepted only in software-trigger mode and on the
	// idle-to-busy transition. Writes while a transfer is in progress do not
	// restart the command list.
	if ((data & 1) && (SB_MDEN & 1) && SB_MDTSEL == 0 && SB_MDST == 0)
	{
		SB_MDST = 1;
		mapleDmaJitterCycles = 0;
		maple_DoDma();
	}
}

static void maple_SB_MDEN_Write(u32 addr, u32 data)
{
	SB_MDEN = data & 1;

	if ((data & 1) == 0 && SB_MDST)
	{
		INFO_LOG(MAPLE, "Maple DMA abort");
		clearMapleDmaState();
		SDCKBOccupied = false;
		SB_MDST = 0;
	}
}

static bool check_mdapro(u64 addr)
{
	const u32 bottom = ((((SB_MDAPRO >> 8) & 0x7f) << 20) | 0x08000000);
	const u32 top = (((SB_MDAPRO & 0x7f) << 20) | 0x080fffff);

	if (addr > 0xffffffff || addr < 0x0c000000 || addr > 0x0fffffff
			|| addr < bottom || addr > top)
	{
		INFO_LOG(MAPLE, "MAPLE ERROR : Invalid address: %08llx. SB_MDAPRO: %x %x",
				(unsigned long long)addr, (SB_MDAPRO >> 8) & 0x7f, SB_MDAPRO & 0x7f);
		return false;
	}
	return true;
}

static void maple_SB_MDSTAR_Write(u32 addr, u32 data)
{
	SB_MDSTAR = data & 0x1fffffe0;
	if (!check_mdapro(data))
		asic_RaiseInterrupt(holly_MAPLE_ILLADDR);
}

static std::shared_ptr<maple_device> resolveAddressedDevice(u32 bus, u32 recipient,
		u32& port, bool& collision)
{
	port = 6;
	collision = false;
	if (bus >= MAPLE_PORTS)
		return nullptr;
	auto mainDevice = MapleDevices[bus][5];
	if (mainDevice != nullptr && mainDevice->get_device_type() == MDT_DreamParaParaController)
	{
		// This proprietary controller does not use standard Maple addressing.
		port = 5;
		return mainDevice;
	}
	if (((recipient >> 6) & 3) != bus || (recipient & 0x3f) == 0)
		return nullptr;
	// DE selects the main device. LM bits identify attached subunits only when
	// DE is clear; a main-device AP may also carry an attached-device mask.
	if ((recipient & 0x20) != 0) {
		port = 5;
		return MapleDevices[bus][port];
	}

	// LM is a multicast mask. Empty selected slots remain silent, one attached
	// device can answer normally, and two or more answers collide on the shared
	// bus rather than silently selecting the lowest numbered unit.
	std::shared_ptr<maple_device> selected;
	for (u32 candidate = 0; candidate < 5; candidate++) {
		if ((recipient & (1u << candidate)) == 0 || MapleDevices[bus][candidate] == nullptr)
			continue;
		if (selected != nullptr) {
			collision = true;
			port = 6;
			return nullptr;
		}
		selected = MapleDevices[bus][candidate];
		port = candidate;
	}
	return selected;
}

std::shared_ptr<maple_device> maple_GetAddressedDevice(u32 bus, u32 recipient, u32& port)
{
	bool collision;
	return resolveAddressedDevice(bus, recipient, port, collision);
}

static constexpr u32 mapleHostBitRate(u32 msys)
{
	// MSYS bits 9:8 select 2, 1, 4 or 8 Mbit/s respectively.
	// Most software leaves the reset value (2 Mbit/s) selected, but the
	// slower and high-speed modes are observable through DMA completion time.
	switch ((msys >> 8) & 3)
	{
	case 1: return 1'000'000;
	case 2: return 4'000'000;
	case 3: return 8'000'000;
	default: return 2'000'000;
	}
}

static constexpr u64 mapleResponseTimeoutCycles(u32 msys)
{
	return (u64)(msys >> 16) * 20 * SH4_MAIN_CLOCK / 1'000'000'000;
}

void maple_ResetBusDevices(u32 bus)
{
	if (bus >= MAPLE_PORTS)
		return;
	std::shared_ptr<maple_device> resetDevices[6];
	u32 resetDeviceCount = 0;
	for (u32 port = 0; port < 6; port++)
	{
		auto device = MapleDevices[bus][port];
		if (device == nullptr)
			continue;
		bool alreadyReset = false;
		for (u32 i = 0; i < resetDeviceCount; i++)
			alreadyReset |= resetDevices[i].get() == device.get();
		if (alreadyReset)
			continue;

		resetDevices[resetDeviceCount++] = device;
		const u32 recipient = (bus << 6) | (port == 5 ? 0x20 : 1u << port);
		const u32 request = MDC_DeviceReset | (recipient << 8) | ((bus << 6) << 16);
		u32 ignoredResponse[256] {};
		device->RawDma(&request, sizeof(request), ignoredResponse);
	}
}

static_assert(mapleHostBitRate(0x000) == 2'000'000);
static_assert(mapleHostBitRate(0x100) == 1'000'000);
static_assert(mapleHostBitRate(0x200) == 4'000'000);
static_assert(mapleHostBitRate(0x300) == 8'000'000);
static_assert(mapleResponseTimeoutCycles(0x3a980000) == 60'000);

static void maple_DoDma()
{
	verify(SB_MDEN & 1);
	verify(SB_MDST & 1);

	DEBUG_LOG(MAPLE, "Maple: DoMapleDma SB_MDSTAR=%x", SB_MDSTAR);
	SB_MST |= 0x80000000;
	SB_MTXDAD = SB_MDSTAR;
	SB_MRXDAD = 0;
	SB_MRXDBD = 0;
	u32 addr = SB_MDSTAR;
	if (!check_mdapro(addr))
	{
		abortMapleDma(holly_MAPLE_ILLADDR);
		return;
	}

	ggpo::getInput(mapleInputState);
	// TODO put this elsewhere and let the card readers handle being called multiple times
	if (settings.platform.isNaomi())
	{
		static u32 last_kcode[std::size(mapleInputState)];
		for (size_t i = 0; i < std::size(mapleInputState); i++)
		{
			if ((mapleInputState[i].kcode & DC_BTN_INSERT_CARD) == 0
					&& (last_kcode[i] & DC_BTN_INSERT_CARD) != 0)
				card_reader::insertCard(i);
			last_kcode[i] = mapleInputState[i].kcode;
		}
	}

	const bool swap_msb = (SB_MMSEL == 0);
	mapleDmaOut.clear();
	mapleDmaDelays.clear();
	mapleDmaRequests.clear();
	mapleDmaIndex = 0;
	mapleDmaTailCycles = 0;
	mapleDmaResponsePending = false;
	mapleDmaResponseOffset = 0;
	mapleDmaResponseBurstPending = false;
	mapleDmaResponseLegacyAtomic = false;
	u32 pendingCycles = 0;
	bool last = false;
	while (!last)
	{
		// Every descriptor fetch is subject to MDAPRO, including the one-word
		// RESET, light-gun and NOP patterns. Checking only MDSTAR would allow a
		// chain of those descriptors to walk out of the protected DMA window.
		if (!check_mdapro(addr))
		{
			abortMapleDma(holly_MAPLE_ILLADDR);
			return;
		}
		const u32 *header = (const u32 *)GetMemPtr(addr, sizeof(u32));
		if (header == nullptr)
		{
			WARN_LOG(MAPLE, "MAPLE ERROR: descriptor address is not in system RAM: %x", addr);
			abortMapleDma(holly_MAPLE_ILLADDR);
			return;
		}
		u32 header_1 = *header;

		last = (header_1 >> 31) == 1;				// is last transfer ?
		u32 plen = (header_1 & 0xFF) + 1;			// transfer length (32-bit unit)
		const u32 maple_op = (header_1 >> 8) & 7;	// Pattern selection: 0 - START, 2 - SDCKB occupy permission, 3 - RESET, 4 - SDCKB occupy cancel, 7 - NOP
		const u32 bus = (header_1 >> 16) & 3;		// maple bus [0..3]
		const u32 descriptorBytes = maple_op == MP_Start
				? (2 + plen) * sizeof(u32) : sizeof(u32);
		pendingCycles += mapleRequestReadCycles(addr, descriptorBytes);

		//this is kinda wrong .. but meh
		//really need to properly process the commands at some point
		switch (maple_op)
		{
		case MP_Start:
		{
			const u32 *destination = (const u32 *)GetMemPtr(addr + sizeof(u32), sizeof(u32));
			if (destination == nullptr)
			{
				WARN_LOG(MAPLE, "MAPLE ERROR: truncated transfer descriptor at %x", addr);
				abortMapleDma(holly_MAPLE_ILLADDR);
				return;
			}
			const u32 rawDestination = *destination;
			u32 header_2 = rawDestination & 0x1FFFFFE0;
			if (!check_mdapro(rawDestination)
					|| !check_mdapro((u64)addr + (2 + plen) * sizeof(u32) - 1))
			{
				abortMapleDma(holly_MAPLE_ILLADDR);
				return;
			}

			u32* p_data = (u32 *)GetMemPtr(addr + 8, plen * sizeof(u32));
			if (p_data == nullptr)
			{
				WARN_LOG(MAPLE, "MAPLE ERROR : INVALID SB_MDSTAR value 0x%X", addr);
				abortMapleDma(holly_MAPLE_ILLADDR);
				return;
			}
			const u32 frame_header = swap_msb ? SWAP32(p_data[0]) : p_data[0];

			//Recipient address
			const u32 reci = (frame_header >> 8) & 0xFF;//0-5;
			//Sender address
			//u32 send = (frame_header >> 16) & 0xFF;
			//Number of additional words in frame
			//u32 inlen = (frame_header >> 24) & 0xFF;

			u32 port;
			// The host frame is transmitted whether or not a device answers it.
			pendingCycles += sh4CyclesForXfer(plen * sizeof(u32) + 3,
					mapleHostBitRate(SB_MSYS) / 8);
			maple_GetAddressedDevice(bus, reci, port);

			MapleDmaRequest request{ MP_Start, header_2, (u8)bus, (u8)port, swap_msb,
					std::min(pendingCycles, (u32)SH4_MAIN_CLOCK),
					std::vector<u32>(p_data, p_data + plen) };
			request.source = addr + 8;
			if (swap_msb) {
				request.data[0] = frame_header;
				for (u32 i = 1; i < plen; i++)
					request.data[i] = SWAP32(request.data[i]);
			}
			mapleDmaRequests.emplace_back(std::move(request));
			mapleDmaOut.emplace_back(header_2, std::vector<u32>());
			mapleDmaDelays.push_back(0);
			pendingCycles = 0;

			//goto next command
			addr += (2 + plen) * sizeof(u32);
		}
		break;

		case MP_SDCKBOccupy:
		{
			pendingCycles += sh4CyclesForXfer(1, mapleHostBitRate(SB_MSYS) / 8);
			mapleDmaRequests.push_back({ MP_SDCKBOccupy, 0, (u8)bus, 5, false,
					std::min(pendingCycles, (u32)SH4_MAIN_CLOCK), {} });
			mapleDmaOut.emplace_back(0, std::vector<u32>());
			mapleDmaDelays.push_back(0);
			pendingCycles = 0;
			addr += 1 * sizeof(u32);
		}
		break;

		case MP_SDCKBOccupyCancel:
			mapleDmaRequests.push_back({ MP_SDCKBOccupyCancel, 0, (u8)bus, 5, false,
					std::min(pendingCycles, (u32)SH4_MAIN_CLOCK), {} });
			mapleDmaOut.emplace_back(0, std::vector<u32>());
			mapleDmaDelays.push_back(0);
			pendingCycles = 0;
			addr += 1 * sizeof(u32);
			break;

		case MP_Reset:
			pendingCycles += sh4CyclesForXfer(1, mapleHostBitRate(SB_MSYS) / 8);
			mapleDmaRequests.push_back({ MP_Reset, 0, (u8)bus, 5, false,
					std::min(pendingCycles, (u32)SH4_MAIN_CLOCK), {} });
			mapleDmaOut.emplace_back(0, std::vector<u32>());
			mapleDmaDelays.push_back(0);
			pendingCycles = 0;
			addr += 1 * sizeof(u32);
			break;

		case MP_NOP:
			addr += 1 * sizeof(u32);
			break;

		default:
			INFO_LOG(MAPLE, "MAPLE: Unknown maple_op == %d length %d", maple_op, plen * 4);
			abortMapleDma(holly_MAPLE_ILLCMD);
			return;
		}
	}
	bsc.endSdramDmaPipeline();

	// Maple bus max speed: 2 Mb/s, actual speed: 1 Mb/s
	// actual measured speed with protocol analyzer for devices (vmu?) is 724-738Kb/s
	// See https://github.com/OrangeFox86/DreamcastControllerUsbPico/blob/main/measurements/Dreamcast-Power-Up-Digital-and-Analog-Player1-Controller-VMU-JumpPack.sal
	if (!SDCKBOccupied)
	{
		mapleDmaTailCycles = std::min(pendingCycles, (u32)SH4_MAIN_CLOCK);
		const u32 cycles = mapleDmaRequests.empty() ? mapleDmaTailCycles
				: mapleDmaRequests[0].hostCycles;
		scheduleMapleDmaStage(cycles);
	}
}

static int maple_schd(int tag, int cycles, int jitter, void *arg)
{
	mapleDmaJitterCycles += std::max(0, jitter);
	if (maple_dma_pending_start)
	{
		maple_dma_pending_start = false;
		if ((SB_MDEN & 1) && SB_MDTSEL == 1 && (SB_MDST & 1))
			maple_DoDma();
		else
			SB_MDST = 0;
		return 0;
	}

	if (SB_MDEN & 1)
	{
		if (mapleDmaIndex < mapleDmaRequests.size() && !mapleDmaResponsePending)
			{
				const MapleDmaRequest& request = mapleDmaRequests[mapleDmaIndex];
				if (request.pattern == MP_Start) {
					SB_MTXDAD = request.source + request.data.size() * sizeof(u32);
					SB_MRXDBD = request.destination;
					SB_MRXDAD = request.destination;
				}
				if (request.pattern != MP_Start)
				{
					if (request.pattern == MP_SDCKBOccupy) {
						auto device = MapleDevices[request.bus][5];
						if (device != nullptr)
							SDCKBOccupied = device->get_lightgun_pos();
					}
					else if (request.pattern == MP_SDCKBOccupyCancel)
						SDCKBOccupied = false;
					else if (request.pattern == MP_Reset)
					{
						// The RESET pattern is a line-level bus operation rather than
						// a Maple command frame. Every device on the selected bus sees
						// it, and no response is written to system memory.
						SDCKBOccupied = false;
						maple_ResetBusDevices(request.bus);
					}

					mapleDmaIndex++;
					if (SDCKBOccupied) {
						// VBlank is the next timing origin; dispatch lateness before
						// occupation must not shorten the scan interval itself.
						mapleDmaJitterCycles = 0;
						return 0;
					}
					if (mapleDmaIndex < mapleDmaRequests.size()) {
						scheduleMapleDmaStage(mapleDmaRequests[mapleDmaIndex].hostCycles);
						return 0;
					}
					if (mapleDmaTailCycles != 0) {
						const u32 tail = mapleDmaTailCycles;
						mapleDmaTailCycles = 0;
						scheduleMapleDmaStage(tail);
						return 0;
					}
				}
				else
				{
					// The command table is DMA memory, not an eager software queue.
					// Re-read the frame when its turn reaches the wire so writes made
					// after MDST (and before this transfer) are observable. States made
					// before the source counter was serialized retain their saved copy.
					const std::vector<u32> *frameData = &request.data;
					std::vector<u32> liveData;
					if (request.source != 0) {
						const size_t frameBytes = request.data.size() * sizeof(u32);
						if (!check_mdapro(request.source)
								|| !check_mdapro((u64)request.source + frameBytes - 1)) {
							abortMapleDma(holly_MAPLE_ILLADDR);
							return 0;
						}
						const u32 *source = (const u32 *)GetMemPtr(request.source, frameBytes);
						if (source == nullptr) {
							abortMapleDma(holly_MAPLE_ILLADDR);
							return 0;
						}
						liveData.assign(source, source + request.data.size());
						if (request.swapWords)
							for (u32& word : liveData)
								word = SWAP32(word);
						frameData = &liveData;
					}
					bool responseCollision = false;
					u32 currentPort;
					const u32 recipient = frameData->empty() ? 0 : ((*frameData)[0] >> 8) & 0xff;
					auto device = resolveAddressedDevice(request.bus, recipient,
							currentPort, responseCollision);
					const bool validFrameLength = !frameData->empty()
							&& frameData->size() == (((*frameData)[0] >> 24) + 1);
				u32 responseCycles;
				if (responseCollision && validFrameLength) {
					// Multiple peripherals answered a multicast request. The Maple
					// receiver reports a wire-level reception error, not no-device.
					mapleDmaOut[mapleDmaIndex].second.assign(1, 0xffffff00);
					responseCycles = (u32)std::min<u64>(mapleResponseTimeoutCycles(SB_MSYS),
							SH4_MAIN_CLOCK);
				}
				else if (device != nullptr && validFrameLength) {
					u32 outbuf[1024 / sizeof(u32)];
					const u32 outlen = device->RawDma(frameData->data(),
							frameData->size() * sizeof(u32), outbuf);
				if (outlen == 0) {
					// A connected device can decline a malformed or unsupported
					// wire-level frame. The host observes the same timeout marker
					// as it does for an empty port.
					mapleDmaOut[mapleDmaIndex].second.assign(1, 0xffffffff);
					responseCycles = (u32)std::min<u64>(mapleResponseTimeoutCycles(SB_MSYS),
							SH4_MAIN_CLOCK);
				}
				else if (!check_mdapro((u64)request.destination + outlen - 1)) {
					abortMapleDma(holly_MAPLE_OVERRUN);
					return 0;
				}
				else {
					if (request.swapWords)
						for (u32 i = 0; i < outlen / 4; i++)
							outbuf[i] = SWAP32(outbuf[i]);
					mapleDmaOut[mapleDmaIndex].second.assign(outbuf, outbuf + outlen / 4);
					responseCycles = sh4CyclesForXfer(outlen + 3, 740'000 / 8);
				}
			}
				else {
					if (device != nullptr)
						WARN_LOG(MAPLE, "MAPLE: frame length mismatch: header %u words, DMA %zu words",
								((*frameData)[0] >> 24) + 1, frameData->size());
					mapleDmaOut[mapleDmaIndex].second.assign(1, 0xffffffff);
				responseCycles = (u32)std::min<u64>(mapleResponseTimeoutCycles(SB_MSYS),
						SH4_MAIN_CLOCK);
			}
			mapleDmaDelays[mapleDmaIndex] = std::min(responseCycles,
					(u32)SH4_MAIN_CLOCK);
			mapleDmaResponsePending = true;
			mapleDmaResponseOffset = 0;
			mapleDmaResponseBurstPending = false;
			mapleDmaResponseLegacyAtomic = false;
			scheduleMapleDmaStage(mapleDmaDelays[mapleDmaIndex]);
			return 0;
				}
		}
		if (mapleDmaIndex < mapleDmaOut.size())
		{
			const auto& pair = mapleDmaOut[mapleDmaIndex];
			const u32 size = pair.second.size() * sizeof(u32);
			if (!mapleDmaResponseLegacyAtomic && mapleDmaResponseOffset < size
					&& !mapleDmaResponseBurstPending) {
				const u32 burstSize = std::min(32u, size - mapleDmaResponseOffset);
				const u32 burstCycles = mapleResponseWriteBurstCycles(
						pair.first + mapleDmaResponseOffset, burstSize);
				mapleDmaResponseBurstPending = true;
				scheduleMapleDmaStage(burstCycles);
				return 0;
			}
			const u32 writeOffset = mapleDmaResponseLegacyAtomic ? 0 : mapleDmaResponseOffset;
			const u32 writeSize = mapleDmaResponseLegacyAtomic ? size
					: std::min(32u, size - writeOffset);
			u8 *p = pair.first == 0 ? nullptr
					: (u8 *)GetMemPtr(pair.first + writeOffset, writeSize);
			if (p == nullptr)
			{
				WARN_LOG(MAPLE, "MAPLE ERROR: response overruns system RAM at %x (%u bytes)", pair.first, size);
				abortMapleDma(holly_MAPLE_OVERRUN);
				return 0;
			}
			memcpy(p, (const u8 *)pair.second.data() + writeOffset, writeSize);
			mapleDmaResponseOffset = writeOffset + writeSize;
			SB_MRXDAD = pair.first + mapleDmaResponseOffset;
			mapleDmaResponseBurstPending = false;
			mapleDmaResponseLegacyAtomic = false;
			if (mapleDmaResponseOffset < size) {
				const u32 burstSize = std::min(32u, size - mapleDmaResponseOffset);
				const u32 burstCycles = mapleResponseWriteBurstCycles(
						pair.first + mapleDmaResponseOffset, burstSize);
				mapleDmaResponseBurstPending = true;
				scheduleMapleDmaStage(burstCycles);
				return 0;
			}
			bsc.endSdramDmaPipeline();
			mapleDmaIndex++;
			mapleDmaResponsePending = false;
			mapleDmaResponseOffset = 0;
			if (mapleDmaRequests.empty() && mapleDmaIndex < mapleDmaOut.size()) {
				mapleDmaResponsePending = true;
				scheduleMapleDmaStage(mapleDmaDelays[mapleDmaIndex]);
				return 0;
			}
			if (mapleDmaIndex < mapleDmaRequests.size()) {
				scheduleMapleDmaStage(mapleDmaRequests[mapleDmaIndex].hostCycles);
				return 0;
			}
			if (mapleDmaTailCycles != 0) {
				const u32 tail = mapleDmaTailCycles;
				mapleDmaTailCycles = 0;
				scheduleMapleDmaStage(tail);
				return 0;
			}
		}
		SB_MDST = 0;
		SB_MST &= 0x7fffffffu;
		asic_RaiseInterrupt(holly_MAPLE_DMA);
	}
	else
	{
		INFO_LOG(MAPLE, "WARNING: MAPLE DMA ABORT");
		SB_MDST = 0; //I really wonder what this means, can the DMA be continued ?
		SB_MST &= 0x7fffffffu;
	}
	bsc.endSdramDmaPipeline();
	mapleDmaOut.clear();
	mapleDmaDelays.clear();
	mapleDmaRequests.clear();
	mapleDmaIndex = 0;
	mapleDmaTailCycles = 0;
	mapleDmaResponsePending = false;
	mapleDmaResponseOffset = 0;
	mapleDmaResponseBurstPending = false;
	mapleDmaResponseLegacyAtomic = false;
	mapleDmaJitterCycles = 0;

	return 0;
}

static void maple_SB_MDAPRO_Write(u32 addr, u32 data)
{
	if ((data >> 16) == 0x6155)
		SB_MDAPRO = data & 0x00007f7f;
}

//Init registers :)
void maple_Init()
{
	hollyRegs.setWriteHandler<SB_MDST_addr>(maple_SB_MDST_Write);
	hollyRegs.setWriteHandler<SB_MDEN_addr>(maple_SB_MDEN_Write);
	hollyRegs.setWriteHandler<SB_MSHTCL_addr>(maple_SB_MSHTCL_Write);
	hollyRegs.setWriteOnly<SB_MDAPRO_addr>(maple_SB_MDAPRO_Write);
	hollyRegs.setWriteHandler<SB_MDSTAR_addr>(maple_SB_MDSTAR_Write);

	maple_schid = sh4_sched_register(0, maple_schd);
}

u64 maple_reconnect_time;

void maple_Reset(bool hard)
{
	maple_ddt_pending_reset = false;
	maple_dma_pending_start = false;
	SDCKBOccupied = false;
	if (maple_schid >= 0)
		sh4_sched_request(maple_schid, -1);
	SB_MDTSEL = 0;
	SB_MDEN   = 0;
	SB_MDST   = 0;
	SB_MST    = 0x000000ff;
	SB_MSYS   = 0x3A980000;
	SB_MSHTCL = 0;
	SB_MDAPRO = 0x00007F00;
	SB_MMSEL  = 1;
	mapleDmaOut.clear();
	mapleDmaDelays.clear();
	mapleDmaRequests.clear();
	mapleDmaIndex = 0;
	mapleDmaTailCycles = 0;
	mapleDmaResponsePending = false;
	mapleDmaResponseOffset = 0;
	mapleDmaResponseBurstPending = false;
	mapleDmaResponseLegacyAtomic = false;
	mapleDmaJitterCycles = 0;
	maple_reconnect_time = 0;
}

void maple_Term()
{
	mcfg_DestroyDevices();
	sh4_sched_unregister(maple_schid);
	maple_schid = -1;
}

void maple_ReconnectDevices()
{
	mcfg_DestroyDevices();
	maple_reconnect_time = sh4_sched_now64() + SH4_MAIN_CLOCK / 10;
}

void maple_ReconnectDevice(int bus, int port)
{
	if (port == 5)
	{
		// main device
		for (int i = 0; i <= 5; i++)
			MapleDevices[bus][i].reset();
	}
	else {
		// sub device
		MapleDevices[bus][port].reset();
	}
	maple_reconnect_time = sh4_sched_now64() + SH4_MAIN_CLOCK / 10;
}

static void maple_handle_reconnect()
{
	if (maple_reconnect_time != 0 && maple_reconnect_time <= sh4_sched_now64())
	{
		maple_reconnect_time = 0;
		mcfg_CreateDevices();
	}
}
