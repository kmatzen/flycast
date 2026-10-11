#include "gtest/gtest.h"

#include "cfg/option.h"
#include "hw/maple/maple_cfg.h"
#include "hw/maple/maple_devs.h"
#include "hw/maple/maple_if.h"
#include "input/maplelink.h"
#include "rend/osd.h"
#include "serialize.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace
{

class VmuTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
		tempDir = std::filesystem::temp_directory_path() / ("flycast-vmu-test-" + std::to_string(suffix));
		std::filesystem::create_directories(tempDir);
		oldVmuPath = config::VMUPath.get();
		config::VMUPath = tempDir.string();
		oldInputKcode = mapleInputState[0].kcode;
		mapleInputState[0].kcode = ~0u;
		device = maple_Create(MDT_SegaVMU);
		device->Setup(0, 0);
	}

	void TearDown() override
	{
		MapleDevices[0][0].reset();
		device.reset();
		config::VMUPath = oldVmuPath;
		mapleInputState[0].kcode = oldInputKcode;
		std::error_code error;
		std::filesystem::remove_all(tempDir, error);
	}

	template<size_t N>
	u32 dma(u8 command, const std::array<u32, N>& payload, std::array<u32, 256>& output)
	{
		std::array<u32, N + 1> input {};
		input[0] = command | (1 << 8) | (0x20 << 16) | (N << 24);
		std::copy(payload.begin(), payload.end(), input.begin() + 1);
		return device->RawDma(input.data(), input.size() * sizeof(u32), output.data());
	}

	std::shared_ptr<maple_device> device;
	std::filesystem::path tempDir;
	std::string oldVmuPath;
	u32 oldInputKcode = ~0u;
};

class TestMapleLink : public BaseMapleLink
{
public:
	TestMapleLink() : BaseMapleLink(true)
	{
		nextResponse.command = MDRS_DeviceReply;
	}
	~TestMapleLink() override
	{
		if (attached)
			unregisterLink(0, 0);
	}

	void attach()
	{
		registerLink(0, 0);
		attached = true;
	}

	bool send(const MapleMsg&) override { return true; }
	bool sendReceive(const MapleMsg& request, MapleMsg& response) override
	{
		lastCommand = request.command;
		response = nextResponse;
		return transferSucceeds;
	}
	bool isConnected() override { return true; }

	u8 lastCommand = 0;
	MapleMsg nextResponse {};
	bool transferSucceeds = true;
	bool attached = false;
};

TEST(VmuMapleLinkTest, BlockSyncIsForwardedAndRequiresAcknowledgement)
{
	TestMapleLink link;
	MapleMsg sync;
	sync.command = MDCF_GetLastError;
	sync.pushData(MFID_1_Storage);
	sync.pushData<u32>(4 << 8);
	MapleMsg response;

	EXPECT_TRUE(link.handleGetLastError(sync, response));
	EXPECT_EQ(MDCF_GetLastError, link.lastCommand);
	EXPECT_EQ(MDRS_DeviceReply, response.command);

	link.nextResponse.command = MDRE_FileError;
	EXPECT_TRUE(link.handleGetLastError(sync, response));
	EXPECT_EQ(MDRE_FileError, response.command);
	link.transferSucceeds = false;
	EXPECT_FALSE(link.handleGetLastError(sync, response));
}

TEST(VmuMapleLinkTest, PhysicalClockConditionIsForwarded)
{
	auto link = std::make_shared<TestMapleLink>();
	link->attach();
	link->nextResponse.command = MDRS_DataTransfer;
	link->nextResponse.pushData(MFID_3_Clock);
	link->nextResponse.pushData<u32>(0x000000ef); // A pressed, active low
	createMapleLinkVmu(0, 0);
	auto device = MapleDevices[0][0];
	ASSERT_NE(nullptr, device);

	std::array<u32, 2> request {
		MDCF_GetCondition | (1 << 8) | (0x20 << 16) | (1 << 24),
		MFID_3_Clock,
	};
	std::array<u32, 8> output {};
	ASSERT_EQ(12u, device->RawDma(request.data(), sizeof(request), output.data()));
	EXPECT_EQ(MDRS_DataTransfer, output[0] & 0xff);
	EXPECT_EQ(MFID_3_Clock, output[1]);
	EXPECT_EQ(0x000000efu, output[2]);
	EXPECT_EQ(MDCF_GetCondition, link->lastCommand);

	MapleDevices[0][0].reset();
}

