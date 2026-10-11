/*
	Basic gdrom syscall emulation
	Adapted from some (very) old pre-nulldc hle code
	Bits and pieces from redream (https://github.com/inolen/redream)
*/

#include <cstdio>
#include "types.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_core.h"

#include "gdrom_hle.h"
#include "hw/gdrom/gdromv3.h"
#include "hw/holly/holly_intc.h"
#include "reios.h"
#include "imgread/common.h"
#include "hw/sh4/modules/mmu.h"

#include <algorithm>
#include <limits>

#define SWAP32(a) ((((a) & 0xff) << 24)  | (((a) & 0xff00) << 8) | (((a) >> 8) & 0xff00) | (((a) >> 24) & 0xff))

#define debugf(...) DEBUG_LOG(REIOS, __VA_ARGS__)
static bool readSectors(u32 addr, u32 sector, u32 count, u32 sectorSize, bool virtualAddr);

struct gdrom_hle_state_t
{
	gdrom_hle_state_t() : params{}, result{} {}

	u32 last_request_id = 0xFFFFFFFF;
	u32 next_request_id = 2;
	gd_return_value status = GDC_OK;
	gd_command command = GDCC_NONE;
	u32 params[4];
	u32 result[4];
	u32 cur_sector = 0;
	u32 multi_read_sector = 0;
	u32 multi_read_offset = 0;
	u32 multi_read_count = 0;
	u32 multi_read_total = 0;
	u32 multi_callback = 0;
	u32 multi_callback_arg = 0;
	bool dma_trans_ended = false;
	u64 xfer_end_time = 0;
	u32 multi_sector_size = 2048;
	u32 transfer_remaining = 0;

	void Serialize(Serializer& ser)
	{
		ser << last_request_id;
		ser << next_request_id;
		ser << status;
		ser << command;
		ser << params;
		ser << result;
		ser << cur_sector;
		ser << multi_read_sector;
		ser << multi_read_offset;
		ser << multi_read_count;
		ser << multi_read_total;
		ser << multi_callback;
		ser << multi_callback_arg;
		ser << dma_trans_ended;
		ser << xfer_end_time;
		ser << multi_sector_size;
		ser << transfer_remaining;

	}
	void Deserialize(Deserializer& deser)
	{
		deser >> last_request_id;
		deser >> next_request_id;
		deser >> status;
		deser >> command;
		deser >> params;
		deser >> result;
		deser >> cur_sector;
		deser >> multi_read_sector;
		deser >> multi_read_offset;
		deser >> multi_read_count;
		deser >> multi_read_total;
		deser >> multi_callback;
		deser >> multi_callback_arg;
		deser >> dma_trans_ended;
		deser >> xfer_end_time;
		if (deser.version() >= Deserializer::V88)
			deser >> multi_sector_size;
		else
			multi_sector_size = 2048;
		if (deser.version() >= Deserializer::V109)
			deser >> transfer_remaining;
		else
			transfer_remaining = command == GDCC_REQ_PIO_TRANS ? params[1] : 0;
	}
};
static gdrom_hle_state_t gd_hle_state;
static u32 SecMode[4] = { 0, 8192, 0, 2048 };

static void setReadError(u32 fad)
{
	gd_hle_state.status = GDC_ERR;
	gd_hle_state.result[0] = 3; // MEDIUM ERROR
	gd_hle_state.result[1] = 0x11; // Unrecoverable read error
	gd_hle_state.result[2] = fad;
	gd_hle_state.result[3] = 0;
}

static void setIllegalRequest()
{
	gd_hle_state.status = GDC_ERR;
	gd_hle_state.result[0] = 5; // ILLEGAL REQUEST
	gd_hle_state.result[1] = 0x24; // Invalid field in CDB
	gd_hle_state.result[2] = 0;
	gd_hle_state.result[3] = 0;
}

static void setInvalidParameter()
{
	gd_hle_state.status = GDC_ERR;
	gd_hle_state.result[0] = 5; // ILLEGAL REQUEST
	gd_hle_state.result[1] = 0x26; // Invalid field in parameter list
	gd_hle_state.result[2] = 0;
	gd_hle_state.result[3] = 0;
}

static void setNoMedia()
{
	gd_hle_state.status = GDC_ERR;
	gd_hle_state.result[0] = 2; // NOT READY
	gd_hle_state.result[1] = 0x3a; // Medium not present
	gd_hle_state.result[2] = 0;
	gd_hle_state.result[3] = 0;
}

static u32 getDiscEndFad()
{
	u8 sessionInfo[6] {};
	libGDR_GetSessionInfo(sessionInfo, 0);
	return (sessionInfo[3] << 16) | (sessionInfo[4] << 8) | sessionInfo[5];
}

static void setInvalidAddress(u32 fad)
{
	gd_hle_state.status = GDC_ERR;
	gd_hle_state.result[0] = 5; // ILLEGAL REQUEST
	gd_hle_state.result[1] = 0x21; // Invalid address
	gd_hle_state.result[2] = fad;
	gd_hle_state.result[3] = 0;
}

static bool validateMediaReady()
{
	const u32 discType = libGDR_GetDiscType();
	if (discType != Open && discType != NoDisk)
		return true;
	setNoMedia();
	return false;
}

static bool validatePositionRequest(u32 fad, u32 sectors)
{
	if (!validateMediaReady())
		return false;

	const u32 endFad = getDiscEndFad();
	if (fad < 150 || fad >= endFad || (sectors != 0 && sectors > endFad - fad))
	{
		setInvalidAddress(fad);
		return false;
	}
	return true;
}

static bool validateReadRequest(u32 fad, u32 sectors)
{
	if (!validatePositionRequest(fad, sectors))
		return false;
	u32 elapsed;
	if (libGDR_GetTrackNumber(fad, elapsed) == 0xaa)
	{
		setInvalidAddress(fad);
		return false;
	}
	return true;
}

static bool normalizeStreamReadRequest(u32 fad, u32 requestedSectors, u32& sectors)
{
	sectors = requestedSectors;
	if (requestedSectors != 0x1ff)
		return validateReadRequest(fad, sectors);

	if (!validateReadRequest(fad, 1))
		return false;
	const u32 endFad = getDiscEndFad();
	sectors = endFad - fad;
	return true;
}

static bool sectorsToBytes(u32 sectors, u32 sectorSize, u32& bytes)
{
	if (sectorSize == 0 || sectors > std::numeric_limits<u32>::max() / sectorSize)
		return false;
	bytes = sectors * sectorSize;
	return true;
}

static bool supportedSectorSize(u32 size)
{
	switch (size)
	{
	case 4:
	case 8:
	case 12:
	case 2048:
	case 2052:
	case 2084:
	case 2088:
	case 2324:
	case 2328:
	case 2336:
	case 2340:
	case 2352:
		return true;
	default:
		return false;
	}
}

static int schedId = -1;

static void resetSectorMode()
{
	SecMode[0] = 0;
	SecMode[1] = 8192;
	SecMode[2] = 0;
	SecMode[3] = 2048;
}

