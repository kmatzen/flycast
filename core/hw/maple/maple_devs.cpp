#include "maple_devs.h"
#include "maple_cfg.h"
#include "maple_helper.h"
#include "maple_if.h"
#include "vmu_emulator.h"
#include "hw/pvr/spg.h"
#include "hw/sh4/sh4_sched.h"
#include "audio/audiostream.h"
#include "oslib/oslib.h"
#include "oslib/storage.h"
#include "oslib/i18n.h"
#include "hw/aica/sgc_if.h"
#include "cfg/option.h"
#include "input/maplelink.h"
#include <zlib.h>
#include <algorithm>
#include <cerrno>
#include <ctime>
#include <thread>
#include <chrono>
#include <memory>

const char* maple_sega_controller_name = "Dreamcast Controller";
const char* maple_sega_vmu_name        = "Visual Memory";
const char* maple_sega_kbd_name        = "Dreamcast Keyboard";
const char* maple_sega_mouse_name      = "Emulated Dreamcast Mouse";
const char* maple_sega_dreameye_name_1 = "Dreamcast Camera Flash  Device";
const char* maple_sega_dreameye_name_2 = "Dreamcast Camera Flash LDevice";
const char* maple_sega_mic_name        = "MicDevice for Dreameye";
const char* maple_sega_purupuru_name   = "Puru Puru Pack";
const char* maple_sega_lightgun_name   = "Dreamcast Gun";
const char* maple_sega_twinstick_name  = "Twin Stick";
const char* maple_ascii_stick_name     = "ASCII STICK";
const char* maple_maracas_controller_name   = "Maracas Controller";
const char* maple_fishing_controller_name   = "Dreamcast Fishing Controller";
const char* maple_popnmusic_controller_name = "pop'n music controller";
const char* maple_racing_controller_name    = "Racing Controller";
const char* maple_densha_controller_name    = "TAITO 001 Controller";

const char* maple_sega_brand = "Produced By or Under License From SEGA ENTERPRISES,LTD.";

//fill in the info
void maple_device::Setup(u32 bus, u32 port, int playerNum)
{
	maple_port = (bus << 6) | (1 << port);
	bus_port = port;
	bus_id = bus;
	logical_port[0] = 'A' + bus_id;
	logical_port[1] = bus_port == 5 ? 'x' : '1' + bus_port;
	logical_port[2] = 0;
	player_num = playerNum == -1 ? bus_id : playerNum;

	config = new MapleConfigMap(this);
	OnSetup();
	MapleDevices[bus][port] = shared_from_this();
}
maple_device::~maple_device()
{
    delete config;
}

bool maple_base::relayMapleLink()
{
	auto link = MapleLink::GetMapleLink(bus_id, bus_port);
	if (link == nullptr)
		return true;
	else
		return link->send(*inMsg);
}

static inline void mutualExclusion(u32& keycode, u32 mask)
{
	if ((keycode & mask) == 0)
		keycode |= mask;
}

// The six buttons visible through the standard VMU clock function are mapped
// to controls that a Dreamcast controller does not consume. Both protocols use
// active-low button bits. MODE and SLEEP are deliberately left released: those
// two buttons are not pollable over Maple on a first-party VMU.
static u8 getVmuButtons(u32 kcode)
{
	mutualExclusion(kcode, DC_DPAD2_UP | DC_DPAD2_DOWN);
	mutualExclusion(kcode, DC_DPAD2_LEFT | DC_DPAD2_RIGHT);
	u8 buttons = 0xff;
	if ((kcode & DC_DPAD2_UP) == 0)
		buttons &= ~0x01;
	if ((kcode & DC_DPAD2_DOWN) == 0)
		buttons &= ~0x02;
	if ((kcode & DC_DPAD2_LEFT) == 0)
		buttons &= ~0x04;
	if ((kcode & DC_DPAD2_RIGHT) == 0)
		buttons &= ~0x08;
	if ((kcode & DC_BTN_C) == 0)
		buttons &= ~0x10;
	if ((kcode & DC_BTN_D) == 0)
		buttons &= ~0x20;
	return buttons;
}

/*
	Sega Dreamcast Controller
	No error checking of any kind, but works just fine
*/
struct maple_sega_controller: maple_base
{
	virtual u32 get_capabilities() {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xfe060f00;	// 4 analog axes (0-3) X Y A B Start U D L R
	}

	virtual u16 getButtonState(const PlainJoystickState &pjs)
	{
		u32 kcode = pjs.kcode;
		mutualExclusion(kcode, DC_DPAD_UP | DC_DPAD_DOWN);
		mutualExclusion(kcode, DC_DPAD_LEFT | DC_DPAD_RIGHT);
		return kcode | 0xF901;		// mask off DPad2, C, D and Z;
	}

	virtual u32 getAnalogAxis(int index, const PlainJoystickState &pjs)
	{
		if (index == 2 || index == 3)
		{
			// Limit the magnitude of the analog axes to 128
			s8 xaxis = pjs.joy[PJAI_X1] - 128;
			s8 yaxis = pjs.joy[PJAI_Y1] - 128;
			limit_joystick_magnitude<128>(xaxis, yaxis);
			if (index == 2)
				return xaxis + 128;
			else
				return yaxis + 128;
		}
		else if (index == 0)
			return pjs.trigger[PJTI_R];		// Right trigger
		else if (index == 1)
			return pjs.trigger[PJTI_L];		// Left trigger
		else
			return 0x80;					// unused
	}

	MapleDeviceType get_device_type() override
	{
		return MDT_SegaController;
	}

	virtual const char *get_device_name()
	{
		return maple_sega_controller_name;
	}

	virtual const char *get_device_brand()
	{
		return maple_sega_brand;
	}

	virtual u32 get_device_current(int get_max_current)
	{
		return get_max_current ? 0x01F4 : 0x01AE; // Max. 50 mA, standby: 43 mA
	}

	u32 dma(u32 cmd) override
	{
		//printf("maple_sega_controller::dma Called 0x%X;Command %d\n", bus_id, cmd);
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			// Fixed Device Status
			// (Device ID)
			//caps
			//4
			w32(MFID_0_Input);

			//struct data
			//3*4
			w32(get_capabilities());
			w32(0);
			w32(0);

			//1	area code (Country specification)
			w8(0xFF);

			//1	direction (Connection method)
			w8(0);

			//30 (Model name)
			wstr(get_device_name(), 30);

			//60 (License)
			wstr(get_device_brand(), 60);

			//2 (Standby current consumption)
			w16(get_device_current(0));

			//2 (Maximum current consumption)
			w16(get_device_current(1));

			if (cmd == MDC_AllStatusReq)
			{
				const char *extra = "Version 1.010,1998/09/28,315-6211-AB   ,Analog Module : The 4th Edition.5/8  +DF";
				wptr(extra, strlen(extra));
				return MDRS_DeviceStatusAll;
			}
			else {
				return MDRS_DeviceStatus;
			}

			//controller condition
		case MDCF_GetCondition:
			{
				MapleDeviceRV validation = validateFunction(MFID_0_Input, 4);
				if (validation != MDRS_JVSNone)
					return validation;
				PlainJoystickState pjs;
				config->GetInput(&pjs);
				//caps
				//4
				w32(MFID_0_Input);

				//state data
				//2 key code
				w16(getButtonState(pjs));

				// analog axes
				for (int axis = 0; axis < 6; axis++)
					w8(getAnalogAxis(axis, pjs));
			}

			return MDRS_DataTransfer;

		case MDC_DeviceReset:
			return MDRS_DeviceReply;

		case MDC_DeviceKill:
			return MDRS_DeviceReply;

		default:
			INFO_LOG(MAPLE, "maple_sega_controller: Unknown maple command %d", cmd);
			return MDRE_UnknownCmd;
		}
	}
};

struct maple_atomiswave_controller: maple_sega_controller
{
	u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xff663f00;	// 6 analog axes, X Y L2/D2(?) A B C Start U D L R
	}

	u16 getButtonState(const PlainJoystickState &pjs) override
	{
		u32 kcode = pjs.kcode;
		mutualExclusion(kcode, AWAVE_UP_KEY | AWAVE_DOWN_KEY);
		mutualExclusion(kcode, AWAVE_LEFT_KEY | AWAVE_RIGHT_KEY);
		return kcode | AWAVE_TRIGGER_KEY;
	}

	u32 getAnalogAxis(int index, const PlainJoystickState &pjs) override {
		if (index < 2 || index > 5)
			return 0x80;
		index -= 2;
		return pjs.joy[index];
	}
};

/*
	Sega Twin Stick Controller
*/
struct maple_sega_twinstick: maple_sega_controller
{
	u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xfefe0000;	// no analog axes, X Y A B D Start U/D/L/R U2/D2/L2/R2
	}

	u16 getButtonState(const PlainJoystickState &pjs) override
	{
		u32 kcode = pjs.kcode;
		mutualExclusion(kcode, DC_DPAD_UP | DC_DPAD_DOWN);
		mutualExclusion(kcode, DC_DPAD_LEFT | DC_DPAD_RIGHT);
		mutualExclusion(kcode, DC_DPAD2_UP | DC_DPAD2_DOWN);
		mutualExclusion(kcode, DC_DPAD2_LEFT | DC_DPAD2_RIGHT);
		return kcode | 0x0101;
	}

	MapleDeviceType get_device_type() override {
		return MDT_TwinStick;
	}

	u32 getAnalogAxis(int index, const PlainJoystickState &pjs) override {
		return 0x80;
	}

	const char *get_device_name() override {
		return maple_sega_twinstick_name;
	}

	u32 get_device_current(int get_max_current) override {
		return get_max_current ? 0x012C : 0x00DC; // Max. 30 mA, standby: 22 mA
	}
};


/*
	Ascii Stick (Arcade/FT Stick)
*/
struct maple_ascii_stick: maple_sega_controller
{
	u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xff070000;	// no analog axes, X Y Z A B C Start U/D/L/R
	}

	u16 getButtonState(const PlainJoystickState &pjs) override
	{
		u32 kcode = pjs.kcode;
		mutualExclusion(kcode, DC_DPAD_UP | DC_DPAD_DOWN);
		mutualExclusion(kcode, DC_DPAD_LEFT | DC_DPAD_RIGHT);
		return kcode | 0xF800;
	}

	MapleDeviceType get_device_type() override {
		return MDT_AsciiStick;
	}

	u32 getAnalogAxis(int index, const PlainJoystickState &pjs) override {
		return 0x80;
	}

	const char *get_device_name() override {
		return maple_ascii_stick_name;
	}

	u32 get_device_current(int get_max_current) override {
		return get_max_current ? 0x0172 : 0x010E; // Max. 37 mA, standby: 27 mA
	}
};

