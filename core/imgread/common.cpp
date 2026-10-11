#include "common.h"
#include "hw/gdrom/gdromv3.h"
#include "cfg/option.h"
#include "stdclass.h"
#include "hw/sh4/sh4_sched.h"
#include "serialize.h"
#include "oslib/i18n.h"
#include <libchdr/cdrom.h>

Disc* chd_parse(const char* file, std::vector<u8> *digest);
Disc* gdi_parse(const char* file, std::vector<u8> *digest);
Disc* cdi_parse(const char* file, std::vector<u8> *digest);
Disc* cue_parse(const char* file, std::vector<u8> *digest);
Disc *cdio_parse(const char *file, std::vector<u8> *digest);

static u32 NullDriveDiscType;
Disc* disc;
static int schedId = -1;

constexpr Disc* (*drivers[])(const char* path, std::vector<u8> *digest)
{
	chd_parse,
	gdi_parse,
	cdi_parse,
	cue_parse,
#ifdef USE_LIBCDIO
	cdio_parse,
#endif
};

static u8 q_subchannel[96];

static u8 toBcd(u32 value)
{
	return (value % 10) | (value / 10) << 4;
}

static u32 computeEdc(const u8 *data, u32 size)
{
	u32 edc = 0;
	while (size-- != 0)
	{
		u32 value = (edc ^ *data++) & 0xff;
		for (u32 bit = 0; bit < 8; bit++)
			value = (value >> 1) ^ ((value & 1) != 0 ? 0xd8018001 : 0);
		edc = (edc >> 8) ^ value;
	}
	return edc;
}

static void storeEdc(u8 *destination, u32 edc)
{
	destination[0] = edc;
	destination[1] = edc >> 8;
	destination[2] = edc >> 16;
	destination[3] = edc >> 24;
}

static void synthesizeRawSector(const u8 *source, u8 *destination, SectorFormat format, u32 fad)
{
	memset(destination, 0, 2352);
	memset(destination + 1, 0xff, 10);
	destination[12] = toBcd(fad / (60 * 75));
	destination[13] = toBcd((fad / 75) % 60);
	destination[14] = toBcd(fad % 75);
	if (format == SECFMT_2048_MODE1)
	{
		destination[15] = 1;
		memcpy(destination + 16, source, 2048);
		storeEdc(destination + 2064, computeEdc(destination, 2064));
		ecc_generate(destination);
	}
	else if (format == SECFMT_2048_MODE2_FORM1)
	{
		destination[15] = 2;
		// The source format doesn't retain the XA subheader. Leave it zeroed
		// and place the Form 1 user data at its raw-sector offset.
		memcpy(destination + 24, source, 2048);
		storeEdc(destination + 2072, computeEdc(destination + 16, 2056));
		ecc_generate(destination);
	}
	else if (format == SECFMT_2324_MODE2_FORM2)
	{
		destination[15] = 2;
		// Cooked Form 2 does not retain the duplicated XA subheader. Mark the
		// form bit in both copies and leave the remaining metadata zeroed.
		destination[18] = destination[22] = 0x20;
		memcpy(destination + 24, source, 2324);
		storeEdc(destination + 2348, computeEdc(destination + 16, 2332));
	}
	else
	{
		verify(format == SECFMT_2336_MODE2);
		destination[15] = 2;
		memcpy(destination + 16, source, 2336);
	}
}