static void cancelActiveTransfer()
{
	if (schedId != -1)
		sh4_sched_request(schedId, -1);
	gd_hle_state.multi_read_sector = 0;
	gd_hle_state.multi_read_offset = 0;
	gd_hle_state.multi_read_count = 0;
	gd_hle_state.multi_read_total = 0;
	gd_hle_state.transfer_remaining = 0;
	gd_hle_state.dma_trans_ended = false;
	gd_hle_state.xfer_end_time = 0;
}

static void resetDriveState()
{
	cancelActiveTransfer();
	gd_hle_state = {};
	resetSectorMode();
	cdda.status = cdda_t::NoInfo;
	switch (libGDR_GetDiscType())
	{
	case Open:
		SecNumber.Status = GD_OPEN;
		break;
	case NoDisk:
		SecNumber.Status = GD_NODISC;
		break;
	default:
		SecNumber.Status = GD_STANDBY;
		break;
	}
	asic_CancelInterrupt(holly_GDROM_DMA);
}

static int getGdromTicks()
{
	const u64 bytes = (u64)std::min(gd_hle_state.multi_read_count, 5u)
			* gd_hle_state.multi_sector_size;
	const u64 busCycles = bytes * 2; // Maximum G1 rate: 50 MHz x 16 bits.
	if (config::FastGDRomLoad)
		return busCycles;
	const u32 rate = gdromReadRate(GD_HardwareInfo.speed);
	const u64 mediaCycles = ((u64)SH4_MAIN_CLOCK * bytes + rate - 1) / rate;
	return std::min<u64>(std::numeric_limits<int>::max(), std::max(busCycles, mediaCycles));
}

static int schedCallback(int tag, int cycles, int jitter, void *arg)
{
	const u32 sect = std::min(gd_hle_state.multi_read_count, 5u);
	if (!readSectors(gd_hle_state.multi_read_offset, gd_hle_state.multi_read_sector, sect,
			gd_hle_state.multi_sector_size, false))
	{
		setReadError(gd_hle_state.multi_read_sector);
		gd_hle_state.multi_read_count = 0;
		return 0;
	}
	gd_hle_state.multi_read_count -= sect;
	gd_hle_state.multi_read_sector += sect;
	gd_hle_state.cur_sector = gd_hle_state.multi_read_sector - 1;
	gd_hle_state.multi_read_offset += sect * gd_hle_state.multi_sector_size;

	if (gd_hle_state.multi_read_count == 0)
	{
		gd_hle_state.result[3] = GDC_WAIT_INTERNAL;
		SecNumber.Status = GD_PAUSE;
		gd_schedule_standby();
		gd_hle_state.status = GDC_COMPLETE;
	}
	gd_hle_state.result[2] = (gd_hle_state.multi_read_total - gd_hle_state.multi_read_count)
			* gd_hle_state.multi_sector_size;

	return getGdromTicks();
}

void reios_init() {
	if (schedId == -1)
		schedId = sh4_sched_register(0, schedCallback);
}

void reios_serialize(Serializer& ser) {
	gd_hle_state.Serialize(ser);
	sh4_sched_serialize(ser, schedId);
	ser << SecMode;
}

void reios_deserialize(Deserializer& deser)
{
	gd_hle_state.Deserialize(deser);
	if (deser.version() >= Deserializer::V54)
		sh4_sched_deserialize(deser, schedId);
	if (deser.version() >= Deserializer::V88)
		deser >> SecMode;
	else
		resetSectorMode();
}

void gdrom_hle_reset() {
	resetDriveState();
}

void reios_term()
{
	if (schedId != -1)
		sh4_sched_unregister(schedId);
	schedId = -1;
}

static void GDROM_HLE_ReadSES()
{
	const u32 session = gd_hle_state.params[0];
	const u32 dest = gd_hle_state.params[1];
	DEBUG_LOG(REIOS, "GDROM: REQ_SES session %u dest %08x", session, dest);

	if (libGDR_GetDiscType() == Open || libGDR_GetDiscType() == NoDisk)
	{
		gd_hle_state.status = GDC_ERR;
		gd_hle_state.result[0] = 2; // NOT READY
		gd_hle_state.result[1] = 0x3a; // Medium not present
		gd_hle_state.result[2] = 0;
		gd_hle_state.result[3] = 0;
		return;
	}
	u8 summary[6] {};
	libGDR_GetSessionInfo(summary, 0);
	if (session > 99 || (session != 0 && session > summary[2]))
	{
		gd_hle_state.status = GDC_ERR;
		gd_hle_state.result[0] = 5; // ILLEGAL REQUEST
		gd_hle_state.result[1] = 0x26; // Invalid field in parameter list
		gd_hle_state.result[2] = 0;
		gd_hle_state.result[3] = 0;
		return;
	}

	u8 response[6] {};
	libGDR_GetSessionInfo(response, session);
	response[0] = SecNumber.Status;
	for (u32 i = 0; i < std::size(response); i++)
		WriteMem8(dest + i, response[i]);
	gd_hle_state.result[2] = sizeof(response);
}

static void GDROM_HLE_ReadTOC()
{
	u32 area = gd_hle_state.params[0];
	u32 dest = gd_hle_state.params[1];

	debugf("GDROM READ TOC : %X %X", area, dest);
	if (libGDR_GetDiscType() == Open || libGDR_GetDiscType() == NoDisk)
	{
		gd_hle_state.status = GDC_ERR;
		gd_hle_state.result[0] = 2; // NOT READY
		gd_hle_state.result[1] = 0x3a; // Medium not present
		return;
	}
	if (area > DoubleDensity)
	{
		setIllegalRequest();
		return;
	}
	if (area == DoubleDensity && libGDR_GetDiscType() != GdRom)
	{
		// Only GD-ROM has a high-density area but no error is reported
		gd_hle_state.status = GDC_OK;
		return;
	}

	u32 toc[102];
	libGDR_GetToc(toc, (DiskArea)area);

	// Swap results to LE
	for (std::size_t i = 0; i < std::size(toc); i++) {
		toc[i] = SWAP32(toc[i]);
	}
	if (!mmu_enabled())
	{
		u32* pDst = (u32*)GetMemPtr(dest, sizeof(toc));
		if (pDst != NULL)
		{
			memcpy(pDst, toc, sizeof(toc));
			gd_hle_state.result[2] = sizeof(toc);
			return;
		}
	}
	for (std::size_t i = 0; i < std::size(toc); i++, dest += 4)
		WriteMem32(dest, toc[i]);
	gd_hle_state.result[2] = sizeof(toc);
}