/*
	Sega Dreamcast Visual Memory Unit
	This is pretty much done (?)
*/
u8 vmu_default[] = {
		0x78,0x9c,0xed,0xd2,0x31,0x4e,0x02,0x61,0x10,0x06,0xd0,0x8f,0x04,0x28,0x4c,0x2c,
		0x28,0x2d,0x0c,0xa5,0x57,0xe0,0x16,0x56,0x16,0x76,0x14,0x1e,0xc4,0x03,0x50,0x98,
		0x50,0x40,0x69,0xc1,0x51,0x28,0xbc,0x8e,0x8a,0x0a,0xeb,0xc2,0xcf,0x66,0x13,0x1a,
		0x13,0xa9,0x30,0x24,0xe6,0xbd,0xc9,0x57,0xcc,0x4c,0x33,0xc5,0x2c,0xb3,0x48,0x6e,
		0x67,0x01,0x00,0x00,0x00,0x00,0x00,0x4e,0xaf,0xdb,0xe4,0x7a,0xd2,0xcf,0x53,0x16,
		0x6d,0x46,0x99,0xb6,0xc9,0x78,0x9e,0x3c,0x5f,0x9c,0xfb,0x3c,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,
		0x00,0x00,0x00,0x80,0x5f,0xd5,0x45,0xfd,0xef,0xaa,0xca,0x6b,0xde,0xf2,0x9e,0x55,
		0x3e,0xf2,0x99,0xaf,0xac,0xb3,0x49,0x95,0xef,0xd4,0xa9,0x9a,0xdd,0xdd,0x0f,0x9d,
		0x52,0xca,0xc3,0x91,0x7f,0xb9,0x9a,0x0f,0x6e,0x92,0xfb,0xee,0xa1,0x2f,0x6d,0x76,
		0xe9,0x64,0x9b,0xcb,0xf4,0xf2,0x92,0x61,0x33,0x79,0xfc,0xeb,0xb7,0xe5,0x44,0xf6,
		0x77,0x19,0x06,0xef,
};

struct maple_sega_vmu: maple_base
{
	static constexpr u32 FileErrorInvalidPartition = 0x01000000;
	static constexpr u32 FileErrorPhase = 0x02000000;
	static constexpr u32 FileErrorInvalidBlock = 0x04000000;
	static constexpr u32 FileErrorWrite = 0x08000000;
	static constexpr u32 FileErrorInvalidLength = 0x10000000;

	FILE *file = nullptr;
	u8 flash_data[128_KB];
	u8 lcd_data[192];
	u8 lcd_data_decoded[48*32];
	bool fullSaveNeeded = false;
	s64 clockOffsetSeconds = 0;
	u8 pendingWrite[512] {};
	u16 pendingWriteBlock = 0xffff;
	u8 nextWritePhase = 0;
	std::array<u8, vmu::Lc86MemoryMap::RomSize> vmuBios {};
	std::unique_ptr<vmu::Emulator> vmuEmulator;
	u64 lastVmuUpdate = 0;
	u64 vmuTimeRemainder = 0;
	u8 firmwareBuzzerPeriod = 0;
	u8 firmwareBuzzerDuty = 0;

	void syncVmuEmulator()
	{
		if (vmuEmulator == nullptr)
			return;
		const u64 now = sh4_sched_now64();
		const u64 elapsed = std::min<u64>(now - lastVmuUpdate, SH4_MAIN_CLOCK);
		lastVmuUpdate = now;
		const u64 scaled = elapsed * 1'000'000'000ull + vmuTimeRemainder;
		const u64 nanoseconds = scaled / SH4_MAIN_CLOCK;
		vmuTimeRemainder = scaled % SH4_MAIN_CLOCK;

		PlainJoystickState input;
		config->GetInput(&input);
		vmuEmulator->setButtons(static_cast<u8>(~getVmuButtons(input.kcode)));
		vmuEmulator->runForNanoseconds(nanoseconds);
		if (vmuEmulator->memory().takeFlashDirty())
		{
			fullSaveNeeded = true;
			fullSave();
		}
		u8 buzzerPeriod;
		u8 buzzerDuty;
		vmuEmulator->buzzerOutput(buzzerPeriod, buzzerDuty);
		if (buzzerPeriod != firmwareBuzzerPeriod || buzzerDuty != firmwareBuzzerDuty)
		{
			firmwareBuzzerPeriod = buzzerPeriod;
			firmwareBuzzerDuty = buzzerDuty;
			aica::sgc::vmuBeep(bus_id, bus_port, buzzerPeriod, buzzerDuty);
		}

		u8 firmwareLcd[sizeof(lcd_data)];
		vmuEmulator->memory().copyLcdFramebuffer(firmwareLcd);
		if (memcmp(lcd_data, firmwareLcd, sizeof(lcd_data)) != 0)
		{
			memcpy(lcd_data, firmwareLcd, sizeof(lcd_data));
			decodeLcd();
			config->SetImage(lcd_data_decoded);
			updateMapleLinkScreen();
		}
	}

	void initializeVmuEmulator()
	{
		vmuEmulator.reset();
		const std::string path = hostfs::findFlash("",
				"vmu_bios.bin;vms_bios.bin;en1005-19991026-315-6208-05.bin");
		if (path.empty())
			return;
		std::unique_ptr<hostfs::File> biosFile(hostfs::storage().openFile(path, "rb"));
		if (biosFile == nullptr || biosFile->size() != static_cast<s64>(vmuBios.size())
				|| biosFile->read(vmuBios.data(), vmuBios.size(), 1) != 1)
		{
			WARN_LOG(MAPLE, "Invalid VMU BIOS file \"%s\"", path.c_str());
			return;
		}
		vmuEmulator = std::make_unique<vmu::Emulator>(vmuBios.data(), vmuBios.size(),
				flash_data, sizeof(flash_data));
		lastVmuUpdate = sh4_sched_now64();
		vmuTimeRemainder = 0;
		firmwareBuzzerPeriod = firmwareBuzzerDuty = 0;
		INFO_LOG(MAPLE, "VMU %s firmware emulation enabled", logical_port);
	}

	static constexpr bool isLeapYear(int year)
	{
		return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
	}

	static constexpr unsigned daysInMonth(int year, unsigned month)
	{
		constexpr unsigned days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
		return month == 2 && isLeapYear(year) ? 29 : days[month - 1];
	}

	// Gregorian civil-date conversion. The epoch is 1970-01-01, but unlike
	// mktime this representation has no host timezone or DST rules attached.
	static constexpr s64 daysFromCivil(int year, unsigned month, unsigned day)
	{
		year -= month <= 2;
		const int era = (year >= 0 ? year : year - 399) / 400;
		const unsigned yearOfEra = static_cast<unsigned>(year - era * 400);
		const unsigned adjustedMonth = static_cast<unsigned>(static_cast<int>(month) + (month > 2 ? -3 : 9));
		const unsigned dayOfYear = (153 * adjustedMonth + 2) / 5 + day - 1;
		const unsigned dayOfEra = yearOfEra * 365 + yearOfEra / 4 - yearOfEra / 100 + dayOfYear;
		return static_cast<s64>(era) * 146097 + dayOfEra - 719468;
	}