TEST(VmuMapleLinkTest, PhysicalLcdWriteWaitsForAckAndMirrorsImage)
{
	auto link = std::make_shared<TestMapleLink>();
	link->attach();
	createMapleLinkVmu(0, 0);
	auto device = MapleDevices[0][0];
	ASSERT_NE(nullptr, device);

	std::array<u32, 51> request {};
	request[0] = MDCF_BlockWrite | (1 << 8) | (0x20 << 16) | (50 << 24);
	request[1] = MFID_2_LCD;
	reinterpret_cast<u8 *>(request.data() + 3)[0] = 0x80;
	std::array<u32, 8> output {};
	ASSERT_EQ(4u, device->RawDma(request.data(), sizeof(request), output.data()));
	EXPECT_EQ(MDRS_DeviceReply, output[0] & 0xff);
	EXPECT_EQ(MDCF_BlockWrite, link->lastCommand);
#ifndef LIBRETRO
	EXPECT_EQ(0xff000000u, vmu_lcd_data[0][0]);
	EXPECT_EQ(0xffffffffu, vmu_lcd_data[0][1]);
#endif

	MapleDevices[0][0].reset();
}

TEST_F(VmuTest, LcdWriteCanBeReadBack)
{
	std::array<u32, 50> writePayload {};
	writePayload[0] = MFID_2_LCD;
	writePayload[1] = 0;
	for (size_t i = 0; i < 48; i++)
		writePayload[i + 2] = 0x01020408u * (i + 1);

	std::array<u32, 256> output {};
	ASSERT_EQ(4u, dma(MDCF_BlockWrite, writePayload, output));
	EXPECT_EQ(MDRS_DeviceReply, output[0] & 0xff);

	std::array<u32, 2> readPayload { MFID_2_LCD, 0 };
	ASSERT_EQ(204u, dma(MDCF_BlockRead, readPayload, output));
	EXPECT_EQ(MDRS_DataTransfer, output[0] & 0xff);
	EXPECT_EQ(MFID_2_LCD, output[1]);
	EXPECT_EQ(0u, output[2]);
	EXPECT_EQ(0, std::memcmp(writePayload.data() + 2, output.data() + 3, 192));
}

TEST_F(VmuTest, LcdMapsMostSignificantBitToTopLeftPixel)
{
	std::array<u32, 50> payload {};
	payload[0] = MFID_2_LCD;
	payload[1] = 0;
	reinterpret_cast<u8 *>(payload.data() + 2)[0] = 0x80;

	std::array<u32, 256> output {};
	ASSERT_EQ(4u, dma(MDCF_BlockWrite, payload, output));
#ifndef LIBRETRO
	EXPECT_EQ(0xff000000u, vmu_lcd_data[0][0]);
	EXPECT_EQ(0xffffffffu, vmu_lcd_data[0][1]);
	EXPECT_EQ(0xffffffffu, vmu_lcd_data[0][47]);
#endif
}

