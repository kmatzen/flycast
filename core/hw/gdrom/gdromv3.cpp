/*
	gdrom, v3
	Overly complex implementation of a very ugly device
*/

#include "gdromv3.h"
#include "gdrom_if.h"
#include "cfg/option.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/modules/dmac.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_sched.h"
#include "imgread/common.h"
#include "serialize.h"

int gdrom_schid;
static int standby_schid = -1;

//Sense: ASC - ASCQ - Key
int sns_asc;
int sns_ascq;
int sns_key;
static u32 sns_fad;

static u32 set_mode_offset;
static read_params_t read_params;
static packet_cmd_t packet_cmd;
static DmaBuffer dma_buff;
static PioBuffer pio_buff;
static u8 pio_sector_cache[2352];
static u32 pio_cached_fad;
static u32 pio_cached_size;
static bool pio_sector_cached;
static u32 pio_data_limit;
static u8 ata_command;
cdda_t cdda;
static bool cdda_completion_reported;
static s32 cdda_scan_step;
static u32 cdda_scan_start;
static u32 cdda_scan_end;

static gd_states gd_state;
static DiscType gd_disk_type;
/*
	GD rom reset -> GDS_WAITCMD

	GDS_WAITCMD -> ATA/SPI command [Command code is on ata_command]
	SPI Command -> GDS_WAITPACKET -> GDS_SPI_* , depending on input

	GDS_SPI_READSECTOR -> Depending on features , it can do quite a few things
*/
static u32 data_write_mode=0;

//Registers
static u32 DriveSel;
static GD_ErrRegT Error;
static GD_InterruptReasonT IntReason;
static GD_FeaturesT Features;
static GD_SecCountT SecCount;
GD_SecNumbT SecNumber;
	
static GD_StatusT GDStatus;
static u8 DeviceControl;
static bool command_interrupt_pending;
static u32 dma_transfer_length;
static u32 dma_direction;
static u32 dma_address_limit;
static u32 dma_cycle_remainder;
static u32 dma_cycle_rate;
static u32 dma_access_timing;
static u32 dma_jitter_carry;
static u32 prefetch_error_fad;
static u32 standby_seconds_remaining;
static u8 max_read_retries;
static bool dma_address_valid;

static constexpr u32 getDmaTransferLength(u32 programmedLength)
{
	return programmedLength == 0 ? 0x02000000
			: programmedLength & 0x01ffffff;
}
static_assert(getDmaTransferLength(0) == 0x02000000);
static_assert(getDmaTransferLength(1) == 1);
static_assert(getDmaTransferLength(0x21) == 0x21);
static_assert(getDmaTransferLength(0x01ffffff) == 0x01ffffff);

static constexpr u32 getDmaAddressLimit(u32 address)
{
	if (address >= 0x00700000 && address < 0x00708000) return 0x00708000;
	if (address >= 0x00800000 && address < 0x00a00000) return 0x00a00000;
	if (address >= 0x01000000 && address < 0x02000000) return 0x02000000;
	if (address >= 0x02700000 && address < 0x03000000) return 0x03000000;
	if (address >= 0x03000000 && address < 0x04000000) return 0x04000000;
	if (address >= 0x04000000 && address < 0x04800000) return 0x04800000;
	if (address >= 0x05000000 && address < 0x05800000) return 0x05800000;
	if (address >= 0x06000000 && address < 0x06800000) return 0x06800000;
	if (address >= 0x07000000 && address < 0x07800000) return 0x07800000;
	if (address >= 0x0c000000 && address < 0x0d000000) return 0x0d000000;
	if (address >= 0x0e000000 && address < 0x0f000000) return 0x0f000000;
	if (address >= 0x14000000 && address < 0x18000000) return 0x18000000;
	return 0;
}
static_assert(getDmaAddressLimit(0x0c000000) == 0x0d000000);
static_assert(getDmaAddressLimit(0x0d000000) == 0);
static_assert(getDmaAddressLimit(0x14000000) == 0x18000000);

static bool dmaAddressProtected(u32 address, u32& limit)
{
	limit = getDmaAddressLimit(address);
	if (limit == 0)
		return false;
	if ((address & 0x1e000000) != 0x0c000000
			&& (address & 0x1f000000) != 0x0e000000)
		return true;

	const u32 top = (SB_GDAPRO >> 8) & 0x7f;
	const u32 bottom = SB_GDAPRO & 0x7f;
	const u32 page = (address >> 20) & 0x7f;
	if (page < top || page > bottom)
		return false;
	limit = std::min(limit, 0x08000000 + ((bottom + 1) << 20));
	return true;
}

static void GDROM_DmaAddressWrite(u32, u32 data)
{
	SB_GDSTAR = data & 0x1fffffe0;
	dma_address_valid = getDmaAddressLimit(data) != 0;
	if (!dma_address_valid)
	{
		SB_GDEN = 0;
		if (SB_GDST != 0)
		{
			SB_GDST = 0;
			dma_jitter_carry = 0;
			sh4_sched_request(gdrom_schid, -1);
		}
		asic_RaiseInterrupt(holly_G1_ILLADDR);
	}
}

static void GDROM_DmaProtectionWrite(u32, u32 data)
{
	if ((data >> 16) == 0x8843)
		SB_GDAPRO = data & 0x00007f7f;
}
#ifdef LIBRETRO
GD_StatusT get_gd_status(void) { return GDStatus; }
#endif

static ByteCount_t ByteCount;
	
//end
GD_HardwareInfo_t GD_HardwareInfo;

#define printf_rm(...) DEBUG_LOG(GDROM, __VA_ARGS__)
#define printf_ata(...) DEBUG_LOG(GDROM, __VA_ARGS__)
#define printf_spi(...) DEBUG_LOG(GDROM, __VA_ARGS__)
#define printf_spicmd(...) DEBUG_LOG(GDROM, __VA_ARGS__)
#define printf_subcode(...) DEBUG_LOG(GDROM, __VA_ARGS__)

void libCore_CDDA_Sector(s16* sector)
{
	if (cdda.status == cdda_t::Playing || cdda.status == cdda_t::Scanning)
	{
		if (libGDR_ReadSector((u8*)sector, cdda.CurrAddr.FAD, 1, 2352, true) == 0)
		{
			// Stop
			if (cdda.CurrAddr.FAD > cdda.StartAddr.FAD)
				cdda.CurrAddr.FAD--; // Stay on the last successfully read sector.
			cdda.status = cdda_t::Failed;
			cdda_completion_reported = false;
			SecNumber.Status = GD_PAUSE;
			gd_schedule_standby();
			sns_key = 5;
			sns_asc = 0;
			sns_ascq = 0x14; // Error during drive playback operation.
			sns_fad = cdda.CurrAddr.FAD;
			GDStatus.CHECK = 1;
			memset(sector, 0, 2352);
		}
		else
		{
			if (cdda.status == cdda_t::Scanning)
			{
				const s64 nextFad = (s64)cdda.CurrAddr.FAD + cdda_scan_step;
				if (nextFad < cdda_scan_start || nextFad >= cdda_scan_end)
				{
					cdda.status = cdda_t::Paused;
					SecNumber.Status = GD_PAUSE;
					gd_schedule_standby();
				}
				else
					cdda.CurrAddr.FAD = nextFad;
			}
			else if (++cdda.CurrAddr.FAD >= cdda.EndAddr.FAD)
			{
				if (cdda.repeats == 0)
				{
					// Stop
					cdda.status = cdda_t::Terminated;
					cdda_completion_reported = false;
					SecNumber.Status = GD_PAUSE;
					gd_schedule_standby();
				}
				else
				{
					// Repeat
					if (cdda.repeats != 15)
						cdda.repeats--;
					cdda.CurrAddr.FAD = cdda.StartAddr.FAD;
				}
			}
		}
	}
	else {
		memset(sector, 0, 2352);
	}
}

static void gd_spi_pio_end(const u8* buffer, u32 len, gd_states next_state = gds_pio_end);
static void gd_process_spi_cmd();
static void gd_process_ata_cmd();
static void gd_set_state(gd_states state);
static u32 getCurrentFad(u32 fallbackFad = 0);
static bool validateSectorType(u32 fad, u32 expectedType);
static bool getReadSectorSize(u32 fad, u32 expectedType, bool header, bool subheader,
		bool data, bool other, u32& sectorSize);

static void completeReadAhead()
{
	if (read_params.next_sector == 0)
		return;
	const u32 nextFad = read_params.next_sector;
	u8 sector[2352];
	if (!gd_read_sector(sector, nextFad, sizeof(sector), false))
		prefetch_error_fad = nextFad;
	else if (prefetch_error_fad == nextFad)
		prefetch_error_fad = 0;
	// getCurrentFad() reports start_sector - 1 once the transfer is empty.
	read_params.start_sector = nextFad + 1;
	read_params.next_sector = 0;
}

static void gd_raise_command_interrupt()
{
	command_interrupt_pending = true;
	if ((DeviceControl & 2) == 0) // nIEN
		asic_RaiseInterrupt(holly_GDROM_CMD);
}

static void gd_spi_error(u8 sense, u8 asc, u8 ascq = 0, u32 errorFad = ~0u)
{
	sns_key = sense;
	Error.Sense = sense;
	sns_asc = asc;
	sns_ascq = ascq;
	sns_fad = errorFad == ~0u ? getCurrentFad() : errorFad;
	GDStatus.CHECK = 1;
	gd_set_state(gds_procpacketdone);
}

static bool gd_media_ready()
{
	if (SecNumber.Status == GD_BUSY)
	{
		gd_spi_error(2, 0x04, 0x01); // NOT READY: drive is becoming ready
		return false;
	}
	const DiscType discType = (DiscType)libGDR_GetDiscType();
	if (discType == Open || discType == NoDisk)
	{
		gd_spi_error(2, 0x3a); // NOT READY: medium not present
		return false;
	}
	return true;
}

bool gd_read_sector(u8 *sector, u32 fad, u32 size, bool form2)
{
	u32 retryLimit = 0;
	// Continuous reading takes priority over recovery. Form 2 uses its own
	// retry-enable bit independently of the Mode 1/Form 1 retry setting.
	const u8 retryEnable = form2 ? 0x01 : 0x08;
	if ((GD_HardwareInfo.read_flags & 0x20) == 0
			&& (GD_HardwareInfo.read_flags & retryEnable) != 0)
		retryLimit = GD_HardwareInfo.read_retry;

	for (u32 retry = 0; retry <= retryLimit; retry++)
	{
		if (libGDR_ReadSector(sector, fad, 1, size, true) == 1)
		{
			max_read_retries = std::max<u32>(max_read_retries, retry);
			return true;
		}
	}
	max_read_retries = std::max<u32>(max_read_retries, retryLimit);
	return false;
}

static void clearMode2DummyBytes(u8 *data, u32 sectorCount, const read_params_t& params)
{
	// Mode 2 non-XA transfers the complete 2336-byte body; its tail is not
	// the four-byte dummy padding used for an XA Form 2 data selection.
	if (params.expected_type == 6)
		return;
	u32 dummySize;
	if (params.expected_type == 4 || params.sector_type == 2084 || params.sector_type == 2088) // Mode 2 Form 1
		dummySize = 28;
	else if (params.expected_type == 5 || params.sector_type == 2336 || params.sector_type == 2340) // Mode 2 Form 2
		dummySize = 4;
	else
		return;

	verify(params.sector_type >= dummySize);
	for (u32 i = 0; i < sectorCount; i++)
		memset(data + (i + 1) * params.sector_type - dummySize, 0, dummySize);
}

bool DmaBuffer::fill(read_params_t& params)
{
	if (!isEmpty())
		return true;
	index = 0;
	verify(params.remaining_sectors > 0);
	verify(params.sector_offset == 0);
	size = 0;
	u32 count = 0;
	while (count < NSECT && params.remaining_sectors > 0)
	{
		if (!validateSectorType(params.start_sector, params.expected_type))
		{
			clear();
			return false;
		}
		if (!getReadSectorSize(params.start_sector, params.expected_type,
				(params.data_select & 8) != 0, (params.data_select & 4) != 0,
				(params.data_select & 2) != 0, (params.data_select & 1) != 0,
				params.sector_type))
		{
			clear();
			return false;
		}
		verify(size + params.sector_type <= sizeof(cache));
		const bool form2 = params.expected_type == 5
				|| (params.expected_type != 6 && params.sector_type == 2336);
		if (!gd_read_sector(cache + size, params.start_sector,
				params.sector_type, form2))
		{
			clear();
			return false;
		}
		clearMode2DummyBytes(cache + size, 1, params);
		size += params.sector_type;
		params.start_sector++;
		params.remaining_sectors--;
		count++;
	}
	return true;
}