static bool readSectors(u32 addr, u32 sector, u32 count, u32 sectorSize, bool virtualAddr)
{
	if (count == 0)
		return true;
	u32 byteCount;
	if (!sectorsToBytes(count, sectorSize, byteCount))
		return false;
	gd_hle_state.cur_sector = sector + count - 1;
	if (!virtualAddr || !mmu_enabled())
	{
		u8 * pDst = GetMemPtr(addr, byteCount);

		if (pDst != NULL)
		{
			const bool form2 = sectorSize == 2324 || sectorSize == 2336;
			for (u32 i = 0; i < count; i++)
				if (!gd_read_sector(pDst + i * sectorSize, sector + i, sectorSize, form2))
					return false;
			return true;
		}
	}
	u8 temp[2352];
	const bool form2 = sectorSize == 2324 || sectorSize == 2336;

	while (count > 0)
	{
		if (!gd_read_sector(temp, sector, sectorSize, form2))
			return false;

		for (u32 i = 0; i < sectorSize; i++)
		{
			if (virtualAddr)
				WriteMem8(addr, temp[i]);
			else
				WriteMem8_nommu(addr, temp[i]);
			addr++;
		}

		sector++;
		count--;
	}
	return true;
}

static bool read_pio_sectors(u32 addr, u32 sector, u32 count, u32 sectorSize)
{
	gd_hle_state.xfer_end_time = 0;
	return readSectors(addr, sector, count, sectorSize, true);
}

static void GDROM_HLE_ReadDMA()
{
	u32 fad = gd_hle_state.params[0] & 0xffffff;
	u32 nsect = gd_hle_state.params[1];
	u32 buffer = gd_hle_state.params[2];
	// params[3] 0

	debugf("GDROM: DMA READ Sector=%d, Num=%d, Buffer=%08x, zero=%x", fad, nsect, buffer, gd_hle_state.params[3]);
	if (!validateReadRequest(fad, nsect))
		return;
	u32 byteCount;
	if (!sectorsToBytes(nsect, SecMode[3], byteCount))
	{
		setIllegalRequest();
		return;
	}

	gd_hle_state.multi_read_sector = fad;
	gd_hle_state.multi_read_offset = buffer;
	gd_hle_state.multi_read_count = nsect;
	gd_hle_state.multi_read_total = nsect;
	gd_hle_state.multi_sector_size = SecMode[3];

	gd_hle_state.result[2] = 0;
	gd_hle_state.result[3] = 0;
	if (nsect == 0)
	{
		gd_hle_state.status = GDC_COMPLETE;
		SecNumber.Status = GD_PAUSE;
		gd_schedule_standby();
	}
	else
	{
		gd_hle_state.cur_sector = fad;
		sh4_sched_request(schedId, getGdromTicks());
	}
}

static void GDROM_HLE_ReadPIO()
{
	u32 fad = gd_hle_state.params[0] & 0xffffff;
	u32 nsect = gd_hle_state.params[1];
	u32 buffer = gd_hle_state.params[2];
	// params[3] seekAhead (wince) or 0

	debugf("GDROM: PIO READ Sector=%d, Num=%d, Buffer=%08x, SeekAhead=%x", fad, nsect, buffer, gd_hle_state.params[3]);
	if (!validateReadRequest(fad, nsect))
		return;
	u32 byteCount;
	if (!sectorsToBytes(nsect, SecMode[3], byteCount))
	{
		setIllegalRequest();
		return;
	}

	if (!read_pio_sectors(buffer, fad, nsect, SecMode[3]))
	{
		setReadError(fad);
		return;
	}
	gd_hle_state.result[2] = byteCount;
	gd_hle_state.result[3] = 0;
}

static u32 currentHeadFad()
{
	if (cdda.status != cdda_t::NoInfo && cdda.CurrAddr.FAD >= 150)
		return cdda.CurrAddr.FAD;
	return gd_hle_state.cur_sector;
}

static void GDCC_HLE_GETSCD()
{
	u32 format = gd_hle_state.params[0];
	u32 size = gd_hle_state.params[1];
	u32 dest = gd_hle_state.params[2];
	// params[3] 0

	DEBUG_LOG(REIOS, "GDROM: GETSCD format %x size %x dest %08x", format, size, dest);
	if (sns_asc != 0)
	{
		// Helps D2 detect the disk change
		gd_hle_state.status = GDC_ERR;
		gd_hle_state.result[0] = sns_key;
		gd_hle_state.result[1] = sns_asc | (sns_ascq << 8);
		gd_hle_state.result[2] = 0x18; // ?
		gd_hle_state.result[3] = 0;
		sns_key = 0;
		sns_asc = 0;
		sns_ascq = 0;
		return;
	}
	if (SecNumber.Status != GD_BUSY && (libGDR_GetDiscType() == Open || libGDR_GetDiscType() == NoDisk))
	{
		gd_hle_state.status = GDC_ERR;
		gd_hle_state.result[0] = 2;
		gd_hle_state.result[1] = 0x3a;	// Media operation command was received but no media is inserted.
		gd_hle_state.result[2] = 0;
		gd_hle_state.result[3] = 0;
		return;
	}
	if (format > 3)
	{
		gd_hle_state.status = GDC_ERR;
		gd_hle_state.result[0] = 5;
		gd_hle_state.result[1] = 0x24;
		gd_hle_state.result[2] = 0;
		gd_hle_state.result[3] = 0;
		return;
	}
	u8 scd[100] {};
	const u32 available = gd_get_subcode(format, currentHeadFad(), scd);
	const u32 transferSize = std::min(size, available);

	if (!mmu_enabled() && GetMemPtr(dest, transferSize) != NULL)
		memcpy(GetMemPtr(dest, transferSize), scd, transferSize);
	else
	{
		for (u32 i = 0; i < transferSize; i++)
			WriteMem8(dest++, scd[i]);
	}
	gd_subcode_status_transferred(transferSize);

	// record size of pio transfer to gdrom
	gd_hle_state.result[2] = transferSize;
}