TEST_F(VmuTest, SaveStateRestoresBlankLcd)
{
	std::vector<u8> blankState(140_KB);
	Serializer serializer(blankState.data(), blankState.size());
	device->serialize(serializer);

	std::array<u32, 50> payload {};
	payload[0] = MFID_2_LCD;
	payload[1] = 0;
	reinterpret_cast<u8 *>(payload.data() + 2)[0] = 0x80;
	std::array<u32, 256> output {};
	ASSERT_EQ(4u, dma(MDCF_BlockWrite, payload, output));
#ifndef LIBRETRO
	ASSERT_EQ(0xff000000u, vmu_lcd_data[0][0]);
#endif

	Deserializer deserializer(blankState.data(), serializer.size());
	device->deserialize(deserializer);
#ifndef LIBRETRO
	for (u32 pixel : vmu_lcd_data[0])
		EXPECT_EQ(0xffffffffu, pixel);
#endif

	std::array<u32, 2> readPayload { MFID_2_LCD, 0 };
	ASSERT_EQ(204u, dma(MDCF_BlockRead, readPayload, output));
	for (size_t i = 3; i < 51; i++)
		EXPECT_EQ(0u, output[i]);
}

TEST_F(VmuTest, StorageRejectsInvalidReadBlockWithoutWrapping)
{
	// Location words are byte-oriented on the Maple wire: partition, phase,
	// block high, block low. Block 256 must not alias block zero.
	std::array<u32, 2> payload { MFID_1_Storage, 0x00010000 };
	std::array<u32, 256> output {};
	ASSERT_EQ(8u, dma(MDCF_BlockRead, payload, output));
	EXPECT_EQ(MDRE_FileError, output[0] & 0xff);
	EXPECT_EQ(0x04000000u, output[1]);
}

TEST_F(VmuTest, StorageRequiresFourExactWritePhases)
{
	std::array<u32, 33> payload {};
	payload[0] = MFID_1_Storage;
	std::array<u32, 256> output {};

	payload[1] = 0x00000400; // phase 4 after wire-byte reversal
	ASSERT_EQ(8u, dma(MDCF_BlockWrite, payload, output));
	EXPECT_EQ(MDRE_FileError, output[0] & 0xff);
	EXPECT_EQ(0x02000000u, output[1]);

	std::array<u32, 32> shortPayload {};
	shortPayload[0] = MFID_1_Storage;
	shortPayload[1] = 0;
	ASSERT_EQ(8u, dma(MDCF_BlockWrite, shortPayload, output));
	EXPECT_EQ(MDRE_FileError, output[0] & 0xff);
	EXPECT_EQ(0x10000000u, output[1]);
}

TEST_F(VmuTest, StorageCommitsCompleteBlockOnPhaseFourSync)
{
	constexpr u32 block = 42;
	constexpr u32 wireBlock = block << 24;
	std::array<u32, 256> output {};
	std::array<u8, 512> original {};
	std::array<u32, 2> readPayload { MFID_1_Storage, wireBlock };
	ASSERT_EQ(524u, dma(MDCF_BlockRead, readPayload, output));
	std::memcpy(original.data(), output.data() + 3, original.size());

	std::array<u8, 512> replacement {};
	for (size_t i = 0; i < replacement.size(); i++)
		replacement[i] = static_cast<u8>(i ^ 0xa5);
	for (u32 phase = 0; phase < 4; phase++)
	{
		std::array<u32, 34> writePayload {};
		writePayload[0] = MFID_1_Storage;
		writePayload[1] = wireBlock | (phase << 8);
		std::memcpy(writePayload.data() + 2, replacement.data() + phase * 128, 128);
		ASSERT_EQ(4u, dma(MDCF_BlockWrite, writePayload, output));
		ASSERT_EQ(MDRS_DeviceReply, output[0] & 0xff);
	}

	// The VMU's 512-byte staging buffer is not committed to flash until the
	// phase-four block-sync command.
	ASSERT_EQ(524u, dma(MDCF_BlockRead, readPayload, output));
	EXPECT_EQ(0, std::memcmp(original.data(), output.data() + 3, original.size()));

	std::array<u32, 2> syncPayload { MFID_1_Storage, wireBlock | (4 << 8) };
	ASSERT_EQ(4u, dma(MDCF_GetLastError, syncPayload, output));
	EXPECT_EQ(MDRS_DeviceReply, output[0] & 0xff);
	ASSERT_EQ(524u, dma(MDCF_BlockRead, readPayload, output));
	EXPECT_EQ(0, std::memcmp(replacement.data(), output.data() + 3, replacement.size()));
}