static bool fillPioBuffer()
{
	const u32 capacity = std::min(PioBuffer::Capacity, pio_data_limit);
	u8 *destination = pio_buff.fill(capacity);
	u32 transferSize = 0;

	while (transferSize < capacity && read_params.remaining_sectors > 0)
	{
		if (read_params.sector_offset == 0
				&& !getReadSectorSize(read_params.start_sector, read_params.expected_type,
						(read_params.data_select & 8) != 0, (read_params.data_select & 4) != 0,
						(read_params.data_select & 2) != 0, (read_params.data_select & 1) != 0,
						read_params.sector_type))
		{
			pio_buff.clear();
			return false;
		}

		if (!pio_sector_cached || pio_cached_fad != read_params.start_sector
				|| pio_cached_size != read_params.sector_type)
		{
			if (!validateSectorType(read_params.start_sector, read_params.expected_type))
			{
				pio_buff.clear();
				return false;
			}
			const bool form2 = read_params.expected_type == 5
					|| (read_params.expected_type != 6 && read_params.sector_type == 2336);
			if (!gd_read_sector(pio_sector_cache, read_params.start_sector,
					read_params.sector_type, form2))
			{
				pio_buff.clear();
				return false;
			}
			clearMode2DummyBytes(pio_sector_cache, 1, read_params);
			pio_cached_fad = read_params.start_sector;
			pio_cached_size = read_params.sector_type;
			pio_sector_cached = true;
		}
		const u32 byteCount = std::min(capacity - transferSize,
				read_params.sector_type - read_params.sector_offset);
		memcpy(destination + transferSize, pio_sector_cache + read_params.sector_offset, byteCount);
		transferSize += byteCount;
		read_params.sector_offset += byteCount;
		if (read_params.sector_offset == read_params.sector_type)
		{
			read_params.sector_offset = 0;
			read_params.start_sector++;
			read_params.remaining_sectors--;
			pio_sector_cached = false;
		}
	}
	pio_buff.resetSize(transferSize);
	return true;
}

const u8 *DmaBuffer::read(u32 len)
{
	verify(len <= size);
	const u8 *p = &cache[index];
	index += len;
	size -= len;
	return p;
}

void DmaBuffer::serialize(Serializer& ser) const
{
	ser << size;
	ser.serialize(&cache[index], size);
}

void DmaBuffer::deserialize(Deserializer& deser)
{
	if (deser.version() < Deserializer::V54)
	{
		deser >> index;
		deser >> size;
		if (index >= sizeof(cache))
			clear();
		else
			size = std::min<u32>(size, sizeof(cache) - index);
		deser >> cache;
		deser.skip(2352 * 16);
	}
	else
	{
		index = 0;
		deser >> size;
		if (size > sizeof(cache))
			throw Deserializer::Exception("Invalid GD-ROM DMA buffer size");
		deser.deserialize(&cache[0], size);
	}
}

void PioBuffer::serialize(Serializer& ser) const
{
	ser << next_state;
	ser << index;
	ser << size;
	ser.serialize(&_data[0], size);
}

void PioBuffer::deserialize(Deserializer& deser)
{
	deser >> next_state;
	deser >> index;
	deser >> size;
	if (deser.version() < Deserializer::V60)
	{
		// Older states stored the indices and size in 16-bit words.
		if (size > Capacity / sizeof(u16) || index > size)
			throw Deserializer::Exception("Invalid GD-ROM PIO buffer state");
		const u32 oldSize = size;
		if (deser.version() < Deserializer::V54)
			deser >> _data;
		else
			deser.deserialize(&_data[0], oldSize * sizeof(u16));
		index *= sizeof(u16);
		size *= sizeof(u16);
	}
	else
	{
		if (size > Capacity || index > size)
			throw Deserializer::Exception("Invalid GD-ROM PIO buffer state");
		deser.deserialize(&_data[0], size);
	}
}

static void gd_set_state(gd_states state)
{
	gd_states prev=gd_state;
	gd_state=state;
	switch(state)
	{
		case gds_waitcmd:
			GDStatus.DRDY=1;   // Can accept ATA command :)
			GDStatus.BSY=0;    // Does not access command block
			break;

		case gds_procata:
			//verify(prev==gds_waitcmd);    // Validate the previous command ;)

			GDStatus.DRDY=0;   // Can't accept ATA command
			GDStatus.BSY=1;    // Accessing command block to process command
			gd_process_ata_cmd();
			break;

		case gds_waitpacket:
			verify(prev==gds_procata); // Validate the previous command ;)

			// Prepare for packet command
			packet_cmd.index=0;

			// Set CoD, clear BSY and IO
			IntReason.CoD=1;
			GDStatus.BSY = 0;
			IntReason.IO=0;

			// Make DRQ valid
			GDStatus.DRQ = 1;

			// ATA can optionally raise the interrupt ...
			// RaiseInterrupt(holly_GDROM_CMD);
			break;

		case gds_procpacket:
			verify(prev==gds_waitpacket); // Validate the previous state ;)

			GDStatus.DRQ=0;     // Can't accept ATA command
			GDStatus.BSY=1;     // Accessing command block to process command
			// A zero Byte Count encodes the maximum 64 KiB PIO transfer.
			pio_data_limit = ByteCount.full == 0 ? PioBuffer::Capacity : ByteCount.full;
			gd_process_spi_cmd();
			break;
			//yep , get/set are the same !
		case gds_pio_get_data:
		case gds_pio_send_data:
			//  When preparations are complete, the following steps are carried out at the device.
			//(1)   Number of bytes to be read is set in "Byte Count" register. 
			ByteCount.full = (u16)pio_buff.getSize();
			//(2)   IO bit is set and CoD bit is cleared. 
			IntReason.IO = state == gds_pio_send_data;
			IntReason.CoD=0;
			//(3)   DRQ bit is set, BSY bit is cleared. 
			GDStatus.DRQ=1;
			GDStatus.BSY=0;
			//(4)   INTRQ is set, and a host interrupt is issued.
			gd_raise_command_interrupt();
			/*
			The number of bytes normally is the byte number in the register at the time of receiving 
			the command, but it may also be the total of several devices handled by the buffer at that point.
			*/
			break;

		case gds_readsector_pio:
			{
				/*
				If more data are to be sent, the device sets the BSY bit and repeats the above sequence 
				from step 7. 
				*/
				GDStatus.BSY=1;

				if (!fillPioBuffer())
				{
					if (gd_state == gds_readsector_pio)
						gd_spi_error(3, 0x11); // MEDIUM ERROR: unrecoverable read error
					break;
				}
				const gd_states nextState = read_params.remaining_sectors == 0
						? gds_pio_end : gds_readsector_pio;
				gd_spi_pio_end(nullptr, 0, nextState);
			}
			break;
			
		case gds_readsector_dma:
			dma_buff.clear();
			if (!dma_buff.fill(read_params) && gd_state == gds_readsector_dma)
				gd_spi_error(3, 0x11); // MEDIUM ERROR: unrecoverable read error
			break;

		case gds_pio_end:
			completeReadAhead();
			GDStatus.DRQ=0;//all data is sent !

			gd_set_state(gds_procpacketdone);
			break;

		case gds_procpacketdone:
			/*
			7.  When the device is ready to send the status, it writes the 
			final status (IO, CoD, DRDY set, BSY, DRQ cleared) to the "Status" register before making INTRQ valid. 
			After checking INTRQ, the host reads the "Status" register to check the completion status. 
			*/
			//Set IO, CoD, DRDY
			GDStatus.DRDY=1;
			IntReason.CoD=1;
			IntReason.IO=1;

			//Clear DRQ,BSY
			GDStatus.DRQ=0;
			GDStatus.BSY=0;
			//Make INTRQ valid
			gd_raise_command_interrupt();

			//command finished !
			gd_set_state(gds_waitcmd);
			break;

		case gds_process_set_mode:
			{
				GD_HardwareInfo_t candidate = GD_HardwareInfo;
				memcpy((u8 *)&candidate + set_mode_offset, pio_buff.data(), pio_buff.getSize());
				const bool reservedBytesSet = candidate._res0[0] != 0 || candidate._res0[1] != 0
						|| candidate._res1 != 0 || candidate._res2[0] != 0 || candidate._res2[1] != 0;
				if (reservedBytesSet || candidate.speed > 7 || (candidate.read_flags & ~0x39) != 0)
					gd_spi_error(5, 0x26); // ILLEGAL REQUEST: invalid parameter data
				else
				{
					GD_HardwareInfo = candidate;
					if (SecNumber.Status == GD_PAUSE)
						gd_schedule_standby();
					gd_set_state(gds_pio_end);
				}
			}
			break;

		case gds_open_wait:
			SecNumber.Status = GD_BUSY;
			GDStatus.DRQ = 0;
			GDStatus.BSY = 1;
			break;

		default :
			die("Unhandled GDROM state ...");
			break;
	}
}


static void gd_abort_transfer()
{
	if (gdrom_schid >= 0)
		sh4_sched_request(gdrom_schid, -1);
	// Cancelling the drive command removes DMARQ but does not terminate the
	// independent HOLLY G1 engine. If it was active, GDST remains set until
	// software disables GDEN, or until a DMA completion/fault occurs.
	read_params = {};
	dma_buff.clear();
	pio_buff.clear();
	pio_sector_cached = false;
	pio_data_limit = PioBuffer::Capacity;
	ByteCount.full = 0;
	GDStatus.DRQ = 0;
}

void gd_media_change()
{
	// A transfer belongs to the medium on which it was started. In particular,
	// don't let a pending DMA callback resume the old command against a newly
	// inserted image of the same disc type.
	gd_abort_transfer();
	prefetch_error_fad = 0;
	cdda.status = cdda_t::NoInfo;
	cdda.StartAddr.FAD = 0;
	cdda.CurrAddr.FAD = 0;
	cdda.EndAddr.FAD = 0;
	cdda.repeats = 0;
	cdda_completion_reported = false;
	cdda_scan_step = 0;
	cdda_scan_start = 0;
	cdda_scan_end = 0;
}

void gd_setdisc(bool mediaChanged)
{
	const DiscType previousDiscType = gd_disk_type;
	cdda.status = cdda_t::NoInfo;

	DiscType newd = (DiscType)libGDR_GetDiscType();
	if (mediaChanged || newd != previousDiscType)
	{
		const bool completingOpenCommand = gd_state == gds_open_wait && newd == Open;
		gd_media_change();
		if (!completingOpenCommand)
			gd_set_state(gds_waitcmd);
	}
	
	switch(newd)
	{
	case NoDisk:
		SecNumber.Status = GD_NODISC;
		//GDStatus.BSY=0;
		//GDStatus.DRDY=1;
		break;

	case Open:
		SecNumber.Status = GD_OPEN;
		//GDStatus.BSY=0;
		GDStatus.DRDY=1;
		break;

	default :
		if (SecNumber.Status == GD_BUSY)
		{
			SecNumber.Status = GD_PAUSE;
			gd_schedule_standby();
		}
		else
			SecNumber.Status = GD_STANDBY;
		//GDStatus.BSY=0;
		//GDStatus.DRDY=1;
		break;
	}

	gd_disk_type = newd;
	SecNumber.DiscFormat = gd_disk_type >> 4;
	if (gd_state == gds_open_wait && newd == Open)
		gd_set_state(gds_procpacketdone);
}

static void gd_reset()
{
	//Reset the drive
	gd_setdisc();
	gd_set_state(gds_waitcmd);
}

static u32 GetFAD(u8* data, bool msf)
{
	if (msf)
		return data[0] * 60 * 75 + data[1] * 75 + data[2];
	else
		return (data[0] << 16) | (data[1] << 8) | data[2];
}

static bool validMSF(const u8 *data)
{
	return data[1] < 60 && data[2] < 75;
}

static u32 getLeadOutFad()
{
	u8 sessionInfo[6] {};
	libGDR_GetSessionInfo(sessionInfo, 0);
	return sessionInfo[3] << 16 | sessionInfo[4] << 8 | sessionInfo[5];
}

u32 gd_get_playback_area_end(u32 fad)
{
	const u32 outerLeadOut = getLeadOutFad();
	u8 sessionInfo[6] {};
	libGDR_GetSessionInfo(sessionInfo, 0);
	const u32 sessionCount = sessionInfo[2];
	for (u32 session = 2; session <= sessionCount; session++)
	{
		libGDR_GetSessionInfo(sessionInfo, session);
		const u32 nextSessionStart = sessionInfo[3] << 16
				| sessionInfo[4] << 8 | sessionInfo[5];
		if (fad >= nextSessionStart)
			continue;

		u32 areaEnd = 0;
		for (u32 track = 1; track <= 99; track++)
		{
			u32 trackStart;
			u32 trackEnd;
			if (libGDR_GetTrack(track, trackStart, trackEnd)
					&& trackStart < nextSessionStart)
				areaEnd = std::max(areaEnd, trackEnd + 1);
		}
		if (areaEnd > fad)
			return areaEnd;
		break;
	}
	return outerLeadOut;
}

void gd_start_cdda_scan(bool reverse, u32 speedParameter)
{
	const s32 speed = speedParameter < 3 ? 2 : speedParameter < 6 ? 5 : speedParameter < 10 ? 9 : 16;
	cdda_scan_step = reverse ? -speed : speed;
	cdda_scan_start = 150;
	cdda_scan_end = gd_get_playback_area_end(cdda.CurrAddr.FAD);

	// Scan is confined to the current session's program area. In particular,
	// GD-ROM's high-density session must not scan backwards across its lead-in
	// and the unreadable inter-session gap.
	u8 sessionInfo[6] {};
	libGDR_GetSessionInfo(sessionInfo, 0);
	const u32 sessionCount = sessionInfo[2];
	for (u32 session = 1; session <= sessionCount; session++)
	{
		libGDR_GetSessionInfo(sessionInfo, session);
		const u32 sessionStart = sessionInfo[3] << 16 | sessionInfo[4] << 8 | sessionInfo[5];
		if (sessionStart <= cdda.CurrAddr.FAD)
		{
			cdda_scan_start = sessionStart;
			continue;
		}

		break;
	}
	cdda.status = cdda_t::Scanning;
	SecNumber.Status = GD_SCAN;
}

