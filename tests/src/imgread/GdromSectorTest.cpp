#include <gtest/gtest.h>

#include <memory>

#include "hw/gdrom/gdrom_if.h"
#include "hw/gdrom/gdromv3.h"
#include "imgread/common.h"
extern "C" {
#include <libchdr/cdrom.h>
}

extern Disc *disc;

namespace
{

class MemorySector final : public TrackFile
{
public:
	explicit MemorySector(SectorFormat format, bool readable = true, bool rawQ = true, u32 *readCount = nullptr)
		: format(format), readable(readable), rawQ(rawQ), readCount(readCount) {}

	bool Read(u32 fad, u8 *dst, SectorFormat *sectorType, u8 *, SubcodeFormat *subcodeType) override
	{
		if (readCount != nullptr)
			(*readCount)++;
		if (!readable)
			return false;
		const u32 size = format == SECFMT_2324_MODE2_FORM2 ? 2324
				: format == SECFMT_2336_MODE2 ? 2336
				: format == SECFMT_2352 ? 2352
				: format == SECFMT_2448_MODE2 ? 2448 : 2048;
		memset(dst, 0x5a, size);
		if (format == SECFMT_2448_MODE2)
			memset(dst + 2352, (fad & 0x3f) | (rawQ ? 0x40 : 0), 96);
		*sectorType = format;
		*subcodeType = SUBFMT_NONE;
		return true;
	}

private:
	SectorFormat format;
	bool readable;
	bool rawQ;
	u32 *readCount;
};

class TransientSector final : public TrackFile
{
public:
	TransientSector(SectorFormat format, u32 failures, u32& reads)
		: format(format), failures(failures), reads(reads) {}

	bool Read(u32, u8 *dst, SectorFormat *sectorType, u8 *, SubcodeFormat *subcodeType) override
	{
		reads++;
		if (failures != 0)
		{
			failures--;
			return false;
		}
		memset(dst, 0x5a, format == SECFMT_2324_MODE2_FORM2 ? 2324 : 2048);
		*sectorType = format;
		*subcodeType = SUBFMT_NONE;
		return true;
	}

private:
	SectorFormat format;
	u32 failures;
	u32& reads;
};

class SeparateSubcodeSector final : public TrackFile
{
public:
	bool Read(u32, u8 *dst, SectorFormat *sectorType, u8 *subcode,
			SubcodeFormat *subcodeType) override
	{
		memset(dst, 0x5a, 2352);
		for (u32 i = 0; i < 96; i++)
			subcode[i] = 0xc0 | (i & 0x3f);
		*sectorType = SECFMT_2352;
		*subcodeType = SUBFMT_96;
		return true;
	}
};

std::unique_ptr<Disc> makeDisc(SectorFormat format, bool readable = true, bool rawQ = true)
{
	auto disc = std::make_unique<Disc>();
	disc->tracks.push_back({ new MemorySector(format, readable, rawQ), 150, 150, 4, 1 });
	disc->sessions.push_back({ 150, 1 });
	disc->LeadOut.CTRL = 4;
	disc->LeadOut.ADR = 1;
	disc->LeadOut.StartFAD = 151;
	disc->EndFAD = 151;
	return disc;
}

} // namespace

TEST(GdromSector, SynthesizesRawMode1Layout)
{
	auto disc = makeDisc(SECFMT_2048_MODE1);
	u8 sector[2352];

	ASSERT_EQ(1u, disc->ReadSectors(150, 1, sector, sizeof(sector), true));
	EXPECT_EQ(0, sector[0]);
	for (u32 i = 1; i <= 10; i++)
		EXPECT_EQ(0xff, sector[i]);
	EXPECT_EQ(0, sector[11]);
	EXPECT_EQ(0x00, sector[12]);
	EXPECT_EQ(0x02, sector[13]);
	EXPECT_EQ(0x00, sector[14]);
	EXPECT_EQ(1, sector[15]);
	EXPECT_EQ(0x5a, sector[16]);
	EXPECT_EQ(0x5a, sector[16 + 2047]);
	EXPECT_NE(0, sector[16 + 2048]);
	EXPECT_TRUE(ecc_verify(sector));
}

TEST(GdromSector, DispatchesReadsOnlyToContainingTrack)
{
	u32 firstReads = 0;
	u32 secondReads = 0;
	Disc image;
	image.tracks.push_back({ new MemorySector(SECFMT_2048_MODE1, true, true, &firstReads), 150, 150, 4, 1 });
	image.tracks.push_back({ new MemorySector(SECFMT_2352, true, true, &secondReads), 151, 151, 0, 1 });
	image.sessions.push_back({ 150, 1 });
	image.LeadOut.StartFAD = image.EndFAD = 152;
	u8 sector[2048] {};

	EXPECT_EQ(1u, image.ReadSectors(150, 1, sector, sizeof(sector), true));
	EXPECT_EQ(1u, firstReads);
	EXPECT_EQ(0u, secondReads);
}

TEST(GdromSector, SynthesizesRawMode2Form1Layout)
{
	auto disc = makeDisc(SECFMT_2048_MODE2_FORM1);
	u8 sector[2352];

	ASSERT_EQ(1u, disc->ReadSectors(150, 1, sector, sizeof(sector), true));
	EXPECT_EQ(2, sector[15]);
	for (u32 i = 16; i < 24; i++)
		EXPECT_EQ(0, sector[i]);
	EXPECT_EQ(0x5a, sector[24]);
	EXPECT_EQ(0x5a, sector[24 + 2047]);
	EXPECT_NE(0, sector[24 + 2048]);
	EXPECT_TRUE(ecc_verify(sector));
}

