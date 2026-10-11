#pragma once
#include "maple_devs.h"
#include <memory>
#include <vector>

struct MapleDmaRequest
{
	u8 pattern;
	u32 destination;
	u8 bus;
	u8 port;
	bool swapWords;
	u32 hostCycles;
	std::vector<u32> data;
	u32 source = 0;
};

extern std::shared_ptr<maple_device> MapleDevices[MAPLE_PORTS][6];

void maple_Init();
void maple_Reset(bool Manual);
void maple_Term();
void maple_ReconnectDevices();
void maple_ReconnectDevice(int bus, int port);
void maple_ResetBusDevices(u32 bus);
std::shared_ptr<maple_device> maple_GetAddressedDevice(u32 bus, u32 recipient, u32& port);

void maple_vblank();
