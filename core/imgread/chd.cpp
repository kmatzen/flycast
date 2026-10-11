#include "common.h"
#include "stdclass.h"
#include "oslib/storage.h"
#include "oslib/i18n.h"
#include "chd.h"

struct CHDDisc : Disc
{
	// tracks are padded to a multiple of this many frames
	static constexpr u32 CD_TRACK_PADDING = 4;
	// lead out (01:30:00), lead in (01:00:00) and pregap (00:02:00) between 2 sessions of MIL-CDs
	static constexpr u32 SESSION_GAP = 6750 + 4500 + 150;

	chd_file *chd = nullptr;
	u8* hunk_mem = nullptr;
	u32 old_hunk = 0;

	u32 hunkbytes = 0;
	u32 sph = 0;

	void tryOpen(const char* file);

	~CHDDisc() override
	{
		delete[] hunk_mem;

		if (chd)
			chd_close(chd);
	}
};

struct CHDTrack : TrackFile
{
	enum class Subcode { None, PackedRW, RawPW };

	CHDDisc* disc;
	s32 Offset;
	u32 fmt;
	SectorFormat sectorFormat;
	bool swap_bytes;
	Subcode subcodeFormat;

	CHDTrack(CHDDisc* disc, s32 Offset, u32 fmt, SectorFormat sectorFormat,
			bool swap_bytes, Subcode subcodeFormat)
	{
		this->disc = disc;
		this->Offset = Offset;
		this->fmt = fmt;
		this->sectorFormat = sectorFormat;
		this->swap_bytes = swap_bytes;
		this->subcodeFormat = subcodeFormat;
	}

	bool Read(u32 FAD, u8* dst, SectorFormat* sector_type, u8* subcode, SubcodeFormat* subcode_type) override
	{
		u32 fad_offs = FAD + Offset;
		u32 hunk=(fad_offs)/disc->sph;
		if (disc->old_hunk!=hunk)
		{
			if (chd_read(disc->chd,hunk,disc->hunk_mem) != CHDERR_NONE)
				return false;
			disc->old_hunk = hunk;
		}

		u32 hunk_ofs = fad_offs%disc->sph;

		memcpy(dst, disc->hunk_mem + hunk_ofs * (2352+96), fmt);

		if (swap_bytes)
		{
			for (u32 i = 0; i < fmt; i += 2)
			{
				u8 b = dst[i];
				dst[i] = dst[i + 1];
				dst[i + 1] = b;
			}
		}
		*sector_type = sectorFormat;

		if (subcodeFormat == Subcode::None)
			*subcode_type = SUBFMT_NONE;
		else
		{
			const u8 *source = disc->hunk_mem + hunk_ofs * (2352 + 96) + 2352;
			memcpy(subcode, source, 96);
			if (subcodeFormat == Subcode::PackedRW)
				for (u32 i = 0; i < 96; i++)
					subcode[i] &= 0x3f;
			*subcode_type = SUBFMT_96;
		}

		return true;
	}
};

static u32 getSectorSize(const std::string& type)
{
	if (type == "AUDIO")
		return 2352;	// PCM Audio
	else if (type == "MODE1" || type == "MODE1/2048")
		return 2048;	// CDROM Mode1 Data (cooked)
	else if (type == "MODE1_RAW" || type == "MODE1/2352")
		return 2352;	// CDROM Mode1 Data (raw)
	else if (type == "MODE2" || type == "MODE2/2336")
		return 2336;	// CDROM XA Mode2 Data
	else if (type == "MODE2_FORM1" || type == "MODE2/2048")
		return 2048;
	else if (type == "MODE2_FORM2" || type == "MODE2/2324")
		return 2324;
	else if (type == "MODE2_FORM_MIX")
		return 2336;
	else if (type == "MODE2_RAW" || type == "MODE2/2352" || type == "CDI/2352")
		return 2352;	// CDROM XA Mode2 Data

	throw FlycastException(strprintf(i18n::T("chd: track type %s is not supported"), type.c_str()));
}

static SectorFormat getSectorFormat(const std::string& type)
{
	if (type == "MODE1" || type == "MODE1/2048")
		return SECFMT_2048_MODE1;
	if (type == "MODE2_FORM1" || type == "MODE2/2048")
		return SECFMT_2048_MODE2_FORM1;
	if (type == "MODE2_FORM2" || type == "MODE2/2324")
		return SECFMT_2324_MODE2_FORM2;
	if (type == "MODE2" || type == "MODE2/2336" || type == "MODE2_FORM_MIX")
		return SECFMT_2336_MODE2;
	return SECFMT_2352;
}