	static void civilFromDays(s64 days, int& year, unsigned& month, unsigned& day)
	{
		days += 719468;
		const s64 era = (days >= 0 ? days : days - 146096) / 146097;
		const unsigned dayOfEra = static_cast<unsigned>(days - era * 146097);
		const unsigned yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524
				+ dayOfEra / 146096) / 365;
		year = static_cast<int>(yearOfEra) + static_cast<int>(era) * 400;
		const unsigned dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
		const unsigned monthPrime = (5 * dayOfYear + 2) / 153;
		day = dayOfYear - (153 * monthPrime + 2) / 5 + 1;
		month = static_cast<unsigned>(static_cast<int>(monthPrime) + (monthPrime < 10 ? 3 : -9));
		year += month <= 2;
	}

	static constexpr s64 civilSeconds(int year, int month, int day, int hour, int minute, int second)
	{
		return daysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day)) * 86400
				+ hour * 3600 + minute * 60 + second;
	}

	void resetClockToHostLocalTime()
	{
		time_t hostSeconds = time(nullptr);
		tm *hostLocal = localtime(&hostSeconds);
		if (hostLocal == nullptr)
		{
			clockOffsetSeconds = 0;
			return;
		}
		clockOffsetSeconds = civilSeconds(hostLocal->tm_year + 1900, hostLocal->tm_mon + 1,
				hostLocal->tm_mday, hostLocal->tm_hour, hostLocal->tm_min, hostLocal->tm_sec)
				- static_cast<s64>(hostSeconds);
	}

	void clearPendingWrite()
	{
		memset(pendingWrite, 0, sizeof(pendingWrite));
		pendingWriteBlock = 0xffff;
		nextWritePhase = 0;
	}

	u32 fileError(u32 error)
	{
		w32(error);
		return MDRE_FileError;
	}

	u32 commitStorageWrite(u16 block)
	{
		if (pendingWriteBlock != block || nextWritePhase != 4)
			return fileError(FileErrorPhase);

		if (file != nullptr)
		{
			if (fullSaveNeeded && !fullSave())
				return fileError(FileErrorWrite);
			if (std::fseek(file, block * 512, SEEK_SET) != 0
					|| std::fwrite(pendingWrite, sizeof(pendingWrite), 1, file) != 1
					|| std::fflush(file) != 0)
			{
				ERROR_LOG(MAPLE, "Failed to save VMU %s: I/O error", logical_port);
				return fileError(FileErrorWrite);
			}
		}

		memcpy(&flash_data[block * 512], pendingWrite, sizeof(pendingWrite));
		clearPendingWrite();
		return MDRS_DeviceReply;
	}

	void decodeLcd()
	{
		for (u32 y = 0; y < 32; y++)
		{
			for (u32 x = 0; x < 48; x++)
			{
				bool pixelOn = (lcd_data[y * 6 + x / 8] & (0x80 >> (x & 7))) != 0;
				lcd_data_decoded[y * 48 + x] = pixelOn ? 0 : 0xff;
			}
		}
	}

	bool isFormatted() const
	{
		const u8 *root = flash_data + 0xff * 512;
		for (u32 i = 0; i < 16; i++)
			if (root[i] != 0x55)
				return false;
		return true;
	}

	MapleDeviceType get_device_type() override
	{
		return MDT_SegaVMU;
	}

	void serialize(Serializer& ser) const override
	{
		maple_base::serialize(ser);
		ser << flash_data;
		ser << lcd_data;
		ser << lcd_data_decoded;
		const s64 clockTimeSeconds = time(nullptr) + clockOffsetSeconds;
		ser << clockTimeSeconds;
		ser << pendingWrite;
		ser << pendingWriteBlock;
		ser << nextWritePhase;
		const bool firmwareEnabled = vmuEmulator != nullptr;
		ser << firmwareEnabled;
		if (firmwareEnabled)
			vmuEmulator->serialize(ser);
	}
	void deserialize(Deserializer& deser) override
	{
		maple_base::deserialize(deser);
		deser >> flash_data;
		deser >> lcd_data;
		deser >> lcd_data_decoded;
		if (deser.version() >= Deserializer::V90)
		{
			s64 clockTimeSeconds;
			deser >> clockTimeSeconds;
			clockOffsetSeconds = clockTimeSeconds - time(nullptr);
			deser >> pendingWrite;
			deser >> pendingWriteBlock;
			deser >> nextWritePhase;
		}
		else
		{
			resetClockToHostLocalTime();
			clearPendingWrite();
		}
		if (nextWritePhase > 4
				|| (nextWritePhase == 0 && pendingWriteBlock != 0xffff)
				|| (nextWritePhase != 0 && pendingWriteBlock >= sizeof(flash_data) / 512))
			clearPendingWrite();
		if (deser.version() >= Deserializer::V150)
		{
			bool firmwareEnabled;
			deser >> firmwareEnabled;
			if (firmwareEnabled)
			{
				if (vmuEmulator == nullptr)
					initializeVmuEmulator();
				if (vmuEmulator == nullptr)
					throw Deserializer::Exception("VMU BIOS required by savestate");
				vmuEmulator->deserialize(deser);
			}
			else
				vmuEmulator.reset();
		}
		else if (vmuEmulator != nullptr)
			vmuEmulator->reset();
		lastVmuUpdate = sh4_sched_now64();
		vmuTimeRemainder = 0;
		firmwareBuzzerPeriod = firmwareBuzzerDuty = 0;
		decodeLcd();
		// A blank LCD is still meaningful state. Always publish the restored
		// framebuffer so loading a state can clear an image drawn afterward.
		config->SetImage(lcd_data_decoded);
		updateMapleLinkScreen();
		fullSaveNeeded = true;
	}

	void updateMapleLinkScreen()
	{
		auto link = MapleLink::GetMapleLink(bus_id, bus_port);
		if (link == nullptr)
			return;

		MapleMsg msg;
		msg.command = MDCF_BlockWrite;
		msg.destAP = maple_port;
		msg.originAP = bus_id << 6;
		msg.pushData(MFID_2_LCD);
		msg.pushData(0);    // PT, phase, block#
		msg.pushData(lcd_data);
		link->send(msg);
	}

	virtual bool fullSave()
	{
		if (file == nullptr)
			return false;
		if (std::fseek(file, 0, SEEK_SET) != 0) {
			ERROR_LOG(MAPLE, "VMU %s: I/O error", logical_port);
			return false;
		}
		if (std::fwrite(flash_data, sizeof(flash_data), 1, file) != 1
				|| std::fflush(file) != 0) {
			ERROR_LOG(MAPLE, "Failed to write the VMU %s to disk", logical_port);
			return false;
		}
		fullSaveNeeded = false;
		return true;
	}

	void initializeVmu()
	{
		INFO_LOG(MAPLE, "Initialising empty VMU %s...", logical_port);

		uLongf dec_sz = sizeof(flash_data);
		int rv = uncompress(flash_data, &dec_sz, vmu_default, sizeof(vmu_default));

		verify(rv == Z_OK);
		verify(dec_sz == sizeof(flash_data));

		fullSave();
	}

	void OnSetup() override
	{
		memset(flash_data, 0, sizeof(flash_data));
		memset(lcd_data, 0, sizeof(lcd_data));
		memset(lcd_data_decoded, 0xff, sizeof(lcd_data_decoded));
		resetClockToHostLocalTime();
		clearPendingWrite();

        // Load existing vmu file if found
        std::string rpath = hostfs::getVmuPath(logical_port, false);
        // this might be a storage url
        hostfs::File *rfile = hostfs::storage().openFile(rpath, "rb");
        if (rfile == nullptr) {
            INFO_LOG(MAPLE, "Unable to open VMU file \"%s\", creating new file", rpath.c_str());
        }
        else
        {
			s64 imageSize = rfile->size();
			if (imageSize != sizeof(flash_data))
			{
				WARN_LOG(MAPLE, "Invalid VMU file \"%s\" size: %lld (expected %zu)",
						rpath.c_str(), (long long)imageSize, sizeof(flash_data));
				// Do not treat a partially read image as a valid card. Otherwise a
				// truncated file can retain a nonzero prefix and silently expose a
				// mostly zero, corrupt volume.
				memset(flash_data, 0, sizeof(flash_data));
			}
			else if (rfile->read(flash_data, sizeof(flash_data), 1) != 1)
			{
				WARN_LOG(MAPLE, "Failed to read the VMU file \"%s\" from disk", rpath.c_str());
				memset(flash_data, 0, sizeof(flash_data));
			}
            delete rfile;
        }
        // Open or create the vmu file to save to
        std::string wpath = hostfs::getVmuPath(logical_port, true);
        file = nowide::fopen(wpath.c_str(), "rb+");
        if (file == nullptr)
        {
            file = nowide::fopen(wpath.c_str(), "wb+");
			if (file == nullptr) {
                ERROR_LOG(MAPLE, "Failed to create VMU save file \"%s\"", wpath.c_str());
			}
			else if (rfile != nullptr)
			{
				// VMU file is being renamed so save it fully now
				// and delete the old file
				if (fullSave())
					nowide::remove(rpath.c_str());
			}
        }

		u8 sum = 0;
		for (u32 i = 0; i < sizeof(flash_data); i++)
			sum |= flash_data[i];

		if (sum == 0)
			// This means the existing VMU file is completely empty and needs to be recreated
			initializeVmu();
		fullSaveNeeded = false;
		initializeVmuEmulator();
	}

	~maple_sega_vmu() override
	{
		aica::sgc::vmuBeep(bus_id, bus_port, 0, 0);
		if (fullSaveNeeded)
			fullSave();
		if (file != nullptr)
			std::fclose(file);
		memset(lcd_data, 0, sizeof(lcd_data));
		updateMapleLinkScreen();
	}

	u32 dma(u32 cmd) override
	{
		syncVmuEmulator();
		//printf("maple_sega_vmu::dma Called for port %d:%d, Command %d\n", bus_id, bus_port, cmd);
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			//caps
			//4
			w32(MFID_1_Storage | MFID_2_LCD | MFID_3_Clock);

			//struct data
			//3*4
			w32( 0x403f7e7e); // for clock
			w32( 0x00100500); // for LCD
			w32( 0x00410f00); // for storage
			//1  area code
			w8(0xFF);
			//1  direction
			w8(0);
			//30
			wstr(maple_sega_vmu_name,30);

			//60
			wstr(maple_sega_brand,60);

			//2
			w16(0x007c);	// 12.4 mA

			//2
			w16(0x0082);	// 13 mA

			if (cmd == MDC_AllStatusReq)
			{
				static constexpr char extra[] =
						"Version 1.005,1999/10/26,315-6208-05,SEGA Visual Memory System BIOS Produced by ";
				static_assert(sizeof(extra) - 1 == 80);
				wptr(extra, sizeof(extra) - 1);
				return MDRS_DeviceStatusAll;
			}
			else {
				return MDRS_DeviceStatus;
			}

			//in[0] is function used
			//out[0] is function used
		case MDCF_GetMediaInfo:
			{
				if (r_count() != 8)
					return MDRE_TransmitAgain;
				u32 function=r32();
				u32 location = r32();
				switch(function)
				{
				case MFID_1_Storage:
					if (location != 0)
						return fileError(FileErrorInvalidPartition);
					DEBUG_LOG(MAPLE, "VMU %s GetMediaInfo storage", logical_port);
					w32(MFID_1_Storage);

					if (!isFormatted())
					{
						// Unformatted state: return predetermined media information
						//total_size;
						w16(0xff);
						//partition_number;
						w16(0);
						//system_area_block;
						w16(0xFF);
						//fat_area_block;
						w16(0xfe);
						//number_fat_areas_block;
						w16(1);
						//file_info_block;
						w16(0xfd);
						//number_info_blocks;
						w16(0xd);
						//volume_icon;
						w8(0);
						//reserved1;
						w8(0);
						//save_area_block;
						w16(0xc8);
						//number_of_save_blocks;
						w16(0x1f);
						//reserverd0 (something for execution files?)
						w32(0);
					}
					else
					{
						// Get data from the vmu system area (block 0xFF)
						wptr(flash_data + 0xFF * 512 + 0x40, 24);
					}
					return MDRS_DataTransfer;//data transfer

				case MFID_2_LCD:
					{
						if (location != 0)
						{
							INFO_LOG(MAPLE, "VMU: MDCF_GetMediaInfo -> bad input |%08X|, returning MDRE_UnknownCmd", location);
							return MDRE_UnknownCmd;
						}
						else
						{
							DEBUG_LOG(MAPLE, "VMU %s GetMediaInfo LCD", logical_port);
							w32(MFID_2_LCD);

							w8(47);             //X dots -1
							w8(31);             //Y dots -1
							w8(((1)<<4) | (0)); //1 Color, 0 contrast levels
							w8(2);              //Padding

							return MDRS_DataTransfer;
						}
					}

				default:
					INFO_LOG(MAPLE, "VMU: MDCF_GetMediaInfo -> Bad function used |%08X|, returning -2", function);
					return MDRE_UnknownFunction;//bad function
				}
			}
			break;

		case MDCF_BlockRead:
			{
				if (r_count() != 8)
					return MDRE_TransmitAgain;
				u32 function=r32();
				switch(function)
				{
				case MFID_1_Storage:
					{
						u32 xo=r32();
						u32 location = SWAP32(xo);
						u32 partition = location >> 24;
						u32 phase = (location >> 16) & 0xff;
						u32 Block = location & 0xffff;
						if (partition != 0)
							return fileError(FileErrorInvalidPartition);
						if (phase != 0)
							return fileError(FileErrorPhase);
						if (Block >= sizeof(flash_data) / 512)
							return fileError(FileErrorInvalidBlock);

						w32(MFID_1_Storage);
						w32(xo);
						DEBUG_LOG(MAPLE, "VMU %s block read: Block %d addr %x len %d", logical_port, Block, Block*512, 512);
						wptr(flash_data+Block*512,512);
					}
					return MDRS_DataTransfer;//data transfer

				case MFID_2_LCD:
					{
						u32 location = r32();
						if (location != 0)
							return MDRE_LCDError;
						DEBUG_LOG(MAPLE, "VMU %s read LCD", logical_port);
						w32(MFID_2_LCD);
						w32(location);
						wptr(lcd_data, sizeof(lcd_data));
					}

					return MDRS_DataTransfer;//data transfer

				case MFID_3_Clock:
					if (r32()!=0)
					{
						INFO_LOG(MAPLE, "VMU: Block read: MFID_3_Clock : invalid params");
						return MDRE_TransmitAgain; //invalid params
					}
					else
					{
						w32(MFID_3_Clock);

						s64 now = static_cast<s64>(time(nullptr)) + clockOffsetSeconds;
						s64 days = now / 86400;
						s64 secondsOfDay = now % 86400;
						if (secondsOfDay < 0)
						{
							secondsOfDay += 86400;
							days--;
						}
						int year;
						unsigned month, day;
						civilFromDays(days, year, month, day);
						if (year < 0 || year > 9999)
							return MDRE_TransmitAgain;

						u8* timebuf=dma_buffer_out;

						w8(static_cast<u8>(year % 256));
						w8(static_cast<u8>(year / 256));
						w8(static_cast<u8>(month));
						w8(static_cast<u8>(day));
						w8(static_cast<u8>(secondsOfDay / 3600));
						w8(static_cast<u8>(secondsOfDay / 60 % 60));
						w8(static_cast<u8>(secondsOfDay % 60));
						w8(static_cast<u8>((days % 7 + 10) % 7)); // 1970-01-01 was Thursday; Monday = 0

						DEBUG_LOG(MAPLE, "VMU: CLOCK Read-> datetime is %04d/%02d/%02d ~ %02d:%02d:%02d!",
								timebuf[0] + timebuf[1] * 256,
								timebuf[2],
								timebuf[3],
								timebuf[4],
								timebuf[5],
								timebuf[6]);

						return MDRS_DataTransfer;//transfer reply ...
					}

				default:
					INFO_LOG(MAPLE, "VMU: cmd MDCF_BlockRead -> Bad function |%08X| used, returning -2", function);
					return MDRE_UnknownFunction;//bad function
				}
			}
			break;

		case MDCF_BlockWrite:
			{
				if (r_count() < 8)
					return MDRE_TransmitAgain;
				u32 function = r32();
				switch (function)
				{
					case MFID_1_Storage:
					{
						u32 bph=r32();
						u32 location = SWAP32(bph);
						u32 Partition = location >> 24;
						u32 Block = location & 0xffff;
						u32 Phase = (location >> 16) & 0xff;
						u32 write_len=r_count();
						if (Partition != 0)
							return fileError(FileErrorInvalidPartition);
						if (Phase >= 4)
							return fileError(FileErrorPhase);
						if (Block >= sizeof(flash_data) / 512)
							return fileError(FileErrorInvalidBlock);
						if (write_len != 512 / 4)
							return fileError(FileErrorInvalidLength);
						if (Phase == 0)
						{
							clearPendingWrite();
							pendingWriteBlock = Block;
						}
						if (pendingWriteBlock != Block || Phase != nextWritePhase)
							return fileError(FileErrorPhase);
						u32 write_adr=Phase*(512/4);
						DEBUG_LOG(MAPLE, "VMU %s block write: Block %d Phase %d addr %x len %d", logical_port, Block, Phase, write_adr, write_len);
						rptr(&pendingWrite[write_adr],write_len);
						nextWritePhase++;
						return MDRS_DeviceReply;
					}

					case MFID_2_LCD:
					{
						u32 location = r32();
						if (location != 0 || r_count() != sizeof(lcd_data))
							return MDRE_LCDError;
						DEBUG_LOG(MAPLE, "VMU %s LCD write", logical_port);
						rptr(lcd_data, sizeof(lcd_data));

						decodeLcd();
						config->SetImage(lcd_data_decoded);
						relayMapleLink();

						return  MDRS_DeviceReply;
					}

					case MFID_3_Clock:
						if (r32()!=0 || r_count()!=8)
						{
							INFO_LOG(MAPLE, "VMU %s clock write invalid params: rcount %d", logical_port, r_count());
							return MDRE_TransmitAgain;	//invalid params ...
						}
						else
						{
							u8 timebuf[8];
							rptr(timebuf,8);
							int year = timebuf[0] + timebuf[1] * 256;
							if (timebuf[2] < 1 || timebuf[2] > 12
									|| timebuf[3] < 1 || timebuf[3] > daysInMonth(year, timebuf[2])
									|| timebuf[4] > 23 || timebuf[5] > 59 || timebuf[6] > 59
									|| timebuf[7] > 6)
								return MDRE_TransmitAgain;
							clockOffsetSeconds = civilSeconds(year, timebuf[2], timebuf[3],
									timebuf[4], timebuf[5], timebuf[6]) - static_cast<s64>(time(nullptr));
							DEBUG_LOG(MAPLE, "VMU: CLOCK Write-> datetime is %04d/%02d/%02d ~ %02d:%02d:%02d",
									timebuf[0]+timebuf[1]*256,timebuf[2],timebuf[3],timebuf[4],timebuf[5],timebuf[6]);
							return  MDRS_DeviceReply;//ok !
						}

					default:
						INFO_LOG(MAPLE, "VMU: command MDCF_BlockWrite -> Unknown function %x", function);
						return  MDRE_UnknownFunction;//bad function
				}
			}
			break;

		case MDCF_GetLastError:
			{
				if (r_count() != 8)
					return MDRE_TransmitAgain;
				u32 function = r32();
				u32 location = SWAP32(r32());
				switch (function)
				{
				case MFID_1_Storage:
					{
					if ((location >> 24) != 0)
						return fileError(FileErrorInvalidPartition);
					if ((location & 0xffff) >= sizeof(flash_data) / 512)
						return fileError(FileErrorInvalidBlock);
					u32 phase = (location >> 16) & 0xff;
					if (phase != 4)
						return fileError(FileErrorPhase);
					return commitStorageWrite(location & 0xffff);
					}

				case MFID_2_LCD:
					return location == 0 ? MDRS_DeviceReply : MDRE_LCDError;

				case MFID_3_Clock:
					return location == 0 ? MDRS_DeviceReply : MDRE_TransmitAgain;

				default:
					return MDRE_UnknownFunction;
				}
			}

		case MDCF_SetCondition:
			{
				if (r_count() != 8)
					return MDRE_TransmitAgain;
				switch(r32())
				{
				case MFID_3_Clock:
				{
					u8 alw = r8();
					u8 ald = r8();
					r16(); // Alarm 2
					INFO_LOG(MAPLE, "BEEP: %d/%d", alw, ald);
					aica::sgc::vmuBeep(bus_id, bus_port, alw, ald);

					relayMapleLink();
					return MDRS_DeviceReply;
				}

				default:
					INFO_LOG(MAPLE, "VMU: command MDCF_SetCondition -> Bad function used, returning MDRE_UnknownFunction");
					return MDRE_UnknownFunction; //bad function
				}
			}
			break;

		case MDCF_GetCondition:
		{
			if (r_count() != 4)
				return MDRE_TransmitAgain;
			if (r32() != MFID_3_Clock)
				return MDRE_UnknownFunction;
			PlainJoystickState pjs;
			config->GetInput(&pjs);
			w32(MFID_3_Clock);
			w8(getVmuButtons(pjs.kcode));
			w8(0);
			w8(0);
			w8(0);
			return MDRS_DataTransfer;
		}

		case MDC_DeviceReset:
			clearPendingWrite();
			aica::sgc::vmuBeep(bus_id, bus_port, 0, 0);
			if (vmuEmulator != nullptr)
			{
				vmuEmulator->reset();
				lastVmuUpdate = sh4_sched_now64();
				vmuTimeRemainder = 0;
				firmwareBuzzerPeriod = firmwareBuzzerDuty = 0;
			}
			return MDRS_DeviceReply;

		case MDC_DeviceKill:
			clearPendingWrite();
			aica::sgc::vmuBeep(bus_id, bus_port, 0, 0);
			return MDRS_DeviceReply;

		default:
			DEBUG_LOG(MAPLE, "Unknown MAPLE COMMAND %d", cmd);
			return MDRE_UnknownCmd;
		}
	}

	const void *getData(size_t& size) const override
	{
		size = sizeof(flash_data);
		return flash_data;
	}
};