static bool convertSector(u8 *in_buff, u8 *out_buff, int from, int to,
		bool preserveSubcode = false)
{
	//get subchannel data, if any
	if (from == 2448)
	{
		memcpy(q_subchannel, in_buff + 2352, 96);
		from -= 96;
	}
	else if (!preserveSubcode)
		memset(q_subchannel, 0, sizeof(q_subchannel));

	//if no conversion
	if (to == from)
	{
		memcpy(out_buff, in_buff, to);
		return true;
	}
	switch (to)
	{
	case 4:
		if (from != 2352)
			return false;
		memcpy(out_buff, &in_buff[12], 4);
		break;
	case 8:
		if (from != 2352)
			return false;
		memcpy(out_buff, &in_buff[16], 8);
		break;
	case 12:
		if (from != 2352)
			return false;
		memcpy(out_buff, &in_buff[12], 12);
		break;
	case 2052:
		if (from != 2352)
			return false;
		memcpy(out_buff, &in_buff[12], 2052);
		break;
	case 2084:
		if (from != 2352)
			return false;
		memcpy(out_buff, &in_buff[16], 2084);
		break;
	case 2088:
		if (from != 2352)
			return false;
		memcpy(out_buff, &in_buff[12], 2088);
		break;
	case 2340:
		if (from != 2352)
			return false;
		memcpy(out_buff, &in_buff[12], 2340);
		break;
	case 2328:
		if (from != 2352)
			return false;
		memcpy(out_buff, &in_buff[24], 2328);
		break;
	case 2324:
		if (from != 2352)
			return false;
		memcpy(out_buff, &in_buff[24], 2324);
		break;
	case 2336:
		if (from != 2352)
			return false;
		memcpy(out_buff, &in_buff[0x10], 2336);
		break;
	case 2048:
		if (from == 2352)
		{
			if (in_buff[15] == 1)
				memcpy(out_buff, &in_buff[0x10], 2048); //0x10 -> mode1
			else
				memcpy(out_buff, &in_buff[0x18], 2048); //0x18 -> mode2 (all forms ?)
		}
		else if (from == 2336)
			memcpy(out_buff, &in_buff[0x8], 2048);	//hmm only possible on mode2.Skip the mode2 header
		else
			return false;
		break;
	case 2352:
		if (from != 2352)
			return false;
		memcpy(out_buff, &in_buff[0], 2352);
		break;
	default :
		INFO_LOG(GDROM, "Sector conversion from %d to %d not supported \n", from , to);
		return false;
	}

	return true;
}

Disc* OpenDisc(const std::string& path, std::vector<u8> *digest)
{
	for (auto driver : drivers)
	{
		Disc *disc = driver(path.c_str(), digest);

		if (disc != nullptr)
			return disc;
	}

	throw FlycastException(i18n::Ts("Unknown disk format"));
}

namespace gdr {

static bool loadDisk(const std::string& path)
{
	termDrive();

	//try all drivers
	std::vector<u8> digest;
	disc = OpenDisc(path, config::GGPOEnable ? &digest : nullptr);

	if (disc != NULL)
	{
		if (config::GGPOEnable)
			MD5Sum().add(digest)
					.getDigest(settings.network.md5.game);
		INFO_LOG(GDROM, "gdrom: Opened image \"%s\"", path.c_str());
	}
	else
	{
		INFO_LOG(GDROM, "gdrom: Failed to open image \"%s\"", path.c_str());
		NullDriveDiscType = NoDisk;
	}

	return disc != NULL;
}

static bool doDiscSwap(const std::string& path);


bool initDrive(const std::string& path)
{
	bool rc = doDiscSwap(path);
	if (rc && disc == nullptr)
	{
		// Drive is busy
		sns_asc = 4;
		sns_ascq = 1;
		sns_key = 2;
		SecNumber.Status = GD_BUSY;
		sh4_sched_request(schedId, SH4_MAIN_CLOCK);
	}
	else {
		gd_setdisc(true);
	}

	return rc;
}

void openLid()
{
	settings.content.path.clear();
	termDrive();
	NullDriveDiscType = Open;
	gd_setdisc(true);
}

static bool doDiscSwap(const std::string& path)
{
	if (path.empty())
	{
		termDrive();
		NullDriveDiscType = NoDisk;
		return true;
	}

	if (loadDisk(path))
		return true;

	NullDriveDiscType = NoDisk;
	return false;
}

void termDrive()
{
	sh4_sched_request(schedId, -1);
	delete disc;
	disc = nullptr;
}

bool isOpen() {
	return disc == nullptr && NullDriveDiscType == Open;
}

bool isLoaded() {
	return disc != nullptr;
}

}	// namespace gdr

//
//convert our nice toc struct to dc's native one :)

static u32 createTrackInfo(const Track& track, u32 fad)
{
	const u32 adr = track.ADR == 0 ? 1 : track.ADR;
	u8 p[4];
	p[0] = (track.CTRL << 4) | adr;
	p[1] = fad >> 16;
	p[2] = fad >> 8;
	p[3] = fad >> 0;

	u32 result;
	memcpy(&result, p, sizeof(result));
	return result;
}