TEST(GdromSector, SynthesizesRawMode2Form2Layout)
{
	auto disc = makeDisc(SECFMT_2324_MODE2_FORM2);
	u8 sector[2352];

	ASSERT_EQ(1u, disc->ReadSectors(150, 1, sector, sizeof(sector), true));
	EXPECT_EQ(2, sector[15]);
	EXPECT_EQ(0x20, sector[18]);
	EXPECT_EQ(0x20, sector[22]);
	EXPECT_EQ(0x5a, sector[24]);
	EXPECT_EQ(0x5a, sector[24 + 2323]);
	EXPECT_NE(0, sector[2348] | sector[2349] | sector[2350] | sector[2351]);
}

TEST(GdromSector, SelectsMode1HeaderAndData)
{
	auto disc = makeDisc(SECFMT_2048_MODE1);
	u8 sector[2052];

	ASSERT_EQ(1u, disc->ReadSectors(150, 1, sector, sizeof(sector), true));
	EXPECT_EQ(0x00, sector[0]);
	EXPECT_EQ(0x02, sector[1]);
	EXPECT_EQ(0x00, sector[2]);
	EXPECT_EQ(1, sector[3]);
	EXPECT_EQ(0x5a, sector[4]);
	EXPECT_EQ(0x5a, sector[2051]);
}

TEST(GdromSector, SelectsMode2Form1DataGroup)
{
	auto disc = makeDisc(SECFMT_2048_MODE2_FORM1);
	u8 sector[2084];

	ASSERT_EQ(1u, disc->ReadSectors(150, 1, sector, sizeof(sector), true));
	for (u32 i = 0; i < 8; i++)
		EXPECT_EQ(0, sector[i]);
	EXPECT_EQ(0x5a, sector[8]);
	EXPECT_EQ(0x5a, sector[8 + 2047]);
	for (u32 i = 8 + 2048; i < sizeof(sector); i++)
		EXPECT_EQ(0, sector[i]);
}

TEST(GdromSector, SelectsMode2DataGroupFrom2336Source)
{
	auto disc = makeDisc(SECFMT_2336_MODE2);
	u8 sector[2336];

	ASSERT_EQ(1u, disc->ReadSectors(150, 1, sector, sizeof(sector), true));
	for (u8 value : sector)
		EXPECT_EQ(0x5a, value);
}

TEST(GdromSector, ConvertsEveryBiosSectorSize)
{
	auto image = makeDisc(SECFMT_2048_MODE1);
	u8 sector[2352] {};
	constexpr u32 sizes[] = { 4, 8, 12, 2048, 2052, 2084, 2088, 2324, 2328, 2336, 2340, 2352 };

	for (u32 size : sizes)
	{
		SCOPED_TRACE(size);
		memset(sector, 0xcc, sizeof(sector));
		EXPECT_EQ(1u, image->ReadSectors(150, 1, sector, size, true));
	}
}

TEST(GdromSector, PacksMixedMode2FormsUsingPerSectorSizes)
{
	Disc image;
	image.type = CdRom_XA;
	image.tracks.push_back({ new MemorySector(SECFMT_2048_MODE2_FORM1), 150, 150, 4, 1 });
	image.tracks.push_back({ new MemorySector(SECFMT_2324_MODE2_FORM2), 151, 151, 4, 1 });
	image.sessions.push_back({ 150, 1 });
	image.LeadOut.StartFAD = image.EndFAD = 152;
	Disc *previousDisc = disc;
	disc = &image;
	read_params_t params { 150, 2, 0, 0, 3, 0, 2 }; // Generic Mode 2, data selected.
	DmaBuffer buffer;

	const bool filled = buffer.fill(params);
	disc = previousDisc;
	ASSERT_TRUE(filled);
	ASSERT_EQ(2084u + 2336u, buffer.getSize());
	const u8 *data = buffer.read(buffer.getSize());
	// Form 1: eight-byte subheader, 2048 data bytes, 28 dummy bytes.
	EXPECT_EQ(0, data[0]);
	EXPECT_EQ(0x5a, data[8]);
	EXPECT_EQ(0, data[2083]);
	// Form 2 begins immediately afterward and carries its form bit.
	EXPECT_EQ(0x20, data[2084 + 2]);
	EXPECT_EQ(0x5a, data[2084 + 8]);
	EXPECT_EQ(0, data[2084 + 2335]);
	EXPECT_EQ(152u, params.start_sector);
	EXPECT_EQ(0u, params.remaining_sectors);

}

TEST(GdromSector, RetriesDataReadsAccordingToDriveMode)
{
	u32 reads = 0;
	Disc image;
	image.tracks.push_back({ new TransientSector(SECFMT_2048_MODE1, 2, reads), 150, 150, 4, 1 });
	image.sessions.push_back({ 150, 1 });
	image.LeadOut.StartFAD = image.EndFAD = 151;
	Disc *previousDisc = disc;
	const GD_HardwareInfo_t previousHardwareInfo = GD_HardwareInfo;
	disc = &image;
	GD_HardwareInfo.read_flags = 0x19; // ECC, normal retry, and Form 2 retry.
	GD_HardwareInfo.read_retry = 2;
	read_params_t params { 150, 1, 2048, 0, 2, 0, 2 };
	DmaBuffer buffer;

	const bool filled = buffer.fill(params);
	disc = previousDisc;
	GD_HardwareInfo = previousHardwareInfo;

	EXPECT_TRUE(filled);
	EXPECT_EQ(4u, reads); // Three validation attempts, then the actual transfer.
}

