#include "common.h"

extern Disc* disc;

u32 libGDR_GetTrackNumber(u32 sector, u32& elapsed)
{
	if (disc != nullptr)
	{
		const Track *precedingTrack = nullptr;
		for (size_t i = 0; i < disc->tracks.size(); i++)
		{
			if (disc->tracks[i].StartFAD <= sector && (sector <= disc->tracks[i].EndFAD || disc->tracks[i].EndFAD == 0))
			{
				elapsed = sector - disc->tracks[i].StartFAD;
				return disc->firstTrack + i;
			}
			if (disc->tracks[i].StartFAD <= sector)
				precedingTrack = &disc->tracks[i];
		}
		if (disc->LeadOut.StartFAD != 0 && sector >= disc->LeadOut.StartFAD)
		{
			elapsed = sector - disc->LeadOut.StartFAD;
			return 0xaa;
		}
		if (precedingTrack != nullptr && precedingTrack->EndFAD != 0
				&& sector > precedingTrack->EndFAD)
		{
			// An unmapped interval after a session's final track is its
			// intermediate lead-out/lead-in area. Report lead-out-relative
			// position instead of pinning every frame to zero.
			elapsed = sector - (precedingTrack->EndFAD + 1);
			return 0xaa;
		}
	}
	elapsed = 0;
	return 0xAA;
}

bool libGDR_GetTrack(u32 track_num, u32& start_fad, u32& end_fad)
{
	if (disc == nullptr || track_num < disc->firstTrack
			|| track_num - disc->firstTrack >= disc->tracks.size())
		return false;
	const size_t index = track_num - disc->firstTrack;
	start_fad = disc->tracks[index].StartFAD;
	end_fad = disc->tracks[index].EndFAD;
	if (end_fad == 0)
	{
		if (index + 1 == disc->tracks.size())
			end_fad = disc->LeadOut.StartFAD - 1;
		else
			end_fad = disc->tracks[index + 1].StartFAD - 1;
	}

	return true;
}

std::string libGDR_GetDiskCatalog()
{
	if (disc != nullptr)
		return disc->catalog;
	else
		return "";
}

std::string libGDR_GetTrackIsrc(u32 trackNum)
{
	if (disc == nullptr || trackNum < disc->firstTrack
			|| trackNum - disc->firstTrack >= disc->tracks.size())
		return "";
	return disc->tracks[trackNum - disc->firstTrack].isrc;
}

void libGDR_GetTrackAdrAndControl(u32 trackNum, u8& adr, u8& ctrl)
{
	if (disc != nullptr && trackNum == 0xaa)
	{
		adr = disc->LeadOut.ADR == 0 ? 1 : disc->LeadOut.ADR;
		ctrl = disc->LeadOut.CTRL;
	}
	else if (disc == nullptr || trackNum < disc->firstTrack
			|| trackNum - disc->firstTrack >= disc->tracks.size())
	{
		adr = 0;
		ctrl = 0;
	}
	else
	{
		const Track& track = disc->tracks[trackNum - disc->firstTrack];
		adr = track.ADR == 0 ? 1 : track.ADR;
		ctrl = track.CTRL;
	}
}

void libGDR_GetTrackAdrAndControlAtFad(u32 fad, u8& adr, u8& ctrl)
{
	u32 elapsed;
	const u32 trackNum = libGDR_GetTrackNumber(fad, elapsed);
	if (disc != nullptr && trackNum == 0xaa && fad < disc->LeadOut.StartFAD)
	{
		// Intermediate lead-out belongs to the preceding program area.
		// Its control/ADR therefore follows that area's final track, not
		// the outer disc lead-out descriptor.
		for (size_t i = disc->tracks.size(); i-- > 0;)
			if (disc->tracks[i].StartFAD <= fad)
			{
				adr = disc->tracks[i].ADR == 0 ? 1 : disc->tracks[i].ADR;
				ctrl = disc->tracks[i].CTRL;
				return;
			}
	}
	libGDR_GetTrackAdrAndControl(trackNum, adr, ctrl);
}