void gd_pause_cdda()
{
	if (cdda.status == cdda_t::Playing || cdda.status == cdda_t::Scanning)
		cdda.status = cdda_t::Paused;
	else if (cdda.status != cdda_t::Paused || cdda.EndAddr.FAD <= cdda.StartAddr.FAD
			|| cdda.CurrAddr.FAD < cdda.StartAddr.FAD || cdda.CurrAddr.FAD >= cdda.EndAddr.FAD)
		cdda.status = cdda_t::NoInfo;
	cdda_scan_step = 0;
	SecNumber.Status = GD_PAUSE;
	gd_schedule_standby();
}

static int standbyCallback(int, int, int jitter, void *)
{
	int remainingJitter = std::max(0, jitter);
	for (;;)
	{
		if (SecNumber.Status != GD_PAUSE || standby_seconds_remaining == 0)
		{
			standby_seconds_remaining = 0;
			return 0;
		}
		if (--standby_seconds_remaining == 0)
		{
			SecNumber.Status = GD_STANDBY;
			cdda.status = cdda_t::NoInfo;
			cdda.EndAddr.FAD = 0;
			cdda.repeats = 0;
			return 0;
		}
		if (SH4_MAIN_CLOCK > remainingJitter)
			return std::max(0, jitter) + SH4_MAIN_CLOCK - remainingJitter;
		remainingJitter -= SH4_MAIN_CLOCK;
	}
}

void gd_schedule_standby()
{
	standby_seconds_remaining = GD_HardwareInfo.standby_hi << 8
			| GD_HardwareInfo.standby_lo;
	if (standby_schid >= 0)
		sh4_sched_request(standby_schid,
				standby_seconds_remaining == 0 ? -1 : SH4_MAIN_CLOCK);
}

bool gd_normalize_playback_start(u32& fad)
{
	u32 elapsed;
	if (libGDR_GetTrackNumber(fad, elapsed) != 0xaa)
		return true;

	// A play request beginning in an intermediate lead-out/lead-in starts at
	// the following session's user program area.
	for (u32 track = 1; track <= 99; track++)
	{
		u32 startFad;
		u32 endFad;
		if (!libGDR_GetTrack(track, startFad, endFad))
			continue;
		if (startFad > fad)
		{
			fad = startFad;
			return true;
		}
	}
	return false;
}

static u32 getCurrentFad(u32 fallbackFad)
{
	switch (SecNumber.Status)
	{
	case GD_STANDBY:
	case GD_OPEN:
	case GD_NODISC:
	case GD_ERROR:
		return 150; // Home position (00:02:00).
	default:
		break;
	}
	if ((cdda.status == cdda_t::Playing || cdda.status == cdda_t::Paused
			|| cdda.status == cdda_t::Scanning || cdda.status == cdda_t::Terminated
			|| cdda.status == cdda_t::Failed || cdda.status == cdda_t::Seeked)
			&& cdda.CurrAddr.FAD >= 150)
		return cdda.CurrAddr.FAD;
	if (fallbackFad >= 150)
		return fallbackFad;
	if (read_params.start_sector > 150)
		return read_params.start_sector - 1;
	if (SecNumber.Status == GD_PAUSE && libGDR_GetDiscType() == GdRom)
		return 45150; // GD-ROM startup pause position is 10:02:00.
	return 150;
}

static bool validateSectorType(u32 fad, u32 expectedType)
{
	u32 elapsed;
	const u32 track = libGDR_GetTrackNumber(fad, elapsed);
	if (track == 0xaa)
	{
		// Addresses in an intermediate lead-out/lead-in or another unmapped
		// gap are not part of a readable program area.
		gd_spi_error(5, 0x21, 0, fad);
		return false;
	}
	u8 adr;
	u8 control;
	libGDR_GetTrackAdrAndControl(track, adr, control);
	const bool dataTrack = (control & 4) != 0;
	if (expectedType == 0 && !dataTrack)
		return true;
	if (expectedType == 1)
	{
		if (!dataTrack)
			return true;
		gd_spi_error(5, 0x64); // ILLEGAL REQUEST: media header mode mismatch
		return false;
	}
	if (!dataTrack)
	{
		gd_spi_error(5, 0x64);
		return false;
	}

	if (expectedType == 6)
	{
		if (libGDR_GetDiscType() != CdRom_XA)
			return true; // Mode 2 non-XA performs no sector-type check.
		gd_spi_error(5, 0x64);
		return false;
	}

	u8 rawSector[2352] {};
	if (!gd_read_sector(rawSector, fad, sizeof(rawSector), expectedType == 5))
	{
		gd_spi_error(3, 0x11); // MEDIUM ERROR: unrecoverable read error
		return false;
	}
	const bool mode1 = rawSector[15] == 1;
	const bool mode2 = rawSector[15] == 2;
	const bool form2 = mode2 && (rawSector[18] & 0x20) != 0;
	if (expectedType == 0)
	{
		if (!mode2 || libGDR_GetDiscType() == CdRom_XA)
			return true;
		gd_spi_error(5, 0x64);
		return false;
	}
	const bool matches = expectedType == 2 ? mode1
			: expectedType == 3 ? mode2
			: expectedType == 4 ? mode2 && !form2
			: expectedType == 5 && form2;
	if (matches)
		return true;

	gd_spi_error(5, 0x64);
	return false;
}

static bool getReadSectorSize(u32 fad, u32 expectedType, bool header, bool subheader,
		bool data, bool other, u32& sectorSize)
{
	if (other || expectedType == 1)
	{
		sectorSize = 2352;
		return true;
	}
	if (!header && !subheader && !data)
	{
		gd_spi_error(5, 0x24); // ILLEGAL REQUEST: invalid command field
		return false;
	}

	// "Any type" and the generic Mode 2 type still have a concrete layout.
	// Determine it from the first sector so Data Select produces the byte
	// group (and dummy tail) belonging to the actual form.
	if (expectedType == 0 || expectedType == 3)
	{
		u32 elapsed;
		const u32 track = libGDR_GetTrackNumber(fad, elapsed);
		u8 adr;
		u8 control;
		libGDR_GetTrackAdrAndControl(track, adr, control);
		if ((control & 4) == 0)
		{
			sectorSize = 2352;
			return true;
		}
		u8 rawSector[2352] {};
		if (!gd_read_sector(rawSector, fad, sizeof(rawSector), expectedType == 5))
		{
			gd_spi_error(3, 0x11); // MEDIUM ERROR: unrecoverable read error
			return false;
		}
		if (rawSector[15] == 1 && expectedType == 0)
			expectedType = 2;
		else if (rawSector[15] == 2)
			expectedType = (rawSector[18] & 0x20) != 0 ? 5 : 4;
		else
		{
			gd_spi_error(5, 0x64); // ILLEGAL REQUEST: media header mode mismatch
			return false;
		}
	}

	if (expectedType == 2)
	{
		// Mode 1 has no subheader, so selections containing it are invalid.
		if (subheader || (!header && !data))
		{
			gd_spi_error(5, 0x24);
			return false;
		}
		sectorSize = (header ? 4 : 0) + (data ? 2048 : 0);
		return true;
	}

	// Mode 2 data transfers automatically include the 8-byte subheader.
	// Form 1 appends 28 dummy bytes and Form 2 appends four, as specified by
	// Sega; both make a stable data group for every sector in the command.
	if (data)
	{
		const u32 mode2DataGroup = expectedType == 4 ? 2084 : 2336;
		sectorSize = mode2DataGroup + (header ? 4 : 0);
	}
	else if (header && subheader)
		sectorSize = 12;
	else if (header)
		sectorSize = 4;
	else if (subheader)
		sectorSize = 8;
	else
	{
		gd_spi_error(5, 0x24);
		return false;
	}
	return true;
}

//disk changes etc
static void gd_disc_change()
{
	gd_setdisc();
	read_params = { 0 };
	set_mode_offset = 0;
	packet_cmd = { 0 };
	dma_buff.clear();
	pio_buff.clear();
	pio_data_limit = PioBuffer::Capacity;
	ata_command = 0;
	cdda = { cdda_t::NoInfo, 0 };
	cdda_completion_reported = false;
	cdda_scan_step = 0;
	cdda_scan_start = 0;
	cdda_scan_end = 0;
	prefetch_error_fad = 0;
	max_read_retries = 0;
}

//This handles the work of setting up the pio regs/state :)
static void gd_spi_pio_end(const u8* buffer, u32 len, gd_states next_state)
{
	if (buffer != nullptr) {
		if (ata_command == ATA_SPI_PACKET)
			len = std::min(len, pio_data_limit);
		verify(len <= PioBuffer::Capacity);
		memcpy(pio_buff.fill(len), buffer, len);
	}
	pio_buff.next_state = next_state;
	if (pio_buff.isEmpty())
		gd_set_state(next_state);
	else
		gd_set_state(gds_pio_send_data);
}
static void gd_spi_pio_read_end(u32 len, gd_states next_state)
{
	len = std::min(len, pio_data_limit);
	verify(len <= PioBuffer::Capacity);
	pio_buff.resetSize(len);
	pio_buff.next_state = next_state;

	if (len == 0)
		gd_set_state(next_state);
	else
		gd_set_state(gds_pio_get_data);
}

static void resetHardwareInfo()
{
	memset(&GD_HardwareInfo, 0, sizeof(GD_HardwareInfo));
	GD_HardwareInfo.speed = 0;
	GD_HardwareInfo.standby_hi = 0;
	GD_HardwareInfo.standby_lo = 0xb4;
	GD_HardwareInfo.read_flags = 0x19;
	GD_HardwareInfo.read_retry = 0x08;
	memcpy(GD_HardwareInfo.drive_info, "SE      ", sizeof(GD_HardwareInfo.drive_info));
	memcpy(GD_HardwareInfo.system_version, "Rev 6.43", sizeof(GD_HardwareInfo.system_version));
	memcpy(GD_HardwareInfo.system_date, "990408", sizeof(GD_HardwareInfo.system_date));
}

