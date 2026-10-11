#include "gtest/gtest.h"
#include "imgread/common.h"
#include <chrono>
#include <filesystem>
#include <fstream>
#include <locale>
#include <vector>

class CueTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
		tempDir = std::filesystem::temp_directory_path() / ("flycast-cue-test-" + std::to_string(suffix));
		std::filesystem::create_directories(tempDir);
	}

	void TearDown() override
	{
		std::error_code error;
		std::filesystem::remove_all(tempDir, error);
	}

	std::filesystem::path tempDir;
};

static void verifyDisk(Disc *disk)
{
	ASSERT_EQ(GdRom, disk->type);
	ASSERT_EQ(3, disk->tracks.size());

	ASSERT_TRUE(disk->tracks[0].isDataTrack());
	ASSERT_EQ(150, disk->tracks[0].StartFAD);

	ASSERT_FALSE(disk->tracks[1].isDataTrack());
	ASSERT_EQ(600, disk->tracks[1].StartFAD);

	ASSERT_TRUE(disk->tracks[2].isDataTrack());
	ASSERT_EQ(45150, disk->tracks[2].StartFAD);
}

TEST_F(CueTest, LoadD)
{
	std::locale::global(std::locale("fr_FR.UTF-8"));
	Disc *disk = OpenDisc(FLYCAST_TEST_FILES "/test_cues/d/cs.cue");
	verifyDisk(disk);
	delete disk;
}

TEST_F(CueTest, LoadE)
{
	Disc *disk = OpenDisc(FLYCAST_TEST_FILES "/test_cues/e/cs.cue");

	ASSERT_EQ(CdRom_XA, disk->type);
	ASSERT_EQ(2, disk->tracks.size());

	ASSERT_FALSE(disk->tracks[0].isDataTrack());
	ASSERT_EQ(150, disk->tracks[0].StartFAD);

	ASSERT_TRUE(disk->tracks[1].isDataTrack());
	ASSERT_EQ(11550, disk->tracks[1].StartFAD);
	delete disk;
}

TEST_F(CueTest, LoadAudio)
{
	Disc *disk = OpenDisc(FLYCAST_TEST_FILES "/test_cues/d/audio.cue");

	ASSERT_EQ(CdDA, disk->type);
	ASSERT_EQ(3, disk->tracks.size());

	ASSERT_FALSE(disk->tracks[0].isDataTrack());
	ASSERT_EQ(150, disk->tracks[0].StartFAD);

	ASSERT_FALSE(disk->tracks[1].isDataTrack());
	ASSERT_EQ(225, disk->tracks[1].StartFAD);

	ASSERT_FALSE(disk->tracks[2].isDataTrack());
	ASSERT_EQ(300, disk->tracks[2].StartFAD);
	delete disk;
}

TEST_F(CueTest, SharedFileTracksCanUseDifferentSectorSizes)
{
	const std::filesystem::path binPath = tempDir / "mixed.bin";
	const std::filesystem::path cuePath = tempDir / "mixed.cue";
	std::vector<u8> data(2 * 2352 + 3 * 2048, 0x11);
	std::fill(data.begin() + 2 * 2352, data.end(), 0x5a);
	{
		std::ofstream bin(binPath, std::ios::binary);
		bin.write((const char *)data.data(), data.size());
	}
	{
		std::ofstream cue(cuePath);
		cue << "FILE \"mixed.bin\" BINARY\n"
				"  TRACK 01 AUDIO\n"
				"    INDEX 01 00:00:00\n"
				"  TRACK 02 MODE1/2048\n"
				"    INDEX 01 00:00:02\n";
	}

	std::unique_ptr<Disc> image(OpenDisc(cuePath.string()));
	ASSERT_EQ(2u, image->tracks.size());
	EXPECT_EQ(150u, image->tracks[0].StartFAD);
	EXPECT_EQ(151u, image->tracks[0].EndFAD);
	EXPECT_EQ(152u, image->tracks[1].StartFAD);
	EXPECT_EQ(154u, image->tracks[1].EndFAD);
	EXPECT_EQ(155u, image->LeadOut.StartFAD);
	u8 sector[2048] {};
	ASSERT_EQ(1u, image->ReadSectors(152, 1, sector, sizeof(sector), true));
	for (u8 value : sector)
		EXPECT_EQ(0x5a, value);
}