TEST(GdromSector, ContinuousModeDoesNotRetryFailedReads)
{
	u32 reads = 0;
	Disc image;
	image.tracks.push_back({ new TransientSector(SECFMT_2048_MODE1, 1, reads), 150, 150, 4, 1 });
	image.sessions.push_back({ 150, 1 });
	image.LeadOut.StartFAD = image.EndFAD = 151;
	Disc *previousDisc = disc;
	const GD_HardwareInfo_t previousHardwareInfo = GD_HardwareInfo;
	disc = &image;
	GD_HardwareInfo.read_flags = 0x39; // Read Continuous overrides retry settings.
	GD_HardwareInfo.read_retry = 8;
	read_params_t params { 150, 1, 2048, 0, 2, 0, 2 };
	DmaBuffer buffer;

	const bool filled = buffer.fill(params);
	disc = previousDisc;
	GD_HardwareInfo = previousHardwareInfo;

	EXPECT_FALSE(filled);
	EXPECT_EQ(1u, reads);
}

TEST(GdromSector, Form2RetryRequiresItsModeBit)
{
	u32 reads = 0;
	Disc image;
	image.type = CdRom_XA;
	image.tracks.push_back({ new TransientSector(SECFMT_2324_MODE2_FORM2, 1, reads), 150, 150, 4, 1 });
	image.sessions.push_back({ 150, 1 });
	image.LeadOut.StartFAD = image.EndFAD = 151;
	Disc *previousDisc = disc;
	const GD_HardwareInfo_t previousHardwareInfo = GD_HardwareInfo;
	disc = &image;
	GD_HardwareInfo.read_flags = 0x18; // General retry enabled, Form 2 retry disabled.
	GD_HardwareInfo.read_retry = 8;
	read_params_t params { 150, 1, 2336, 0, 5, 0, 2 };
	DmaBuffer buffer;

	const bool filled = buffer.fill(params);
	disc = previousDisc;
	GD_HardwareInfo = previousHardwareInfo;

	EXPECT_FALSE(filled);
	EXPECT_EQ(1u, reads);
}

// The two retry-enable flags select distinct sector families.
TEST(GdromSector, Form2RetryIsIndependentOfNormalRetry)
{
	u32 reads = 0;
	Disc image;
	image.type = CdRom_XA;
	image.tracks.push_back({ new TransientSector(SECFMT_2324_MODE2_FORM2, 1, reads), 150, 150, 4, 1 });
	image.sessions.push_back({ 150, 1 });
	image.LeadOut.StartFAD = image.EndFAD = 151;
	Disc *previousDisc = disc;
	const GD_HardwareInfo_t previousHardwareInfo = GD_HardwareInfo;
	disc = &image;
	GD_HardwareInfo.read_flags = 0x11; // ECC and Form 2 retry; normal retry disabled.
	GD_HardwareInfo.read_retry = 1;
	read_params_t params { 150, 1, 2336, 0, 5, 0, 2 };
	DmaBuffer buffer;

	const bool filled = buffer.fill(params);
	disc = previousDisc;
	GD_HardwareInfo = previousHardwareInfo;

	EXPECT_TRUE(filled);
	EXPECT_EQ(3u, reads); // Two validation attempts, then the actual transfer.
}

TEST(GdromSector, ReqStatClearsRetryCountOnlyWhenTransferred)
{
	u32 reads = 0;
	Disc image;
	image.tracks.push_back({ new TransientSector(SECFMT_2048_MODE1, 1, reads), 150, 150, 4, 1 });
	image.sessions.push_back({ 150, 1 });
	image.LeadOut.StartFAD = image.EndFAD = 151;
	Disc *previousDisc = disc;
	const GD_HardwareInfo_t previousHardwareInfo = GD_HardwareInfo;
	const GD_SecNumbT previousSecNumber = SecNumber;
	disc = &image;
	GD_HardwareInfo.read_flags = 0x18;
	GD_HardwareInfo.read_retry = 1;
	SecNumber.Status = GD_PAUSE;
	SecNumber.DiscFormat = CdRom >> 4;
	read_params_t params { 150, 1, 2048, 0, 2, 0, 2 };
	DmaBuffer buffer;
	if (!buffer.fill(params))
	{
		disc = previousDisc;
		GD_HardwareInfo = previousHardwareInfo;
		SecNumber = previousSecNumber;
		FAIL() << "transient sector did not recover";
		return;
	}
	WriteMem_gdrom(GD_COMMAND_Write, ATA_EXEC_DIAG, 1);

	auto requestStatus = [](u8 offset, u8 allocation, u16 byteLimit) {
		WriteMem_gdrom(GD_FEATURES_Write, 0, 1);
		WriteMem_gdrom(GD_BYCTLLO, byteLimit, 1);
		WriteMem_gdrom(GD_BYCTLHI, byteLimit >> 8, 1);
		WriteMem_gdrom(GD_COMMAND_Write, ATA_SPI_PACKET, 1);
		const u8 packet[12] { SPI_REQ_STAT, 0, offset, 0, allocation };
		for (u32 i = 0; i < sizeof(packet); i += 2)
			WriteMem_gdrom(GD_DATA, packet[i] | packet[i + 1] << 8, 2);
	};

	// The task-file byte limit truncates this response before retry byte 8.
	requestStatus(0, 10, 4);
	ReadMem_gdrom(GD_DATA, 2);
	ReadMem_gdrom(GD_DATA, 2);
	requestStatus(8, 1, 1);
	EXPECT_EQ(1u, ReadMem_gdrom(GD_DATA, 2) & 0xff);
	// Transferring byte 8 consumes the report.
	requestStatus(8, 1, 1);
	EXPECT_EQ(0u, ReadMem_gdrom(GD_DATA, 2) & 0xff);

	disc = previousDisc;
	GD_HardwareInfo = previousHardwareInfo;
	SecNumber = previousSecNumber;
}