struct maple_microphone: maple_base
{
	u32 gain = 0xf;
	bool sampling = false;
	bool eight_khz = false;

	~maple_microphone() override
	{
		if (sampling)
			StopAudioRecording();
	}
	MapleDeviceType get_device_type() override
	{
		return MDT_Microphone;
	}

	void serialize(Serializer& ser) const override
	{
		maple_base::serialize(ser);
		ser << gain;
		ser << sampling;
		ser << eight_khz;
	}
	void deserialize(Deserializer& deser) override
	{
		if (sampling)
			StopAudioRecording();
		maple_base::deserialize(deser);
		deser >> gain;
		deser >> sampling;
		deser >> eight_khz;
		deser.skip(480 - sizeof(u32) - sizeof(bool) * 2, Deserializer::V23);
		if (sampling)
			StartAudioRecording(eight_khz);
	}

	void OnSetup() override
	{
		gain = 0xf;
		sampling = false;
		eight_khz = false;
	}

	u32 dma(u32 cmd) override
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			DEBUG_LOG(MAPLE, "maple_microphone::dma MDC_DeviceRequest");

			//caps
			//4
			w32(MFID_4_Mic);

			//struct data
			//3*4
			w32(0xf0000000);
			w32(0);
			w32(0);

			//1	area code
			w8(0xFF);

			//1	direction
			w8(0);

			//30
			wstr(maple_sega_mic_name, 30);

			//60
			wstr(maple_sega_brand, 60);

			//2
			w16(0x012C);	// 30 mA

			//2
			w16(0x012C);	// 30 mA

			return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

		case MDC_DeviceReset:
		case MDC_DeviceKill:
			DEBUG_LOG(MAPLE, "maple_microphone::dma MDC_DeviceReset");
			if (sampling)
				StopAudioRecording();
			OnSetup();
			return MDRS_DeviceReply;

		case MDCF_MICControl:
		{
			if (r_count() != 8)
				return MDRE_TransmitAgain;
			u32 function=r32();

			switch(function)
			{
			case MFID_4_Mic:
			{
				u32 subcommand = r8();
				u32 dt1 = r8();
				u16 dt23 = r16();

				switch(subcommand)
				{
				case 0x01:	// Get_Sampling_Data
				{
					gain = dt1;
					w32(MFID_4_Mic);

					u8 micdata[240 * 2] {};
					// Recording is process-global, while the Maple sampling state is
					// per device. Do not let an idle microphone consume samples from a
					// backend that another microphone has started.
					u32 samples = sampling ? std::min(RecordAudio(micdata, 240), 240u) : 0;

					//32 bit header
			        //status: bit   7      6     5 4      3      2    1    0
			        //        name  EX_BIT SBFOV 0 14LSB1 14LSB0 SMPL ulaw Fs
					w8((sampling << 2) | eight_khz);
					w8(gain); // gain
					w8(0);    //(unused)
					w8(samples); // sample count (max 240)

					wptr(micdata, ((samples + 1) >> 1) << 2);

					return MDRS_DataTransfer;
				}

				case 0x02:	// Basic_Control
				{
					DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl Basic_Control DT1 %02x", dt1);
					const bool newEightKhz = ((dt1 >> 2) & 3) == 1;
					const bool newSampling = (dt1 & 0x80) != 0;
					if (sampling && (!newSampling || newEightKhz != eight_khz))
						StopAudioRecording();
					if (newSampling && (!sampling || newEightKhz != eight_khz))
						StartAudioRecording(newEightKhz);
					eight_khz = newEightKhz;
					sampling = newSampling;
					return MDRS_DeviceReply;
				}

				case 0x03:	// AMP_GAIN
					gain = dt1;
					DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl set gain %x", gain);
					return MDRS_DeviceReply;

				case 0x04:	// EXTU_BIT
					DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl EXTU_BIT %#010x", dt1);
					return MDRS_DeviceReply;

				case 0x05:	// Volume_Mode
					DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl Volume_Mode %#010x", dt1);
					return MDRS_DeviceReply;

				case MDRE_TransmitAgain:
					// Seaman polls this normal retry response continuously while waiting
					// for microphone data. It is protocol flow, not an emulator warning;
					// warning-level output can dominate accurate-interpreter execution.
					DEBUG_LOG(MAPLE, "maple_microphone::dma MDCF_MICControl MDRE_TransmitAgain");
					//apparently this doesnt matter
					//wptr(micdata, SIZE_OF_MIC_DATA);
					return MDRS_DeviceReply;//MDRS_DataTransfer;

				default:
					INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED DT1 %02x DT23 %04x", dt1, dt23);
					return MDRE_UnknownFunction;
				}
			}

			default:
				INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED function %#010x", function);
				return MDRE_UnknownFunction;
			}
			break;
		}

