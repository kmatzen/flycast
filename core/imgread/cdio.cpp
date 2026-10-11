/*
	Copyright 2024 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "build.h"
#ifdef USE_LIBCDIO
#include "types.h"
#include "imgread/common.h"
#include <cstring>
#include <cdio/cdio.h>
#include <cdio/logging.h>
#include <vector>

namespace hostfs
{

const std::vector<std::string>& getCdromDrives()
{
	static std::vector<std::string> cdromDevices;
	static bool devicesFetched;

	if (devicesFetched)
		return cdromDevices;
	devicesFetched = true;
	// Set a custom log handler
	cdio_log_set_handler([](cdio_log_level_t level, const char message[]) {
		switch (level)
		{
		case CDIO_LOG_DEBUG:
			DEBUG_LOG(GDROM, "%s", message);
			break;
		case CDIO_LOG_INFO:
			INFO_LOG(GDROM, "%s", message);
			break;
		case CDIO_LOG_WARN:
			WARN_LOG(GDROM, "%s", message);
			break;
		case CDIO_LOG_ERROR:
		case CDIO_LOG_ASSERT:
			ERROR_LOG(GDROM, "%s", message);
			break;
		}
	});
	// Get the list of all hardware devices
	char **list = cdio_get_devices(DRIVER_DEVICE);
	if (list != nullptr)
	{
		for (char **dev = &list[0]; *dev != nullptr; dev++)
			cdromDevices.push_back(*dev);
		cdio_free_device_list(list);
	}

	return cdromDevices;
}

}

struct CdioDrive;

struct CdioTrack : public TrackFile
{
	CdioTrack(CdioDrive& disk, track_format_t format)
		: disk(disk), format(format) {}
	bool Read(u32 FAD, u8 *dst, SectorFormat *sector_type, u8 *subcode, SubcodeFormat *subcode_type);

	CdioDrive& disk;
	track_format_t format;
};

struct CdioDrive : public Disc
{
	bool open(const char *path)
	{
		const std::vector<std::string>& devices = hostfs::getCdromDrives();
		if (!devices.empty())
		{
			// If the list isn't empty, check that an entry exists for the current path
			std::string lpath(path);
#ifdef _WIN32
			if (lpath.substr(0, 4) != "\\\\.\\")
				lpath = "\\\\.\\" + lpath;
#endif
			bool found = false;
			for (const std::string& dev : devices)
				if (dev == lpath) {
					found = true;
					break;
				}
			if (!found) {
				WARN_LOG(GDROM, "%s isn't a CD device", path);
				return false;
			}
		}
		pCdio = cdio_open(path, DRIVER_DEVICE);
		if (pCdio == nullptr) {
			WARN_LOG(GDROM, "Can't open CD device %s", path);
			return false;
		}
		track_t firstTrk = cdio_get_first_track_num(pCdio);
		track_t lastTrk = cdio_get_last_track_num(pCdio);
		if (firstTrk == CDIO_INVALID_TRACK || lastTrk == CDIO_INVALID_TRACK)
		{
			WARN_LOG(GDROM, "Can't find first and/or last track");
			close();
			return false;
		}

		Session session;
		firstTrack = firstTrk;
		session.StartFAD = 150;
		session.FirstTrack = firstTrk;
		sessions.push_back(session);
		if (char *mcn = cdio_get_mcn(pCdio); mcn != nullptr)
		{
			catalog = mcn;
			cdio_free(mcn);
		}
		type = CdDA;	// TODO more CD types
		bool hasAudio = false;
		bool hasData = false;
		bool hasXa = false;
		bool hasCdi = false;

		for (int i = firstTrk; i <= lastTrk; i++)
		{
			lba_t lba = cdio_get_track_lba(pCdio, i);
			if (lba == CDIO_INVALID_LBA || lba < 0 || static_cast<u64>(lba) > 0xffffff)
			{
				WARN_LOG(GDROM, "Invalid start address for track %d: %d", i, lba);
				close();
				return false;
			}
			track_format_t format = cdio_get_track_format(pCdio, i);
			if (format == TRACK_FORMAT_ERROR)
			{
				WARN_LOG(GDROM, "Can't determine format of track %d", i);
				close();
				return false;
			}
			hasAudio |= format == TRACK_FORMAT_AUDIO;
			hasData |= format != TRACK_FORMAT_AUDIO;
			hasXa |= format == TRACK_FORMAT_XA;
			hasCdi |= format == TRACK_FORMAT_CDI;
			Track t;
			t.ADR = 1;	// Current-position Q mode.
			t.CTRL = format == TRACK_FORMAT_AUDIO ? 0 : CDIO_CDROM_DATA_TRACK;
			if (char *isrc = cdio_get_track_isrc(pCdio, i); isrc != nullptr)
			{
				t.isrc = isrc;
				cdio_free(isrc);
			}
			if (format == TRACK_FORMAT_AUDIO)
			{
				if (cdio_get_track_preemphasis(pCdio, i) == CDIO_TRACK_FLAG_TRUE)
					t.CTRL |= CDIO_TRACK_FLAG_PRE_EMPHASIS;
				if (cdio_get_track_copy_permit(pCdio, i) == CDIO_TRACK_FLAG_TRUE)
					t.CTRL |= CDIO_TRACK_FLAG_COPY_PERMITTED;
				if (cdio_get_track_channels(pCdio, i) == 4)
					t.CTRL |= CDIO_TRACK_FLAG_FOUR_CHANNEL_AUDIO;
			}
			t.StartFAD = lba;
			lsn_t last = cdio_get_track_last_lsn(pCdio, i);
			if (last == CDIO_INVALID_LSN)
			{
				WARN_LOG(GDROM, "Can't get last address of track %d", i);
				close();
				return false;
			}
			lba_t end = cdio_lsn_to_lba(last);
			if (end < lba || static_cast<u64>(end) > 0xffffff)
			{
				WARN_LOG(GDROM, "Invalid end address for track %d: %d", i, end);
				close();
				return false;
			}
			t.EndFAD = end;
			if (i == firstTrk)
				sessions.front().StartFAD = t.StartFAD;
			INFO_LOG(GDROM, "Track #%d: start %d end %d format %d control %x", i,
					t.StartFAD, t.EndFAD, format, t.CTRL);
			t.file = new CdioTrack(*this, format);
			tracks.push_back(t);
		}

		// Some physical-device backends can report the first sector of the
		// last session directly. Use it to expose CD Extra/MIL-CD session data
		// through REQ_SES. Backends such as macOS do not implement this query,
		// so fall back to the characteristic audio-to-data track transition.
		size_t lastSessionTrack = 0;
		lsn_t lastSessionLsn;
		if (cdio_get_last_session(pCdio, &lastSessionLsn) == DRIVER_OP_SUCCESS
				&& lastSessionLsn != CDIO_INVALID_LSN)
		{
			const lba_t lastSessionFad = cdio_lsn_to_lba(lastSessionLsn);
			for (size_t i = 0; i < tracks.size(); i++)
			{
				if (lastSessionFad >= static_cast<lba_t>(tracks[i].StartFAD)
						&& lastSessionFad <= static_cast<lba_t>(tracks[i].EndFAD))
				{
					lastSessionTrack = i;
					break;
				}
				if (lastSessionFad < static_cast<lba_t>(tracks[i].StartFAD))
				{
					lastSessionTrack = i;
					break;
				}
			}
		}
		else
		{
			for (size_t i = 1; i < tracks.size(); i++)
				if (!tracks[i - 1].isDataTrack() && tracks[i].isDataTrack())
				{
					lastSessionTrack = i;
					break;
				}
		}
		if (lastSessionTrack != 0)
		{
			Session lastSession;
			lastSession.StartFAD = tracks[lastSessionTrack].StartFAD;
			lastSession.FirstTrack = static_cast<u8>(firstTrack + lastSessionTrack);
			sessions.push_back(lastSession);
			INFO_LOG(GDROM, "Last session: track %u FAD %u", lastSession.FirstTrack,
					lastSession.StartFAD);
		}

		lba_t leadout = cdio_get_track_lba(pCdio, CDIO_CDROM_LEADOUT_TRACK);
		if (leadout == CDIO_INVALID_LBA || leadout < 0 || static_cast<u64>(leadout) > 0xffffff
				|| leadout <= static_cast<lba_t>(tracks.back().EndFAD))
		{
			WARN_LOG(GDROM, "Invalid leadout address: %d", leadout);
			close();
			return false;
		}
		LeadOut.StartFAD = leadout;
		LeadOut.ADR = 1;
		LeadOut.CTRL = tracks.back().CTRL;
		EndFAD = leadout;
		type = hasCdi ? CdRom_CDI : !hasData ? CdDA : hasAudio || hasXa ? CdRom_XA : CdRom;

		return true;
	}

	void close()
	{
		if (pCdio != nullptr) {
			cdio_destroy(pCdio);
			pCdio = nullptr;
		}
	}
	~CdioDrive() {
		close();
	}

	CdIo_t *pCdio = nullptr;
};

bool CdioTrack::Read(u32 FAD, u8 *dst, SectorFormat *sector_type, u8 *subcode, SubcodeFormat *subcode_type)
{
	lsn_t lsn = cdio_lba_to_lsn(FAD);
	if (format == TRACK_FORMAT_AUDIO)
	{
		*sector_type = SECFMT_2352;
		if (cdio_read_audio_sector(disk.pCdio, dst, lsn) != DRIVER_OP_SUCCESS) {
			WARN_LOG(GDROM, "Read audio fad %d failed", FAD);
			return false;
		}
	}
	else if (format == TRACK_FORMAT_DATA)
	{
		*sector_type = SECFMT_2048_MODE1;
		if (cdio_read_mode1_sector(disk.pCdio, dst, lsn, false) != DRIVER_OP_SUCCESS)
		{
			WARN_LOG(GDROM, "Read mode 1 data fad %d failed", FAD);
			return false;
		}
	}
	else
	{
		if (cdio_read_mode2_sector(disk.pCdio, dst, lsn, false) == DRIVER_OP_SUCCESS)
		{
			*sector_type = SECFMT_2048_MODE2_FORM1;
		}
		else if (cdio_read_mode2_sector(disk.pCdio, dst, lsn, true) == DRIVER_OP_SUCCESS)
		{
			// libcdio requests the cooked Mode 2 Form 2 user-data area from
			// physical-device backends (2324 bytes, without the XA subheader).
			*sector_type = SECFMT_2324_MODE2_FORM2;
		}
		else
		{
			WARN_LOG(GDROM, "Read mode 2 data fad %d failed", FAD);
			return false;
		}
	}

	return true;
}

Disc *cdio_parse(const char *file, std::vector<u8> *digest)
{
	INFO_LOG(GDROM, "Opening CDIO device %s", file);
	CdioDrive *disk = new CdioDrive();

	if (disk->open(file))
	{
		if (digest != nullptr)
			digest->clear();
		return disk;
	}
	else {
		delete disk;
		return nullptr;
	}
}

#endif	// USE_LIBCDIO