TEST_F(VmuTest, StorageSyncFlushesCommittedBlockToDisk)
{
	constexpr u32 block = 44;
	constexpr u32 wireBlock = block << 24;
	std::array<u8, 512> replacement {};
	for (size_t i = 0; i < replacement.size(); i++)
		replacement[i] = static_cast<u8>(0x5a ^ i);

	std::array<u32, 256> output {};
	for (u32 phase = 0; phase < 4; phase++)
	{
		std::array<u32, 34> payload {};
		payload[0] = MFID_1_Storage;
		payload[1] = wireBlock | (phase << 8);
		std::memcpy(payload.data() + 2, replacement.data() + phase * 128, 128);
		ASSERT_EQ(4u, dma(MDCF_BlockWrite, payload, output));
	}
	std::array<u32, 2> syncPayload { MFID_1_Storage, wireBlock | (4 << 8) };
	ASSERT_EQ(4u, dma(MDCF_GetLastError, syncPayload, output));

	// The command acknowledgement is the commit boundary. The backing image
	// must be externally visible even while the VMU object still owns the file.
	std::ifstream image(tempDir / "vmu_save_A1.bin", std::ios::binary);
	ASSERT_TRUE(image.good());
	image.seekg(block * 512);
	std::array<u8, 512> diskBlock {};
	image.read(reinterpret_cast<char *>(diskBlock.data()), diskBlock.size());
	ASSERT_EQ(static_cast<std::streamsize>(diskBlock.size()), image.gcount());
	EXPECT_EQ(replacement, diskBlock);
}

TEST_F(VmuTest, SaveStatePreservesPartialStorageWrite)
{
	constexpr u32 block = 43;
	constexpr u32 wireBlock = block << 24;
	std::array<u8, 512> replacement {};
	for (size_t i = 0; i < replacement.size(); i++)
		replacement[i] = static_cast<u8>(i * 3 + 7);

	std::array<u32, 256> output {};
	std::array<u32, 34> phasePayload {};
	phasePayload[0] = MFID_1_Storage;
	phasePayload[1] = wireBlock;
	std::memcpy(phasePayload.data() + 2, replacement.data(), 128);
	ASSERT_EQ(4u, dma(MDCF_BlockWrite, phasePayload, output));
	ASSERT_EQ(MDRS_DeviceReply, output[0] & 0xff);

	std::vector<u8> state(140_KB);
	Serializer serializer(state.data(), state.size());
	device->serialize(serializer);

	// Disturb the staging state so this test proves deserialize restores it,
	// rather than merely continuing the live partial write.
	phasePayload[1] = (block + 1) << 24;
	ASSERT_EQ(4u, dma(MDCF_BlockWrite, phasePayload, output));
	Deserializer deserializer(state.data(), serializer.size());
	device->deserialize(deserializer);

	for (u32 phase = 1; phase < 4; phase++)
	{
		phasePayload[1] = wireBlock | (phase << 8);
		std::memcpy(phasePayload.data() + 2, replacement.data() + phase * 128, 128);
		ASSERT_EQ(4u, dma(MDCF_BlockWrite, phasePayload, output));
		ASSERT_EQ(MDRS_DeviceReply, output[0] & 0xff);
	}

	std::array<u32, 2> syncPayload { MFID_1_Storage, wireBlock | (4 << 8) };
	ASSERT_EQ(4u, dma(MDCF_GetLastError, syncPayload, output));
	ASSERT_EQ(MDRS_DeviceReply, output[0] & 0xff);

	std::array<u32, 2> readPayload { MFID_1_Storage, wireBlock };
	ASSERT_EQ(524u, dma(MDCF_BlockRead, readPayload, output));
	EXPECT_EQ(0, std::memcmp(replacement.data(), output.data() + 3, replacement.size()));
}