		default:
			INFO_LOG(MAPLE, "maple_microphone::dma UNHANDLED MAPLE COMMAND %d", cmd);
			return MDRE_UnknownCmd;
		}
	}
};

struct maple_sega_purupuru : maple_base
{
	u16 AST = 19, AST_ms = 5000;
	u32 VIBSET = 0;

	~maple_sega_purupuru() override
	{
		if (config != nullptr)
			config->SetVibration(0, 0, 0);
	}

	void resetVibration()
	{
		AST = 19;
		AST_ms = 5000;
		VIBSET = 0;
		if (config != nullptr)
			config->SetVibration(0, 0, 0);
	}

	void OnSetup() override
	{
		resetVibration();
	}

	MapleDeviceType get_device_type() override
	{
		return MDT_PurupuruPack;
	}

	void serialize(Serializer& ser) const override
	{
		maple_base::serialize(ser);
		ser << AST;
		ser << AST_ms;
		ser << VIBSET;
	}
	void deserialize(Deserializer& deser) override
	{
		maple_base::deserialize(deser);
		deser >> AST;
		deser >> AST_ms;
		deser >> VIBSET;
	}

	u32 dma(u32 cmd) override
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			//caps
			//4
			w32(MFID_8_Vibration);

			//struct data
			//3*4
			w32(0x00000101);
			w32(0);
			w32(0);

			//1	area code
			w8(0xFF);

			//1	direction
			w8(0);

			//30
			wstr(maple_sega_purupuru_name, 30);

			//60
			wstr(maple_sega_brand, 60);

			//2
			w16(0x00C8);	// 20 mA

			//2
			w16(0x0640);	// 160 mA

			if (cmd == MDC_AllStatusReq)
			{
				// Captured from an original HKT-8600 Puru Puru Pack. The
				// punctuation and padding are part of the 80-byte free status.
				static constexpr char extra[] =
						"Version 1.000,1998/11/10,315-6211-AH   ,Vibration Motor:1 , Fm:4 - 30Hz ,Pow:7  ";
				static_assert(sizeof(extra) - 1 == 80);
				wptr(extra, sizeof(extra) - 1);
				return MDRS_DeviceStatusAll;
			}
			else {
				return MDRS_DeviceStatus;
			}

			//get last vibration
		case MDCF_GetCondition:
			{
				MapleDeviceRV validation = validateFunction(MFID_8_Vibration, 4);
				if (validation != MDRS_JVSNone)
					return validation;

			w32(MFID_8_Vibration);

			w32(VIBSET);

			return MDRS_DataTransfer;
			}

		case MDCF_GetMediaInfo:
			{
				MapleDeviceRV validation = validateFunction(MFID_8_Vibration, 8);
				if (validation != MDRS_JVSNone)
					return validation;

			w32(MFID_8_Vibration);

			// PuruPuru vib specs
			w32(0x3B07E010);

			return MDRS_DataTransfer;
			}

		case MDCF_BlockRead:
			{
				MapleDeviceRV validation = validateFunction(MFID_8_Vibration, 8);
				if (validation != MDRS_JVSNone)
					return validation;

			w32(MFID_8_Vibration);
			w32(0);

			w16(2);
			w16(AST);

			return MDRS_DataTransfer;
			}

		case MDCF_BlockWrite:
			{
				MapleDeviceRV validation = validateFunction(MFID_8_Vibration, 12);
				if (validation != MDRS_JVSNone)
					return validation;

			//Auto-stop time
			AST = dma_buffer_in[10];
			AST_ms = AST * 250 + 250;
			relayMapleLink();

			return MDRS_DeviceReply;
			}

		case MDCF_SetCondition:
			{
				MapleDeviceRV validation = validateFunction(MFID_8_Vibration, 8);
				if (validation != MDRS_JVSNone)
					return validation;

			memcpy(&VIBSET, &dma_buffer_in[4], sizeof(VIBSET));
			{
				//Do the rumble thing!
				u8 POW_POS = (VIBSET >> 8) & 0x7;
				u8 POW_NEG = (VIBSET >> 12) & 0x7;
				u8 FREQ = (VIBSET >> 16) & 0xFF;
				s16 INC = (VIBSET >> 24) & 0xFF;
				if (VIBSET & 0x8000)			// INH
					INC = -INC;
				else if (!(VIBSET & 0x0800))	// EXH
					INC = 0;
				bool CNT = VIBSET & 1;

				float power = std::min((POW_POS + POW_NEG) / 7.0, 1.0);

				u32 duration_ms;
				if (FREQ > 0 && (!CNT || INC))
					duration_ms = std::min((int)(1000 * (INC ? abs(INC) * std::max(POW_POS, POW_NEG) : 1) / FREQ), (int)AST_ms);
				else
					duration_ms = AST_ms;
				float inclination;
				if (INC == 0 || power == 0)
					inclination = 0.0;
				else
					inclination = FREQ / (1000.0 * INC * std::max(POW_POS, POW_NEG));
				if (config != nullptr)
					config->SetVibration(power, inclination, duration_ms);

				relayMapleLink();
			}

			return MDRS_DeviceReply;
			}

		case MDC_DeviceReset:
		case MDC_DeviceKill:
			resetVibration();
			return MDRS_DeviceReply;

		default:
			INFO_LOG(MAPLE, "UNKOWN MAPLE COMMAND %d", cmd);
			return MDRE_UnknownCmd;
		}
	}
};

struct maple_keyboard : maple_base
{
	MapleDeviceType get_device_type() override
	{
		return MDT_Keyboard;
	}

	u32 dma(u32 cmd) override
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			//caps
			//4
			w32(MFID_6_Keyboard);

			//struct data
			//3*4
			w8((u8)settings.input.keyboardLangId);
			switch (settings.input.keyboardLangId)
			{
			case KeyboardLayout::JP:
				w8(2);	// 92 keys
				break;
			case KeyboardLayout::US:
				w8(5);	// 104 keys
				break;
			default:
				w8(6);	// 105 keys
				break;
			}
			w8(7);		// modifier byte plus six-key rollover
			w8(0x80);	// keyboard-controlled LEDs

			w32(0);
			w32(0);
			//1	area code
			w8(0xFF);
			//1	direction
			w8(0);
			// Product name (30)
			wstr(maple_sega_kbd_name, 30);

			// License (60)
			wstr(maple_sega_brand, 60);

			// Low-consumption standby current (2)
			w16(0x015E);	// 35 mA

			// Maximum current consumption (2)
			w16(0x01F4);	// 50 mA

			if (cmd == MDC_AllStatusReq)
			{
				// Captured from the licensed Mad Catz Dreamcast keyboard. Unlike
				// the first-party controller, this device supplies 40 free-status
				// bytes rather than the maximum 80.
				static constexpr char extra[] =
						"Version 1.000,1999/09/29,Mad Catz Inc.  ";
				static_assert(sizeof(extra) - 1 == 40);
				wptr(extra, sizeof(extra) - 1);
				return MDRS_DeviceStatusAll;
			}
			return MDRS_DeviceStatus;

		case MDCF_GetCondition:
			{
				MapleDeviceRV validation = validateFunction(MFID_6_Keyboard, 4);
				if (validation != MDRS_JVSNone)
					return validation;
				u8 shift;
				u8 keys[6];
				config->GetKeyboardInput(shift, keys);

				w32(MFID_6_Keyboard);
				//struct data
				//int8 shift          ; shift keys pressed (bitmask)	//1
				w8(shift);
				//int8 led            ; leds currently lit			//1
				w8(0);
				//int8 key[6]         ; normal keys pressed			//6
				for (std::size_t i = 0; i < std::size(keys); i++)
					w8(keys[i]);
			}
			return MDRS_DataTransfer;

		case MDC_DeviceReset:
			return MDRS_DeviceReply;

		case MDC_DeviceKill:
			return MDRS_DeviceReply;

		default:
			INFO_LOG(MAPLE, "Keyboard: unknown MAPLE COMMAND %d", cmd);
			return MDRE_UnknownCmd;
		}
	}
};

struct maple_mouse : maple_base
{
	MapleDeviceType get_device_type() override
	{
		return MDT_Mouse;
	}

	static u16 mo_cvt(int delta)
	{
		return (u16)std::min(0x3FF, std::max(0, delta + 0x200));
	}

	u32 dma(u32 cmd) override
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			//caps
			//4
			w32(MFID_9_Mouse);

			//struct data
			//3*4
			w32(0x00070E00);	// Mouse, 3 buttons, 3 axes
			w32(0);
			w32(0);
			//1	area code
			w8(0xFF);
			//1	direction
			w8(0);
			// Product name (30)
			wstr(maple_sega_mouse_name, 30);

			// License (60)
			wstr(maple_sega_brand, 60);

			// Low-consumption standby current (2)
			w16(0x0190);	// 40 mA

			// Maximum current consumption (2)
			w16(0x01f4);	// 50 mA

			return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

		case MDCF_GetCondition:
			{
				MapleDeviceRV validation = validateFunction(MFID_9_Mouse, 4);
				if (validation != MDRS_JVSNone)
					return validation;
				u8 buttons;
				int x, y, wheel;
				config->GetMouseInput(buttons, x, y, wheel);

				w32(MFID_9_Mouse);
				// The standard mouse condition starts with a 16-bit active-low
				// button field followed by a reserved 16-bit field.
				w16(buttons);
				w16(0);
				//int16 axis1         ; horizontal movement (0-$3FF) (little endian)
				w16(mo_cvt(x));
				//int16 axis2         ; vertical movement (0-$3FF) (little endian)
				w16(mo_cvt(y));
				//int16 axis3         ; mouse wheel movement (0-$3FF) (little endian)
				w16(mo_cvt(wheel));
				//int16 axis4         ; ? movement (0-$3FF) (little endian)
				w16(mo_cvt(0));
				//int16 axis5         ; ? movement (0-$3FF) (little endian)
				w16(mo_cvt(0));
				//int16 axis6         ; ? movement (0-$3FF) (little endian)
				w16(mo_cvt(0));
				//int16 axis7         ; ? movement (0-$3FF) (little endian)
				w16(mo_cvt(0));
				//int16 axis8         ; ? movement (0-$3FF) (little endian)
				w16(mo_cvt(0));
			}

