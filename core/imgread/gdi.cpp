#include "common.h"
#include "stdclass.h"
#include "oslib/storage.h"
#include "oslib/i18n.h"
#include <sstream>

static Disc* load_gdi(const char* file, std::vector<u8> *digest)
{
	std::unique_ptr<hostfs::File> descriptor(hostfs::storage().openFile(file, "rb"));
	if (descriptor == nullptr)
	{
		WARN_LOG(COMMON, "Cannot open file '%s' errno %d", file, errno);
		throw FlycastException(strprintf(i18n::T("Cannot open GDI file %s"), file));
	}

	hostfs::FileInfo fileInfo = hostfs::storage().getFileInfo(file);
	size_t gdi_len = fileInfo.size;

	char gdi_data[16384] {};

	if (gdi_len >= sizeof(gdi_data))
	{
		throw FlycastException(i18n::Ts("GDI file too big"));
	}

	if (descriptor->read(gdi_data, 1, gdi_len) != gdi_len)
		throw FlycastException(i18n::Ts("Invalid GDI file: truncated descriptor"));

	std::istringstream gdi(gdi_data);
	gdi.imbue(std::locale::classic());

	u32 trackCount = 0;
	gdi >> trackCount;
	if (trackCount < 3 || trackCount > 99) {
		WARN_LOG(GDROM, "Invalid track count: %d", trackCount);
		throw FlycastException(i18n::Ts("Invalid GDI file"));
	}

	INFO_LOG(GDROM, "GDI: %d tracks", trackCount);

	std::string basepath = hostfs::storage().getParentPath(file);

	MD5Sum md5;

	std::unique_ptr<Disc> disc = std::make_unique<Disc>();
	for (u32 i = 0; i < trackCount; i++)
	{
		std::string track_filename;

		u32 TRACK = 0, FADS = 0, CTRL = 0, SSIZE = 0;
		s32 OFFSET = 0;
		//TRACK FADS CTRL SSIZE file OFFSET
		if (!(gdi >> TRACK >> FADS >> CTRL >> SSIZE))
			throw FlycastException(i18n::T("Invalid GDI file"));

		char last;
		do {
			if (!(gdi >> last))
				throw FlycastException(i18n::T("Invalid GDI file"));
		} while (isspace((unsigned char)last));
		
		if (last == '"')
		{
			gdi >> std::noskipws;
			for(;;) {
				if (!(gdi >> last))
					throw FlycastException(i18n::T("Invalid GDI file"));
				if (last == '"')
					break;
				track_filename += last;
			}
			gdi >> std::skipws;
		}
		else
		{
			gdi >> track_filename;
			track_filename = last + track_filename;
		}
		if (!(gdi >> OFFSET))
			throw FlycastException(i18n::T("Invalid GDI file"));

		if (TRACK != i + 1) {
			WARN_LOG(GDROM, "Track number out of bounds: %d", TRACK);
			throw FlycastException(i18n::Ts("Invalid GDI file"));
		}
		if (CTRL != 0 && CTRL != 4) {
			WARN_LOG(GDROM, "Unsupported track type: %d", CTRL);
			throw FlycastException(i18n::Ts("Invalid GDI file"));
		}
		switch (TRACK)
		{
		case 1:
			if (CTRL != 4) {
				WARN_LOG(GDROM, "Track 1 must be a data track");
				throw FlycastException(i18n::Ts("Invalid GDI file"));
			}
			break;
		case 2:
			if (CTRL != 0) {
				WARN_LOG(GDROM, "Track 2 must be an audio track");
				throw FlycastException(i18n::Ts("Invalid GDI file"));
			}
			break;
		case 3:
			if (CTRL != 4) {
				WARN_LOG(GDROM, "Track 3 must be a data track");
				throw FlycastException(i18n::Ts("Invalid GDI file"));
			}
			if (FADS != 45000) {
				WARN_LOG(GDROM, "Track 3 should start at 45000, not %d", FADS);
				throw FlycastException(i18n::Ts("Invalid GDI file"));
			}
			break;
		default:
			break;
		}
		if (SSIZE != 2352 && SSIZE != 2048) {
			WARN_LOG(GDROM, "Unsupported sector size: %d", SSIZE);
			throw FlycastException(i18n::Ts("Invalid GDI file"));
		}
		if (OFFSET < 0)
			throw FlycastException(i18n::Ts("Invalid GDI file: negative track offset"));
		if (FADS > 0xffffff - 150)
			throw FlycastException(i18n::Ts("Invalid GDI file: track address overflow"));

		DEBUG_LOG(GDROM, "file[%d] \"%s\": FAD:%d, CTRL:%d, SSIZE:%d, OFFSET:%d", TRACK, track_filename.c_str(), FADS, CTRL, SSIZE, OFFSET);

		Track t;
		t.StartFAD = FADS + 150;
		t.CTRL = CTRL;

		std::string path = hostfs::storage().getSubPath(basepath, track_filename);
		std::unique_ptr<hostfs::File> trackFile(hostfs::storage().openFile(path, "rb"));
		if (trackFile == nullptr)
			throw FlycastException(strprintf(i18n::T("GDI file: Cannot open track %s"), path.c_str()));
		if (digest != nullptr)
			md5.add(trackFile.get());
		hostfs::FileInfo fileInfo = hostfs::storage().getFileInfo(path);
		if ((u64)OFFSET > fileInfo.size)
			throw FlycastException(i18n::Ts("Invalid GDI file: track offset exceeds file size"));
		if (fileInfo.size - OFFSET < SSIZE)
			throw FlycastException(i18n::Ts("Invalid GDI file: empty track"));
		if ((fileInfo.size - OFFSET) % SSIZE != 0)
			WARN_LOG(GDROM, "Warning: Size of track %s is not multiple of sector size %d", track_filename.c_str(), SSIZE);
		const u64 sectorCount = (fileInfo.size - OFFSET) / SSIZE;
		if (sectorCount > std::numeric_limits<u32>::max() - t.StartFAD)
			throw FlycastException(i18n::Ts("Invalid GDI file: track range overflow"));
		t.EndFAD = t.StartFAD + (u32)sectorCount - 1;
		if (!disc->tracks.empty())
		{
			if (t.StartFAD <= disc->tracks.back().StartFAD)
				throw FlycastException(i18n::Ts("Invalid GDI file: tracks are not ordered"));
			disc->tracks.back().EndFAD = std::min(disc->tracks.back().EndFAD, t.StartFAD - 1);
		}
		t.file = new RawTrackFile(trackFile.release(), OFFSET, t.StartFAD, SSIZE,
				SSIZE == 2048 ? SECFMT_2048_MODE1 : SECFMT_2352);
		disc->tracks.push_back(t);
	}

	if (disc->tracks.size() != trackCount) {
		WARN_LOG(GDROM, "Track count %d but found %d tracks", trackCount, (int)disc->tracks.size());
		throw FlycastException(i18n::Ts("Invalid GDI file"));
	}
	disc->FillGDSession();
	if (digest != nullptr)
		*digest = md5.getDigest();

	return disc.release();
}


Disc* gdi_parse(const char* file, std::vector<u8> *digest)
{
	if (get_file_extension(file) != "gdi")
		return nullptr;

	return load_gdi(file, digest);
}