TEST_F(VmuTest, StorageRejectsOutOfOrderPhases)
{
	std::array<u32, 34> payload {};
	payload[0] = MFID_1_Storage;
	payload[1] = 7u << 24; // block 7, phase 0
	std::array<u32, 256> output {};
	ASSERT_EQ(4u, dma(MDCF_BlockWrite, payload, output));

	payload[1] |= 2u << 8; // skip phase 1
	ASSERT_EQ(8u, dma(MDCF_BlockWrite, payload, output));
	EXPECT_EQ(MDRE_FileError, output[0] & 0xff);
	EXPECT_EQ(0x02000000u, output[1]);
}

TEST_F(VmuTest, ResetDiscardsPendingStorageWrite)
{
	constexpr u32 wireBlock = 9u << 24;
	std::array<u32, 34> payload {};
	payload[0] = MFID_1_Storage;
	std::array<u32, 256> output {};
	for (u32 phase = 0; phase < 4; phase++)
	{
		payload[1] = wireBlock | (phase << 8);
		ASSERT_EQ(4u, dma(MDCF_BlockWrite, payload, output));
	}

	std::array<u32, 0> noPayload {};
	ASSERT_EQ(4u, dma(MDC_DeviceReset, noPayload, output));
	std::array<u32, 2> syncPayload { MFID_1_Storage, wireBlock | (4 << 8) };
	ASSERT_EQ(8u, dma(MDCF_GetLastError, syncPayload, output));
	EXPECT_EQ(MDRE_FileError, output[0] & 0xff);
	EXPECT_EQ(0x02000000u, output[1]);
}

TEST_F(VmuTest, MediaInfoRequiresRootBlockFormatSignature)
{
	constexpr u32 wireRootBlock = 0xff000000;
	std::array<u8, 512> malformedRoot {};
	// This used to fool the single-field format check while exposing the
	// deliberately invalid partition value that follows it.
	malformedRoot[0x40] = 0xff;
	malformedRoot[0x42] = 0x77;
	malformedRoot[0x43] = 0x77;

	std::array<u32, 256> output {};
	for (u32 phase = 0; phase < 4; phase++)
	{
		std::array<u32, 34> writePayload {};
		writePayload[0] = MFID_1_Storage;
		writePayload[1] = wireRootBlock | (phase << 8);
		std::memcpy(writePayload.data() + 2, malformedRoot.data() + phase * 128, 128);
		ASSERT_EQ(4u, dma(MDCF_BlockWrite, writePayload, output));
	}
	std::array<u32, 2> syncPayload { MFID_1_Storage, wireRootBlock | (4 << 8) };
	ASSERT_EQ(4u, dma(MDCF_GetLastError, syncPayload, output));

	std::array<u32, 2> mediaPayload { MFID_1_Storage, 0 };
	ASSERT_EQ(32u, dma(MDCF_GetMediaInfo, mediaPayload, output));
	ASSERT_EQ(MDRS_DataTransfer, output[0] & 0xff);
	const u8 *media = reinterpret_cast<const u8 *>(output.data() + 2);
	EXPECT_EQ(0xff, media[0] | media[1] << 8);
	EXPECT_EQ(0, media[2] | media[3] << 8);
}

TEST_F(VmuTest, TruncatedImageIsReinitializedInsteadOfPartiallyLoaded)
{
	MapleDevices[0][0].reset();
	device.reset();
	{
		std::ofstream truncated(tempDir / "vmu_save_A1.bin", std::ios::binary | std::ios::trunc);
		ASSERT_TRUE(truncated.good());
		const char prefix[] = { 0x12, 0x34, 0x56 };
		truncated.write(prefix, sizeof(prefix));
	}

	device = maple_Create(MDT_SegaVMU);
	device->Setup(0, 0);
	size_t size = 0;
	const u8 *flash = static_cast<const u8 *>(device->getData(size));
	ASSERT_NE(nullptr, flash);
	ASSERT_EQ(128_KB, size);
	for (u32 i = 0; i < 16; i++)
		EXPECT_EQ(0x55, flash[0xff * 512 + i]);
}