TEST(GdromSector, RejectsNonexistentSession)
{
	auto image = makeDisc(SECFMT_2048_MODE1);
	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	const int previousSense = sns_key;
	const int previousAsc = sns_asc;
	const int previousAscq = sns_ascq;
	disc = image.get();
	SecNumber.Status = GD_PAUSE;
	SecNumber.DiscFormat = CdRom >> 4;

	WriteMem_gdrom(GD_COMMAND_Write, ATA_EXEC_DIAG, 1);
	WriteMem_gdrom(GD_FEATURES_Write, 0, 1);
	WriteMem_gdrom(GD_BYCTLLO, 6, 1);
	WriteMem_gdrom(GD_BYCTLHI, 0, 1);
	WriteMem_gdrom(GD_COMMAND_Write, ATA_SPI_PACKET, 1);
	const u8 packet[12] { SPI_REQ_SES, 0, 2, 0, 6 };
	for (u32 i = 0; i < sizeof(packet); i += 2)
		WriteMem_gdrom(GD_DATA, packet[i] | packet[i + 1] << 8, 2);

	EXPECT_NE(0u, ReadMem_gdrom(GD_STATUS_Read, 1) & 1); // CHECK
	EXPECT_EQ(5, sns_key);
	EXPECT_EQ(0x26, sns_asc);
	EXPECT_EQ(0, sns_ascq);
	EXPECT_EQ(0u, ReadMem_gdrom(GD_STATUS_Read, 1) & 8); // no data phase

	WriteMem_gdrom(GD_COMMAND_Write, ATA_EXEC_DIAG, 1);
	disc = previousDisc;
	SecNumber = previousSecNumber;
	sns_key = previousSense;
	sns_asc = previousAsc;
	sns_ascq = previousAscq;
}

TEST(GdromSector, ReportsMixedMode2PioByteCount)
{
	Disc image;
	image.type = CdRom_XA;
	image.tracks.push_back({ new MemorySector(SECFMT_2048_MODE2_FORM1), 150, 150, 4, 1 });
	image.tracks.push_back({ new MemorySector(SECFMT_2324_MODE2_FORM2), 151, 151, 4, 1 });
	image.sessions.push_back({ 150, 1 });
	image.LeadOut.StartFAD = image.EndFAD = 152;
	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	disc = &image;
	SecNumber.Status = GD_PAUSE;
	SecNumber.DiscFormat = CdRom_XA >> 4;

	// Clear prior packet sense/state, select PIO, and permit a full data block.
	WriteMem_gdrom(GD_COMMAND_Write, ATA_EXEC_DIAG, 1);
	WriteMem_gdrom(GD_FEATURES_Write, 0, 1);
	WriteMem_gdrom(GD_BYCTLLO, 0xff, 1);
	WriteMem_gdrom(GD_BYCTLHI, 0xff, 1);
	WriteMem_gdrom(GD_COMMAND_Write, ATA_SPI_PACKET, 1);
	const u8 packet[12] {
		SPI_CD_READ, 0x26, // Data selected, generic Mode 2, FAD address.
		0, 0, 150, 0, 0, 0,
		0, 0, 2, 0
	};
	for (u32 i = 0; i < sizeof(packet); i += 2)
		WriteMem_gdrom(GD_DATA, packet[i] | packet[i + 1] << 8, 2);

	const u32 byteCount = ReadMem_gdrom(GD_BYCTLLO, 1)
			| ReadMem_gdrom(GD_BYCTLHI, 1) << 8;
	std::vector<u8> output(byteCount);
	for (u32 i = 0; i < byteCount; i += 2)
	{
		const u16 word = ReadMem_gdrom(GD_DATA, 2);
		output[i] = word;
		if (i + 1 < byteCount)
			output[i + 1] = word >> 8;
	}
	disc = previousDisc;
	SecNumber = previousSecNumber;

	ASSERT_EQ(2084u + 2336u, byteCount);
	EXPECT_EQ(0, output[0]);
	EXPECT_EQ(0x5a, output[8]);
	EXPECT_EQ(0, output[2083]);
	EXPECT_EQ(0x20, output[2084 + 2]);
	EXPECT_EQ(0x5a, output[2084 + 8]);
	EXPECT_EQ(0, output.back());
	EXPECT_EQ(0u, ReadMem_gdrom(GD_STATUS_Read, 1) & 8); // DRQ cleared.
}