static void gd_process_ata_cmd()
{
	//Any ATA command clears these bits, unless aborted/error :p
	Error.ABRT=0;
	
	if (sns_key == 0x0 			// No sense
			|| sns_key == 0xB	// Aborted
			|| sns_key == 6) 	// Unit attention
		GDStatus.CHECK=0;
	else
		GDStatus.CHECK=1;

	switch (ata_command)
	{
	case ATA_NOP:
		printf_ata("ATA_NOP");
		/*
			Setting "abort" in the error register 
			Setting an error in the status register 
			Clearing "busy" in the status register 
			Asserting the INTRQ signal
		*/
		
		gd_abort_transfer();
		Error.ABRT = 1;
		Error.Sense = sns_key;
		GDStatus.BSY = 0;
		GDStatus.CHECK = 1;
		GDStatus.DRQ = 0;

		gd_raise_command_interrupt();
		gd_set_state(gds_waitcmd);
		break;

	case ATA_SOFT_RESET:
		{
			printf_ata("ATA_SOFT_RESET");
			gd_abort_transfer();
			asic_CancelInterrupt(holly_GDROM_CMD);
			command_interrupt_pending = false;
			// SPI Soft Reset restores interface defaults. ATA SRST is not
			// supported by this drive, and only the drive-select bit is
			// specified to survive this command; nIEN must not remain latched.
			DeviceControl = 0;
			gd_reset();
			resetHardwareInfo();
			read_params = {};
			prefetch_error_fad = 0;
			max_read_retries = 0;
			packet_cmd = {};
			dma_buff.clear();
			pio_buff.clear();
			pio_data_limit = PioBuffer::Capacity;
			set_mode_offset = 0;
			cdda = {};
			cdda_completion_reported = false;
			cdda_scan_step = 0;
			cdda_scan_start = 0;
			cdda_scan_end = 0;
			GDStatus.full = 0;
			Error.full = 1;
			sns_key = 6; // UNIT ATTENTION after host soft reset
			sns_asc = 0x29;
			sns_ascq = 0;
			sns_fad = 150;
			SecCount.full = 1;
			const DiscType discType = (DiscType)libGDR_GetDiscType();
			SecNumber.DiscFormat = discType >> 4;
			SecNumber.Status = discType == Open ? GD_OPEN
					: discType == NoDisk ? GD_NODISC : GD_PAUSE;
			if (SecNumber.Status == GD_PAUSE)
				gd_schedule_standby();
			IntReason.full = 1;
			Features.full = 0;
			// Soft reset reinitializes the task file, except for ATA's DRV bit.
			// DRV is fixed to zero on this device, so optional LUN bits clear.
			DriveSel = 0xa0;
			// DC Checker expects these values
			ByteCount.low = 0x14;
			ByteCount.hi = 0xEB;
			// The reset task-file signature has Status=00h. Return to command
			// acceptance without gd_set_state(gds_waitcmd), which would
			// incorrectly synthesize DRDY in that signature.
			gd_state = gds_waitcmd;
		}
		break;

	case ATA_EXEC_DIAG:
		printf_ata("ATA_EXEC_DIAG");
		Error.full = 1;	// No error
		sns_key = 0;
		sns_asc = 0;
		sns_ascq = 0;
		sns_fad = 0;
		GDStatus.BSY = 0;
		GDStatus.CHECK = 0;

		gd_raise_command_interrupt();
		gd_set_state(gds_waitcmd);
		break;

	case ATA_SPI_PACKET:
		printf_ata("ATA_SPI_PACKET");
		if ((Features.full & ~1) != 0)
		{
			// During normal packet operation only bit 0 (DMA) is defined.
			// Reject reserved task-file feature bits before requesting the CDB.
			Error.full = 0;
			Error.ABRT = 1;
			GDStatus.BSY = 0;
			GDStatus.DRQ = 0;
			GDStatus.CHECK = 1;
			gd_raise_command_interrupt();
			gd_set_state(gds_waitcmd);
		}
		else
			gd_set_state(gds_waitpacket);
		break;

	case ATA_IDENTIFY_DEV:
		printf_ata("ATA_IDENTIFY_DEV: offset %d len %d", packet_cmd.data_8[2], packet_cmd.data_8[4]);
		GDStatus.BSY = 0;
		gd_spi_pio_end((const u8*)&reply_a1[0], 0x50);
		break;

	case ATA_SET_FEATURES:
		printf_ata("ATA_SET_FEATURES");
		{
			const u32 transferMode = SecCount.full >> 3;
			const u32 modeValue = SecCount.full & 7;
			const bool validMode = transferMode == 0 // PIO default
					|| transferMode == 1             // PIO flow control
					|| transferMode == 2             // single-word DMA
					|| transferMode == 4;            // multi-word DMA
			// The protocol table reserves PIO-default mode values 2 through 7
			// (00000 00x); the other supported classes accept all three-bit
			// mode values.
			if (Features.full != 3 || !validMode || (transferMode == 0 && modeValue > 1))
			{
				Error.full = 0;
				Error.ABRT = 1;
				GDStatus.CHECK = 1;
			}
			else
			{
				Error.full = 0;
				GDStatus.CHECK = 0;
			}
		}
		GDStatus.DSC = 0;
		GDStatus.DF = 0;
		GDStatus.DRQ = 0;
		gd_raise_command_interrupt();
		gd_set_state(gds_waitcmd);
		break;

    case ATA_IDENTIFY:
        printf_ata("ATA_IDENTIFY");

        // Set Signature
        DriveSel = 0xa0;

        SecCount.full = 1;
        SecNumber.full = 1;
        ByteCount.low = 0x14;
        ByteCount.hi = 0xeb;

        // ABORT command
        Error.full = 0x4;

        GDStatus.full = 0;
        GDStatus.DRDY = 1;
        GDStatus.CHECK = 1;

		gd_raise_command_interrupt();
        gd_set_state(gds_waitcmd);
        break;

	default:
		WARN_LOG(GDROM, "Unknown ATA command %x", ata_command);
		Error.ABRT = 1;
		Error.Sense = 5;	// illegal request
		GDStatus.BSY = 0;
		GDStatus.CHECK = 1;
		gd_raise_command_interrupt();
		gd_set_state(gds_waitcmd);

		break;
	};
}

static u16 ccitt_crc(void *data, int len)
{
	static constexpr u16 crctable[256] = {
	    0x0000, 0x1021, 0x2042, 0x3063, 0x4084, 0x50a5, 0x60c6, 0x70e7,
	    0x8108, 0x9129, 0xa14a, 0xb16b, 0xc18c, 0xd1ad, 0xe1ce, 0xf1ef,
	    0x1231, 0x0210, 0x3273, 0x2252, 0x52b5, 0x4294, 0x72f7, 0x62d6,
	    0x9339, 0x8318, 0xb37b, 0xa35a, 0xd3bd, 0xc39c, 0xf3ff, 0xe3de,
	    0x2462, 0x3443, 0x0420, 0x1401, 0x64e6, 0x74c7, 0x44a4, 0x5485,
	    0xa56a, 0xb54b, 0x8528, 0x9509, 0xe5ee, 0xf5cf, 0xc5ac, 0xd58d,
	    0x3653, 0x2672, 0x1611, 0x0630, 0x76d7, 0x66f6, 0x5695, 0x46b4,
	    0xb75b, 0xa77a, 0x9719, 0x8738, 0xf7df, 0xe7fe, 0xd79d, 0xc7bc,
	    0x48c4, 0x58e5, 0x6886, 0x78a7, 0x0840, 0x1861, 0x2802, 0x3823,
	    0xc9cc, 0xd9ed, 0xe98e, 0xf9af, 0x8948, 0x9969, 0xa90a, 0xb92b,
	    0x5af5, 0x4ad4, 0x7ab7, 0x6a96, 0x1a71, 0x0a50, 0x3a33, 0x2a12,
	    0xdbfd, 0xcbdc, 0xfbbf, 0xeb9e, 0x9b79, 0x8b58, 0xbb3b, 0xab1a,
	    0x6ca6, 0x7c87, 0x4ce4, 0x5cc5, 0x2c22, 0x3c03, 0x0c60, 0x1c41,
	    0xedae, 0xfd8f, 0xcdec, 0xddcd, 0xad2a, 0xbd0b, 0x8d68, 0x9d49,
	    0x7e97, 0x6eb6, 0x5ed5, 0x4ef4, 0x3e13, 0x2e32, 0x1e51, 0x0e70,
	    0xff9f, 0xefbe, 0xdfdd, 0xcffc, 0xbf1b, 0xaf3a, 0x9f59, 0x8f78,
	    0x9188, 0x81a9, 0xb1ca, 0xa1eb, 0xd10c, 0xc12d, 0xf14e, 0xe16f,
	    0x1080, 0x00a1, 0x30c2, 0x20e3, 0x5004, 0x4025, 0x7046, 0x6067,
	    0x83b9, 0x9398, 0xa3fb, 0xb3da, 0xc33d, 0xd31c, 0xe37f, 0xf35e,
	    0x02b1, 0x1290, 0x22f3, 0x32d2, 0x4235, 0x5214, 0x6277, 0x7256,
	    0xb5ea, 0xa5cb, 0x95a8, 0x8589, 0xf56e, 0xe54f, 0xd52c, 0xc50d,
	    0x34e2, 0x24c3, 0x14a0, 0x0481, 0x7466, 0x6447, 0x5424, 0x4405,
	    0xa7db, 0xb7fa, 0x8799, 0x97b8, 0xe75f, 0xf77e, 0xc71d, 0xd73c,
	    0x26d3, 0x36f2, 0x0691, 0x16b0, 0x6657, 0x7676, 0x4615, 0x5634,
	    0xd94c, 0xc96d, 0xf90e, 0xe92f, 0x99c8, 0x89e9, 0xb98a, 0xa9ab,
	    0x5844, 0x4865, 0x7806, 0x6827, 0x18c0, 0x08e1, 0x3882, 0x28a3,
	    0xcb7d, 0xdb5c, 0xeb3f, 0xfb1e, 0x8bf9, 0x9bd8, 0xabbb, 0xbb9a,
	    0x4a75, 0x5a54, 0x6a37, 0x7a16, 0x0af1, 0x1ad0, 0x2ab3, 0x3a92,
	    0xfd2e, 0xed0f, 0xdd6c, 0xcd4d, 0xbdaa, 0xad8b, 0x9de8, 0x8dc9,
	    0x7c26, 0x6c07, 0x5c64, 0x4c45, 0x3ca2, 0x2c83, 0x1ce0, 0x0cc1,
	    0xef1f, 0xff3e, 0xcf5d, 0xdf7c, 0xaf9b, 0xbfba, 0x8fd9, 0x9ff8,
	    0x6e17, 0x7e36, 0x4e55, 0x5e74, 0x2e93, 0x3eb2, 0x0ed1, 0x1ef0,
	};

	u16 crc = 0;
	u8 *p = (u8 *)data;

	while (len-- > 0)
		crc = ((crc & 0xff) << 8) ^ crctable[((crc >> 8) & 0xff) ^ *p++];

	return ~crc;
}

static int bin2bcd(int v)
{
	return (v % 10) | ((v / 10) << 4);
}