TEST_F(VmuTest, ClockWriteUpdatesClockAndReadReportsWeekday)
{
	std::array<u32, 4> writePayload { MFID_3_Clock, 0, 0, 0 };
	u8 *date = reinterpret_cast<u8 *>(writePayload.data() + 2);
	date[0] = 0xf0; // 2032, little endian
	date[1] = 0x07;
	date[2] = 4;
	date[3] = 5;
	date[4] = 6;
	date[5] = 7;
	date[6] = 8;
	date[7] = 0; // Monday

	std::array<u32, 256> output {};
	ASSERT_EQ(4u, dma(MDCF_BlockWrite, writePayload, output));
	ASSERT_EQ(MDRS_DeviceReply, output[0] & 0xff);

	std::array<u32, 2> readPayload { MFID_3_Clock, 0 };
	ASSERT_EQ(16u, dma(MDCF_BlockRead, readPayload, output));
	ASSERT_EQ(MDRS_DataTransfer, output[0] & 0xff);
	ASSERT_EQ(MFID_3_Clock, output[1]);
	const u8 *readDate = reinterpret_cast<const u8 *>(output.data() + 2);
	EXPECT_EQ(2032, readDate[0] + readDate[1] * 256);
	EXPECT_EQ(4, readDate[2]);
	EXPECT_EQ(5, readDate[3]);
	EXPECT_EQ(6, readDate[4]);
	EXPECT_EQ(7, readDate[5]);
	EXPECT_GE(readDate[6], 8);
	EXPECT_LE(readDate[6], 9); // The clock may advance by one second during the test.
	EXPECT_EQ(0, readDate[7]);
}

TEST_F(VmuTest, ClockSupportsFullDocumentedYearRange)
{
	auto writeAndRead = [this](u16 year)
	{
		std::array<u32, 4> writePayload { MFID_3_Clock, 0, 0, 0 };
		u8 *date = reinterpret_cast<u8 *>(writePayload.data() + 2);
		date[0] = year & 0xff;
		date[1] = year >> 8;
		date[2] = 1;
		date[3] = 2;
		date[4] = 12;
		date[5] = 34;
		date[6] = 56;
		date[7] = 0;
		std::array<u32, 256> output {};
		EXPECT_EQ(4u, dma(MDCF_BlockWrite, writePayload, output));
		EXPECT_EQ(MDRS_DeviceReply, output[0] & 0xff);
		std::array<u32, 2> readPayload { MFID_3_Clock, 0 };
		EXPECT_EQ(16u, dma(MDCF_BlockRead, readPayload, output));
		return std::array<u8, 8> {
			reinterpret_cast<u8 *>(output.data() + 2)[0],
			reinterpret_cast<u8 *>(output.data() + 2)[1],
			reinterpret_cast<u8 *>(output.data() + 2)[2],
			reinterpret_cast<u8 *>(output.data() + 2)[3],
			reinterpret_cast<u8 *>(output.data() + 2)[4],
			reinterpret_cast<u8 *>(output.data() + 2)[5],
			reinterpret_cast<u8 *>(output.data() + 2)[6],
			reinterpret_cast<u8 *>(output.data() + 2)[7],
		};
	};

	for (u16 year : { u16(0), u16(9999) })
	{
		auto date = writeAndRead(year);
		EXPECT_EQ(year, date[0] | date[1] << 8);
		EXPECT_EQ(1, date[2]);
		EXPECT_EQ(2, date[3]);
		EXPECT_EQ(12, date[4]);
		EXPECT_EQ(34, date[5]);
		EXPECT_GE(date[6], 56);
		EXPECT_LE(date[6], 57);
	}
}