TEST(GdromSector, BuffersSectorAcrossPartialPioWindows)
{
	u32 reads = 0;
	Disc image;
	image.type = CdDA;
	image.tracks.push_back({ new MemorySector(SECFMT_2352, true, true, &reads), 150, 150, 0, 1 });
	image.sessions.push_back({ 150, 1 });
	image.LeadOut = { nullptr, 151, 0, 0, 1 };
	image.EndFAD = 151;
	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	disc = &image;
	SecNumber.Status = GD_PAUSE;
	SecNumber.DiscFormat = CdDA >> 4;

	WriteMem_gdrom(GD_COMMAND_Write, ATA_EXEC_DIAG, 1);
	WriteMem_gdrom(GD_FEATURES_Write, 0, 1);
	WriteMem_gdrom(GD_BYCTLLO, 4, 1);
	WriteMem_gdrom(GD_BYCTLHI, 0, 1);
	WriteMem_gdrom(GD_COMMAND_Write, ATA_SPI_PACKET, 1);
	const u8 packet[12] {
		SPI_CD_READ, 0x02, // CD-DA expected; data selection is ignored.
		0, 0, 150, 0, 0, 0, 0, 0, 1, 0
	};
	for (u32 i = 0; i < sizeof(packet); i += 2)
		WriteMem_gdrom(GD_DATA, packet[i] | packet[i + 1] << 8, 2);
	for (u32 i = 0; i < 2352; i += 2)
		EXPECT_EQ(0x5a5au, ReadMem_gdrom(GD_DATA, 2));

	EXPECT_EQ(1u, reads);
	WriteMem_gdrom(GD_COMMAND_Write, ATA_SOFT_RESET, 1);
	disc = previousDisc;
	SecNumber = previousSecNumber;
}

TEST(GdromSector, RejectsUnsupportedSectorSize)
{
	auto image = makeDisc(SECFMT_2048_MODE1);
	u8 sector[32];
	memset(sector, 0xcc, sizeof(sector));

	EXPECT_EQ(0u, image->ReadSectors(150, 1, sector, 17, true));
	for (u8 value : sector)
		EXPECT_EQ(0xcc, value);
}

TEST(GdromSector, StopOnMissStopsAtUnreadableSector)
{
	auto disc = makeDisc(SECFMT_2048_MODE1, false);
	u8 sector[2352] {};

	EXPECT_EQ(0u, disc->ReadSectors(150, 1, sector, sizeof(sector), true));
}

TEST(GdromSector, LeadoutStartsAfterLastReadableSector)
{
	auto disc = makeDisc(SECFMT_2048_MODE1);
	u8 sector[2048] {};

	EXPECT_EQ(1u, disc->ReadSectors(150, 1, sector, sizeof(sector), true));
	EXPECT_EQ(0u, disc->ReadSectors(151, 1, sector, sizeof(sector), true));
}

TEST(GdromToc, UsesAreaSpecificGdromLeadoutControl)
{
	Disc image;
	image.type = GdRom;
	image.tracks.push_back({ nullptr, 150, 449, 4, 1 });
	image.tracks.push_back({ nullptr, 450, 599, 3, 1 }); // Audio, copy allowed, pre-emphasis.
	image.tracks.push_back({ nullptr, 45150, 45299, 4, 1 });
	image.sessions.push_back({ 150, 1 });
	image.sessions.push_back({ 45150, 3 });
	image.LeadOut = { nullptr, 549300, 0, 4, 1 };
	image.EndFAD = 549300;
	Disc *previousDisc = disc;
	disc = &image;
	u32 toc[102];

	libGDR_GetToc(toc, SingleDensity);
	const u8 *singleDensityLeadout = reinterpret_cast<const u8 *>(&toc[101]);
	EXPECT_EQ(0x31, singleDensityLeadout[0]);
	EXPECT_EQ(0x00, singleDensityLeadout[1]);
	EXPECT_EQ(0x02, singleDensityLeadout[2]);
	EXPECT_EQ(0x58, singleDensityLeadout[3]); // FAD 600

	libGDR_GetToc(toc, DoubleDensity);
	const u8 *doubleDensityLeadout = reinterpret_cast<const u8 *>(&toc[101]);
	EXPECT_EQ(0x41, doubleDensityLeadout[0]);
	EXPECT_EQ(0x08, doubleDensityLeadout[1]);
	EXPECT_EQ(0x61, doubleDensityLeadout[2]);
	EXPECT_EQ(0xb4, doubleDensityLeadout[3]); // FAD 549300

	disc = previousDisc;
}

TEST(GdromToc, PreservesNonstandardFirstTrackNumber)
{
	Disc image;
	image.firstTrack = 4;
	image.tracks.push_back({ nullptr, 150, 299, 0, 1, "USABC2300004" });
	image.tracks.push_back({ nullptr, 300, 449, 4, 1, "USABC2300005" });
	image.sessions.push_back({ 150, 4 });
	image.LeadOut = { nullptr, 450, 0, 4, 1 };
	image.EndFAD = 450;
	Disc *previousDisc = disc;
	disc = &image;
	u32 toc[102];
	u32 elapsed;
	u32 startFad;
	u32 endFad;

	libGDR_GetToc(toc, SingleDensity);
	const u8 *bytes = reinterpret_cast<const u8 *>(toc);
	EXPECT_EQ(0xff, bytes[2 * 4]); // Track 3 does not exist.
	EXPECT_EQ(0x01, bytes[3 * 4]); // Track 4 is audio.
	EXPECT_EQ(0x41, bytes[4 * 4]); // Track 5 is data.
	EXPECT_EQ(4, bytes[99 * 4 + 1]);
	EXPECT_EQ(5, bytes[100 * 4 + 1]);
	EXPECT_EQ(4u, libGDR_GetTrackNumber(150, elapsed));
	EXPECT_EQ(5u, libGDR_GetTrackNumber(300, elapsed));
	EXPECT_TRUE(libGDR_GetTrack(5, startFad, endFad));
	EXPECT_EQ(300u, startFad);
	EXPECT_EQ(449u, endFad);
	EXPECT_EQ("USABC2300005", libGDR_GetTrackIsrc(5));

	disc = previousDisc;
}