			return MDRS_DataTransfer;

		case MDC_DeviceReset:
			return MDRS_DeviceReply;

		case MDC_DeviceKill:
			return MDRS_DeviceReply;

		default:
			INFO_LOG(MAPLE, "Mouse: unknown MAPLE COMMAND %d", cmd);
			return MDRE_UnknownCmd;
		}
	}
};

struct maple_lightgun : maple_base
{
	virtual u32 transform_kcode(u32 kcode)
	{
		mutualExclusion(kcode, DC_DPAD_UP | DC_DPAD_DOWN);
		mutualExclusion(kcode, DC_DPAD_LEFT | DC_DPAD_RIGHT);
		if ((kcode & DC_BTN_RELOAD) == 0)
			kcode &= ~DC_BTN_A;	// trigger
		return kcode | 0xFF01;
	}

	MapleDeviceType get_device_type() override
	{
		return MDT_LightGun;
	}

	u32 dma(u32 cmd) override
	{
		switch (cmd)
		{
		case MDC_DeviceRequest:
		case MDC_AllStatusReq:
			//caps
			//4
			w32(MFID_7_LightGun | MFID_0_Input);

			//struct data
			//3*4
			w32(0);				// Light gun
			w32(0xFE000000);	// Controller
			w32(0);
			//1	area code
			w8(1);				// FF: Worldwide, 01: North America
			//1	direction
			w8(0);
			// Product name (30)
			wstr(maple_sega_lightgun_name, 30);

			// License (60)
			wstr(maple_sega_brand, 60);

			// Low-consumption standby current (2)
			w16(0x0069);	// 10.5 mA

			// Maximum current consumption (2)
			w16(0x0120);	// 28.8 mA

			return cmd == MDC_DeviceRequest ? MDRS_DeviceStatus : MDRS_DeviceStatusAll;

		case MDCF_GetCondition:
		{
			MapleDeviceRV validation = validateFunction(MFID_0_Input, 4);
			if (validation != MDRS_JVSNone)
				return validation;
			PlainJoystickState pjs;
			config->GetInput(&pjs);

			//caps
			//4
			w32(MFID_0_Input);

			//state data
			//2 key code
			w16(transform_kcode(pjs.kcode));

			//6 analog (not used)
			w16(0);
			w32(0x80808080);
		}
		return MDRS_DataTransfer;

		case MDC_DeviceReset:
			return MDRS_DeviceReply;

		case MDC_DeviceKill:
			return MDRS_DeviceReply;

		default:
			INFO_LOG(MAPLE, "Light gun: unknown MAPLE COMMAND %d", cmd);
			return MDRE_UnknownCmd;
		}
	}

	bool get_lightgun_pos() override
	{
		PlainJoystickState pjs;
		config->GetInput(&pjs);
		int x, y;
		config->GetAbsCoordinates(x, y);

		if ((pjs.kcode & DC_BTN_RELOAD) == 0)
			read_lightgun_position(-1, -1);
		else
			read_lightgun_position(x, y);
		return true;
	}
};

struct atomiswave_lightgun : maple_lightgun
{
	u32 transform_kcode(u32 kcode) override {
		mutualExclusion(kcode, AWAVE_UP_KEY | AWAVE_DOWN_KEY);
		mutualExclusion(kcode, AWAVE_LEFT_KEY | AWAVE_RIGHT_KEY);
		// No need for reload on AW
		return (kcode & AWAVE_TRIGGER_KEY) == 0 ? ~AWAVE_BTN0_KEY : ~0;
	}
};

struct maple_maracas_controller: maple_sega_controller
{
	u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0x0f093c00;	// 4 analog axes (2-5) A B C D Z Start
	}

	u16 getButtonState(const PlainJoystickState &pjs) override {
		return pjs.kcode | 0xf6f0;		// mask off DPad2, X, Y, DPad;
	}

	MapleDeviceType get_device_type() override {
		return MDT_MaracasController;
	}

	u32 getAnalogAxis(int index, const PlainJoystickState &pjs) override {
		if (index < 2 || index > 5)
			return 0;
		return pjs.joy[index -2];
		/* // This should be tested with real maracas to see if it is worth implementing or not
		u8 maracas_saturation_reduction = 2;
		s32 axis_val = (pjs.joy[index -2] - 0x80) / maracas_saturation_reduction + 0x80;
		if      (axis_val <    0) axis_val = 0;
		else if (axis_val > 0xff) axis_val = 0xFF;
		return axis_val; */
	}

	const char *get_device_name() override {
		return maple_maracas_controller_name;
	}

	u32 get_device_current(int get_max_current) override {
		return get_max_current ? 0x0546 : 0x044C; // Max. 130 mA, standby: 100 mA
	}
};

struct maple_fishing_controller: maple_sega_controller
{
	u32 analogToDPad = ~0;

	u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0x0fe063f00;	// Ra,La,Da,Ua,A,B,X,Y,Start,A1,A2,A3,A4,A5,A6
	}

	u16 getButtonState(const PlainJoystickState &pjs) override
	{
		// Analog to DPad handling
		if (pjs.joy[PJAI_X1] < 0x30) {
			analogToDPad &= ~DC_DPAD_LEFT;
			analogToDPad |= DC_DPAD_RIGHT;
		}
		else if (pjs.joy[PJAI_X1] > 0xd0) {
			analogToDPad &= ~DC_DPAD_RIGHT;
			analogToDPad |= DC_DPAD_LEFT;
		}
		else
		{
			if (pjs.joy[PJAI_X1] >= 0x40)
				analogToDPad |= DC_DPAD_LEFT;
			if (pjs.joy[PJAI_X1] <= 0xc0)
				analogToDPad |= DC_DPAD_RIGHT;
		}
		if (pjs.joy[PJAI_Y1] < 0x30) {
			analogToDPad &= ~DC_DPAD_UP;
			analogToDPad |= DC_DPAD_DOWN;
		}
		else if (pjs.joy[PJAI_Y1] > 0xd0) {
			analogToDPad &= ~DC_DPAD_DOWN;
			analogToDPad |= DC_DPAD_UP;
		}
		else
		{
			if (pjs.joy[PJAI_Y1] >= 0x40)
				analogToDPad |= DC_DPAD_UP;
			if (pjs.joy[PJAI_Y1] <= 0xc0)
				analogToDPad |= DC_DPAD_DOWN;
		}
		u32 kcode = pjs.kcode & analogToDPad;
		mutualExclusion(kcode, DC_DPAD_UP   | DC_DPAD_DOWN);
		mutualExclusion(kcode, DC_DPAD_LEFT | DC_DPAD_RIGHT);
		return kcode | 0xf901;		// mask off DPad2, D, Z, C;
	}

	MapleDeviceType get_device_type() override {
		return MDT_FishingController;
	}

	u32 getAnalogAxis(int index, const PlainJoystickState &pjs) override
	{
		// In the XYZ axes, acceleration sensor outputs 80 ± 8H (home position)
		//   in the static state (± 0G), F0h or greater for maximum force (+10G)
		//   in the positive direction and 11h or less
		//   for the maximum force (-10G) applied in the negative direction
		// From the perspective of the player operating the controller:
		//   X: Right is positive, left is negative
		//   Y: Down is positive, up is negative
		//   Z: Forward is positive, backward is negative
		switch (index)
		{
		case 0:
			return pjs.trigger[PJTI_R];		// A1: Reel handle output
		case 1:
			return pjs.joy[PJAI_X3];		// A2: acceleration sensor Z
		case 2:
			return pjs.joy[PJAI_X1];		// A3: analog stick X
		case 3:
			return pjs.joy[PJAI_Y1];		// A4: analog stick Y
		case 4:
			return pjs.joy[PJAI_X2];		// A5: acceleration sensor X
		case 5:
			return pjs.joy[PJAI_Y2];		// A6: acceleration sensor Y
		default:
			return 0x80;
		}
	}

	const char *get_device_name() override {
		return maple_fishing_controller_name;
	}

	u32 get_device_current(int get_max_current) override {
		return get_max_current ? 0x0960 : 0x0258; // Max. 240 mA, standby: 60 mA
	}
};

struct maple_popnmusic_controller: maple_sega_controller
{
	u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xff060000;	// no analog axes, X Y A B C Start U/D/L/R
	}

	u16 getButtonState(const PlainJoystickState &pjs) override {
		return pjs.kcode | 0xf100; // mask off DPad2 and Z
	}

	MapleDeviceType get_device_type() override {
		return MDT_PopnMusicController;
	}

	u32 getAnalogAxis(int index, const PlainJoystickState &pjs) override {
		if (index == 0 || index == 1)
			return 0;		// Right and left triggers
		return 0x80;
	}

	const char *get_device_name() override {
		return maple_popnmusic_controller_name;
	}

	u32 get_device_current(int get_max_current) override {
		return get_max_current ? 0x012C : 0x00AA; // Max. 30 mA, standby: 17 mA
	}
};

struct maple_racing_controller: maple_sega_controller
{
	u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xfe000700;	// Steering only: Ra,La,Da,Ua,A,B,Start,A1,A2,A3
	}

	u16 getButtonState(const PlainJoystickState &pjs) override
	{
		// Ra, La are ON when A3 threshold values (La: 40h, Ra: BEh) are exceeded
		u32 kcode = pjs.kcode;
		if (pjs.joy[PJAI_X1] < 0x40)
			kcode &= ~DC_DPAD_LEFT;
		else if (pjs.joy[PJAI_X1] > 0xBE)
			kcode &= ~DC_DPAD_RIGHT;
		mutualExclusion(kcode, DC_DPAD_UP   | DC_DPAD_DOWN);
		mutualExclusion(kcode, DC_DPAD_LEFT | DC_DPAD_RIGHT);
		return kcode | 0xff01;	// mask off DPad2, D, X, Y, Z, C
	}

	MapleDeviceType get_device_type() override {
		return MDT_RacingController;
	}

	u32 getAnalogAxis(int index, const PlainJoystickState &pjs) override
	{
		switch (index)
		{
		case 0: return pjs.trigger[PJTI_R];	 // A1: lever, 0 at rest
		case 1: return pjs.trigger[PJTI_L];	 // A2: lever, 0 at rest
		case 2: return pjs.joy[PJAI_X1];	 // A3: 0-0xff, 0x80 at rest
		default: return 0x80;				 // unused
		}
	}

	const char *get_device_name() override {
		return maple_racing_controller_name;
	}

	u32 get_device_current(int get_max_current) override {
		return get_max_current ? 0x0226 : 0x01B8; // Max. 55 mA, standby: 44 mA
	}
};

struct maple_densha_controller: maple_sega_controller
{
	u32 get_capabilities() override {
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C

		return 0xff0f3f00;	// Ra,La,Da,Ua A,B,C,D,X,Y,Z,Start Xa,Ya,Xb,Yb Analog levers R,L
	}

	u16 getButtonState(const PlainJoystickState &pjs) override {
		// Ra,La,Da,Ua are used together, corresponding to the brake lever.
		return pjs.kcode | 0xF000; // mask off DPad2
	}