template<bool dma>
static void multi_xfer()
{
	u32 dest = gd_hle_state.params[0];
	u32 size = gd_hle_state.params[1];

	size = std::min(size, gd_hle_state.multi_read_count);
	while (size > 0)
	{
		u8 buf[2352];
		const bool form2 = gd_hle_state.multi_sector_size == 2324
				|| gd_hle_state.multi_sector_size == 2336;
		if (!gd_read_sector(buf, gd_hle_state.multi_read_sector,
				gd_hle_state.multi_sector_size, form2))
		{
			setReadError(gd_hle_state.multi_read_sector);
			gd_hle_state.multi_read_count = 0;
			return;
		}
		while (size > 0)
		{
			int remaining = gd_hle_state.multi_sector_size - gd_hle_state.multi_read_offset;
			if (size >= 4 && remaining >= 4 && (dest & 3) == 0)
			{
				u32 value;
				memcpy(&value, &buf[gd_hle_state.multi_read_offset], sizeof(value));
				if constexpr (dma)
					WriteMem32_nommu(dest, value);
				else
					WriteMem32(dest, value);
				dest += 4;
				gd_hle_state.multi_read_offset += 4;
				gd_hle_state.multi_read_count -= 4;
				size -= 4;
			}
			else if (size >= 2 && remaining >= 2 && (dest & 1) == 0)
			{
				u16 value;
				memcpy(&value, &buf[gd_hle_state.multi_read_offset], sizeof(value));
				if constexpr (dma)
					WriteMem16_nommu(dest, value);
				else
					WriteMem16(dest, value);
				dest += 2;
				gd_hle_state.multi_read_offset += 2;
				gd_hle_state.multi_read_count -= 2;
				size -= 2;
			}
			else
			{
				if constexpr (dma)
					WriteMem8_nommu(dest, buf[gd_hle_state.multi_read_offset]);
				else
					WriteMem8(dest, buf[gd_hle_state.multi_read_offset]);
				dest++;
				gd_hle_state.multi_read_offset++;
				gd_hle_state.multi_read_count--;
				size--;
			}
			if (gd_hle_state.multi_read_offset >= gd_hle_state.multi_sector_size)
			{
				verify(gd_hle_state.multi_read_offset == gd_hle_state.multi_sector_size);
				gd_hle_state.multi_read_sector++;
				gd_hle_state.multi_read_offset = 0;
				break;
			}
		}
	}
	gd_hle_state.transfer_remaining = 0;
	if (!dma)
	{
		gd_hle_state.result[2] = gd_hle_state.multi_read_total - gd_hle_state.multi_read_count;
		gd_hle_state.result[3] = GDC_WAIT_INTERNAL;
		if (gd_hle_state.multi_callback != 0)
		{
			Sh4cntx.r[4] = gd_hle_state.multi_callback_arg;
			Sh4cntx.pc = gd_hle_state.multi_callback;
		}
	}
	else
	{
		gd_hle_state.result[2] = gd_hle_state.multi_read_total - gd_hle_state.multi_read_count;
		gd_hle_state.result[3] = gd_hle_state.multi_read_count > 0 ? GDC_WAIT_IRQ : GDC_WAIT_INTERNAL;
		gd_hle_state.dma_trans_ended = true;
		if (gd_hle_state.multi_read_count == 0)
			gd_hle_state.status = GDC_COMPLETE;
		asic_RaiseInterrupt(holly_GDROM_DMA);
	}
}