TEST(GdromToc, EncodesTrackAndLeadoutBytes)
{
	auto image = makeDisc(SECFMT_2048_MODE1);
	Disc *previousDisc = disc;
	disc = image.get();
	u32 toc[102];

	libGDR_GetToc(toc, SingleDensity);
	const u8 *track = reinterpret_cast<const u8 *>(&toc[0]);
	EXPECT_EQ(0x41, track[0]);
	EXPECT_EQ(0x00, track[1]);
	EXPECT_EQ(0x00, track[2]);
	EXPECT_EQ(150, track[3]);
	const u8 *first = reinterpret_cast<const u8 *>(&toc[99]);
	EXPECT_EQ(0x41, first[0]);
	EXPECT_EQ(1, first[1]);
	const u8 *leadout = reinterpret_cast<const u8 *>(&toc[101]);
	EXPECT_EQ(0x41, leadout[0]);
	EXPECT_EQ(151, leadout[3]);
	disc = previousDisc;
}

TEST(GdromToc, ReportsSessionCountAndLeadout)
{
	auto image = makeDisc(SECFMT_2048_MODE1);
	u8 info[6] {};

	image->GetSessionInfo(info, 0);
	EXPECT_EQ(1, info[2]);
	EXPECT_EQ(0, info[3]);
	EXPECT_EQ(0, info[4]);
	EXPECT_EQ(151, info[5]);
	image->GetSessionInfo(info, 1);
	EXPECT_EQ(1, info[2]);
	EXPECT_EQ(0, info[3]);
	EXPECT_EQ(0, info[4]);
	EXPECT_EQ(150, info[5]);
}

TEST(GdromDiscType, UsesProtocolDiscFormatValues)
{
	EXPECT_EQ(0x20, CdRom_XA);
	EXPECT_EQ(0x20, CdRom_Extra);
	EXPECT_EQ(0x30, CdRom_CDI);
	EXPECT_EQ(CdDA, GuessDiscType(false, false, true));
	EXPECT_EQ(CdRom, GuessDiscType(true, false, false));
	EXPECT_EQ(CdRom_XA, GuessDiscType(false, true, false));
	EXPECT_EQ(CdRom_Extra, GuessDiscType(true, false, true));
}

TEST(GdromTaskFile, SoftResetPublishesLiveDiscStatus)
{
	auto image = makeDisc(SECFMT_2048_MODE2_FORM1);
	image->type = CdRom_XA;
	Disc *previousDisc = disc;
	disc = image.get();

	WriteMem_gdrom(GD_COMMAND_Write, ATA_SOFT_RESET, 1);
	EXPECT_EQ(0x21u, ReadMem_gdrom(GD_SECTNUM, 1));
	EXPECT_EQ(0x00u, ReadMem_gdrom(GD_STATUS_Read, 1));

	disc = previousDisc;
	gd_setdisc(true);
}

TEST(GdromSubcode, ReturnsCatalogFromImageMetadata)
{
	auto image = makeDisc(SECFMT_2048_MODE1);
	image->tracks[0].EndFAD = 151;
	image->LeadOut.StartFAD = 152;
	image->EndFAD = 152;
	image->catalog = "0123456789012";
	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	const cdda_t previousCdda = cdda;
	disc = image.get();
	SecNumber.Status = GD_PAUSE;
	cdda.status = cdda_t::NoInfo;
	u8 response[24] {};

	EXPECT_EQ(24u, gd_get_subcode(2, 151, response));
	EXPECT_EQ(0x80, response[8]);
	EXPECT_EQ(0, memcmp(response + 9, image->catalog.data(), 13));
	EXPECT_EQ(1, response[23]);
	cdda = previousCdda;
	SecNumber = previousSecNumber;
	disc = previousDisc;
}

TEST(GdromSubcode, RejectsMalformedCatalogMetadata)
{
	auto image = makeDisc(SECFMT_2048_MODE1);
	image->catalog = "1234INVALID89";
	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	const cdda_t previousCdda = cdda;
	disc = image.get();
	SecNumber.Status = GD_PAUSE;
	cdda.status = cdda_t::NoInfo;
	u8 response[24] {};

	EXPECT_EQ(24u, gd_get_subcode(2, 150, response));
	EXPECT_EQ(0, response[8]);
	for (u32 i = 9; i <= 21; i++)
		EXPECT_EQ('0', response[i]);

	cdda = previousCdda;
	SecNumber = previousSecNumber;
	disc = previousDisc;
}

TEST(GdromSubcode, DefaultsTrackAdrToCurrentPositionMode)
{
	auto image = makeDisc(SECFMT_2048_MODE1);
	Disc *previousDisc = disc;
	disc = image.get();
	u8 adr = 0;
	u8 control = 0;

	libGDR_GetTrackAdrAndControl(1, adr, control);
	EXPECT_EQ(1, adr);
	EXPECT_EQ(4, control);
	disc = previousDisc;
}