static u32 createTrackInfoFirstLast(const Track& track, u32 tracknum)
{
	return createTrackInfo(track, tracknum << 16);
}

u32 libGDR_ReadSector(u8 *buff, u32 startSector, u32 sectorCount, u32 sectorSize, bool stopOnMiss)
{
	if (disc != nullptr)
		return disc->ReadSectors(startSector, sectorCount, buff, sectorSize, stopOnMiss);
	if (stopOnMiss)
		return 0;
	memset(buff, 0, sectorCount * sectorSize);
	return sectorCount;
}

void libGDR_GetToc(u32* to, DiskArea area)
{
	memset(to, 0xFF, 102 * 4);
	if (!disc)
		return;
	if (disc->tracks.empty())
		return;

	//can't get toc on the second area on discs that don't have it
	if (area == DoubleDensity && disc->type != GdRom)
		return;

	//normal CDs: 1 .. tc
	//GDROM: area0 is 1 .. 2, area1 is 3 ... tc

	u32 first_track = disc->firstTrack;
	u32 last_track = disc->firstTrack + disc->tracks.size() - 1;
	if (area == DoubleDensity)
		first_track = 3;
	else if (disc->type == GdRom)
	{
		first_track = 1;
		last_track = 2;
	}

	if (first_track < disc->firstTrack || last_track < first_track
			|| last_track - disc->firstTrack >= disc->tracks.size())
		return;
	//Generate the TOC info

	//-1 for 1..99 0 ..98
	to[99] = createTrackInfoFirstLast(disc->tracks[first_track - disc->firstTrack], first_track);
	to[100] = createTrackInfoFirstLast(disc->tracks[last_track - disc->firstTrack], last_track);

	if (disc->type == GdRom && area == SingleDensity)
		// The low-density lead-out follows audio track 2 and carries that
		// area's control/ADR, not the data flags of the outer GD lead-out.
		to[101] = createTrackInfo(disc->tracks[1], disc->tracks[1].EndFAD + 1);
	else
		to[101] = createTrackInfo(disc->LeadOut, disc->LeadOut.StartFAD);

	for (u32 track = first_track; track <= last_track; track++)
	{
		const Track& entry = disc->tracks[track - disc->firstTrack];
		to[track - 1] = createTrackInfo(entry, entry.StartFAD);
	}
}

void libGDR_GetSessionInfo(u8* to, u8 session)
{
	if (disc != nullptr)
		disc->GetSessionInfo(to, session);
}

DiscType GuessDiscType(bool m1, bool m2, bool da)
{
	if (m2)
		return  CdRom_XA;
	else if (da && m1)
		return CdRom_Extra;
	else if (m1)
		return CdRom;
	else if (da)
		return CdDA;
	else
		return NoDisk;
}

bool Disc::readSector(u32 FAD, u8 *dst, SectorFormat *sector_type, u8 *subcode, SubcodeFormat *subcode_type)
{
	for (size_t i = tracks.size(); i-- > 0; )
	{
		Track& track = tracks[i];
		if (FAD < track.StartFAD || (track.EndFAD != 0 && FAD > track.EndFAD))
			continue;
		*subcode_type = SUBFMT_NONE;
		if (track.Read(FAD, dst, sector_type, subcode, subcode_type))
			return true;
	}

	return false;
}