static void GD_HLE_Command(gd_command cc)
{
	switch(cc)
	{
	case GDCC_GETTOC:
	case GDCC_GETTOC2:
		GDROM_HLE_ReadTOC();
		break;

	case GDCC_REQ_SES:
		GDROM_HLE_ReadSES();
		break;

	case GDCC_INIT:
		DEBUG_LOG(REIOS, "GDROM: CMD INIT");
		// The BIOS reports a pending disc-change notification through INIT.
		// Callers retry the command after receiving UNIT ATTENTION, so consume
		// it only after publishing the sense data for this request.
		if (sns_key == 6)
		{
			gd_hle_state.status = GDC_ERR;
			gd_hle_state.result[0] = sns_key;
			gd_hle_state.result[1] = sns_asc | (sns_ascq << 8);
			gd_hle_state.result[2] = 0;
			gd_hle_state.result[3] = 0;
			sns_key = 0;
			sns_asc = 0;
			sns_ascq = 0;
			break;
		}
		if (!validateMediaReady())
			break;
		gd_hle_state.multi_callback = 0;
		gd_hle_state.multi_read_count = 0;
		cdda.status = cdda_t::NoInfo;
		break;

	case GDCC_NOP:
		DEBUG_LOG(REIOS, "GDROM: CMD NOP");
		break;

	case GDCC_OPEN:
		DEBUG_LOG(REIOS, "GDROM: CMD OPEN");
		cdda.status = cdda_t::NoInfo;
		cdda.EndAddr.FAD = 0;
		cdda.repeats = 0;
		if (libGDR_GetDiscType() != Open)
		{
			// The console has a manual top-loading lid. Keep this request active
			// until the frontend reports that the lid has actually been opened.
			SecNumber.Status = GD_BUSY;
			return;
		}
		SecNumber.Status = GD_OPEN;
		break;

	case GDCC_READABORT:
		DEBUG_LOG(REIOS, "GDROM: CMD DMA ABORT");
		cancelActiveTransfer();
		asic_CancelInterrupt(holly_GDROM_DMA);
		break;

	case GDCC_PIOREAD:
		GDROM_HLE_ReadPIO();
		if (gd_hle_state.status != GDC_ERR)
		{
			SecNumber.Status = GD_PAUSE;
			gd_schedule_standby();
			cdda.status = cdda_t::NoInfo;
		}
		break;

	case GDCC_DMAREAD:
		if (gd_hle_state.multi_read_count == 0)
		{
			GDROM_HLE_ReadDMA();
			if (gd_hle_state.status != GDC_ERR)
				cdda.status = cdda_t::NoInfo;
		}
		return;

	case GDCC_PLAY2:
		{
			u32 startFad = gd_hle_state.params[0] & 0xffffff;
			u32 endFad = gd_hle_state.params[1] & 0xffffff;
			if (!validatePositionRequest(startFad, 1))
				break;
			if (!gd_normalize_playback_start(startFad))
			{
				setInvalidAddress(startFad);
				break;
			}
			const u32 leadOutFad = gd_get_playback_area_end(startFad);
			if (endFad != 0 && endFad <= startFad)
			{
				setInvalidAddress(endFad);
				break;
			}
			const bool endsAtLeadOut = endFad == 0 || endFad >= leadOutFad;
			endFad = endsAtLeadOut ? leadOutFad : endFad;
			cdda.StartAddr.FAD = startFad;
			cdda.EndAddr.FAD = endFad;
			cdda.repeats = endsAtLeadOut ? 0 : gd_hle_state.params[2] & 0xf;
			// params[3] debug (0)
			DEBUG_LOG(REIOS, "GDROM: CMD PLAYSEC from %d to %d repeats %d", cdda.StartAddr.FAD, cdda.EndAddr.FAD, cdda.repeats);
			cdda.status = cdda_t::Playing;
			cdda.CurrAddr.FAD = cdda.StartAddr.FAD;
			SecNumber.Status = GD_PLAY;
		}
		break;

	case GDCC_RELEASE:
		DEBUG_LOG(REIOS, "GDROM: CMD RELEASE");
		// params[0] reptime (ignored)
		// params[1] debug (0)
		if (!validateMediaReady())
			break;
		if (cdda.status == cdda_t::Paused || cdda.status == cdda_t::Scanning)
		{
			if (cdda.StartAddr.FAD < 150 || cdda.CurrAddr.FAD >= cdda.EndAddr.FAD)
			{
				setInvalidParameter();
				break;
			}
			cdda.status = cdda_t::Playing;
			SecNumber.Status = GD_PLAY;
		}
		else if (cdda.status != cdda_t::Playing)
			setInvalidParameter();
		break;

	case GDCC_STOP:
		DEBUG_LOG(REIOS, "GDROM: CMD STOP");
		if (!validateMediaReady())
			break;
		cdda.status = cdda_t::NoInfo;
		cdda.StartAddr.FAD = cdda.CurrAddr.FAD = 150;
		cdda.EndAddr.FAD = 0;
		cdda.repeats = 0;
		SecNumber.Status = GD_STANDBY;
		break;

	case GDCC_SEEK:
		DEBUG_LOG(REIOS, "GDROM: CMD SEEK");
		if (!validatePositionRequest(gd_hle_state.params[0] & 0xffffff, 1))
			break;
		cdda.CurrAddr.FAD = cdda.StartAddr.FAD = gd_hle_state.params[0] & 0xffffff;
		cdda.EndAddr.FAD = 0;
		cdda.repeats = 0;
		// params[1] debug (0)
		cdda.status = cdda_t::Seeked;
		SecNumber.Status = GD_PAUSE;
		gd_schedule_standby();
		break;

	case GDCC_PLAY:
		if (libGDR_GetDiscType() != Open && libGDR_GetDiscType() != NoDisk)
		{
			u32 first_track = gd_hle_state.params[0];
			u32 last_track = gd_hle_state.params[1];
			u32 firstEnd;
			u32 lastStart;
			u32 lastEnd;
			if (first_track > last_track
					|| !libGDR_GetTrack(first_track, cdda.StartAddr.FAD, firstEnd)
					|| !libGDR_GetTrack(last_track, lastStart, lastEnd))
			{
				setIllegalRequest();
				break;
			}
			// Track end FADs are inclusive; CDDA playback ranges are exclusive.
			cdda.EndAddr.FAD = lastEnd + 1;
			const u32 areaEndFad = gd_get_playback_area_end(cdda.StartAddr.FAD);
			const bool endsAtLeadOut = cdda.EndAddr.FAD >= areaEndFad;
			cdda.EndAddr.FAD = std::min(cdda.EndAddr.FAD, areaEndFad);
			cdda.repeats = endsAtLeadOut ? 0 : gd_hle_state.params[2] & 0xf;
			// params[3] debug (0)
			DEBUG_LOG(REIOS, "GDROM: CMD PLAY first_track %x last_track %x repeats %x start_fad %x end_fad %x", first_track, last_track, cdda.repeats,
					cdda.StartAddr.FAD, cdda.EndAddr.FAD);
			cdda.status = cdda_t::Playing;
			cdda.CurrAddr.FAD = cdda.StartAddr.FAD;
			SecNumber.Status = GD_PLAY;
		}
		else
		{
			setNoMedia();
		}
		break;

	case GDCC_PAUSE:
		DEBUG_LOG(REIOS, "GDROM: CMD PAUSE");
		if (!validateMediaReady())
			break;
		gd_pause_cdda();
		break;

	case GDCC_DMA_READ_REQ:
	case GDCC_PIOREADREQ:
		{
			u32 sector = gd_hle_state.params[0] & 0xffffff;
			const u32 requested = gd_hle_state.params[1];
			u32 num;

			if (!normalizeStreamReadRequest(sector, requested, num))
				break;
			DEBUG_LOG(REIOS, "GDROM: CMD %s STREAM Sector=%d, Num=%d (requested %d)",
					cc == GDCC_DMA_READ_REQ ? "DMA" : "PIO", sector, num, requested);
			u32 byteCount;
			if (!sectorsToBytes(num, SecMode[3], byteCount))
			{
				setIllegalRequest();
				break;
			}
			gd_hle_state.status = GDC_CONTINUE;
			gd_hle_state.multi_read_sector = sector;
			gd_hle_state.multi_read_count = byteCount;
			gd_hle_state.multi_read_total = gd_hle_state.multi_read_count;
			gd_hle_state.multi_read_offset = 0;
			gd_hle_state.multi_sector_size = SecMode[3];
			gd_hle_state.transfer_remaining = 0;
			gd_hle_state.result[2] = SecMode[3];
			gd_hle_state.result[3] = num > 0 ? GDC_WAIT_IRQ : GDC_WAIT_INTERNAL;
		}
		break;

	case GDCC_GETSCD:
		GDCC_HLE_GETSCD();
		break;

	case GDCC_REQ_MODE:
		{
			u32 dest = gd_hle_state.params[0];
			debugf("GDROM: REQ_MODE dest:%x", dest);
			WriteMem32(dest, GD_HardwareInfo.speed);
			WriteMem32(dest + 4, (GD_HardwareInfo.standby_hi << 8) | GD_HardwareInfo.standby_lo);
			WriteMem32(dest + 8, GD_HardwareInfo.read_flags);
			WriteMem32(dest + 12, GD_HardwareInfo.read_retry);

			// record size of pio transfer to gdrom
			gd_hle_state.result[2] = 0xa;
		}
		break;

	case GDCC_SET_MODE:
		{
			auto [speed, standby, read_flags, read_retry] = gd_hle_state.params;

			debugf("GDROM: SET_MODE speed %x standby %x read_flags %x read_retry %x", speed, standby, read_flags, read_retry);
			if (speed > 7 || standby > 0xffff || read_flags > 0xff
					|| (read_flags & ~0x39) != 0 || read_retry > 0xff)
			{
				setInvalidParameter();
				break;
			}

			GD_HardwareInfo.speed = speed;
			GD_HardwareInfo.standby_hi = (standby & 0xff00) >> 8;
			GD_HardwareInfo.standby_lo = standby & 0xff;
			GD_HardwareInfo.read_flags = read_flags;
			GD_HardwareInfo.read_retry = read_retry;
			if (SecNumber.Status == GD_PAUSE)
				gd_schedule_standby();

			// record size of pio transfer to gdrom
			gd_hle_state.result[2] = 0xa;
		}
		break;

	case GDCC_SCAN:
		{
			const u32 direction = gd_hle_state.params[0];
			const u32 speed = gd_hle_state.params[1];
			DEBUG_LOG(REIOS, "GDROM: CMD SCAN direction %u speed %u", direction, speed);
			if (libGDR_GetDiscType() == Open || libGDR_GetDiscType() == NoDisk)
			{
				gd_hle_state.status = GDC_ERR;
				gd_hle_state.result[0] = 2;
				gd_hle_state.result[1] = 0x3a;
			}
			else if (direction > 1
					|| (cdda.status != cdda_t::Playing && cdda.status != cdda_t::Paused
							&& cdda.status != cdda_t::Scanning)
					|| cdda.StartAddr.FAD < 150 || cdda.CurrAddr.FAD >= cdda.EndAddr.FAD)
			{
				gd_hle_state.status = GDC_ERR;
				gd_hle_state.result[0] = 5;
				gd_hle_state.result[1] = 0x26;
			}
			else
			{
				gd_start_cdda_scan(direction != 0, speed);
			}
		}
		break;

	case GDCC_GET_VERSION:
		{
			u32 dest = gd_hle_state.params[0];
			// params[1] 0

			debugf("GDROM: GDCC_GET_VERSION dest %x", dest);

			char ver[] = "GDC Version 1.10 1999-03-31 ";
			u32 len = (u32)strlen(ver);

			// 0x8c0013b8 (offset 0xd0 in the gdrom state struct) is then loaded and
			// overwrites the last byte. no idea what this is, but seems to be hard
			// coded to 0x02 on boot
			ver[len - 1] = 0x02;

			for (u32 i = 0; i < len; i++)
				WriteMem8(dest++, ver[i]);
		}
		break;

	case GDCC_REQ_STAT:
		{
			// odd, but this function seems to get passed 4 unique pointers
			u32 dst0 = gd_hle_state.params[0];	// repeat
			u32 dst1 = gd_hle_state.params[1];	// track
			u32 dst2 = gd_hle_state.params[2];	// toc
			u32 dst3 = gd_hle_state.params[3];	// index

			debugf("GDROM: GDCC_REQ_STAT dst0=%08x dst1=%08x dst2=%08x dst3=%08x", dst0, dst1, dst2, dst3);

			// bit   |  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0
			// byte  |     |     |     |     |     |     |     |
			// ------------------------------------------------------
			// 0     |  0  |  0  |  0  |  0  | status
			// ------------------------------------------------------
			// 1     |  0  |  0  |  0  |  0  | repeat count
			// ------------------------------------------------------
			// 2-3   |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0
			WriteMem32(dst0, (cdda.repeats << 8) | SecNumber.Status);

			// bit   |  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0
			// byte  |     |     |     |     |     |     |     |
			// ------------------------------------------------------
			// 0     | subcode q track number
			// ------------------------------------------------------
			// 1-3   |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0
			const u32 fad = currentHeadFad();
			u32 elapsed;
			u32 tracknum = libGDR_GetTrackNumber(fad, elapsed);
			WriteMem32(dst1, tracknum);

			// bit   |  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0
			// byte  |     |     |     |     |     |     |     |
			// ------------------------------------------------------
			// 0-2  | fad (little-endian)
			// ------------------------------------------------------
			// 3    | ADR                    | Control
			u8 adr, ctrl;
			libGDR_GetTrackAdrAndControlAtFad(fad, adr, ctrl);
			u32 out = (adr << 28) | (ctrl << 24) | (fad & 0x00ffffff);
			WriteMem32(dst2, out);

			// bit   |  7  |  6  |  5  |  4  |  3  |  2  |  1  |  0
			// byte  |     |     |     |     |     |     |     |
			// ------------------------------------------------------
			// 0     | subcode q index number
			// ------------------------------------------------------
			// 1-3   |  0  |  0  |  0  |  0  |  0  |  0  |  0  |  0
			WriteMem32(dst3, 1);

			// record pio transfer size
			gd_hle_state.result[2] = 0xa;
		}
		break;

	case GDCC_MULTI_DMAREAD:
	case GDCC_MULTI_PIOREAD:
		{
			u32 sector = gd_hle_state.params[0] & 0xffffff;
			const u32 requested = gd_hle_state.params[1];
			u32 num;
			// params[2] seekAhead (wince)
			bool dma = cc == GDCC_MULTI_DMAREAD;

			if (!normalizeStreamReadRequest(sector, requested, num))
				break;
			DEBUG_LOG(REIOS, "GDROM: MULTI_%sREAD Sector=%d, Num=%d (requested %d) SeekAhead=%d",
					dma ? "DMA" : "PIO", sector, num, requested, gd_hle_state.params[2]);

			u32 byteCount;
			if (!sectorsToBytes(num, SecMode[3], byteCount))
			{
				setIllegalRequest();
				break;
			}
			gd_hle_state.status = GDC_CONTINUE;
			gd_hle_state.multi_read_sector = sector;
			gd_hle_state.multi_read_count = byteCount;
			gd_hle_state.multi_read_total = gd_hle_state.multi_read_count;
			gd_hle_state.multi_read_offset = 0;
			gd_hle_state.multi_sector_size = SecMode[3];
			gd_hle_state.transfer_remaining = 0;

			// wild guesses here
			gd_hle_state.result[2] = 0;
			gd_hle_state.result[3] = 0;
		}
		break;

	case GDCC_REQ_DMA_TRANS:
	case GDCC_REQ_PIO_TRANS:
		{
			u32 dest = gd_hle_state.params[0];
			u32 size = gd_hle_state.params[1];
			bool dma = cc == GDCC_REQ_DMA_TRANS;
			DEBUG_LOG(REIOS, "GDROM: REQ_%s_TRANS dest %x size %x", dma ? "DMA" : "PIO",
					dest, size);
			if (dma)
				multi_xfer<true>();
			else
				multi_xfer<false>();
		}
		break;

	default:
		WARN_LOG(REIOS, "GDROM: Unknown GDROM CC:%X", cc);
		setIllegalRequest();
		break;
	}
	if (gd_hle_state.status == GDC_BUSY)
		gd_hle_state.status = GDC_COMPLETE;
	gd_hle_state.command = GDCC_NONE;
}