static bool encodeIsrc(const std::string& isrc, u8 *destination)
{
	if (isrc.size() != 12)
		return false;

	u8 encoded[12];
	for (u32 i = 0; i < 12; i++)
	{
		u8 value = isrc[i];
		if (value >= 'a' && value <= 'z')
			value -= 'a' - 'A';
		const bool valid = i < 2 ? value >= 'A' && value <= 'Z'
				: i < 5 ? (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9')
				: value >= '0' && value <= '9';
		if (!valid)
			return false;
		// Q-channel mode 3 stores the five country/owner characters in
		// six bits. The remaining seven characters are ASCII-compatible
		// digit nibbles (0011dddd).
		encoded[i] = i < 5 ? value & 0x3f : value;
	}
	memcpy(destination, encoded, sizeof(encoded));
	return true;
}

static void encodeSubcode(u8 *buffer, u8 data)
{
	for (u32 bit = 0; bit < 8; bit++)
		buffer[bit] = (buffer[bit] & ~0x40) | ((data & (0x80 >> bit)) ? 0x40 : 0);
}

u32 gd_get_subcode(u32 format, u32 fad, u8 *subc_info)
{
	subc_info[0] = 0;
	switch (cdda.status)
	{
	case cdda_t::NoInfo:
	case cdda_t::Seeked:
	default:
		subc_info[1] = 0x15;	// No audio status info
		break;
	case cdda_t::Playing:
	case cdda_t::Scanning:
		subc_info[1] = 0x11;	// Audio playback in progress
		break;
	case cdda_t::Paused:
		subc_info[1] = cdda.EndAddr.FAD > cdda.StartAddr.FAD
				&& cdda.CurrAddr.FAD >= cdda.StartAddr.FAD
				&& cdda.CurrAddr.FAD < cdda.EndAddr.FAD
				? 0x12 : 0x15; // Paused playback, or no valid audio status.
		break;
	case cdda_t::Terminated:
		subc_info[1] = cdda_completion_reported ? 0x15 : 0x13;
		break;
	case cdda_t::Failed:
		subc_info[1] = cdda_completion_reported ? 0x15 : 0x14;
		break;
	}
	switch (format)
	{
	case 0:	// Raw subcode
		subc_info[2] = 0;
		subc_info[3] = 100;
		libGDR_ReadSubChannel(subc_info + 4, 96, getCurrentFad(fad));
		// Preserve real P-W data. Packed CHD subcode only carries R-W, so add
		// Q when it is absent without disturbing those stored channels.
		if (std::any_of(subc_info + 4, subc_info + 100,
				[](u8 value) { return (value & 0x40) != 0; }))
			break;
		{
			u32 curFad = getCurrentFad(fad);
			u32 elapsed;
			u32 tracknum = libGDR_GetTrackNumber(curFad, elapsed);
			u8 Qch[12];
			u8 adr, ctrl;
			libGDR_GetTrackAdrAndControlAtFad(curFad, adr, ctrl);
			Qch[0] = (ctrl << 4) | adr;
			// Track numbers are BCD in raw Q, except the lead-out designator,
			// which is the literal AAh value.
			Qch[1] = tracknum == 0xaa ? 0xaa : bin2bcd(tracknum);
			Qch[2] = bin2bcd(1); 					// index
			Qch[3] = bin2bcd(elapsed / 60 / 75); 	// min
			Qch[4] = bin2bcd((elapsed / 75) % 60); 	// sec
			Qch[5] = bin2bcd(elapsed % 75); 		// frame
			Qch[6] = 0;
			Qch[7] = bin2bcd(curFad / 60 / 75); 	// toc min
			Qch[8] = bin2bcd((curFad / 75) % 60);	// toc sec
			Qch[9] = bin2bcd(curFad % 75); 			// toc frame
			u16 crc = ccitt_crc(Qch, 10);
			Qch[10] = crc >> 8;
			Qch[11] = crc;
			u8 *p = &subc_info[4];
			for (u8 q : Qch)
			{
				encodeSubcode(p, q);
				p += 8;
			}
		}
		break;

	case 1:	// Q data only
		{
			u32 curFad = getCurrentFad(fad);
			u32 elapsed;
			u32 tracknum = libGDR_GetTrackNumber(curFad, elapsed);

			//2 DATA Length MSB (0 = 0h)
			subc_info[2] = 0;
			//3 DATA Length LSB (14 = Eh)
			subc_info[3] = 0xE;
			//4 Control ADR
			u8 adr, ctrl;
			libGDR_GetTrackAdrAndControlAtFad(curFad, adr, ctrl);
			subc_info[4] = (ctrl << 4) | adr;
			//5-13	DATA-Q
			u8* data_q = &subc_info[5 - 1];
			//-When ADR = 1
			//1 TNO - track number
			data_q[1] = tracknum;
			//2 X - index within track
			data_q[2] = 1;
			//3-5   Elapsed FAD within track
			data_q[3] = elapsed >> 16;
			data_q[4] = elapsed >> 8;
			data_q[5] = elapsed;
			//6 ZERO
			data_q[6] = 0;
			//7-9 FAD
			data_q[7] = curFad >> 16;
			data_q[8] = curFad >> 8;
			data_q[9] = curFad;
			DEBUG_LOG(GDROM, "gd_get_subcode: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
					 subc_info[0], subc_info[1], subc_info[2], subc_info[3],
					 subc_info[4], subc_info[5], subc_info[6], subc_info[7],
					 subc_info[8], subc_info[9], subc_info[10], subc_info[11],
					 subc_info[12], subc_info[13]);
		}
		break;

	case 2:	// Media catalog number (UPC/bar code)
		{
			//2 DATA Length MSB (0 = 0h)
			subc_info[2] = 0;
			//3 DATA Length LSB (24 = 18h)
			subc_info[3] = 0x18;
			//4 Format Code
			subc_info[4] = 2;
			//5-7 reserved
			subc_info[5] = 0;
			subc_info[6] = 0;
			subc_info[7] = 0;
			const std::string catalog = libGDR_GetDiskCatalog();
			const bool validCatalog = catalog.size() == 13
					&& std::all_of(catalog.begin(), catalog.end(),
							[](char value) { return value >= '0' && value <= '9'; });
			//8 MCVal (bit 7)
			subc_info[8] = validCatalog ? 0x80 : 0;
			//9-21 Media catalog number
			memset(&subc_info[9], '0', 13);
			if (validCatalog)
				memcpy(&subc_info[9], catalog.data(), 13);
			//22 ZERO, 23 absolute frame within the current second (binary)
			subc_info[22] = 0;
			subc_info[23] = getCurrentFad(fad) % 75;
			DEBUG_LOG(GDROM, "gd_get_subcode: format 2 (Media catalog number). audio %x", subc_info[1]);
		}
		break;

	case 3:	// International standard recording code (ISRC)
		{
			subc_info[2] = 0;
			subc_info[3] = 0x18;
			subc_info[4] = 3;
			memset(&subc_info[5], 0, 4);
			u32 elapsed;
			const u32 track = libGDR_GetTrackNumber(getCurrentFad(fad), elapsed);
			const std::string isrc = libGDR_GetTrackIsrc(track);
			subc_info[8] = encodeIsrc(isrc, &subc_info[9]) ? 0x80 : 0; // TCVal
			subc_info[21] = 0;
			subc_info[22] = getCurrentFad(fad) % 75;
			subc_info[23] = 0;
		}
		break;
	}
	return subc_info[3];
}

void gd_subcode_status_transferred(u32 bytes)
{
	// Audio status is byte 1. Terminal status is reported only once, and only
	// after that byte was actually delivered to the host.
	if (bytes > 1 && (cdda.status == cdda_t::Terminated || cdda.status == cdda_t::Failed))
		cdda_completion_reported = true;
}

static bool packetBytesZero(u32 first, u32 last)
{
	for (u32 i = first; i <= last; i++)
		if (packet_cmd.data_8[i] != 0)
			return false;
	return true;
}

static bool packetReservedFieldsValid()
{
	const u8 *p = packet_cmd.data_8;
	switch (p[0])
	{
	case SPI_TEST_UNIT:
	case SPI_CD_OPEN:
		return packetBytesZero(1, 11);
	case SPI_REQ_STAT:
	case SPI_REQ_MODE:
	case SPI_SET_MODE:
	case SPI_REQ_SES:
		return p[1] == 0 && p[3] == 0 && packetBytesZero(5, 11);
	case SPI_REQ_ERROR:
		return packetBytesZero(1, 3) && packetBytesZero(5, 11);
	case SPI_GET_TOC:
		return (p[1] & 0xfe) == 0 && p[2] == 0 && packetBytesZero(5, 11);
	case SPI_CD_PLAY:
		if ((p[1] & 0xf8) != 0 || p[5] != 0 || (p[6] & 0xf0) != 0 || p[7] != 0 || p[11] != 0)
			return false;
		return (p[1] & 7) != 7 || (packetBytesZero(2, 6) && packetBytesZero(8, 10));
	case SPI_CD_SEEK:
		if ((p[1] & 0xf8) != 0 || !packetBytesZero(5, 11))
			return false;
		return ((p[1] & 7) != 3 && (p[1] & 7) != 4) || packetBytesZero(2, 4);
	case SPI_CD_SCAN:
		return p[1] == 0 && (p[2] & 0xfe) == 0 && packetBytesZero(4, 11);
	case SPI_CD_READ:
		return packetBytesZero(5, 7) && p[11] == 0;
	case SPI_CD_READ2:
		return p[5] == 0 && p[11] == 0;
	case SPI_GET_SCD:
		return (p[1] & 0xf0) == 0 && p[2] == 0 && packetBytesZero(5, 11);
	default:
		return true; // Vendor-specific commands have undocumented packet fields.
	}
}

static void gd_process_spi_cmd()
{

	printf_spi("Sense: %02x %02x %02x", sns_asc, sns_ascq, sns_key);

	printf_spi("SPI command %02x;",packet_cmd.data_8[0]);
	printf_spi("Params: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
		packet_cmd.data_8[0], packet_cmd.data_8[1], packet_cmd.data_8[2], packet_cmd.data_8[3], packet_cmd.data_8[4], packet_cmd.data_8[5],
		packet_cmd.data_8[6], packet_cmd.data_8[7], packet_cmd.data_8[8], packet_cmd.data_8[9], packet_cmd.data_8[10], packet_cmd.data_8[11] );

	// REQ_ERROR retrieves the previous command's sense data. Issuing any
	// other command clears it before that command is processed. Unit
	// attention is the exception: it must be reported until the host reads it.
	if (packet_cmd.data_8[0] != SPI_REQ_ERROR)
	{
		if (sns_key == 6)
		{
			Error.Sense = sns_key;
			GDStatus.CHECK = 1;
			gd_set_state(gds_procpacketdone);
			return;
		}
		sns_key = 0;
		sns_asc = 0;
		sns_ascq = 0;
		sns_fad = 0;
		Error.Sense = 0;
		GDStatus.CHECK = 0;
	}
	if (!packetReservedFieldsValid())
	{
		gd_spi_error(5, 0x24); // ILLEGAL REQUEST: invalid command field
		return;
	}

	switch(packet_cmd.data_8[0])
	{
	case SPI_TEST_UNIT:
		printf_spicmd("SPI_TEST_UNIT");
		if (gd_media_ready())
			gd_set_state(gds_procpacketdone);
		break;

	case SPI_REQ_MODE:
		if ((packet_cmd.data_8[2] & 1) != 0 || packet_cmd.data_8[2] >= sizeof(GD_HardwareInfo))
		{
			gd_spi_error(5, 0x21); // ILLEGAL REQUEST: invalid address
			break;
		}
		printf_spicmd("SPI_REQ_MODE cd-rom speed %d flags %x retry %x", GD_HardwareInfo.speed, GD_HardwareInfo.read_flags, GD_HardwareInfo.read_retry);
		{
			const u32 offset = packet_cmd.data_8[2];
			const u32 count = offset < sizeof(GD_HardwareInfo)
					? std::min<u32>(packet_cmd.data_8[4], sizeof(GD_HardwareInfo) - offset)
					: 0;
			gd_spi_pio_end((u8 *)&GD_HardwareInfo + std::min<u32>(offset, sizeof(GD_HardwareInfo)), count);
		}
		break;

		/////////////////////////////////////////////////
		// *FIXME* CHECK FOR DMA, Diff Settings !?!@$#!@%
	case SPI_CD_READ:
	case SPI_CD_READ2:
		{
#define readcmd packet_cmd.GDReadBlock
			if (!gd_media_ready())
				break;

			if (readcmd.expdtype == 7 || (readcmd.prmtype && !validMSF(&readcmd.b[2])))
			{
				gd_spi_error(5, 0x24); // ILLEGAL REQUEST: invalid command field
				break;
			}
			const u32 startFad = GetFAD(&readcmd.b[2], readcmd.prmtype);
			const u32 leadOutFad = getLeadOutFad();
			if (startFad < 150 || startFad >= leadOutFad)
			{
				gd_spi_error(5, 0x21, 0, startFad); // ILLEGAL REQUEST: invalid address
				break;
			}
			u32 nextFad = 0;
			if (packet_cmd.data_8[0] == SPI_CD_READ2)
			{
				if (readcmd.prmtype && !validMSF(&readcmd.b[8]))
				{
					gd_spi_error(5, 0x24); // ILLEGAL REQUEST: invalid command field
					break;
				}
				nextFad = GetFAD(&readcmd.b[8], readcmd.prmtype);
				if (nextFad < 150 || nextFad >= leadOutFad)
				{
					gd_spi_error(5, 0x21, 0, nextFad); // ILLEGAL REQUEST: invalid address
					break;
				}
			}
			if (prefetch_error_fad == startFad)
			{
				prefetch_error_fad = 0;
				gd_spi_error(3, 0x11, 0, startFad);
				break;
			}
			if (!validateSectorType(startFad, readcmd.expdtype))
				break;

			cdda.status = cdda_t::NoInfo;
			cdda.EndAddr.FAD = 0;
			cdda.repeats = 0;
			cdda_scan_step = 0;
			u32 sector_type;
			if (!getReadSectorSize(startFad, readcmd.expdtype, readcmd.head, readcmd.subh,
					readcmd.data, readcmd.other, sector_type))
				break;

			read_params.start_sector = startFad;
			if (packet_cmd.data_8[0] == SPI_CD_READ)
				read_params.remaining_sectors = (readcmd.b[8] << 16) | (readcmd.b[9] << 8) | (readcmd.b[10]);
			else
				read_params.remaining_sectors = (readcmd.b[6] << 8) | readcmd.b[7];
			read_params.sector_type = sector_type;//yeah i know , not really many types supported...
			read_params.sector_offset = 0;
			pio_sector_cached = false;
			read_params.expected_type = readcmd.expdtype;
			read_params.next_sector = nextFad;
			read_params.data_select = readcmd.head << 3 | readcmd.subh << 2
					| readcmd.data << 1 | readcmd.other;

			printf_spicmd("SPI_CD_READ - Sector=%d Size=%d/%d DMA=%d",read_params.start_sector,read_params.remaining_sectors,read_params.sector_type,Features.CDRead.DMA);
			if (read_params.remaining_sectors == 0) {
				dma_buff.clear();
				pio_buff.clear();
				completeReadAhead();
				gd_set_state(gds_procpacketdone);
			}
			else if (Features.CDRead.DMA == 1) {
				pio_buff.clear();
				gd_set_state(gds_readsector_dma);
			}
			else {
				dma_buff.clear();
				gd_set_state(gds_readsector_pio);
			}
		}
		break;

	case SPI_GET_TOC:
		{
			printf_spicmd("SPI_GET_TOC");
			if (!gd_media_ready())
				break;
			const DiskArea area = (DiskArea)(packet_cmd.data_8[1] & 1);
			if (area == DoubleDensity && libGDR_GetDiscType() != GdRom)
			{
				gd_spi_error(5, 0x2c, 0x01); // Command not supported by inserted media
				break;
			}
			//printf("SPI_GET_TOC - %d\n",(packet_cmd.data_8[4]) | (packet_cmd.data_8[3]<<8) );
			u32 toc_gd[102];
			
			//toc - dd/sd
			libGDR_GetToc(&toc_gd[0], area);
			 
			gd_spi_pio_end((u8*)&toc_gd[0], std::min((u32)packet_cmd.data_8[4] | (packet_cmd.data_8[3] << 8), (u32)sizeof(toc_gd)));
		}
		break;

		//mount/map drive ? some kind of reset/unlock ??
		//seems like a non data command :)
	case 0x70:
		printf_spicmd("SPI : unknown ? [0x70]");
		//printf("SPI : unknown ? [0x70]\n");
		/*GDStatus.full=0x50; //FIXME
		RaiseInterrupt(holly_GDROM_CMD);*/

		gd_set_state(gds_procpacketdone);
		break;


	// Command 71 seems to trigger some sort of authentication check(?).
	// Update Sept 1st 2010: It looks like after a sequence of events the drive ends up having a specific state.
	// If the drive is fed with a "bootable" disc it ends up in "PAUSE" state. On all other cases it ends up in "STANDBY".
	// Cmd 70 and Error Handling / Sense also seem to take part in the above mentioned sequence of events.
	// This is more or less a hack until more info about this command becomes available. ~Psy
	case 0x71:
		{
			printf_spicmd("SPI : unknown ? [0x71]");
			//printf("SPI : unknown ? [0x71]\n");
			extern const u32 reply_71_sz;

			gd_spi_pio_end((const u8*)&reply_71[0], reply_71_sz);//uCount


			if (libGDR_GetDiscType()==GdRom || libGDR_GetDiscType()==CdRom_XA)
			{
				SecNumber.Status=GD_PAUSE;
				gd_schedule_standby();
			}
			else
				SecNumber.Status=GD_STANDBY;
		}
		break;
	case SPI_SET_MODE:
		{
			printf_spicmd("SPI_SET_MODE");
			const u32 Offset = packet_cmd.data_8[2];
			if ((Offset & 1) != 0 || Offset >= 10)
			{
				gd_spi_error(5, 0x21); // ILLEGAL REQUEST: invalid address
				break;
			}
			const u32 Count = Offset < 10 ? std::min<u32>(packet_cmd.data_8[4], 10 - Offset) : 0;	// limit to writable area
			set_mode_offset = std::min<u32>(Offset, 10);
			gd_spi_pio_read_end(Count,gds_process_set_mode);
		}

		break;
		
	case SPI_REQ_STAT:
		{
			const u32 offset = packet_cmd.data_8[2];
			if ((offset & 1) != 0 || offset >= 10)
			{
				gd_spi_error(5, 0x21); // ILLEGAL REQUEST: invalid address
				break;
			}
			u32 curFad = getCurrentFad();
			printf_spicmd("SPI_REQ_STAT: status %d fmt %d fad %d", SecNumber.Status, SecNumber.DiscFormat, curFad);
			u32 elapsed;
			u32 tracknum = libGDR_GetTrackNumber(curFad, elapsed);
			u8 stat[10] {};

			//0  0   0   0   0   STATUS
			stat[0] = SecNumber.Status;   //low nibble
			//1 Disc Format Repeat Count
			stat[1] = (u8)(SecNumber.DiscFormat << 4) | cdda.repeats;
			//2 Control ADR
			u8 adr, ctrl;
			libGDR_GetTrackAdrAndControlAtFad(curFad, adr, ctrl);
			stat[2] = (ctrl << 4) | adr;
			//3 Track #
			stat[3] = tracknum;
			//4 Index #
			stat[4] = 1;
			//5 FAD
			stat[5] = curFad >> 16;
			//6 FAD
			stat[6] = curFad >> 8;
			//7 FAD
			stat[7] = curFad;
			//8 Max Read Error Retry Times
			stat[8] = max_read_retries;
			//9 0   0   0   0   0   0   0   0
			stat[9] = 0;

			
			const u32 count = offset < sizeof(stat)
					? std::min<u32>(packet_cmd.data_8[4], sizeof(stat) - offset)
					: 0;
			const u32 transferCount = std::min(count, pio_data_limit);
			gd_spi_pio_end(&stat[std::min<u32>(offset, sizeof(stat))], transferCount);
			if (offset <= 8 && offset + transferCount > 8)
				max_read_retries = 0;
		}
		break;

	case SPI_REQ_ERROR:
		{
			printf_spicmd("SPI_REQ_ERROR");
			u8 resp[10] {};
			resp[0]=0xF0;
			resp[1]=0;
			resp[2]=sns_key;//sense
			resp[3]=0;
			resp[4] = 0;
			resp[5] = sns_fad >> 16;
			resp[6] = sns_fad >> 8;
			resp[7] = sns_fad; // FAD where the error occurred
			resp[8]=sns_asc;//Additional Sense Code
			resp[9]=sns_ascq;//Additional Sense Code Qualifier

			gd_spi_pio_end(resp, std::min<u32>(packet_cmd.data_8[4], sizeof(resp)));
			sns_key = 0;
			sns_asc = 0;
			sns_ascq = 0;
			sns_fad = 0;
			Error.Sense = 0;
			GDStatus.CHECK = 0;
		}
		break;

	case SPI_REQ_SES:
		{
			printf_spicmd("SPI_REQ_SES: status %d", SecNumber.Status);
			if (!gd_media_ready())
				break;
			const u32 requestedSession = packet_cmd.data_8[2];
			u8 summary[6] {};
			libGDR_GetSessionInfo(summary, 0);
			if (requestedSession > 99
					|| (requestedSession != 0 && requestedSession > summary[2]))
			{
				gd_spi_error(5, 0x26); // ILLEGAL REQUEST: invalid session number
				break;
			}

			u8 ses_inf[6] {};
			libGDR_GetSessionInfo(ses_inf, requestedSession);
			ses_inf[0]=SecNumber.Status;
			gd_spi_pio_end(ses_inf, std::min<u32>(packet_cmd.data_8[4], sizeof(ses_inf)));
		}
		break;

	case SPI_CD_OPEN:
		printf_spicmd("SPI_CD_OPEN");
		cdda.status = cdda_t::NoInfo;
		cdda.EndAddr.FAD = 0;
		cdda.repeats = 0;
		cdda_scan_step = 0;
		if (SecNumber.Status == GD_OPEN)
			gd_set_state(gds_procpacketdone);
		else
			// The Dreamcast has a manual top-loading lid. The command remains
			// busy until the frontend reports that the lid was opened.
			gd_set_state(gds_open_wait);
		break;

	case SPI_CD_PLAY:
		{
			if (!gd_media_ready())
				break;
			const u32 param_type = packet_cmd.data_8[1] & 7;
			printf_spicmd("SPI_CD_PLAY param_type=%d", param_type);

			if (param_type == 1 || param_type == 2)
			{
				bool min_sec_frame = param_type == 2;
				if (min_sec_frame && (!validMSF(&packet_cmd.data_8[2]) || !validMSF(&packet_cmd.data_8[8])))
				{
					gd_spi_error(5, 0x24); // ILLEGAL REQUEST: invalid command field
					break;
				}
				u32 startFad = GetFAD(&packet_cmd.data_8[2], min_sec_frame);
				u32 endFad = GetFAD(&packet_cmd.data_8[8], min_sec_frame);
				const u32 leadOutFad = getLeadOutFad();
				if (startFad < 150 || startFad >= leadOutFad || !gd_normalize_playback_start(startFad)
						|| (endFad != 0 && endFad <= startFad))
				{
					gd_spi_error(5, 0x21, 0, startFad); // ILLEGAL REQUEST: invalid address
					break;
				}
				const u32 areaEndFad = gd_get_playback_area_end(startFad);
				const bool endsAtLeadOut = endFad == 0 || endFad >= areaEndFad;
				if (endFad == 0)
					endFad = areaEndFad;
				else
					endFad = std::min(endFad, areaEndFad);
				cdda.CurrAddr.FAD = cdda.StartAddr.FAD = startFad;
				cdda.EndAddr.FAD = endFad;
				// Reaching lead-out always terminates after the first pass, even
				// when a repeat count was supplied.
				cdda.repeats = endsAtLeadOut ? 0 : packet_cmd.data_8[6] & 0xF;
				cdda.status = cdda_t::Playing;
				cdda_completion_reported = false;
				cdda_scan_step = 0;
				SecNumber.Status = GD_PLAY;

				GDStatus.DSC = 1;
			}
			else if (param_type == 7)
			{
				if (cdda.status == cdda_t::Paused || cdda.status == cdda_t::Scanning)
				{
					if (cdda.StartAddr.FAD < 150 || cdda.CurrAddr.FAD >= cdda.EndAddr.FAD)
					{
						gd_spi_error(5, 0x26); // No preceding playback range
						break;
					}
					cdda.status = cdda_t::Playing;
					SecNumber.Status = GD_PLAY;
				}
				else if (cdda.status != cdda_t::Playing)
				{
					gd_spi_error(5, 0x26); // ILLEGAL REQUEST: invalid parameter
					break;
				}
			}
			else
			{
				gd_spi_error(5, 0x24); // ILLEGAL REQUEST: invalid command field
				break;
			}

			DEBUG_LOG(GDROM, "CDDA StartAddr=%d EndAddr=%d repeats=%d status=%d CurrAddr=%d",cdda.StartAddr.FAD,
					cdda.EndAddr.FAD, cdda.repeats, cdda.status, cdda.CurrAddr.FAD);

			gd_set_state(gds_procpacketdone);
		}
		break;

	case SPI_CD_SEEK:
		{
			if (!gd_media_ready())
				break;
			const u32 param_type = packet_cmd.data_8[1] & 7;
			printf_spicmd("SPI_CD_SEEK param_type=%d", param_type);

			if (param_type == 1 || param_type == 2)
			{
				bool min_sec_frame = param_type == 2;
				if (min_sec_frame && !validMSF(&packet_cmd.data_8[2]))
				{
					gd_spi_error(5, 0x24); // ILLEGAL REQUEST: invalid command field
					break;
				}
				const u32 seekFad = GetFAD(&packet_cmd.data_8[2], min_sec_frame);
				if (seekFad < 150 || seekFad >= getLeadOutFad())
				{
					gd_spi_error(5, 0x21, 0, seekFad); // ILLEGAL REQUEST: invalid address
					break;
				}
				cdda.StartAddr.FAD = cdda.CurrAddr.FAD = seekFad;
				cdda.EndAddr.FAD = 0;
				cdda.repeats = 0;
				// Positioning the head invalidates audio status. This is distinct
				// from pausing an active CDDA playback operation.
				cdda.status = cdda_t::Seeked;
				cdda_scan_step = 0;
				SecNumber.Status = GD_SEEK;
				GDStatus.DSC = 0;
				sh4_sched_request(gdrom_schid, SH4_MAIN_CLOCK / 50);	// 20 ms
			}
			else if (param_type == 3)
			{
				//stop audio , goto home
				cdda.StartAddr.FAD = cdda.CurrAddr.FAD = 150;
				cdda.EndAddr.FAD = 0;
				cdda.repeats = 0;
				cdda.status = cdda_t::NoInfo;
				cdda_scan_step = 0;
				SecNumber.Status = GD_BUSY;
				GDStatus.DSC = 0;
				sh4_sched_request(gdrom_schid, SH4_MAIN_CLOCK / 50);	// 20 ms
			}
			else if (param_type == 4)
			{
				gd_pause_cdda();
			}
			else
			{
				gd_spi_error(5, 0x24); // ILLEGAL REQUEST: invalid command field
				break;
			}

			DEBUG_LOG(GDROM, "CDDA StartAddr=%d EndAddr=%d repeats=%d status=%d CurrAddr=%d",cdda.StartAddr.FAD,
					cdda.EndAddr.FAD, cdda.repeats, cdda.status, cdda.CurrAddr.FAD);

			gd_set_state(gds_procpacketdone);
		}
		break;

	case SPI_CD_SCAN:
		{
			printf_spicmd("SPI_CD_SCAN");
			if (!gd_media_ready())
				break;
			if (cdda.status != cdda_t::Playing && cdda.status != cdda_t::Paused
					&& cdda.status != cdda_t::Scanning)
			{
				gd_spi_error(5, 0x26); // ILLEGAL REQUEST: invalid parameter
				break;
			}
			if (cdda.StartAddr.FAD < 150 || cdda.CurrAddr.FAD >= cdda.EndAddr.FAD)
			{
				gd_spi_error(5, 0x26); // ILLEGAL REQUEST: invalid parameter
				break;
			}

			gd_start_cdda_scan((packet_cmd.data_8[2] & 1) != 0, packet_cmd.data_8[3]);
			gd_set_state(gds_procpacketdone);
		}
		break;

	case SPI_GET_SCD:
		{
			printf_spicmd("SPI_GET_SCD");
			if (!gd_media_ready())
				break;

			const u32 format = packet_cmd.data_8[1] & 0xF;
			if ((packet_cmd.data_8[1] & 0xf0) != 0 || format > 3)
			{
				gd_spi_error(5, 0x24); // ILLEGAL REQUEST: invalid command field
				break;
			}
			const u32 alloc_len = (packet_cmd.data_8[3] << 8) | packet_cmd.data_8[4];
			u8 subc_info[100] {};
			u32 size = gd_get_subcode(format, getCurrentFad(), subc_info);
			const u32 transferSize = std::min(std::min(std::min(size, alloc_len),
					(u32)sizeof(subc_info)), pio_data_limit);
			gd_spi_pio_end(subc_info, transferSize);
			gd_subcode_status_transferred(transferSize);
		}
		break;

	default:
		INFO_LOG(GDROM, "GDROM: Unhandled Sega SPI frame: %X", packet_cmd.data_8[0]);
		gd_spi_error(5, 0x20); // ILLEGAL REQUEST: unsupported command
		break;
	}
}
//Read handler
u32 ReadMem_gdrom(u32 Addr, u32 sz)
{	
	switch (Addr)
	{
		//cancel interrupt
	case GD_STATUS_Read :
		asic_CancelInterrupt(holly_GDROM_CMD);	//Clear INTRQ signal
		command_interrupt_pending = false;
		printf_rm("GDROM: STATUS [cancel int](v=%X)",GDStatus.full);
		return GDStatus.full;

	case GD_ALTSTAT_Read:
		//printf_rm("GDROM: AltStatus (v=%X)",GDStatus.full);
		return GDStatus.full;

	case GD_BYCTLLO	:
		printf_rm("GDROM: Read From GD_BYCTLLO");
		return ByteCount.low;

	case GD_BYCTLHI	:
		printf_rm("GDROM: Read From GD_BYCTLHI");
		return ByteCount.hi;

	case GD_DATA:
		if(2!=sz)
		{
			INFO_LOG(GDROM, "GDROM: Bad size on DATA REG Read");
			return 0;
		}
		if (gd_state != gds_pio_send_data)
		{
			INFO_LOG(GDROM, "GDROM: Illegal Read From DATA");
			return 0;
		}
			if (pio_buff.atEnd())
			{
				INFO_LOG(GDROM, "GDROM: Illegal Read From DATA (underflow)");
			}
			else
			{
				const u32 transferSize = std::min<u32>(sizeof(u16), pio_buff.getRemaining());
				u32 rv = pio_buff.read();
				ByteCount.full -= transferSize;
				if (pio_buff.atEnd())
				{
					verify(pio_buff.next_state != gds_pio_send_data);
					//end of pio transfer !
					gd_set_state(pio_buff.next_state);
				}
				return rv;
			}
		return 0;

	case GD_DRVSEL:
		printf_rm("GDROM: Read From DriveSel");
		return DriveSel;

	case GD_ERROR_Read:
		printf_rm("GDROM: Read from ERROR Register");
		return Error.full;

	case GD_IREASON_Read:
		printf_rm("GDROM: Read from INTREASON Register");
		return IntReason.full;

	case GD_SECTNUM:
//		printf_rm("GDROM: Read from SecNumber Register (v=%X)", SecNumber.full);
		return SecNumber.full;

	default:
		INFO_LOG(GDROM, "GDROM: Unhandled read from address %X, Size:%X",Addr,sz);
		return 0;
	}
}

//Write Handler
void WriteMem_gdrom(u32 Addr, u32 data, u32 sz)
{
	switch(Addr)
	{
	//ATA_IOPORT_WR_CYLINDER_LOW
	case GD_BYCTLLO:
		printf_rm("GDROM: Write to GD_BYCTLLO = %X, Size:%X",data,sz);
		ByteCount.low =(u8) data;
		break;

	//ATA_IOPORT_WR_CYLINDER_HIGH
	case GD_BYCTLHI: 
		printf_rm("GDROM: Write to GD_BYCTLHI = %X, Size:%X",data,sz);
		ByteCount.hi =(u8) data;
		break;

	case GD_DATA: 
		{
			if(2!=sz)
			{
				INFO_LOG(GDROM, "GDROM: Bad size on DATA REG");
				return;
			}
			if (gd_state == gds_waitpacket)
			{
				packet_cmd.data_16[packet_cmd.index]=(u16)data;
				packet_cmd.index+=1;
				if (packet_cmd.index==6)
					gd_set_state(gds_procpacket);
			}
			else if (gd_state == gds_pio_get_data)
			{
				const u32 transferSize = std::min<u32>(sizeof(u16), pio_buff.getRemaining());
				pio_buff.write((u16)data);
				ByteCount.full -= transferSize;
				if (pio_buff.atEnd())
				{
					verify(pio_buff.next_state != gds_pio_get_data);
					gd_set_state(pio_buff.next_state);
				}
			}
			else
			{
				INFO_LOG(GDROM, "GDROM: Illegal Write to DATA");
			}
			return;
		}

	case GD_DEVCTRL_Write:
		{
			const bool interruptsWereDisabled = (DeviceControl & 2) != 0;
			DeviceControl = data & 6;
			const bool interruptsDisabled = (DeviceControl & 2) != 0;
			if (interruptsDisabled)
				asic_CancelInterrupt(holly_GDROM_CMD);
			else if (interruptsWereDisabled && command_interrupt_pending)
				asic_RaiseInterrupt(holly_GDROM_CMD);
		}
		break;

	//ATA_IOPORT_WR_DEVICE_HEAD
	case GD_DRVSEL: 
		// Sega's task file uses bits 0-3 as a logical-unit number. Bit 4 is
		// fixed at zero; it is not the ATA master/slave selector on this device.
		DriveSel = 0xa0 | (data & 0x0f);
		break;

		// By writing "3" as Feature Number and issuing the Set Feature command,
		// the PIO or DMA transfer mode set in the Sector Count register can be selected.
		// The actual transfer mode is specified by the Sector Counter Register. 

	case GD_FEATURES_Write:
		printf_rm("GDROM: Write to GD_FEATURES");
		Features.full =(u8) data;
		break;

	case GD_SECTCNT_Write:
		DEBUG_LOG(GDROM, "GDROM: Write to SecCount = %X", data);
		SecCount.full =(u8) data;
		break;

	case GD_SECTNUM:
		INFO_LOG(GDROM, "GDROM: Write to SecNum; not possible = %X", data);
		break;

	case GD_COMMAND_Write:
		if (sz != 1)
			INFO_LOG(GDROM, "GDROM: Bad size on GD_COMMAND reg");
		else
		{
			if (data != ATA_NOP && data != ATA_SOFT_RESET && gd_state != gds_waitcmd)
			{
				const u32 errorFad = getCurrentFad();
				gd_abort_transfer();
				gd_spi_error(4, 0x4e, 0, errorFad); // New command while one is executing.
				break;
			}
			ata_command = (u8)data;
			gd_set_state(gds_procata);
		}
		break;

	default:
		INFO_LOG(GDROM, "GDROM: Unhandled write to address %X <= %X, Size:%X",Addr,data,sz);
		break;
	}
}

static constexpr u32 G1DmaUnit = 32;

static constexpr u32 getG1DmaCycles(u32 timing, u32 bytes, bool firstUnit)
{
	// G1 is clocked at 50 MHz. The manual defines a transfer as
	// (B + C) + (A + D) * words - A; subsequent units in the same burst
	// omit the initial B/C setup and the compensating -A.
	const u32 a = ((timing >> 12) & 0xf) + 3;
	const u32 b = ((timing >> 8) & 0xf) + 3;
	const u32 c = ((timing >> 4) & 0xf) + 1;
	const u32 d = (timing & 0xf) + 1;
	const u32 words = (bytes + 1) / 2;
	const u32 g1Cycles = (a + d) * words + (firstUnit ? b + c - a : 0);
	return g1Cycles * (SH4_MAIN_CLOCK / 50'000'000);
}
static_assert(getG1DmaCycles(0xffff, 2, true) == 200);
static_assert(getG1DmaCycles(0xffff, 32, true) == 2240);
static_assert(getG1DmaCycles(0xffff, 32, false) == 2176);

static int getGDROMTicks()
{
	if (SB_GDST & 1)
	{
		const u32 remaining = dma_transfer_length - SB_GDLEND;
		const u32 bytes = std::min(remaining, G1DmaUnit);
		const u32 busCycles = getG1DmaCycles(dma_access_timing, bytes, SB_GDLEND == 0);
		if (dma_direction == 0)
			return busCycles;
		// Large transfers are paced by the optical medium. Once data is buffered,
		// the programmed G1 bus timing remains the limiting interface rate.
		// Fast-load mode deliberately bypasses physical pacing.
		const u32 rate = config::FastGDRomLoad ? 4'000'000'000u
				: gdromReadRate(GD_HardwareInfo.speed);
		if (dma_cycle_rate != rate) {
			dma_cycle_rate = rate;
			dma_cycle_remainder = 0;
		}
		const u64 numerator = (u64)SH4_MAIN_CLOCK * bytes + dma_cycle_remainder;
		dma_cycle_remainder = numerator % rate;
		const u32 pacedCycles = std::max<u32>(1, numerator / rate);
		if (config::FastGDRomLoad)
			return pacedCycles;
		return remaining > 10240 ? std::max(pacedCycles, busCycles) : busCycles;
	}
	else
		return 0;
}

//is this needed ?
static int GDRomschd(int tag, int cycles, int jitter, void *arg)
{
	const u32 dispatchJitter = std::max(0, jitter);
	const u64 overdueCycles = (u64)dma_jitter_carry + dispatchJitter;
	dma_jitter_carry = 0;
	if (SecNumber.Status == GD_SEEK)
	{
		SecNumber.Status = GD_PAUSE;
		gd_schedule_standby();
		GDStatus.DSC = 1;
	}
	else if (SecNumber.Status == GD_BUSY)
	{
		SecNumber.Status = GD_STANDBY;
		GDStatus.DSC = 1;
	}
	if (!(SB_GDST & 1) || !(SB_GDEN & 1))
		return 0;

	u32 src = SB_GDSTARD;
	const u32 remaining = dma_transfer_length - SB_GDLEND;
	u32 len = remaining;
	
	len = std::min(len, G1DmaUnit);
	const u32 addressBytesRemaining = src < dma_address_limit ? dma_address_limit - src : 0;
	const bool addressOverrun = len > addressBytesRemaining;
	len = std::min(len, addressBytesRemaining);
	const u32 transferWindow = len;
	// do we need to do this for GDROM DMA?
	if (0x8201 != (DMAC_DMAOR.full & DMAOR_MASK))
	{
		INFO_LOG(GDROM, "GDROM: DMAOR has invalid settings (%X)", DMAC_DMAOR.full);
		//return;
	}

	if(len == 0)
	{
		INFO_LOG(GDROM, "GDROM: Len: %X, Abnormal Termination !", len);
	}

	u32 transferred = 0;
	bool readError = false;
	if (dma_direction == 1)
	{
		while (len)
		{
			if (dma_buff.isEmpty() && read_params.remaining_sectors == 0)
				break;
			if (!dma_buff.fill(read_params))
			{
				if (gd_state == gds_readsector_dma)
					gd_spi_error(3, 0x11); // MEDIUM ERROR: unrecoverable read error
				readError = true;
				break;
			}
			// transfer up to len bytes
			const u32 buff_size = std::min(dma_buff.getSize(), len);
			if (buff_size == 0)
				break;

			WriteMemBlock_nommu_ptr(src, (const u32 *)dma_buff.read(buff_size), buff_size);
			src += buff_size;
			len -= buff_size;
			transferred += buff_size;
		}
	}
	else
	{
		// Direction 0 is a valid transfer to the GD-ROM interface. No standard
		// SPI command consumes DMA output data, but the root-bus reads, counters,
		// timing and completion interrupt remain externally visible.
		while (len)
		{
			ReadMem8_nommu(src++);
			len--;
			transferred++;
		}
	}

	const bool payloadComplete = dma_direction == 1
			&& read_params.remaining_sectors == 0 && dma_buff.isEmpty();
	const bool programmedLengthReached = (u64)SB_GDLEND + transferred >= dma_transfer_length;
	// A programmed length or drive payload ending mid-burst still occupies a
	// complete root-bus unit.
	const bool finalBusUnit = transferred != 0 && (programmedLengthReached || payloadComplete);
	const u32 busAdvance = finalBusUnit
			? gdromDmaBusLength(transferred) : transferred;
	const bool dmaComplete = (u64)SB_GDLEND + busAdvance >= dma_transfer_length;
	// GD-DMA always performs complete 32-byte root-bus transactions. When the
	// transfer ends within the final unit, the architecture requires the unused
	// destination bytes to be padded with zeroes.
	if (dma_direction == 1 && finalBusUnit && !readError && busAdvance > transferred)
	{
		const u32 padding = busAdvance - transferred;
		verify(addressBytesRemaining >= busAdvance);
		static const u32 zeroes[G1DmaUnit / sizeof(u32)] {};
		WriteMemBlock_nommu_ptr(src, zeroes, padding);
	}
	SB_GDLEND = (SB_GDLEND + busAdvance) & 0x01ffffe0;
	SB_GDSTARD = (SB_GDSTARD + busAdvance) & 0x1fffffe0;
	if (addressOverrun && transferred == transferWindow && !readError)
	{
		SB_GDST = 0;
		SB_GDEN = 0;
		dma_jitter_carry = 0;
		asic_RaiseInterrupt(holly_G1_OVERRUN);
		return 0;
	}

	if (dmaComplete)
	{
		SB_GDST = 0;
		asic_RaiseInterrupt(holly_GDROM_DMA);
	}
	else if (readError)
	{
		// A drive failure reports through the GD-ROM command interrupt. It is
		// not a normal G1 DMA completion, must not assert the end-of-DMA
		// interrupt, and leaves G1 waiting for software to disable it.
		return 0;
	}
	// Read ALL sectors and all buffer
	if (payloadComplete)
	{
		//verify(!SB_GDST&1) -> dc can do multi read dma
		completeReadAhead();
		gd_set_state(gds_procpacketdone);
		// If GDLEN exceeds the rounded drive payload, G1 remains started but no
		// further DMARQ can arrive. Software must terminate it through GDEN.
		if (!dmaComplete)
			return 0;
	}

	const u32 nextTicks = getGDROMTicks();
	if (nextTicks == 0)
		return 0;
	u32 delay;
	if (overdueCycles >= nextTicks) {
		dma_jitter_carry = std::min<u64>(overdueCycles - nextTicks,
				std::numeric_limits<u32>::max());
		delay = 0;
	}
	else
		delay = nextTicks - overdueCycles;
	// The scheduler subtracts this callback's jitter from the returned delay.
	// Add it back because delay already accounts for all overdue time, including
	// residual lateness carried from an earlier zero-deadline callback.
	return std::min<u64>((u64)delay + dispatchJitter, std::numeric_limits<int>::max());
}

//DMA Start
static void GDROM_DmaStart(u32 addr, u32 data)
{
	if ((data & 1) == 0 || SB_GDST != 0)
		return;
	if (SB_GDEN == 0)
	{
		INFO_LOG(GDROM, "Invalid GD-DMA start, SB_GDEN=0. Ignoring it.");
		return;
	}
	u32 addressLimit;
	if (!dma_address_valid || !dmaAddressProtected(SB_GDSTAR, addressLimit))
	{
		SB_GDEN = 0;
		asic_RaiseInterrupt(!dma_address_valid || getDmaAddressLimit(SB_GDSTAR) == 0
				? holly_G1_ILLADDR : holly_G1_OVERRUN);
		return;
	}
	SB_GDST = 1;
	SB_GDSTARD = SB_GDSTAR;
	SB_GDLEND = 0;
	// The G1 interface samples these registers at start. The programmed
	// length is byte-granular, but the bus transaction is padded to 32 bytes;
	// zero denotes the maximum 32 MiB transfer.
	dma_transfer_length = getDmaTransferLength(SB_GDLEN);
	dma_direction = SB_GDDIR;
	dma_address_limit = addressLimit;
	dma_cycle_remainder = 0;
	dma_cycle_rate = 0;
	dma_access_timing = dma_direction == 0 ? SB_G1GDWC : SB_G1GDRC;
	dma_jitter_carry = 0;
	DEBUG_LOG(GDROM, "GDROM-DMA start addr %08X len %d fad %x", SB_GDSTAR, SB_GDLEN, read_params.start_sector);

	// Even transfers shorter than one CPU timeslice must not become visible
	// during the write that starts DMA. The scheduler retains the requested
	// deadline and reports any dispatch jitter to the callback.
	const int ticks = getGDROMTicks();
	if (ticks)
		sh4_sched_request(gdrom_schid, ticks);
}

static void GDROM_DmaEnable(u32 addr, u32 data)
{
	SB_GDEN = data & 1;
	if (SB_GDEN == 0 && SB_GDST == 1)
	{
		printf_spi("GD-DMA aborted");
		SB_GDST = 0;
		dma_jitter_carry = 0;
		sh4_sched_request(gdrom_schid, -1);
	}
}

//Init/Term/Res
void gdrom_reg_Init()
{
	hollyRegs.setWriteHandler<SB_GDSTAR_addr>(GDROM_DmaAddressWrite);
	hollyRegs.setWriteOnly<SB_GDAPRO_addr>(GDROM_DmaProtectionWrite);
	gdrom_schid = sh4_sched_register(0, &GDRomschd);
	standby_schid = sh4_sched_register(0, &standbyCallback);
	gd_disc_change();
	const DiscType resetDiscType = (DiscType)libGDR_GetDiscType();
	sns_key = 6;
	sns_asc = resetDiscType == Open || resetDiscType == NoDisk ? 0x29 : 0x28;
	sns_ascq = 0;
	sns_fad = 150;
}

void gdrom_reg_Term()
{
	sh4_sched_unregister(gdrom_schid);
	gdrom_schid = -1;
	sh4_sched_unregister(standby_schid);
	standby_schid = -1;
}

void gdrom_reg_Reset(bool hard)
{
	if (hard)
	{
		hollyRegs.setWriteHandler<SB_GDST_addr>(GDROM_DmaStart);
		hollyRegs.setWriteHandler<SB_GDEN_addr>(GDROM_DmaEnable);

		resetHardwareInfo();
		gdr::termDrive();
	}
	SB_GDST = 0;
	SB_GDEN = 0;
	dma_transfer_length = 0;
	dma_direction = 0;
	dma_address_limit = 0;
	dma_cycle_remainder = 0;
	dma_cycle_rate = 0;
	dma_access_timing = 0;
	dma_jitter_carry = 0;
	dma_address_valid = false;
	max_read_retries = 0;
	standby_seconds_remaining = 0;
	if (standby_schid >= 0)
		sh4_sched_request(standby_schid, -1);

	gd_state = gds_waitcmd;
	sns_asc = 0;
	sns_ascq = 0;
	sns_key = 0;
	sns_fad = 0;
	set_mode_offset = 0;
	read_params = {};
	packet_cmd = {};
	dma_buff.clear();
	pio_buff.clear();
	pio_sector_cached = false;
	pio_data_limit = PioBuffer::Capacity;
	ata_command = 0;
	cdda = {};
	cdda_completion_reported = false;
	cdda_scan_step = 0;
	cdda_scan_start = 0;
	cdda_scan_end = 0;
	gd_disk_type = NoDisk;

	data_write_mode = 0;
	DriveSel = 0xa0;
	Error = {};
	IntReason = {};
	Features = {};
	SecCount = {};
	SecNumber = {};
	GDStatus = {};
	DeviceControl = 0;
	command_interrupt_pending = false;
	ByteCount = {};

	gd_disc_change();
	const DiscType resetDiscType = (DiscType)libGDR_GetDiscType();
	sns_key = 6;
	sns_asc = resetDiscType == Open || resetDiscType == NoDisk ? 0x29 : 0x28;
	sns_ascq = 0;
	sns_fad = 150;
}

namespace gdrom
{

static u32 inferDataSelect(u32 sectorSize)
{
	switch (sectorSize)
	{
	case 4: return 8;             // header
	case 8: return 4;             // subheader
	case 12: return 8 | 4;        // header + subheader
	case 2052: return 8 | 2;      // header + data
	case 2088:
	case 2340: return 8 | 2;      // header + implicit subheader + data
	case 2352: return 1;          // all/other
	default: return 2;            // data (Mode 2 adds its subheader)
	}
}

void serialize(Serializer& ser)
{
	ser << GD_HardwareInfo;

	ser << sns_asc;
	ser << sns_ascq;
	ser << sns_key;

	ser << packet_cmd;
	ser << set_mode_offset;
	ser << read_params.start_sector;
	ser << read_params.remaining_sectors;
	ser << read_params.sector_type;
	ser << read_params.sector_offset;
	ser << read_params.expected_type;
	ser << read_params.next_sector;
	ser << read_params.data_select;
	dma_buff.serialize(ser);
	pio_buff.serialize(ser);
	ser << ata_command;
	ser << cdda;
	ser << gd_state;
	ser << gd_disk_type;
	ser << data_write_mode;
	ser << DriveSel;
	ser << Error;

	ser << IntReason;
	ser << Features;
	ser << SecCount;
	ser << SecNumber;
	ser << GDStatus;
	ser << ByteCount;
	ser << DeviceControl;
	ser << command_interrupt_pending;
	ser << cdda_scan_step;
	ser << cdda_scan_end;
	ser << pio_data_limit;
	ser << sns_fad;
	ser << cdda_completion_reported;
	ser << dma_transfer_length;
	ser << dma_direction;
	ser << dma_address_limit;
	ser << dma_cycle_remainder;
	ser << dma_cycle_rate;
	ser << dma_jitter_carry;
	ser << prefetch_error_fad;
	ser << cdda_scan_start;
	ser << standby_seconds_remaining;
	sh4_sched_serialize(ser, standby_schid);
	ser << max_read_retries;
	ser << dma_address_valid;
	ser << dma_access_timing;
	ser << pio_cached_fad << pio_cached_size << pio_sector_cached;
	ser << pio_sector_cache;
}

void deserialize(Deserializer& deser)
{
	deser >> GD_HardwareInfo;

	deser >> sns_asc;
	deser >> sns_ascq;
	deser >> sns_key;

	deser >> packet_cmd;
	deser >> set_mode_offset;
	if (deser.version() >= Deserializer::V123)
	{
		deser >> read_params.start_sector;
		deser >> read_params.remaining_sectors;
		deser >> read_params.sector_type;
		deser >> read_params.sector_offset;
		deser >> read_params.expected_type;
		deser >> read_params.next_sector;
		deser >> read_params.data_select;
	}
	else if (deser.version() >= Deserializer::V80)
	{
		deser >> read_params.start_sector;
		deser >> read_params.remaining_sectors;
		deser >> read_params.sector_type;
		deser >> read_params.sector_offset;
		deser >> read_params.expected_type;
		deser >> read_params.next_sector;
		read_params.data_select = inferDataSelect(read_params.sector_type);
	}
	else if (deser.version() >= Deserializer::V68)
	{
		deser >> read_params.start_sector;
		deser >> read_params.remaining_sectors;
		deser >> read_params.sector_type;
		deser >> read_params.sector_offset;
		deser >> read_params.expected_type;
		read_params.next_sector = 0;
		read_params.data_select = inferDataSelect(read_params.sector_type);
	}
	else if (deser.version() >= Deserializer::V60)
	{
		deser >> read_params.start_sector;
		deser >> read_params.remaining_sectors;
		deser >> read_params.sector_type;
		deser >> read_params.sector_offset;
		read_params.expected_type = 0;
		read_params.next_sector = 0;
		read_params.data_select = inferDataSelect(read_params.sector_type);
	}
	else
	{
		deser >> read_params.start_sector;
		deser >> read_params.remaining_sectors;
		deser >> read_params.sector_type;
		read_params.sector_offset = 0;
		read_params.expected_type = 0;
		read_params.next_sector = 0;
		read_params.data_select = inferDataSelect(read_params.sector_type);
	}
	if (deser.version() >= Deserializer::V17) {
		dma_buff.deserialize(deser);
	}
	else {
		deser >> packet_cmd;
		dma_buff.clear();
	}
	pio_buff.deserialize(deser);
	deser.skip<u32>(Deserializer::V44); // set_mode_offset (repeat)
	deser >> ata_command;
	deser >> cdda;
	deser >> gd_state;
	deser >> gd_disk_type;
	if (deser.version() < Deserializer::V75)
	{
		// Older states used format 3 for CD Extra and the reserved format 4
		// for CD-I. Convert those persisted protocol values to the corrected
		// format assignments.
		if ((u32)gd_disk_type == 0x30)
			gd_disk_type = CdRom_Extra;
		else if ((u32)gd_disk_type == 0x40)
			gd_disk_type = CdRom_CDI;
	}
	deser >> data_write_mode;
	deser >> DriveSel;
	deser >> Error;

	deser >> IntReason;
	deser >> Features;
	deser >> SecCount;
	deser >> SecNumber;
	if (deser.version() < Deserializer::V75
			&& (gd_disk_type == CdRom_Extra || gd_disk_type == CdRom_CDI))
		SecNumber.DiscFormat = gd_disk_type >> 4;
	deser >> GDStatus;
	deser >> ByteCount;
	if (deser.version() >= Deserializer::V60)
	{
		deser >> DeviceControl;
		deser >> command_interrupt_pending;
		deser >> cdda_scan_step;
		deser >> cdda_scan_end;
		deser >> pio_data_limit;
	}
	else
	{
		DeviceControl = 0;
		command_interrupt_pending = false;
		cdda_scan_step = 0;
		cdda_scan_end = 0;
		pio_data_limit = PioBuffer::Capacity;
	}
	if (deser.version() >= Deserializer::V65)
		deser >> sns_fad;
	else
		sns_fad = 0;
	if (deser.version() >= Deserializer::V69)
		deser >> cdda_completion_reported;
	else
		cdda_completion_reported = false;
	if (deser.version() >= Deserializer::V78)
	{
		deser >> dma_transfer_length;
		deser >> dma_direction;
		deser >> dma_address_limit;
		if (deser.version() < Deserializer::V60)
			dma_transfer_length = getDmaTransferLength(SB_GDLEN);
	}
	else
	{
		dma_transfer_length = getDmaTransferLength(SB_GDLEN);
		dma_direction = SB_GDDIR;
		if (!dmaAddressProtected(SB_GDSTARD, dma_address_limit))
			dma_address_limit = SB_GDSTARD;
	}
	if (deser.version() >= Deserializer::V102)
		deser >> dma_cycle_remainder >> dma_cycle_rate;
	else {
		dma_cycle_remainder = 0;
		dma_cycle_rate = 0;
	}
	if (deser.version() >= Deserializer::V128)
		deser >> dma_jitter_carry;
	else
		dma_jitter_carry = 0;
	if (deser.version() >= Deserializer::V80)
		deser >> prefetch_error_fad;
	else
		prefetch_error_fad = 0;
	if (deser.version() >= Deserializer::V114)
		deser >> cdda_scan_start;
	else
		cdda_scan_start = 150;
	if (deser.version() >= Deserializer::V129)
	{
		deser >> standby_seconds_remaining;
		sh4_sched_deserialize(deser, standby_schid);
	}
	else
	{
		standby_seconds_remaining = 0;
		if (SecNumber.Status == GD_PAUSE)
			gd_schedule_standby();
	}
	if (deser.version() >= Deserializer::V130)
		deser >> max_read_retries;
	else
		max_read_retries = 0;
	if (deser.version() >= Deserializer::V137)
		deser >> dma_address_valid;
	else
		dma_address_valid = getDmaAddressLimit(SB_GDSTAR) != 0;
	if (deser.version() >= Deserializer::V138)
		deser >> dma_access_timing;
	else
		dma_access_timing = dma_direction == 0 ? SB_G1GDWC : SB_G1GDRC;
	if (deser.version() >= Deserializer::V139)
	{
		deser >> pio_cached_fad >> pio_cached_size >> pio_sector_cached;
		deser >> pio_sector_cache;
		if (pio_cached_size > sizeof(pio_sector_cache))
			throw Deserializer::Exception("Invalid GD-ROM PIO sector cache size");
	}
	else
	{
		pio_cached_fad = 0;
		pio_cached_size = 0;
		pio_sector_cached = false;
	}
}

}
