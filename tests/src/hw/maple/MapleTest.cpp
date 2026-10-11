#include <gtest/gtest.h>

#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_if.h"

#include <sstream>

bool receiveMsg(MapleMsg& msg, std::istream& stream);

namespace {

class FunctionValidationDevice final : public maple_base
{
public:
	FunctionValidationDevice(u32 expectedFunction, u32 expectedPayloadSize)
		: expectedFunction(expectedFunction), expectedPayloadSize(expectedPayloadSize)
	{
		config = nullptr;
	}

	MapleDeviceType get_device_type() override { return MDT_None; }

	u32 dma(u32) override
	{
		return validateFunction(expectedFunction, expectedPayloadSize);
	}

private:
	u32 expectedFunction;
	u32 expectedPayloadSize;
};

class ResetTrackingDevice final : public maple_device
{
public:
	u32 RawDma(const u32 *input, u32 inputSize, u32 *) override
	{
		callCount++;
		lastCommand = inputSize >= sizeof(u32) ? input[0] & 0xff : 0;
		return 0;
	}
	MapleDeviceType get_device_type() override { return MDT_None; }

	u32 callCount = 0;
	u32 lastCommand = 0;
};

u32 validate(u32 expectedFunction, u32 expectedPayloadSize, const u32 *payload, u32 payloadSize)
{
	FunctionValidationDevice device(expectedFunction, expectedPayloadSize);
	u32 output[2] {};
	u32 outputSize = 0;
	return device.Dma(0, payload, payloadSize, output, outputSize);
}

TEST(Maple, FunctionValidationAcceptsExactPayload)
{
	const u32 payload[] = { MFID_0_Input };
	EXPECT_EQ(MDRS_JVSNone, validate(MFID_0_Input, sizeof(payload), payload, sizeof(payload)));
}

TEST(Maple, FunctionValidationRejectsWrongFunction)
{
	const u32 payload[] = { MFID_9_Mouse };
	EXPECT_EQ(MDRE_UnknownFunction, validate(MFID_0_Input, sizeof(payload), payload, sizeof(payload)));
}

TEST(Maple, FunctionValidationRejectsTruncatedAndTrailingData)
{
	const u32 payload[] = { MFID_0_Input, 0 };
	EXPECT_EQ(MDRE_TransmitAgain, validate(MFID_0_Input, 4, payload, 0));
	EXPECT_EQ(MDRE_TransmitAgain, validate(MFID_0_Input, 4, payload, sizeof(payload)));
}

u8 controllerResponse(const u32 *request, u32 requestSize)
{
	auto controller = maple_Create(MDT_SegaController);
	controller->config = nullptr;
	controller->bus_id = 0;
	u32 response[8] {};
	controller->RawDma(request, requestSize, response);
	return response[0] & 0xff;
}

TEST(Maple, ControllerRejectsMalformedConditionRequests)
{
	const u32 truncated[] = { MDCF_GetCondition };
	EXPECT_EQ(MDRE_TransmitAgain, controllerResponse(truncated, sizeof(truncated)));

	const u32 wrongFunction[] = { MDCF_GetCondition, MFID_9_Mouse };
	EXPECT_EQ(MDRE_UnknownFunction, controllerResponse(wrongFunction, sizeof(wrongFunction)));

	const u32 trailingData[] = { MDCF_GetCondition, MFID_0_Input, 0 };
	EXPECT_EQ(MDRE_TransmitAgain, controllerResponse(trailingData, sizeof(trailingData)));
}

TEST(Maple, ControllerRejectsPayloadOnStandardNoPayloadCommands)
{
	const u32 commands[] = {
		MDC_DeviceRequest,
		MDC_AllStatusReq,
		MDC_DeviceReset,
		MDC_DeviceKill,
	};
	for (u32 command : commands)
	{
		const u32 request[] = { command, 0 };
		EXPECT_EQ(MDRE_TransmitAgain, controllerResponse(request, sizeof(request)));
	}
}

TEST(Maple, VmuRejectsOutOfRangeGetLastErrorPhase)
{
	auto vmu = maple_Create(MDT_SegaVMU);
	vmu->config = nullptr;
	vmu->bus_id = 0;
	vmu->bus_port = 0;
	const u32 request[] = {
		MDCF_GetLastError,
		MFID_1_Storage,
		SWAP32(4 << 16),
	};
	u32 response[3] {};
	EXPECT_EQ(8u, vmu->RawDma(request, sizeof(request), response));
	EXPECT_EQ(MDRE_FileError, response[0] & 0xff);
	EXPECT_EQ(0x02000000u, response[1]);
}

TEST(Maple, MessageDataUsesWordSizedLength)
{
	MapleMsg message;
	message.pushData<u32>(0x12345678);
	const u8 tail[] = { 0xaa, 0xbb, 0xcc };
	message.pushData(tail, sizeof(tail));

	EXPECT_EQ(2, message.size);
	EXPECT_EQ(8u, message.getDataSize());
	EXPECT_EQ(0x12345678u, message.readData<u32>());
	EXPECT_EQ(0xaa, message.data[4]);
	EXPECT_EQ(0xbb, message.data[5]);
	EXPECT_EQ(0xcc, message.data[6]);
	EXPECT_EQ(0, message.data[7]);
}

TEST(Maple, ParaParaRejectsTruncatedChallenge)
{
	auto controller = maple_Create(MDT_DreamParaParaController);
	u32 response[8] {};
	const u32 truncated[] = { 0 };
	EXPECT_EQ(0u, controller->RawDma(truncated, sizeof(truncated), response));

	const u32 deviceRequest[] = { 0x08000000 };
	EXPECT_EQ(28u, controller->RawDma(deviceRequest, sizeof(deviceRequest), response));
}

TEST(Maple, NewVibrationPackReportsIdleCondition)
{
	auto vibration = maple_Create(MDT_PurupuruPack);
	const u32 request[] = { MDCF_GetCondition, MFID_8_Vibration };
	u32 response[4] {};
	ASSERT_EQ(12u, vibration->RawDma(request, sizeof(request), response));
	EXPECT_EQ(MDRS_DataTransfer, response[0] & 0xff);
	EXPECT_EQ(MFID_8_Vibration, response[1]);
	EXPECT_EQ(0u, response[2]);
}

TEST(Maple, VibrationPackReportsCapturedExtendedStatus)
{
	auto vibration = maple_Create(MDT_PurupuruPack);
	const u32 request[] = { MDC_AllStatusReq };
	u32 response[64] {};
	ASSERT_EQ(196u, vibration->RawDma(request, sizeof(request), response));
	EXPECT_EQ(MDRS_DeviceStatusAll, response[0] & 0xff);
	EXPECT_EQ(48u, response[0] >> 24);

	static constexpr char expected[] =
			"Version 1.000,1998/11/10,315-6211-AH   ,Vibration Motor:1 , Fm:4 - 30Hz ,Pow:7  ";
	static_assert(sizeof(expected) - 1 == 80);
	EXPECT_EQ(0, memcmp(reinterpret_cast<const u8 *>(&response[1]) + 112,
			expected, sizeof(expected) - 1));
}

TEST(Maple, VibrationPackPreservesRawConditionWord)
{
	auto vibration = maple_Create(MDT_PurupuruPack);
	vibration->config = nullptr;
	const u32 effect = 0x2617a010;
	const u32 setRequest[] = { MDCF_SetCondition, MFID_8_Vibration, effect };
	u32 response[4] {};
	ASSERT_EQ(4u, vibration->RawDma(setRequest, sizeof(setRequest), response));
	EXPECT_EQ(MDRS_DeviceReply, response[0] & 0xff);

	const u32 getRequest[] = { MDCF_GetCondition, MFID_8_Vibration };
	ASSERT_EQ(12u, vibration->RawDma(getRequest, sizeof(getRequest), response));
	EXPECT_EQ(MDRS_DataTransfer, response[0] & 0xff);
	EXPECT_EQ(effect, response[2]);
}

TEST(Maple, KeyboardReportsCapturedDeviceStatus)
{
	auto keyboard = maple_Create(MDT_Keyboard);
	const u32 request[] = { MDC_AllStatusReq };
	u32 response[64] {};
	ASSERT_EQ(156u, keyboard->RawDma(request, sizeof(request), response));
	EXPECT_EQ(MDRS_DeviceStatusAll, response[0] & 0xff);
	EXPECT_EQ(38u, response[0] >> 24);

	const u8 *status = reinterpret_cast<const u8 *>(&response[1]);
	EXPECT_EQ(0, memcmp(status + 18, "Dreamcast Keyboard            ", 30));
	EXPECT_EQ(0x5e, status[108]);
	EXPECT_EQ(0x01, status[109]);
	EXPECT_EQ(0xf4, status[110]);
	EXPECT_EQ(0x01, status[111]);
	static constexpr char expected[] =
			"Version 1.000,1999/09/29,Mad Catz Inc.  ";
	static_assert(sizeof(expected) - 1 == 40);
	EXPECT_EQ(0, memcmp(status + 112, expected, sizeof(expected) - 1));
}

TEST(Maple, VmuReportsCapturedFirmwareStatus)
{
	auto vmu = maple_Create(MDT_SegaVMU);
	const u32 request[] = { MDC_AllStatusReq };
	u32 response[64] {};
	ASSERT_EQ(196u, vmu->RawDma(request, sizeof(request), response));
	EXPECT_EQ(MDRS_DeviceStatusAll, response[0] & 0xff);
	EXPECT_EQ(48u, response[0] >> 24);

	static constexpr char expected[] =
			"Version 1.005,1999/10/26,315-6208-05,SEGA Visual Memory System BIOS Produced by ";
	static_assert(sizeof(expected) - 1 == 80);
	EXPECT_EQ(0, memcmp(reinterpret_cast<const u8 *>(&response[1]) + 112,
			expected, sizeof(expected) - 1));
}

TEST(Maple, JvsBridgeRejectsMalformedMapleFrames)
{
	auto jvs = MIE::Create();
	u32 response[8] {};
	const u32 shortFrame[] = { MDC_JVSCommand };
	EXPECT_EQ(0u, jvs->RawDma(shortFrame, 3, response));

	const u32 standardWithPayload[] = { MDC_DeviceRequest, 0 };
	ASSERT_EQ(4u, jvs->RawDma(standardWithPayload, sizeof(standardWithPayload), response));
	EXPECT_EQ(MDRE_TransmitAgain, response[0] & 0xff);

	ASSERT_EQ(4u, jvs->RawDma(shortFrame, sizeof(shortFrame), response));
	EXPECT_EQ(MDRE_TransmitAgain, response[0] & 0xff);

	const u32 truncatedTransfer[] = { MDC_JVSCommand, 0x17 };
	ASSERT_EQ(4u, jvs->RawDma(truncatedTransfer, sizeof(truncatedTransfer), response));
	EXPECT_EQ(MDRE_TransmitAgain, response[0] & 0xff);

	const u32 truncatedFirmware[] = { MDC_JVSUploadFirmware, 0 };
	ASSERT_EQ(4u, jvs->RawDma(truncatedFirmware, sizeof(truncatedFirmware), response));
	EXPECT_EQ(MDRE_TransmitAgain, response[0] & 0xff);
}

TEST(Maple, JvsBridgeUsesConfiguredSourceAddress)
{
	auto jvs = MIE::Create();
	jvs->Setup(0, 5);
	u32 response[4] {};
	// DE selects the main device; the LM bits in this multicast-form address
	// must not be echoed as though they were part of the bridge's own AP.
	const u32 request[] = { MDC_DeviceReset | (0x23u << 8) };

	ASSERT_EQ(4u, jvs->RawDma(request, sizeof(request), response));
	EXPECT_EQ(MDRS_DeviceReply, response[0] & 0xff);
	EXPECT_EQ(0x20u, (response[0] >> 16) & 0xff);
}

TEST(Maple, RfidBridgeRejectsMalformedCommands)
{
	auto rfid = RFIDReaderWriter::Create();
	u32 response[8] { 0xdeadbeef };
	const u32 shortFrame[] = { MDCF_GetCondition };
	EXPECT_EQ(0u, rfid->RawDma(shortFrame, 3, response));
	EXPECT_EQ(0xdeadbeefu, response[0]);

	ASSERT_EQ(4u, rfid->RawDma(shortFrame, sizeof(shortFrame), response));
	EXPECT_EQ(MDRE_TransmitAgain, response[0] & 0xff);

	const u32 wrongFunction[] = { MDCF_GetCondition, MFID_0_Input };
	ASSERT_EQ(4u, rfid->RawDma(wrongFunction, sizeof(wrongFunction), response));
	EXPECT_EQ(MDRE_UnknownFunction, response[0] & 0xff);

	const u32 outOfRangeWrite[] = { 0xb1, 0x00000140, 0 };
	ASSERT_EQ(4u, rfid->RawDma(outOfRangeWrite, sizeof(outOfRangeWrite), response));
	EXPECT_EQ(MDRE_TransmitAgain, response[0] & 0xff);

	const u32 truncatedDecrement[] = { 0xd1 };
	ASSERT_EQ(4u, rfid->RawDma(truncatedDecrement, sizeof(truncatedDecrement), response));
	EXPECT_EQ(MDRE_TransmitAgain, response[0] & 0xff);
}

TEST(Maple, RfidBridgeFramesErrorsForItsConfiguredBus)
{
	auto rfid = RFIDReaderWriter::Create();
	rfid->Setup(2, 5);
	const u32 recipient = 0xa0;
	const u32 sender = 0x80;
	const u32 request[] = { 0x55u | (recipient << 8) | (sender << 16) };
	u32 response[4] {};

	ASSERT_EQ(4u, rfid->RawDma(request, sizeof(request), response));
	EXPECT_EQ(MDRE_UnknownCmd, response[0] & 0xff);
	EXPECT_EQ(sender, (response[0] >> 8) & 0xff);
	EXPECT_EQ(recipient, (response[0] >> 16) & 0xff);
}

TEST(Maple, DreamPotatoParsesExactMapleMessages)
{
	MapleMsg message;
	std::istringstream emptyReply("07 00 20 00\r\n");
	ASSERT_TRUE(receiveMsg(message, emptyReply));
	EXPECT_EQ(MDRS_DeviceReply, message.command);
	EXPECT_EQ(0, message.size);

	std::istringstream dataReply("08 00 20 01 01 02 A0 ff\r\n");
	ASSERT_TRUE(receiveMsg(message, dataReply));
	ASSERT_EQ(1, message.size);
	EXPECT_EQ(0x01, message.data[0]);
	EXPECT_EQ(0x02, message.data[1]);
	EXPECT_EQ(0xa0, message.data[2]);
	EXPECT_EQ(0xff, message.data[3]);

	std::istringstream truncated("08 00 20 01 01 02 A0\r\n");
	EXPECT_FALSE(receiveMsg(message, truncated));
	std::istringstream trailing("07 00 20 00 FF\r\n");
	EXPECT_FALSE(receiveMsg(message, trailing));
}

TEST(Maple, MouseReturnsCanonicalConditionLayout)
{
	auto mouse = maple_Create(MDT_Mouse);
	mouse->bus_id = 0;
	mouse->player_num = 0;
	mouse->config = new MapleConfigMap(mouse.get());

	mapleInputState[0].mouseButtons = 0xfa;
	mapleInputState[0].relPos.x = -600;
	mapleInputState[0].relPos.y = 17;
	mapleInputState[0].relPos.wheel = 600;
	const u32 request[] = { MDCF_GetCondition, MFID_9_Mouse };
	u32 response[8] {};
	ASSERT_EQ(28u, mouse->RawDma(request, sizeof(request), response));
	EXPECT_EQ(MDRS_DataTransfer, response[0] & 0xff);
	EXPECT_EQ(6u, response[0] >> 24);
	EXPECT_EQ(MFID_9_Mouse, response[1]);

	const u8 *condition = reinterpret_cast<const u8 *>(&response[2]);
	u16 axis[8];
	memcpy(axis, condition + 4, sizeof(axis));
	EXPECT_EQ(0xfa, condition[0]);
	EXPECT_EQ(0, condition[1]);
	EXPECT_EQ(0, condition[2]);
	EXPECT_EQ(0, condition[3]);
	EXPECT_EQ(0, axis[0]);
	EXPECT_EQ(0x211, axis[1]);
	EXPECT_EQ(0x3ff, axis[2]);
	for (int i = 3; i < 8; i++)
		EXPECT_EQ(0x200, axis[i]);

	ASSERT_EQ(28u, mouse->RawDma(request, sizeof(request), response));
	memcpy(axis, reinterpret_cast<const u8 *>(&response[2]) + 4, sizeof(axis));
	EXPECT_EQ(0x200, axis[0]);
	EXPECT_EQ(0x200, axis[1]);
	EXPECT_EQ(0x200, axis[2]);
}

TEST(Maple, BusResetReachesEachDistinctDeviceOnce)
{
	constexpr u32 bus = 3;
	auto mainDevice = std::make_shared<ResetTrackingDevice>();
	auto subDevice = std::make_shared<ResetTrackingDevice>();
	MapleDevices[bus][5] = mainDevice;
	MapleDevices[bus][0] = subDevice;
	MapleDevices[bus][1] = subDevice;

	maple_ResetBusDevices(bus);

	EXPECT_EQ(1u, mainDevice->callCount);
	EXPECT_EQ(MDC_DeviceReset, mainDevice->lastCommand);
	EXPECT_EQ(1u, subDevice->callCount);
	EXPECT_EQ(MDC_DeviceReset, subDevice->lastCommand);
	for (auto& device : MapleDevices[bus])
		device.reset();
}

TEST(Maple, AddressingDoesNotAliasMissingSubunitToMainDevice)
{
	constexpr u32 bus = 2;
	auto mainDevice = std::make_shared<ResetTrackingDevice>();
	auto subDevice = std::make_shared<ResetTrackingDevice>();
	MapleDevices[bus][5] = mainDevice;
	MapleDevices[bus][1] = subDevice;
	u32 port;

	EXPECT_EQ(mainDevice, maple_GetAddressedDevice(bus, (bus << 6) | 0x20, port));
	EXPECT_EQ(5u, port);
	EXPECT_EQ(mainDevice, maple_GetAddressedDevice(bus, (bus << 6) | 0x22, port));
	EXPECT_EQ(5u, port);
	EXPECT_EQ(subDevice, maple_GetAddressedDevice(bus, (bus << 6) | 0x02, port));
	EXPECT_EQ(1u, port);
	EXPECT_EQ(nullptr, maple_GetAddressedDevice(bus, (bus << 6) | 0x01, port));
	EXPECT_EQ(0u, port);
	EXPECT_EQ(nullptr, maple_GetAddressedDevice(bus, ((bus ^ 1) << 6) | 0x20, port));
	EXPECT_EQ(6u, port);
	for (auto& device : MapleDevices[bus])
		device.reset();
}

TEST(Maple, MulticastAddressRequiresExactlyOneResponder)
{
	constexpr u32 bus = 3;
	auto first = std::make_shared<ResetTrackingDevice>();
	auto second = std::make_shared<ResetTrackingDevice>();
	u32 port;

	MapleDevices[bus][0] = first;
	EXPECT_EQ(first, maple_GetAddressedDevice(bus, (bus << 6) | 0x03, port));
	EXPECT_EQ(0u, port); // The other selected slot is empty and remains silent.

	MapleDevices[bus][1] = second;
	EXPECT_EQ(nullptr, maple_GetAddressedDevice(bus, (bus << 6) | 0x03, port));
	EXPECT_EQ(6u, port); // Two responders collide; neither is chosen arbitrarily.

	for (auto& device : MapleDevices[bus])
		device.reset();
}

TEST(Maple, MulticastResponderUsesItsOwnSourceAddress)
{
	constexpr u32 bus = 3;
	auto vmu = maple_Create(MDT_SegaVMU);
	vmu->Setup(bus, 0);
	const u32 recipient = (bus << 6) | 0x03;
	const u32 sender = bus << 6;
	const u32 request[] = { MDC_DeviceRequest | (recipient << 8) | (sender << 16) };
	u32 response[32] {};

	ASSERT_EQ(116u, vmu->RawDma(request, sizeof(request), response));
	EXPECT_EQ(sender, (response[0] >> 8) & 0xff);
	EXPECT_EQ((bus << 6) | 0x01, (response[0] >> 16) & 0xff);
}

TEST(Maple, MicrophoneSwitchesActiveSampleRateAndResetsState)
{
	auto microphone = maple_Create(MDT_Microphone);
	u32 response[8] {};
	auto control = [&](u8 subcommand, u8 data) {
		const u32 request[] = {
			MDCF_MICControl | (2u << 24),
			MFID_4_Mic,
			u32(subcommand) | (u32(data) << 8),
		};
		return microphone->RawDma(request, sizeof(request), response);
	};

	ASSERT_EQ(4u, control(0x02, 0x80)); // sampling at 11.025 kHz
	EXPECT_EQ(MDRS_DeviceReply, response[0] & 0xff);
	ASSERT_EQ(4u, control(0x02, 0x84)); // remain active, switch to 8 kHz
	EXPECT_EQ(MDRS_DeviceReply, response[0] & 0xff);
	ASSERT_EQ(12u, control(0x01, 0x0f));
	EXPECT_EQ(MDRS_DataTransfer, response[0] & 0xff);
	EXPECT_EQ(MFID_4_Mic, response[1]);
	EXPECT_EQ(0x05u, response[2] & 0xff); // SMPL and 8 kHz flags
	EXPECT_EQ(0u, response[2] >> 24);     // no samples without a backend

	ASSERT_EQ(4u, control(0x02, 0x00)); // KOS stop request
	EXPECT_EQ(MDRS_DeviceReply, response[0] & 0xff);
	ASSERT_EQ(12u, control(0x01, 0x0f));
	EXPECT_EQ(0u, response[2] & 0xff);
	EXPECT_EQ(0u, response[2] >> 24);

	const u32 reset[] = { MDC_DeviceReset };
	ASSERT_EQ(4u, microphone->RawDma(reset, sizeof(reset), response));
	ASSERT_EQ(12u, control(0x01, 0x0f));
	EXPECT_EQ(0u, response[2] & 0xff);

	const u32 truncated[] = { MDCF_MICControl, MFID_4_Mic };
	ASSERT_EQ(4u, microphone->RawDma(truncated, sizeof(truncated), response));
	EXPECT_EQ(MDRE_TransmitAgain, response[0] & 0xff);
}

} // namespace