void CHDDisc::tryOpen(const char* file)
{
	hostfs::File *fp = hostfs::storage().openFile(file, "rb");
	if (fp == nullptr)
	{
		WARN_LOG(COMMON, "Cannot open file '%s' errno %d", file, errno);
		throw FlycastException(strprintf(i18n::T("Cannot open CHD file %s"), file));
	}

	chd_error err = chd_open_file(fp, CHD_OPEN_READ, nullptr, &chd);

	if (err != CHDERR_NONE)
		// libchdr closes the file even in case of error (well, except in one case)
		throw FlycastException(strprintf(i18n::T("Invalid CHD file %s"), file));

	INFO_LOG(GDROM, "chd: parsing file %s", file);

	const chd_header* head = chd_get_header(chd);

	hunkbytes = head->hunkbytes;
	hunk_mem = new u8[hunkbytes];
	old_hunk=0xFFFFFFF;

	sph = hunkbytes/(2352+96);

	if (hunkbytes % (2352 + 96) != 0)
		throw FlycastException(strprintf(i18n::T("Invalid hunkbytes for CHD file %s"), file));

	u32 tag;
	u8 flags;
	char temp[512];
	u32 temp_len;
	u32 total_frames = 150;

	u32 Offset = 0;
	bool isGdrom = head->version < 5;	// MIL-CDs only supported starting with CHD v5
	bool needAudioSwap = false;
	u32 lastPregap = 0;
	bool hasMode2Track = false;

	for(;;)
	{
		char type[16], subtype[16], pgtype[16], pgsub[16];
		int tkid=-1, frames=0, pregap=0, postgap=0, padframes=0;
		strcpy(subtype, "NONE");
		strcpy(pgtype, "NONE");
		strcpy(pgsub, "NONE");

		err = chd_get_metadata(chd, CDROM_TRACK_METADATA2_TAG, (u32)tracks.size(), temp, sizeof(temp), &temp_len, &tag, &flags);
		if (err == CHDERR_NONE)
		{
			//"TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d PREGAP:%d PGTYPE:%s PGSUB:%s POSTGAP:%d"
			sscanf(temp, CDROM_TRACK_METADATA2_FORMAT, &tkid, type, subtype, &frames, &pregap, pgtype, pgsub, &postgap);
			// CD-Rom audio tracks are always stored big-endian
			needAudioSwap = true;
		}
		else if (CHDERR_NONE== (err = chd_get_metadata(chd, CDROM_TRACK_METADATA_TAG, (u32)tracks.size(), temp, sizeof(temp), &temp_len, &tag, &flags)) )
		{
			//CDROM_TRACK_METADATA_FORMAT	"TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d"
			sscanf(temp, CDROM_TRACK_METADATA_FORMAT, &tkid, type, subtype, &frames);
			// CD-Rom audio tracks are always stored big-endian
			needAudioSwap = true;
		}
		else
		{
			err = chd_get_metadata(chd, GDROM_OLD_METADATA_TAG, (u32)tracks.size(), temp, sizeof(temp), &temp_len, &tag, &flags);
			if (err != CHDERR_NONE)
			{
				err = chd_get_metadata(chd, GDROM_TRACK_METADATA_TAG, (u32)tracks.size(), temp, sizeof(temp), &temp_len, &tag, &flags);
				if (err == CHDERR_NONE)
					needAudioSwap = true;
			}

			if (err != CHDERR_NONE)
				break;
			//GDROM_TRACK_METADATA_FORMAT	"TRACK:%d TYPE:%s SUBTYPE:%s FRAMES:%d PAD:%d PREGAP:%d PGTYPE:%s PGSUB:%s POSTGAP:%d"
			sscanf(temp, GDROM_TRACK_METADATA_FORMAT, &tkid, type, subtype, &frames, &padframes, &pregap, pgtype, pgsub, &postgap);
			isGdrom = true;
		}

		if (tkid != (int)tracks.size() + 1)
			throw FlycastException(i18n::Ts("Unexpected track number"));
		if (frames <= 0 || pregap < 0 || postgap < 0 || padframes < 0)
			throw FlycastException(i18n::Ts("Invalid CHD track geometry"));

		CHDTrack::Subcode subcodeFormat;
		if (strcmp(subtype, "NONE") == 0)
			subcodeFormat = CHDTrack::Subcode::None;
		else if (strcmp(subtype, "RW") == 0)
			subcodeFormat = CHDTrack::Subcode::PackedRW;
		else if (strcmp(subtype, "RW_RAW") == 0)
			subcodeFormat = CHDTrack::Subcode::RawPW;
		else
			throw FlycastException(strprintf(i18n::T("Invalid CHD subcode type: %s"), subtype));
		DEBUG_LOG(GDROM, "%s", temp);
		// When PGTYPE is prefixed with 'V', the pregap sectors are included in the track data,
		// and thus counted in FRAMES. Otherwise they only take up space on the disc.
		const u32 pregapInFile = pregap > 0 && pgtype[0] == 'V' ? (u32)pregap : 0;
		if ((int)pregapInFile > frames)
			throw FlycastException(i18n::Ts("Invalid CHD: pregap is longer than the track"));
		if ((u32)padframes > (u32)frames - pregapInFile)
			throw FlycastException(i18n::Ts("Invalid CHD: padding is longer than the track"));
		const u64 trackEnd = (u64)total_frames + pregap + frames - pregapInFile;
		if (trackEnd + postgap > std::numeric_limits<u32>::max())
			throw FlycastException(i18n::Ts("Invalid CHD: track range overflow"));

		Track t;
		// StartFAD is the position of INDEX 01, hence after the pregap
		t.StartFAD = total_frames + pregap;
		lastPregap = pregap;
		t.EndFAD = (u32)trackEnd - 1 - padframes;
		// Postgap frames occupy disc addresses but are not stored in the CHD.
		total_frames = (u32)trackEnd + postgap;
		t.CTRL = strcmp(type,"AUDIO") == 0 ? 0 : 4;

		u32 sectorSize = getSectorSize(type);
		hasMode2Track |= strncmp(type, "MODE2", 5) == 0 || strncmp(type, "CDI", 3) == 0;
		t.file = new CHDTrack(this, Offset + pregapInFile - t.StartFAD, sectorSize,
							  getSectorFormat(type),
							  // audio tracks are byteswapped in recent CHDv5+
							  !t.isDataTrack() && needAudioSwap, subcodeFormat);

		// CHD files are padded, so we have to respect the offset
		int padded = (frames + CD_TRACK_PADDING - 1) / CD_TRACK_PADDING;
		Offset += padded * CD_TRACK_PADDING;

		tracks.push_back(t);
	}

	if (isGdrom)
	{
		if (total_frames != 549300)
			WARN_LOG(GDROM, "WARNING: chd: Total GD-Rom frames is wrong: %u frames (549300 expected) in %zu tracks", total_frames, tracks.size());
		if (tracks.size() < 3)
			throw FlycastException(i18n::Ts("Invalid CHD: less than 3 tracks"));
		FillGDSession();
	}
	else
	{
		if (tracks.empty())
			throw FlycastException(i18n::Ts("Invalid CHD: no track found"));

		Session ses;
		ses.FirstTrack = 1;
		ses.StartFAD = tracks[0].StartFAD;
		sessions.push_back(ses);
		DEBUG_LOG(GDROM, "session 1: FAD %d", ses.StartFAD);

		// MIL-CDs are multisession CD-Roms whose last session holds the bootable data track.
		// CHD files don't carry any session information so we assume that a multi-track disc
		// ending with a data track is a MIL-CD, and that its last session has a single track.
		if (tracks.size() > 1 && tracks.back().isDataTrack())
		{
			type = CdRom_XA;
			ses.FirstTrack = tracks.size();
			// SESSION_GAP includes the pregap of the first track of the second session.
			// When these sectors are part of the track data they have already been accounted for.
			const u32 gap = SESSION_GAP - std::min(lastPregap, 150u);
			tracks.back().StartFAD += gap;
			tracks.back().EndFAD += gap;
			((CHDTrack *)tracks.back().file)->Offset -= gap;
			ses.StartFAD = tracks.back().StartFAD;
			sessions.push_back(ses);
			DEBUG_LOG(GDROM, "session 2: track %d FAD %d", ses.FirstTrack, ses.StartFAD);

			EndFAD = LeadOut.StartFAD = total_frames + gap;
		}
		else
		{
			// Single-session disc: mixed mode or audio CD. Single-track CD-ROMs aren't
			// supported with the exception of naomi mj1/cdp-10002b.chd
			const bool hasData = std::any_of(tracks.begin(), tracks.end(),
					[](const Track& track) { return track.isDataTrack(); });
			const bool hasAudio = std::any_of(tracks.begin(), tracks.end(),
					[](const Track& track) { return !track.isDataTrack(); });
			type = !hasData ? CdDA : hasAudio || hasMode2Track ? CdRom_XA : CdRom;
			EndFAD = LeadOut.StartFAD = total_frames;
		}
		LeadOut.ADR = 1;
		LeadOut.CTRL = tracks.back().CTRL;
	}
}


Disc* chd_parse(const char* file, std::vector<u8> *digest)
{
#ifdef LIBRETRO
	if (!strstr(&file[strlen(file) - 3], "chd"))
		return nullptr;
#else
	if (get_file_extension(file) != "chd")
		return nullptr;
#endif

	CHDDisc* rv = new CHDDisc();

	try {
		rv->tryOpen(file);
		if (digest != nullptr)
		{
			digest->resize(sizeof(chd_get_header(rv->chd)->sha1));
			memcpy(digest->data(), chd_get_header(rv->chd)->sha1, digest->size());
		}
		return rv;
	} catch (...) {
		delete rv;
		throw;
	}
}