	MapleDeviceType get_device_type() override {
		return MDT_DenshaDeGoController;
	}

	u32 getAnalogAxis(int index, const PlainJoystickState &pjs) override {
		if (index == 2 || index == 3)
			return 0;
		if (index == 0 || index == 1 || index == 4 || index == 5)
			return 0xff;
		return 0xff;
	}

	const char *get_device_name() override {
		return maple_densha_controller_name;
	}

	u32 get_device_current(int get_max_current) override {
		return get_max_current ? 0x01F4 : 0x00DC; // Max. 50 mA, standby: 22 mA
	}
};

struct FullController : maple_sega_controller
{
	u32 get_capabilities() override
	{
		// byte 0: 0  0  0  0  0  0  0  0
		// byte 1: 0  0  a5 a4 a3 a2 a1 a0
		// byte 2: R2 L2 D2 U2 D  X  Y  Z
		// byte 3: R  L  D  U  St A  B  C
		return 0xffff3f00;	// 6 axes, all buttons
	}

	u16 getButtonState(const PlainJoystickState &pjs) override
	{
		u32 kcode = pjs.kcode;
		mutualExclusion(kcode, DC_DPAD_UP | DC_DPAD_DOWN);
		mutualExclusion(kcode, DC_DPAD_LEFT | DC_DPAD_RIGHT);
		mutualExclusion(kcode, DC_DPAD2_UP | DC_DPAD2_DOWN);
		mutualExclusion(kcode, DC_DPAD2_LEFT | DC_DPAD2_RIGHT);
		return kcode;
	}

	u32 getAnalogAxis(int index, const PlainJoystickState &pjs) override
	{
		if (index == 4 || index == 5)
		{
			// Limit the magnitude of the analog axes to 128
			s8 xaxis = pjs.joy[PJAI_X2] - 128;
			s8 yaxis = pjs.joy[PJAI_Y2] - 128;
			limit_joystick_magnitude<128>(xaxis, yaxis);
			if (index == 4)
				return xaxis + 128;
			else
				return yaxis + 128;
		}
		return maple_sega_controller::getAnalogAxis(index, pjs);
	}

	const char *get_device_name() override {
		return "Dreamcast PantherDC Controller";
	}

	MapleDeviceType get_device_type() override {
		return MDT_SegaControllerXL;
	}
};

struct maple_dreamparapara_controller : maple_device
{
	static constexpr u16 START    = 1 << 0;
	static constexpr u16 LEFT     = 1 << 1;
	static constexpr u16 SELECT   = 1 << 2;
	static constexpr u16 RIGHT    = 1 << 3;
	static constexpr u16 ARROW_UL = 1 << 9;
	static constexpr u16 ARROW_U  = 1 << 10;
	static constexpr u16 ARROW_L  = 1 << 11;
	static constexpr u16 ARROW_UR = 1 << 12;
	static constexpr u16 ARROW_R  = 1 << 15;

	static u16 unshift(u32 value)
	{
		u16 result = 0;
		for (u32 i = 1, o = 0; i < 32; i += 2, o++)
		{
			if (value & (1 << i))
				result |= (1 << o);
		}
		return result;
	}

	MapleDeviceType get_device_type() override
	{
		return MDT_DreamParaParaController;
	}

	u16 get_state()
	{
		u16 state = 0;
		PlainJoystickState pjs;
		config->GetInput(&pjs);
		if (pjs.kcode & DC_BTN_START)  state |= START;
		if (pjs.kcode & DC_DPAD_LEFT)  state |= LEFT;
		if (pjs.kcode & DC_BTN_RELOAD) state |= SELECT;
		if (pjs.kcode & DC_DPAD_RIGHT) state |= RIGHT;
		if (pjs.kcode & DC_BTN_A)      state |= ARROW_L;
		if (pjs.kcode & DC_BTN_B)      state |= ARROW_UL;
		if (pjs.kcode & DC_BTN_X)      state |= ARROW_U;
		if (pjs.kcode & DC_BTN_Y)      state |= ARROW_UR;
		if (pjs.kcode & DC_BTN_C)      state |= ARROW_R;
		return state;
	}

	u32 RawDma(const u32 *buffer_in, u32 buffer_in_len, u32 *buffer_out) override
	{
		if (buffer_in_len < sizeof(u32))
			return 0;
		u32 outlen = 0;

		if (buffer_in[0] == 0x08000000)
		{
			*buffer_out++ = 0x80888888;
			strncpy((char*)buffer_out, "Flycast DreamParaPara", 24); // official name unknown
			outlen = 4 + 24;
		}
		else
		{
			if (buffer_in_len < 2 * sizeof(u32))
				return 0;
			u16 state = ~get_state();
			u8 method = (unshift(buffer_in[0]) >> 11) & 3;
			u8 val14 = unshift(buffer_in[0]) & 0xff;
			u16 val18 = unshift(buffer_in[1]);
			u8 key0 = val18 & 0xff;
			u8 key1 = val18 >> 8;
			u16 chk = 0;

			buffer_out[0] = 0;
			buffer_out[1] = 0;

			switch (method)
			{
			case 0:
				buffer_out[1] |= (((state >>  0) & 0xff) ^ key1) << 24;
				buffer_out[0] |= (((state >>  8) & 0xff) ^ key0) <<  8;
				buffer_out[1] |= (((state >> 16) & 0xff) ^ key1) << 16;
				buffer_out[0] |= (((state >> 24) & 0xff) ^ key0) << 16;

				chk |= (val14 & 0xf) << 4;
				chk |= val14 >> 4;
				chk ^= key0;
				chk |= (val18 & 0xff & key1 & 0xf8) << 5;

				buffer_out[0] |= (chk & 0x1f00) << 16;
				buffer_out[0] |= (chk & 0x00ff);
				break;

			case 1:
				buffer_out[1] |= (((state >>  0) & 0xff)       ) <<  8;
				buffer_out[1] |= (((state >>  8) & 0xff) ^ key1) << 16;
				buffer_out[1] |= (((state >> 16) & 0xff) ^ key0) <<  0;
				buffer_out[1] |= (((state >> 24) & 0xff)       ) << 24;

				chk |= val14 ^ key1;
				chk |= ((val14 | key0) & 0x1f) << 8;

				buffer_out[0] |= (chk & 0x1f00) << 16;
				buffer_out[0] |= (chk & 0x00ff);
				break;

			case 2:
				buffer_out[1] |= (((state >>  0) & 0xff) ^ key1) << 24;
				buffer_out[1] |= (((state >>  8) & 0xff) ^ key1) <<  8;
				buffer_out[1] |= (((state >> 16) & 0xff)       ) << 16;
				buffer_out[0] |= (((state >> 24) & 0xff) ^ key0) <<  0;

				chk |= (val14 & 0xc0) >> 6;
				chk |= (val14 & 0x3f) << 2;
				chk |= (val18 & 0x7c) << 6;

				buffer_out[0] |= (chk & 0x1f00) << 16;
				buffer_out[1] |= (chk & 0x00ff);
				break;

			case 3:
				buffer_out[1] |= (((state >>  0) & 0xff)       ) <<  8;
				buffer_out[1] |= (((state >>  8) & 0xff) ^ key0) <<  0;
				buffer_out[0] |= (((state >> 16) & 0xff)       ) << 16;
				buffer_out[1] |= (((state >> 24) & 0xff) ^ key1) << 24;

				chk |= ~key1 & 0xff;
				chk |= ((key0 ^ val14) & 0x1f) << 8;

				buffer_out[0] |= (chk & 0x1f00) << 16;
				buffer_out[1] |= (chk & 0x00ff) << 16;
				break;
			}

			outlen = 8;
		}

		verify(u8(outlen / 4) * 4 == outlen);
		return outlen;
	}
};

std::shared_ptr<maple_device> maple_Create(MapleDeviceType type)
{
	switch(type)
	{
	case MDT_SegaController:
		if (!settings.platform.isAtomiswave())
			return std::make_shared<maple_sega_controller>();
		else
			return std::make_shared<maple_atomiswave_controller>();
	case MDT_Microphone:		return std::make_shared<maple_microphone>();
	case MDT_SegaVMU:			return std::make_shared<maple_sega_vmu>();
	case MDT_PurupuruPack:		return std::make_shared<maple_sega_purupuru>();
	case MDT_Keyboard:			return std::make_shared<maple_keyboard>();
	case MDT_Mouse:				return std::make_shared<maple_mouse>();
	case MDT_LightGun:
		if (!settings.platform.isAtomiswave())
			return std::make_shared<maple_lightgun>();
		else
			return std::make_shared<atomiswave_lightgun>();
	case MDT_NaomiJamma:		return MIE::Create();
	case MDT_TwinStick:			return std::make_shared<maple_sega_twinstick>();
	case MDT_AsciiStick:		return std::make_shared<maple_ascii_stick>();
	case MDT_MaracasController:	return std::make_shared<maple_maracas_controller>();
	case MDT_FishingController:	return std::make_shared<maple_fishing_controller>();
	case MDT_PopnMusicController:	return std::make_shared<maple_popnmusic_controller>();
	case MDT_RacingController:	return std::make_shared<maple_racing_controller>();
	case MDT_DenshaDeGoController:	return std::make_shared<maple_densha_controller>();
	case MDT_SegaControllerXL:	return std::make_shared<FullController>();
	case MDT_DreamParaParaController:	return std::make_shared<maple_dreamparapara_controller>();
	case MDT_RFIDReaderWriter:	return RFIDReaderWriter::Create();

	default:
		ERROR_LOG(MAPLE, "Invalid device type %d", type);
		die("Invalid maple device type");
		break;
	}
	return nullptr;
}

struct MapleLinkVmu : public maple_sega_vmu
{
	bool cachedBlocks[256]; //!< Set to true for block that has been loaded/written
	bool userNotified = false;

	void OnSetup() override
	{
		// All data must be re-read
		memset(cachedBlocks, 0, sizeof(cachedBlocks));

		// Ensure file is not being used
		if (file != nullptr) {
			std::fclose(file);
			file = nullptr;
		}

		memset(flash_data, 0, sizeof(flash_data));
		memset(lcd_data, 0, sizeof(lcd_data));
		memset(lcd_data_decoded, 0xff, sizeof(lcd_data_decoded));
		clearPendingWrite();
		resetClockToHostLocalTime();
		fullSaveNeeded = false;
	}

	bool fullSave() override
	{
		// Skip virtual save when using MapleLink VMU
		DEBUG_LOG(MAPLE, "Full save ignored for MapleLink VMU");
		return true;
	}

	void serialize(Serializer& ser) const override {
		throw Serializer::Exception("Can't save linked VMU data");
	}

	void deserialize(Deserializer& deser) override
	{
		// Ignore the VMU data from the loaded state
		u8 savedData[sizeof(flash_data)];
		memcpy(savedData, flash_data, sizeof(savedData));
		maple_sega_vmu::deserialize(deser);
		memcpy(flash_data, savedData, sizeof(savedData));
	}