#define r Sh4cntx.r

void gdrom_hle_op()
{
	if (SYSCALL_GDROM == r[6])		// GDROM SYSCALL
	{
		switch(r[7])				// COMMAND CODE
		{
		case GDROM_REQ_CMD:
			// Enqueue a command for the GDROM subsystem to execute.
			//
			// Args:
			//	r4 = command code
			//	r5 = pointer to parameter block for the command, can be NULL if the command does not take parameters
			//
			// Returns: a request id (>0) if successful, 0 if failed
			debugf("GDROM: HLE REQ_CMD CC:%X  param ptr: %X bios status %d", r[4], r[5], gd_hle_state.status);
			if (gd_hle_state.status != GDC_OK)
			{
				r[0] = 0;
			}
			else
			{
				for (int i = 0; i < 4; i++)
				{
					try {
						gd_hle_state.params[i] = r[5] == 0 ? 0 : ReadMem32(r[5] + i * 4);
					} catch (SH4ThrownException&) {
						// Ignore page faults. happens for commands not taking params
						gd_hle_state.params[i] = 0;
					}
				}
				memset(gd_hle_state.result, 0, sizeof(gd_hle_state.result));
				if (gd_hle_state.next_request_id == ~0u || gd_hle_state.next_request_id == 0)
					gd_hle_state.next_request_id = 1;
				gd_hle_state.last_request_id = r[0] = gd_hle_state.next_request_id++;
				gd_hle_state.status = GDC_BUSY;
				gd_hle_state.command = (gd_command)r[4];
				gd_hle_state.multi_read_count = 0;
				gd_hle_state.transfer_remaining = 0;
			}
			break;

		case GDROM_GET_CMD_STAT:
			// Check if an enqueued command has completed.
			//
			// Args:
			//	r4 = request id
			//	r5 = pointer to four 32 bit integers to receive extended status information. The first is a generic error code:
			// GDC_ERR_NOERR          0x00
			// GDC_ERR_RECOVERED      0x01
			// GDC_ERR_NOTREADY       0x02
			// GDC_ERR_MEDIUM         0x03
			// GDC_ERR_HARDWARE       0x04
			// GDC_ERR_ILLEGALREQUEST 0x05
			// GDC_ERR_UNITATTENTION  0x06
			// GDC_ERR_DATAPROTECT    0x07
			// GDC_ERR_ABORTED        0x0B
			// GDC_ERR_NOREADABLE     0x10
			// GDC_ERR_G1SEMAPHORE    0x20
			//
			// Returns:
			//	GDC_OK - no such request active (idle)
			//	GDC_BUSY - request is still being processed
			//	GDC_COMPLETE - request has completed (if queried again, you will get GDC_OK)
			//	GDC_CONTINUE - multi request has data available
			//  GDC_SMPHR_BUSY ?
			//	GDC_ERR - request has failed (examine extended status information for cause of failure)
			if (r[4] != gd_hle_state.last_request_id)
			{
				r[0] = GDC_OK; // no such request active
				try {
					for (u32 i = 0; i < std::size(gd_hle_state.result); i++)
						WriteMem32(r[5] + i * 4, 0);
				} catch (SH4ThrownException&) {
				}
			}
			else
			{
				try {
					WriteMem32(r[5], gd_hle_state.result[0]);		// error
					WriteMem32(r[5] + 4, gd_hle_state.result[1]);	// error1
					WriteMem32(r[5] + 8, gd_hle_state.result[2]);	// size
					WriteMem32(r[5] + 12, gd_hle_state.result[3]);	// wait state (if busy)
				} catch (SH4ThrownException&) {
				}
				if (gd_hle_state.status == GDC_OK || gd_hle_state.status == GDC_BUSY)
				{
					r[0] = gd_hle_state.status;
				}
				else
				{
					if (gd_hle_state.status == GDC_CONTINUE && gd_hle_state.command == GDCC_REQ_PIO_TRANS)
						r[0] = GDC_BUSY;	// Bust-a-move 4 likes this
					else
						r[0] = gd_hle_state.status;	// completed or error
					// Fixes NBA 2K
					if (gd_hle_state.status == GDC_CONTINUE && gd_hle_state.multi_read_count == 0)
					{
						gd_hle_state.status = GDC_COMPLETE;
						gd_hle_state.result[3] = 0;
					}
					else if (gd_hle_state.status != GDC_CONTINUE)
					{
						gd_hle_state.status = GDC_OK;
						gd_hle_state.last_request_id = 0xFFFFFFFF;
					}
				}
			}
			debugf("GDROM: HLE GET_CMD_STAT REQID:%X  param ptr: %X -> %X : %x %x %x %x", r[4], r[5], r[0],
					gd_hle_state.result[0], gd_hle_state.result[1], gd_hle_state.result[2], gd_hle_state.result[3]);
			break;

		case GDROM_EXEC_SERVER:
			// In order for enqueued commands to get processed, this function must be called a few times.
			debugf("GDROM: HLE EXEC_SERVER");
			if (gd_hle_state.status == GDC_BUSY || (gd_hle_state.status == GDC_CONTINUE && gd_hle_state.command == GDCC_REQ_PIO_TRANS))
			{
				GD_HLE_Command(gd_hle_state.command);
			}
			break;

		case GDROM_INIT_SYSTEM:
			// Initialize the GDROM subsystem. Should be called before any requests are enqueued.
			DEBUG_LOG(REIOS, "GDROM: HLE INIT_SYSTEM");
			resetDriveState();
			break;

		case GDROM_RESET:
			// Resets the drive.
			DEBUG_LOG(REIOS, "GDROM: HLE RESET");
			resetDriveState();
			// A drive reset invalidates the initiator's previous view of the
			// device. Match the ATA soft-reset path by reporting this once on
			// the next command (normally INIT).
			sns_key = 6; // UNIT ATTENTION
			sns_asc = 0x29; // Power on, reset, or bus device reset occurred
			sns_ascq = 0;
			r[0] = GDC_OK;
			break;

		case GDROM_GET_DRV_STAT:
			{
				// Checks the general condition of the drive.
				//
				// Args:
				//	r4 = pointer to two 32 bit integers, to receive the drive status. The first is the current drive status (gd_drv_stat),
				//       the second is the type of disc inserted (if any).
				//
				// Returns: 0 OK, -1 ERR, 1 BUSY, 2 COMPLETE, 3 CONTINUE
				gd_drv_stat status;
				u32 discType;
				if (SecNumber.Status == GD_BUSY)
				{
					status = GD_STAT_BUSY;
					discType = 0;
				}
				else
				{
					discType = libGDR_GetDiscType();
					switch (discType)
					{
					case Open:
						status = GD_STAT_OPEN;
						discType = 0;
						break;
					case NoDisk:
						status = GD_STAT_NODISC;
						discType = 0;
						break;
					default:
						switch (SecNumber.Status)
						{
						case GD_STANDBY:
							status = GD_STAT_STANDBY;
							break;
						case GD_PLAY:
							status = GD_STAT_PLAY;
							break;
						case GD_SEEK:
							status = GD_STAT_SEEK;
							break;
						case GD_SCAN:
							status = GD_STAT_SCAN;
							break;
						case GD_PAUSE:
						default:
							status = gd_hle_state.status == GDC_CONTINUE ? GD_STAT_PLAY : GD_STAT_PAUSE;
							break;
						}
						if (ip_meta.isGDROM())
							discType = GdRom;
						break;
					}
				}
				WriteMem32(r[4], (u32)status);
				WriteMem32(r[4] + 4, discType);
				debugf("GDROM: HLE GET_DRV_STAT r4:%X -> %x %x", r[4], status, discType);
				r[0] = GDC_OK;
			}
			break;

		case GDROM_READ_ABORT:
			// Tries to abort a previously enqueued command.
			//
			// Args:
			//	r4 = request id
			//
			// Returns: GDC_OK, GDC_ERR
			INFO_LOG(REIOS, "GDROM: HLE GDROM_ABORT_COMMAND req id%x", r[4]);
			if (r[4] == gd_hle_state.last_request_id
					&& (gd_hle_state.status == GDC_CONTINUE || gd_hle_state.status == GDC_BUSY || gd_hle_state.status == GDC_COMPLETE))
			{
				r[0] = GDC_OK;
				cancelActiveTransfer();
				gd_hle_state.status = GDC_OK;
				gd_hle_state.command = GDCC_NONE;
				gd_hle_state.last_request_id = 0xFFFFFFFF;
				memset(gd_hle_state.result, 0, sizeof(gd_hle_state.result));
			}
			else
			{
				r[0] = GDC_ERR;
			}
			break;


		case GDROM_CHANGE_DATA_TYPE:
			// Sets/gets the sector format for read commands.
			//
			// Args:
			//	r4 = pointer to a struct of four 32 bit integers containing new values, or to receive the old values
			//	Field	Function
			//	0 	Get/Set, if 0 the mode will be set, if 1 it will be queried.
			//	1 	Sector part: 0x1000 whole sector, 0x2000 data area
			//	2 	1024 = mode 1, 2048 = mode 2, 0 = auto detect
			//	3 	Sector size in bytes (normally 2048)
			//
			// Returns: GDC_OK, GDC_ERR
			DEBUG_LOG(REIOS, "GDROM: HLE CHANGE_DATA_TYPE PTR_r4:%X", r[4]);
			try
			{
				const u32 operation = ReadMem32(r[4]);
				if (operation == 1)
				{
					WriteMem32(r[4], 1);
					for (u32 i = 1; i < std::size(SecMode); i++)
						WriteMem32(r[4] + i * 4, SecMode[i]);
					r[0] = GDC_OK;
				}
				else if (operation == 0)
				{
					u32 newMode[4];
					for (u32 i = 0; i < std::size(newMode); i++)
						newMode[i] = ReadMem32(r[4] + i * 4);

					if ((newMode[1] != 0x1000 && newMode[1] != 0x2000)
							|| (newMode[2] != 0 && newMode[2] != 1024 && newMode[2] != 2048)
							|| !supportedSectorSize(newMode[3]))
					{
						r[0] = GDC_ERR;
					}
					else
					{
						std::copy(std::begin(newMode), std::end(newMode), std::begin(SecMode));
						r[0] = GDC_OK;
					}
				}
				else
				{
					r[0] = GDC_ERR;
				}
			}
			catch (SH4ThrownException&)
			{
				r[0] = GDC_ERR;
			}
			break;

		case GDROM_G1_DMA_END:
			DEBUG_LOG(REIOS, "GDROM: G1_DMA_END callback %x arg %x", r[4], r[5]);
			gd_hle_state.multi_callback = r[4];
			gd_hle_state.multi_callback_arg = r[5];
			r[0] = GDC_OK;
			if (gd_hle_state.multi_callback != 0 && gd_hle_state.dma_trans_ended)	// FIXME hack for 2K sports games
			{
				r[4] = gd_hle_state.multi_callback_arg;
				Sh4cntx.pc = gd_hle_state.multi_callback;
				gd_hle_state.dma_trans_ended = false;
			}
			asic_CancelInterrupt(holly_GDROM_DMA);
			break;

		case GDROM_REQ_DMA_TRANS:
			{
				if (r[4] != gd_hle_state.last_request_id || gd_hle_state.status != GDC_CONTINUE)
				{
					r[0] = GDC_ERR;
					break;
				}
				gd_hle_state.params[0] = ReadMem32(r[5]);		// buffer
				gd_hle_state.params[1] = ReadMem32(r[5] + 4);	// size
				DEBUG_LOG(REIOS, "GDROM: REQ_DMA_TRANS req_id %x dest %x size %x",
						r[4], gd_hle_state.params[0], gd_hle_state.params[1]);
				if ((gd_hle_state.params[0] & 31) != 0 || gd_hle_state.params[1] < 32
						|| gd_hle_state.params[1] > gd_hle_state.multi_read_count)
					r[0] = GDC_ERR;
				else
				{
					gd_hle_state.transfer_remaining = gd_hle_state.params[1];
					multi_xfer<true>();
					r[0] = GDC_OK;
				}
			}
			break;

		case GDROM_REQ_PIO_TRANS:
			{
				if (r[4] != gd_hle_state.last_request_id || gd_hle_state.status != GDC_CONTINUE)
				{
					r[0] = GDC_ERR;
					break;
				}
				gd_hle_state.params[0] = ReadMem32(r[5]);
				gd_hle_state.params[1] = ReadMem32(r[5] + 4);
				DEBUG_LOG(REIOS, "GDROM: REQ_PIO_TRANS req_id %x dest %x size %x",
						r[4], gd_hle_state.params[0], gd_hle_state.params[1]);
				if ((gd_hle_state.params[0] & 1) != 0 || gd_hle_state.params[1] < 2
						|| gd_hle_state.params[1] > gd_hle_state.multi_read_count)
					r[0] = GDC_ERR;
				else
				{
					gd_hle_state.transfer_remaining = gd_hle_state.params[1];
					gd_hle_state.command = GDCC_REQ_PIO_TRANS;
					r[0] = GDC_OK;
				}
			}
			break;

		case GDROM_CHECK_DMA_TRANS:
			{
				// r4 handle
				u32 len_addr = r[5];
				DEBUG_LOG(REIOS, "GDROM: CHECK_DMA_TRANS req_id %x len_addr %x -> %x", r[4], len_addr, gd_hle_state.transfer_remaining);
				if (r[4] != gd_hle_state.last_request_id)
				{
					r[0] = GDC_ERR;
					break;
				}
				try {
					WriteMem32(len_addr, gd_hle_state.transfer_remaining);
					r[0] = gd_hle_state.status == GDC_CONTINUE || gd_hle_state.status == GDC_COMPLETE
							? GDC_OK : GDC_ERR;
				} catch (SH4ThrownException&) {
					r[0] = GDC_ERR;
				}
			}
			break;

		case GDROM_SET_PIO_CALLBACK:
			DEBUG_LOG(REIOS, "GDROM: SET_PIO_CALLBACK callback %x arg %x", r[4], r[5]);
			gd_hle_state.multi_callback = r[4];
			gd_hle_state.multi_callback_arg = r[5];
			r[0] = GDC_OK;
			break;

		case GDROM_CHECK_PIO_TRANS:
			{
				u32 len_addr = r[5];
				DEBUG_LOG(REIOS, "GDROM: CHECK_PIO_TRANS req_id %x len_addr %x -> %x", r[4], len_addr, gd_hle_state.transfer_remaining);
				if (r[4] != gd_hle_state.last_request_id)
				{
					r[0] = GDC_ERR;
				}
				else if (gd_hle_state.status == GDC_CONTINUE || gd_hle_state.status == GDC_COMPLETE)
				{
					try {
						WriteMem32(len_addr, gd_hle_state.transfer_remaining);
						r[0] = GDC_OK;
					} catch (SH4ThrownException&) {
						r[0] = GDC_ERR;
					}
				}
				else
				{
					r[0] = GDC_ERR;
				}
			}
			break;

		default:
			WARN_LOG(REIOS, "GDROM: Unknown SYSCALL: %X",r[7]);
			break;
		}
	}
	else							// MISC 
	{
		switch(r[7])
		{
		case MISC_INIT:
			// Initializes all the syscall vectors to their default values.
			// Returns: zero
			WARN_LOG(REIOS, "GDROM: MISC_INIT not implemented");
			r[0] = GDC_OK;
			break;

		case MISC_SETVECTOR:
			// Sets/clears the handler for one of the eight superfunctions for this vector. Setting a handler is only allowed if it not currently set.
			//
			// Args:
			//	r4 = superfunction number (0-7)
			//	r5 = pointer to handler function, or NULL to clear
			//
			// Returns: zero if successful, -1 if setting/clearing the handler fails
			WARN_LOG(REIOS, "GDROM: MISC_SETVECTOR not implemented");
			break;

		default:
			WARN_LOG(REIOS, "GDROM: Unknown MISC command %x", r[7]);
			break;
		}
	}
}