TEST(GdromSubcode, ReportsLeadoutQIdentityAndPosition)
{
	auto image = makeDisc(SECFMT_2048_MODE1);
	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	const cdda_t previousCdda = cdda;
	disc = image.get();
	SecNumber.Status = GD_PAUSE;
	cdda.status = cdda_t::NoInfo;
	u8 response[14] {};

	EXPECT_EQ(14u, gd_get_subcode(1, 151, response));
	EXPECT_EQ(0x41, response[4]);
	EXPECT_EQ(0xaa, response[5]);
	EXPECT_EQ(0, response[7]);
	EXPECT_EQ(0, response[8]);
	EXPECT_EQ(0, response[9]);
	EXPECT_EQ(151, response[13]);

	cdda = previousCdda;
	SecNumber = previousSecNumber;
	disc = previousDisc;
}

TEST(GdromSubcode, ReturnsIsrcFromCurrentTrackMetadata)
{
	auto image = makeDisc(SECFMT_2048_MODE1);
	image->tracks[0].EndFAD = 151;
	image->LeadOut.StartFAD = 152;
	image->EndFAD = 152;
	image->tracks[0].isrc = "USABC2300001";
	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	const cdda_t previousCdda = cdda;
	disc = image.get();
	SecNumber.Status = GD_PAUSE;
	cdda.status = cdda_t::NoInfo;
	u8 response[24] {};

	EXPECT_EQ(24u, gd_get_subcode(3, 151, response));
	EXPECT_EQ(0x80, response[8]);
	const u8 encodedIsrc[12] { 0x15, 0x13, 0x01, 0x02, 0x03,
			'2', '3', '0', '0', '0', '0', '1' };
	EXPECT_EQ(0, memcmp(response + 9, encodedIsrc, sizeof(encodedIsrc)));
	EXPECT_EQ(1, response[22]);
	cdda = previousCdda;
	SecNumber = previousSecNumber;
	disc = previousDisc;
}

TEST(GdromSubcode, RejectsMalformedIsrcMetadata)
{
	auto image = makeDisc(SECFMT_2048_MODE1);
	image->tracks[0].isrc = "US-ABC-23-00001";
	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	const cdda_t previousCdda = cdda;
	disc = image.get();
	SecNumber.Status = GD_PAUSE;
	cdda.status = cdda_t::NoInfo;
	u8 response[24] {};

	EXPECT_EQ(24u, gd_get_subcode(3, 150, response));
	EXPECT_EQ(0, response[8]);
	for (u32 i = 9; i <= 20; i++)
		EXPECT_EQ(0, response[i]);

	cdda = previousCdda;
	SecNumber = previousSecNumber;
	disc = previousDisc;
}

TEST(GdromSubcode, ReportsPlaybackCompletionOnlyOnce)
{
	const cdda_t previousCdda = cdda;
	cdda.status = cdda_t::Terminated;
	u8 response[14] {};

	EXPECT_EQ(14u, gd_get_subcode(1, 150, response));
	EXPECT_EQ(0x13, response[1]);
	gd_subcode_status_transferred(1);
	memset(response, 0, sizeof(response));
	EXPECT_EQ(14u, gd_get_subcode(1, 150, response));
	EXPECT_EQ(0x13, response[1]);
	gd_subcode_status_transferred(2);
	memset(response, 0, sizeof(response));
	EXPECT_EQ(14u, gd_get_subcode(1, 150, response));
	EXPECT_EQ(0x15, response[1]);
	cdda = previousCdda;
}

TEST(GdromSubcode, DistinguishesPlaybackPauseFromSeekPause)
{
	const cdda_t previousCdda = cdda;
	const GD_SecNumbT previousSecNumber = SecNumber;
	u8 response[14] {};
	cdda.status = cdda_t::Paused;
	cdda.EndAddr.FAD = 0;

	EXPECT_EQ(14u, gd_get_subcode(1, 150, response));
	EXPECT_EQ(0x15, response[1]);

	cdda.EndAddr.FAD = 300;
	cdda.StartAddr.FAD = 150;
	cdda.CurrAddr.FAD = 150;
	EXPECT_EQ(14u, gd_get_subcode(1, 150, response));
	EXPECT_EQ(0x12, response[1]);

	cdda.status = cdda_t::Terminated;
	gd_pause_cdda();
	EXPECT_EQ(cdda_t::NoInfo, cdda.status);
	EXPECT_EQ(14u, gd_get_subcode(1, 150, response));
	EXPECT_EQ(0x15, response[1]);
	cdda = previousCdda;
	SecNumber = previousSecNumber;
}

TEST(GdromSubcode, ReadsRawDataAtReportedFad)
{
	auto image = makeDisc(SECFMT_2448_MODE2);
	image->tracks[0].EndFAD = 151;
	image->LeadOut.StartFAD = 152;
	image->EndFAD = 152;
	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	const cdda_t previousCdda = cdda;
	disc = image.get();
	SecNumber.Status = GD_PAUSE;
	cdda.status = cdda_t::NoInfo;
	u8 sector[2352];
	u8 response[100] {};

	EXPECT_EQ(1u, libGDR_ReadSector(sector, 150, 1, sizeof(sector), true));
	EXPECT_EQ(100u, gd_get_subcode(0, 151, response));
	for (u32 i = 4; i < sizeof(response); i++)
		EXPECT_EQ(0x57, response[i]);

	cdda = previousCdda;
	SecNumber = previousSecNumber;
	disc = previousDisc;
}