TEST_F(VmuTest, ClockRejectsInvalidCalendarDates)
{
	std::array<u32, 4> payload { MFID_3_Clock, 0, 0, 0 };
	u8 *date = reinterpret_cast<u8 *>(payload.data() + 2);
	date[0] = 0x34; // 2100 is not a leap year
	date[1] = 0x08;
	date[2] = 2;
	date[3] = 29;
	date[4] = 12;
	date[5] = 0;
	date[6] = 0;
	date[7] = 0;
	std::array<u32, 256> output {};
	ASSERT_EQ(4u, dma(MDCF_BlockWrite, payload, output));
	EXPECT_EQ(MDRE_TransmitAgain, output[0] & 0xff);

	date[0] = 0x00; // Year 2000 is a leap year.
	date[1] = 0x07;
	ASSERT_EQ(4u, dma(MDCF_BlockWrite, payload, output));
	EXPECT_EQ(MDRS_DeviceReply, output[0] & 0xff);
}

TEST_F(VmuTest, SaveStateRestoresClockWithoutHostTimeDrift)
{
	auto setClock = [this](int year, std::array<u32, 256>& output)
	{
		std::array<u32, 4> payload { MFID_3_Clock, 0, 0, 0 };
		u8 *date = reinterpret_cast<u8 *>(payload.data() + 2);
		date[0] = year & 0xff;
		date[1] = year >> 8;
		date[2] = 6;
		date[3] = 15;
		date[4] = 12;
		date[5] = 30;
		date[6] = 0;
		date[7] = 0;
		return dma(MDCF_BlockWrite, payload, output);
	};

	std::array<u32, 256> output {};
	ASSERT_EQ(4u, setClock(2032, output));

	std::vector<u8> state(140_KB);
	Serializer serializer(state.data(), state.size());
	device->serialize(serializer);
	ASSERT_EQ(4u, setClock(2040, output));

	Deserializer deserializer(state.data(), serializer.size());
	device->deserialize(deserializer);
	std::array<u32, 2> readPayload { MFID_3_Clock, 0 };
	ASSERT_EQ(16u, dma(MDCF_BlockRead, readPayload, output));
	const u8 *date = reinterpret_cast<const u8 *>(output.data() + 2);
	EXPECT_EQ(2032, date[0] + date[1] * 256);
	EXPECT_EQ(6, date[2]);
	EXPECT_EQ(15, date[3]);
}

TEST_F(VmuTest, ClockConditionReportsReleasedButtons)
{
	std::array<u32, 1> payload { MFID_3_Clock };
	std::array<u32, 256> output {};
	ASSERT_EQ(12u, dma(MDCF_GetCondition, payload, output));
	EXPECT_EQ(MDRS_DataTransfer, output[0] & 0xff);
	EXPECT_EQ(MFID_3_Clock, output[1]);
	EXPECT_EQ(0x000000ffu, output[2]);
}

TEST_F(VmuTest, ClockConditionReportsMappedVmuButtons)
{
	// DPad2 and C/D are unused by a standard Dreamcast controller, allowing
	// all six Maple-visible VMU buttons to be mapped without stealing controls.
	mapleInputState[0].kcode &= ~(DC_DPAD2_UP | DC_DPAD2_LEFT | DC_BTN_C | DC_BTN_D);
	std::array<u32, 1> payload { MFID_3_Clock };
	std::array<u32, 256> output {};
	ASSERT_EQ(12u, dma(MDCF_GetCondition, payload, output));
	EXPECT_EQ(MDRS_DataTransfer, output[0] & 0xff);
	EXPECT_EQ(MFID_3_Clock, output[1]);
	EXPECT_EQ(0x000000cau, output[2]);
}

TEST_F(VmuTest, ClockConditionRejectsImpossibleDpadPairs)
{
	mapleInputState[0].kcode &= ~(DC_DPAD2_UP | DC_DPAD2_DOWN
			| DC_DPAD2_LEFT | DC_DPAD2_RIGHT);
	std::array<u32, 1> payload { MFID_3_Clock };
	std::array<u32, 256> output {};
	ASSERT_EQ(12u, dma(MDCF_GetCondition, payload, output));
	EXPECT_EQ(0x000000ffu, output[2]);
}

} // namespace
