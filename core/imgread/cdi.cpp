#include "common.h"
#include "stdclass.h"
#include "oslib/storage.h"
#include "oslib/i18n.h"
#include "chdpsr/cdipsr.h"

Disc* cdi_parse(const char* file, std::vector<u8> *digest)
{
	if (get_file_extension(file) != "cdi")
		return nullptr;

	hostfs::File *fsource = hostfs::storage().openFile(file, "rb");

	if (fsource == nullptr)
	{
		WARN_LOG(GDROM, "Cannot open file '%s' errno %d", file, errno);
		throw FlycastException(strprintf(i18n::T("Cannot open CDI file %s"), file));
	}
	try {
		image_s image = { 0 };
		track_s track = { 0 };
		if (!CDI_init(fsource, &image, file))
			throw FlycastException(strprintf(i18n::T("Invalid CDI file %s"), file));
		if ((u64)image.header_offset >= (u64)image.length)
			throw FlycastException(i18n::Ts("Invalid CDI image: header offset is out of range"));
		const u64 dataEnd = image.version == CDI_V35
				? (u64)image.length - image.header_offset : (u64)image.header_offset;
	
		if (!CDI_get_sessions(fsource, &image)) {
			WARN_LOG(GDROM, "Invalid CDI file '%s': can't get sessions", file);
			throw FlycastException(i18n::Ts("Invalid CDI image"));
		}
		if (image.sessions > 99) {
			WARN_LOG(GDROM, "Invalid CDI file '%s': too many sessions (%d)", file, image.sessions);
			throw FlycastException(i18n::Ts("Invalid CDI image"));
		}
		if (image.sessions == 0)
			throw FlycastException(i18n::Ts("Invalid CDI image: no sessions"));

		std::unique_ptr<Disc> rv = std::make_unique<Disc>();

		image.remaining_sessions = image.sessions;

		/////////////////////////////////////////////////////////////// Loop sessions

		bool ft=true, CD_M2=false,CD_M1=false,CD_DA=false;

		while(image.remaining_sessions > 0)
		{
			ft=true;
			image.global_current_session++;

			if (!CDI_get_tracks(fsource, &image)) {
				WARN_LOG(GDROM, "Invalid CDI file '%s': can't get tracks", file);
				throw FlycastException(i18n::Ts("Invalid CDI image"));
			}

			image.header_position = fsource->tell();

			if (image.tracks == 0)
				INFO_LOG(GDROM, "Open session");
			else
			{
				if (image.tracks > 100) {
					WARN_LOG(GDROM, "Invalid CDI file '%s': too many tracks (%d)", file, image.tracks);
					throw FlycastException(i18n::Ts("Invalid CDI image"));
				}
				// Clear cuesheet
				image.remaining_tracks = image.tracks;

				///////////////////////////////////////////////////////////////// Loop tracks

				while(image.remaining_tracks > 0)
				{
					track.global_current_track++;
					track.number = image.tracks - image.remaining_tracks + 1;

					if (!CDI_read_track (fsource, &image, &track))
						throw FlycastException(i18n::Ts("Invalid CDI image"));
					if (track.global_current_track > 99)
						throw FlycastException(i18n::Ts("Invalid CDI image: more than 99 tracks"));
					if (track.length <= 0 || track.pregap_length < 0 || track.total_length <= 0)
						throw FlycastException(i18n::Ts("Invalid CDI image: invalid track length"));
					if ((u64)track.total_length < (u64)track.length + (u64)track.pregap_length)
						throw FlycastException(i18n::Ts("Invalid CDI image: truncated track metadata"));

					if (track.sector_size != 2048 && track.sector_size != 2336 && track.sector_size != 2352 && track.sector_size != 2448)
					{
						WARN_LOG(GDROM, "Invalid sector size: %lu", track.sector_size);
						throw FlycastException(i18n::Ts("Invalid CDI sector size"));
					}
					if (track.position < 0 || (u64)track.position > dataEnd
							|| (u64)track.total_length > (dataEnd - (u64)track.position) / track.sector_size)
						throw FlycastException(i18n::Ts("Invalid CDI image: track data exceeds image"));
					const u64 startFad = (u64)track.start_lba + track.pregap_length;
					const u64 endFad = startFad + track.length - 1;
					const u64 nextFad = (u64)track.start_lba + track.total_length;
					if (startFad > 0xffffff || endFad > 0xffffff || nextFad > 0xffffff)
						throw FlycastException(i18n::Ts("Invalid CDI image: track address overflow"));
					if (!rv->tracks.empty() && startFad <= rv->tracks.back().EndFAD)
						throw FlycastException(i18n::Ts("Invalid CDI image: overlapping track addresses"));

					image.header_position = fsource->tell();

					// Show info
#if 0
					printf("Saving  ");
					printf("Track: %2d  ",track.global_current_track);
					printf("Type: ");
					switch(track.mode)
					{
					case 0 : printf("Audio/"); break;
					case 1 : printf("Mode1/"); break;
					case 2 :
					default: printf("Mode2/"); break;
					}
					printf("%lu  ",track.sector_size);

					printf("Pregap: %-3ld  ",track.pregap_length);
					printf("Size: %-6ld  ",track.length);
					printf("LBA: %-6ld  ",track.start_lba);
#endif
					if (ft)
					{
						ft = false;
						Session s;
						s.StartFAD = startFad;
						s.FirstTrack = (u8)track.global_current_track;
						rv->sessions.push_back(s);
					}

					Track t;
					if (track.mode==2)
						CD_M2=true;
					if (track.mode==1)
						CD_M1=true;
					if (track.mode==0)
						CD_DA=true;

					t.ADR=1;//hmm is that ok ?

					t.CTRL=track.mode==0?0:4;
					t.StartFAD = startFad;
					t.EndFAD = endFad;
					hostfs::File *trackFile = hostfs::storage().openFile(file, "rb");
					if (trackFile == nullptr) {
						WARN_LOG(GDROM, "Cannot re-open file '%s' errno %d", file, errno);
						throw FlycastException(i18n::Ts("Cannot re-open CDI file"));
					}
					SectorFormat sectorFormat = SECFMT_2352;
					if (track.sector_size == 2048)
						sectorFormat = track.mode == 1 ? SECFMT_2048_MODE1 : SECFMT_2048_MODE2_FORM1;
					else if (track.sector_size == 2336)
						sectorFormat = SECFMT_2336_MODE2;
					else if (track.sector_size == 2448)
						sectorFormat = SECFMT_2448_MODE2;
					t.file = new RawTrackFile(trackFile,
							track.position + track.pregap_length * track.sector_size,
							t.StartFAD, track.sector_size, sectorFormat);

					rv->tracks.push_back(t);

					fsource->seek(track.position, SEEK_SET);
					fsource->seek(track.total_length * track.sector_size, SEEK_CUR);
					rv->EndFAD = nextFad - 1;
					track.position = fsource->tell();

					fsource->seek(image.header_position, SEEK_SET);

					image.remaining_tracks--;
				}
			}

			CDI_skip_next_session(fsource, &image);

			image.remaining_sessions--;
		}
		if (rv->tracks.empty())
			throw FlycastException(i18n::Ts("Invalid CDI image: no tracks"));
		if (digest != nullptr)
			*digest = MD5Sum().add(fsource).getDigest();
		delete fsource;

		rv->type=GuessDiscType(CD_M1,CD_M2,CD_DA);

		rv->LeadOut.StartFAD = ++rv->EndFAD;
		rv->LeadOut.ADR = 1;
		rv->LeadOut.CTRL = rv->tracks.empty() ? 4 : rv->tracks.back().CTRL;

		return rv.release();
	} catch (...) {
		delete fsource;
		throw;
	}
}