u32 Disc::ReadSectors(u32 FAD, u32 count, u8* dst, u32 fmt, bool stopOnMiss, LoadProgress *progress)
{
	u8 temp[2448];
	SectorFormat secfmt;
	SubcodeFormat subfmt;

	for (u32 i = 0; i < count; i++)
	{
		subfmt = SUBFMT_NONE;
		if (progress != nullptr)
		{
			if (progress->cancelled)
				throw LoadCancelledException();
			progress->label = "Loading...";
			progress->progress = (float)i / count;
		}
		if (FAD >= LeadOut.StartFAD && stopOnMiss)
			return i;
		if (!readSector(FAD, temp, &secfmt, q_subchannel, &subfmt))
		{
			WARN_LOG(GDROM, "Sector Read miss FAD: %d", FAD);
			if (stopOnMiss)
				return i;
			memset(temp, 0, sizeof(temp));
			secfmt = SECFMT_2352;
		}

		//TODO: Proper sector conversions
		if (secfmt == SECFMT_2352) {
			if (!convertSector(temp, dst, 2352, fmt, subfmt == SUBFMT_96))
				return i;
		}
		else if (fmt == 2048 && secfmt == SECFMT_2336_MODE2) {
			memcpy(dst, temp + 8, 2048);
		}
		else if (fmt == 2048 && (secfmt == SECFMT_2048_MODE1 || secfmt == SECFMT_2048_MODE2_FORM1)) {
			memcpy(dst, temp, 2048);
		}
		else if (secfmt == SECFMT_2048_MODE1 || secfmt == SECFMT_2048_MODE2_FORM1
				|| secfmt == SECFMT_2324_MODE2_FORM2
				|| secfmt == SECFMT_2336_MODE2) {
			u8 rawSector[2352];
			synthesizeRawSector(temp, rawSector, secfmt, FAD);
			if (!convertSector(rawSector, dst, 2352, fmt, subfmt == SUBFMT_96))
				return i;
		}
		else if (secfmt == SECFMT_2448_MODE2) {
			if (!convertSector(temp, dst, 2448, fmt))
				return i;
		}
		else {
			WARN_LOG(GDROM, "ERROR: UNABLE TO CONVERT SECTOR. THIS IS FATAL. Format: %d Sector format: %d", fmt, secfmt);
			return i;
		}
		dst += fmt;
		FAD++;
	}
	return count;
}

void libGDR_ReadSubChannel(u8 *buff, u32 len, u32 fad)
{
	// q_subchannel is populated as a side effect of sector conversion. Refresh
	// it at the requested head position instead of returning data cached by an
	// unrelated earlier read.
	if (disc != nullptr)
	{
		u8 sector[2352];
		if (disc->ReadSectors(fad, 1, sector, sizeof(sector), true) != 1)
			memset(q_subchannel, 0, sizeof(q_subchannel));
	}
	else
		memset(q_subchannel, 0, sizeof(q_subchannel));
	memcpy(buff, q_subchannel, len);
}

u32 libGDR_GetDiscType()
{
	// Pretend no disk is inserted if a disk swapping is in progress
	if (!sh4_sched_is_scheduled(schedId) && disc != nullptr)
		return disc->type;
	else
		return NullDriveDiscType;
}

static int discSwapCallback(int tag, int sch_cycl, int jitter, void *arg)
{
	if (disc != nullptr)
		// The lid was closed
		sns_asc = 0x28;
	else
		// No disc inserted at the time of power-on, reset or hard reset, or TOC cannot be read.
		sns_asc = 0x29;
	sns_ascq = 0;
	sns_key = 6;
	gd_setdisc(true);

	return 0;
}

namespace gdr
{

void insertDisk(const std::string& path)
{
	if (!doDiscSwap(path))
		throw FlycastException(i18n::Ts("This media cannot be loaded"));
	gd_media_change();
	settings.content.path = path;
	// Drive is busy after the lid was closed
	sns_asc = 4;
	sns_ascq = 1;
	sns_key = 2;
	SecNumber.Status = GD_BUSY;
	sh4_sched_request(schedId, SH4_MAIN_CLOCK); // 1 s
}

}

void libGDR_init()
{
	verify(schedId == -1);
	schedId = sh4_sched_register(0, discSwapCallback);
}
void libGDR_term()
{
	gdr::termDrive();
	sh4_sched_unregister(schedId);
	schedId = -1;
}

void libGDR_serialize(Serializer& ser)
{
	ser << NullDriveDiscType;
	ser << q_subchannel;
	sh4_sched_serialize(ser, schedId);
}
void libGDR_deserialize(Deserializer& deser)
{
	deser >> NullDriveDiscType;
	deser >> q_subchannel;
	if (deser.version() >= Deserializer::V46)
		sh4_sched_deserialize(deser, schedId);
	else
		sh4_sched_request(schedId, -1);
}