TEST(GdromSubcode, PreservesSeparatelyStoredRawPw)
{
	auto image = std::make_unique<Disc>();
	image->tracks.push_back({ new SeparateSubcodeSector(), 150, 150, 0, 1 });
	image->sessions.push_back({ 150, 1 });
	image->LeadOut = { nullptr, 151, 0, 0, 1 };
	image->EndFAD = 151;
	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	const cdda_t previousCdda = cdda;
	disc = image.get();
	SecNumber.Status = GD_PAUSE;
	cdda.status = cdda_t::NoInfo;
	u8 response[100] {};

	EXPECT_EQ(100u, gd_get_subcode(0, 150, response));
	for (u32 i = 0; i < 96; i++)
		EXPECT_EQ(0xc0 | (i & 0x3f), response[4 + i]);

	cdda = previousCdda;
	SecNumber = previousSecNumber;
	disc = previousDisc;
}

TEST(GdromSubcode, SynthesizesMissingQWithoutOverwritingRw)
{
	auto image = makeDisc(SECFMT_2448_MODE2, true, false);
	image->tracks[0].EndFAD = 151;
	image->LeadOut.StartFAD = 152;
	image->EndFAD = 152;
	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	const cdda_t previousCdda = cdda;
	disc = image.get();
	SecNumber.Status = GD_PAUSE;
	cdda.status = cdda_t::NoInfo;
	u8 response[100] {};

	EXPECT_EQ(100u, gd_get_subcode(0, 151, response));
	bool hasQ = false;
	for (u32 i = 4; i < sizeof(response); i++)
	{
		EXPECT_EQ(0x17, response[i] & 0x3f);
		hasQ |= (response[i] & 0x40) != 0;
	}
	EXPECT_TRUE(hasQ);

	cdda = previousCdda;
	SecNumber = previousSecNumber;
	disc = previousDisc;
}

TEST(GdromCdda, NormalizesIntermediateGapToNextSession)
{
	auto image = std::make_unique<Disc>();
	image->firstTrack = 4;
	image->tracks.push_back({ new MemorySector(SECFMT_2352), 150, 449, 0, 1 });
	image->tracks.push_back({ new MemorySector(SECFMT_2352), 45150, 45299, 0, 1 });
	image->sessions.push_back({ 150, 4 });
	image->sessions.push_back({ 45150, 5 });
	image->LeadOut = { nullptr, 45300, 0, 0, 1 };
	image->EndFAD = 45300;
	Disc *previousDisc = disc;
	disc = image.get();
	u32 fad = 45000;
	u32 elapsed;
	u8 adr;
	u8 control;

	EXPECT_TRUE(gd_normalize_playback_start(fad));
	EXPECT_EQ(45150u, fad);
	EXPECT_EQ(450u, gd_get_playback_area_end(150));
	EXPECT_EQ(45300u, gd_get_playback_area_end(45150));
	fad = 45300;
	EXPECT_FALSE(gd_normalize_playback_start(fad));
	EXPECT_EQ(0xaau, libGDR_GetTrackNumber(450, elapsed));
	EXPECT_EQ(0u, elapsed);
	EXPECT_EQ(0xaau, libGDR_GetTrackNumber(451, elapsed));
	EXPECT_EQ(1u, elapsed);
	libGDR_GetTrackAdrAndControlAtFad(450, adr, control);
	EXPECT_EQ(1, adr);
	EXPECT_EQ(0, control); // Intermediate lead-out follows audio track 4.
	libGDR_GetTrackAdrAndControlAtFad(45300, adr, control);
	EXPECT_EQ(1, adr);
	EXPECT_EQ(4, control); // Outer lead-out uses the disc descriptor.

	disc = previousDisc;
}

TEST(GdromCdda, ReverseScanStopsAtCurrentSessionLeadIn)
{
	auto image = std::make_unique<Disc>();
	image->firstTrack = 4;
	image->tracks.push_back({ new MemorySector(SECFMT_2352), 150, 449, 0, 1 });
	image->tracks.push_back({ new MemorySector(SECFMT_2352), 45150, 45299, 0, 1 });
	image->sessions.push_back({ 150, 4 });
	image->sessions.push_back({ 45150, 5 });
	image->LeadOut = { nullptr, 45300, 0, 0, 1 };
	image->EndFAD = 45300;

	Disc *previousDisc = disc;
	const GD_SecNumbT previousSecNumber = SecNumber;
	const cdda_t previousCdda = cdda;
	disc = image.get();
	cdda.status = cdda_t::Playing;
	cdda.StartAddr.FAD = 45150;
	cdda.CurrAddr.FAD = 45160;
	cdda.EndAddr.FAD = 45200;

	gd_start_cdda_scan(true, 10);
	s16 audio[2352 / sizeof(s16)] {};
	libCore_CDDA_Sector(audio);

	EXPECT_EQ(cdda_t::Paused, cdda.status);
	EXPECT_EQ(45160u, cdda.CurrAddr.FAD);
	EXPECT_EQ(GD_PAUSE, SecNumber.Status);

	cdda = previousCdda;
	SecNumber = previousSecNumber;
	disc = previousDisc;
}