	MapleLink::Ptr getMapleLink()
	{
		MapleLink::Ptr link = MapleLink::GetMapleLink(bus_id, bus_port);
		if (link == nullptr)
			ERROR_LOG(MAPLE, "MapleLinkVmu[%s]: MapleLink is null", logical_port);
		return link;
	}

	static bool validLinkedResponse(const MapleMsg& request, const MapleMsg& response)
	{
		if (response.destAP != request.originAP || response.originAP != request.destAP)
			return false;
		switch (response.command)
		{
		case MDRS_DeviceReply:
		case MDRE_UnknownFunction:
		case MDRE_UnknownCmd:
		case MDRE_TransmitAgain:
			return response.size == 0;
		case MDRE_FileError:
		case MDRE_LCDError:
		case MDRE_ARGunError:
			return response.size == 1;
		case MDRS_DataTransfer:
			return response.size != 0;
		default:
			return false;
		}
	}

	u32 readBlock(unsigned block)
	{
		if (cachedBlocks[block])
			return MDRS_JVSNone;

		MapleLink::Ptr link = getMapleLink();
		if (link == nullptr)
			return fileError(FileErrorWrite);

		MapleMsg txMsg;
		txMsg.command = MDCF_BlockRead;
		txMsg.originAP = inMsg->originAP;
		txMsg.destAP = inMsg->destAP;
		txMsg.pushData(MFID_1_Storage);
		txMsg.pushData<u32>(block << 24); // (BE) partition #, phase, block #
		MapleMsg rxMsg;
		if (link->sendReceive(txMsg, rxMsg)
				&& validLinkedResponse(txMsg, rxMsg)
				&& rxMsg.command == MDRS_DataTransfer
				&& rxMsg.size == 130
				&& rxMsg.readData<u32>(0) == MFID_1_Storage
				&& rxMsg.readData<u32>(1) == (block << 24))
		{
			DEBUG_LOG(MAPLE, "MapleLinkVmu[%s]: read block %d", logical_port, block);
			memcpy(&flash_data[block * 4 * 128], &rxMsg.data[8], 4 * 128);
			cachedBlocks[block] = true;
		}
		else {
			ERROR_LOG(MAPLE, "Failed to read VMU %s: I/O error", logical_port);
			return fileError(FileErrorWrite);
		}
		return MDRS_JVSNone;
	}

	u32 dma(u32 cmd) override
	{
		// Physical VMU logic
		if (dma_count_in >= 4)
		{
			const u32 functionId = inMsg->readData<u32>(0);
			if (functionId == MFID_3_Clock
					&& (cmd == MDCF_GetCondition || cmd == MDCF_BlockRead || cmd == MDCF_BlockWrite))
			{
				if ((cmd == MDCF_GetCondition && dma_count_in != 4)
						|| (cmd == MDCF_BlockRead && (dma_count_in != 8 || inMsg->readData<u32>(1) != 0))
						|| (cmd == MDCF_BlockWrite && (dma_count_in != 16 || inMsg->readData<u32>(1) != 0)))
					return MDRE_TransmitAgain;

				MapleLink::Ptr link = getMapleLink();
				if (link == nullptr)
					return MDRE_TransmitAgain;
				MapleMsg response;
				if (!link->sendReceive(*inMsg, response))
				{
					ERROR_LOG(MAPLE, "MapleLinkVmu[%s]: clock command %02x failed", logical_port, cmd);
					return MDRE_TransmitAgain;
				}
				if (!validLinkedResponse(*inMsg, response))
					return MDRE_TransmitAgain;
				if (response.command == MDRS_DataTransfer)
				{
					if (cmd == MDCF_BlockWrite)
						return MDRE_TransmitAgain;
					const u8 expectedSize = cmd == MDCF_GetCondition ? 2 : 3;
					if (response.size != expectedSize
							|| response.readData<u32>() != MFID_3_Clock)
						return MDRE_TransmitAgain;
					wptr(response.data, response.getDataSize());
					return response.command;
				}
				if (cmd != MDCF_BlockWrite && response.command == MDRS_DeviceReply)
					return MDRE_TransmitAgain;
				if (cmd != MDCF_BlockWrite || response.command != MDRS_DeviceReply)
				{
					wptr(response.data, response.getDataSize());
					return response.command;
				}
				// Keep the synthetic fallback clock synchronized if the link later
				// disconnects. The validated request is consumed by the base below.
			}
			else if (functionId == MFID_3_Clock && cmd == MDCF_SetCondition)
			{
				if (dma_count_in != 8)
					return MDRE_TransmitAgain;
				MapleLink::Ptr link = getMapleLink();
				if (link == nullptr)
					return MDRE_TransmitAgain;
				MapleMsg response;
				if (!link->sendReceive(*inMsg, response))
					return MDRE_TransmitAgain;
				if (response.command == MDRS_DeviceReply)
						aica::sgc::vmuBeep(bus_id, bus_port, inMsg->data[4], inMsg->data[5]);
				else
					wptr(response.data, response.getDataSize());
				return response.command;
			}
			else if (functionId == MFID_2_LCD && (cmd == MDCF_BlockRead || cmd == MDCF_BlockWrite))
			{
				const u32 expectedSize = cmd == MDCF_BlockWrite ? 8 + sizeof(lcd_data) : 8;
				if (dma_count_in != expectedSize || inMsg->readData<u32>(1) != 0)
					return MDRE_LCDError;
				MapleLink::Ptr link = getMapleLink();
				if (link == nullptr)
					return MDRE_TransmitAgain;
				MapleMsg response;
				if (!link->sendReceive(*inMsg, response))
					return MDRE_TransmitAgain;
				if (cmd == MDCF_BlockWrite && response.command == MDRS_DeviceReply)
				{
					memcpy(lcd_data, inMsg->data + 8, sizeof(lcd_data));
					decodeLcd();
					config->SetImage(lcd_data_decoded);
				}
				else
				{
					wptr(response.data, response.getDataSize());
					if (cmd == MDCF_BlockRead && response.command == MDRS_DataTransfer
							&& response.size == 50
							&& response.readData<u32>(0) == MFID_2_LCD
							&& response.readData<u32>(1) == 0)
					{
						memcpy(lcd_data, response.data + 8, sizeof(lcd_data));
						decodeLcd();
						config->SetImage(lcd_data_decoded);
					}
				}
				return response.command;
			}

			if (functionId == MFID_1_Storage)
			{
				switch (cmd)
				{
					case MDCF_BlockWrite:
					{
						if (dma_count_in != 8 + 512 / 4)
							return MDRE_TransmitAgain;
						u32 location = SWAP32(inMsg->readData<u32>(1));
						u32 partition = location >> 24;
						u32 phase = (location >> 16) & 0xff;
						u32 block = location & 0xffff;
						if (partition != 0)
							return fileError(FileErrorInvalidPartition);
						if (phase >= 4)
							return fileError(FileErrorPhase);
						if (block >= sizeof(flash_data) / 512)
							return fileError(FileErrorInvalidBlock);
						if (phase != 0 && (pendingWriteBlock != block || phase != nextWritePhase))
							return fileError(FileErrorPhase);
						if (!userNotified)
					{
						os_notify("ATTENTION: You are saving to a physical VMU", 6000,
								"Do not disconnect the VMU or close the game");
						userNotified = true;
					}
					MapleLink::Ptr link = getMapleLink();
					if (link == nullptr)
						return fileError(FileErrorWrite);
					MapleMsg rxMsg;
					if (!link->sendReceive(*inMsg, rxMsg)) {
						ERROR_LOG(MAPLE, "Failed to write VMU %s: I/O error", logical_port);
						return fileError(FileErrorWrite);
					}
						if (!validLinkedResponse(*inMsg, rxMsg))
							return fileError(FileErrorWrite);
						if (rxMsg.command != MDRS_DeviceReply)
					{
						wptr(rxMsg.data, rxMsg.getDataSize());
						return rxMsg.command;
					}
					DEBUG_LOG(MAPLE, "MapleLinkVmu[%s]: write block %d", logical_port, inMsg->data[7]);
					break;
				}

					case MDCF_BlockRead:
					{
						if (dma_count_in != 8)
							return MDRE_TransmitAgain;
						u32 location = SWAP32(inMsg->readData<u32>(1));
						if ((location >> 24) != 0)
							return fileError(FileErrorInvalidPartition);
						if (((location >> 16) & 0xff) != 0)
							return fileError(FileErrorPhase);
						if ((location & 0xffff) >= sizeof(flash_data) / 512)
							return fileError(FileErrorInvalidBlock);
						u8 block = inMsg->data[7];
						u32 rc = readBlock(block);
					if (rc != MDRS_JVSNone)
						return rc;
					break;
				}

					case MDCF_GetMediaInfo:
					{
						if (dma_count_in != 8)
							return MDRE_TransmitAgain;
						if (inMsg->readData<u32>(1) != 0)
							return fileError(FileErrorInvalidPartition);
						// block 255 contains the media info
						u32 rc = readBlock(255);
						if (rc != MDRS_JVSNone)
							return rc;
						break;
					}

					case MDCF_GetLastError:
					{
						if (dma_count_in != 8)
							return MDRE_TransmitAgain;
						u32 location = SWAP32(inMsg->readData<u32>(1));
						if ((location >> 24) != 0)
							return fileError(FileErrorInvalidPartition);
						if ((location & 0xffff) >= sizeof(flash_data) / 512)
							return fileError(FileErrorInvalidBlock);
						if (((location >> 16) & 0xff) != 4
								|| pendingWriteBlock != (location & 0xffff)
								|| nextWritePhase != 4)
							return fileError(FileErrorPhase);
					MapleLink::Ptr link = getMapleLink();
					if (link == nullptr)
						return fileError(FileErrorWrite);
					MapleMsg response;
						if (!link->handleGetLastError(*inMsg, response)) {
						ERROR_LOG(MAPLE, "MapleLinkVmu[%s]::GetLastError: I/O error", logical_port);
							return fileError(FileErrorWrite);
						}
						if (!validLinkedResponse(*inMsg, response))
							return fileError(FileErrorWrite);
					if (response.command != MDRS_DeviceReply)
					{
						wptr(response.data, response.getDataSize());
						return response.command;
					}
					break;
				}

				default:
					// do nothing
					break;
				}
			}
		}
		u32 response = maple_sega_vmu::dma(cmd);
		if (response == MDRS_DeviceReply && cmd == MDCF_GetLastError
				&& inMsg->getDataSize() == 8
				&& inMsg->readData<u32>() == MFID_1_Storage)
			cachedBlocks[inMsg->data[7]] = true;
		return response;
	}

	bool linkStatus() override
	{
		auto link = MapleLink::GetMapleLink(bus_id, bus_port);
		if (link == nullptr)
			return false;
		return link->isConnected();
	}
};

void createMapleLinkVmu(int bus, int port)
{
	INFO_LOG(MAPLE, "MapleLinkVmu created on %d,%d", bus, port);
	auto vmu = std::make_shared<MapleLinkVmu>();
	vmu->Setup(bus, port);
}
