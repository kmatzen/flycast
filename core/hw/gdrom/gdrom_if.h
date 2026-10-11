#pragma once
#include "types.h"

void gdrom_reg_Init();
void gdrom_reg_Term();
void gdrom_reg_Reset(bool hard);

u32  ReadMem_gdrom(u32 Addr, u32 sz);
void WriteMem_gdrom(u32 Addr, u32 data, u32 sz);
void libCore_CDDA_Sector(s16* sector);
u32 gd_get_subcode(u32 format, u32 fad, u8 *subc_info);
void gd_subcode_status_transferred(u32 bytes);
void gd_media_change();
void gd_setdisc(bool mediaChanged = false);
void gd_start_cdda_scan(bool reverse, u32 speedParameter);
void gd_pause_cdda();
void gd_schedule_standby();
bool gd_normalize_playback_start(u32& fad);
u32 gd_get_playback_area_end(u32 fad);
bool gd_read_sector(u8 *sector, u32 fad, u32 size, bool form2);

enum DiscType
{
	CdDA=0x00,
	CdRom=0x10,
	CdRom_XA=0x20,
	CdRom_Extra=0x20,
	CdRom_CDI=0x30,
	GdRom=0x80,

	NoDisk=0x1,			//These are a bit hacky .. but work for now ...
	Open=0x2,			//tray is open :)
};

namespace gdrom
{
void serialize(Serializer& ser);
void deserialize(Deserializer& deser);
}
