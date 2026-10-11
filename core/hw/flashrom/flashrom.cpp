/*
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
#include "flashrom.h"
#include "oslib/oslib.h"
#include "stdclass.h"
#include "oslib/storage.h"
#include "hw/sh4/sh4_sched.h"

bool MemChip::Load(const std::string& file)
{
	hostfs::File *f = hostfs::storage().openFile(file, "rb");
	if (f)
	{
		bool rv = f->read(data + write_protect_size, 1, size - write_protect_size) == size - write_protect_size;
		delete f;
		if (rv)
			this->load_filename = file;

		return rv;
	}
	return false;
}

void MemChip::Load(const u8 *data, size_t size)
{
	verify(size == this->size - write_protect_size);
	memcpy(this->data + write_protect_size, data, this->size - write_protect_size);
}

void WritableChip::Save(const std::string& file)
{
	FILE *f = nowide::fopen(file.c_str(), "wb");
	if (f == nullptr)
	{
		ERROR_LOG(FLASHROM, "Cannot save flash/nvmem to file '%s'", file.c_str());
		return;
	}
	if (std::fwrite(data + write_protect_size, 1, size - write_protect_size, f) != size - write_protect_size)
		ERROR_LOG(FLASHROM, "Failed or truncated write to flash file '%s'", file.c_str());
	std::fclose(f);
}

bool MemChip::Load(const std::string &prefix, const std::string &names_ro, const std::string &title)
{
	std::string fullpath = hostfs::findFlash(prefix, names_ro);
	if (!fullpath.empty() && Load(fullpath))
	{
		INFO_LOG(FLASHROM, "Loaded %s as %s", fullpath.c_str(),
				title.c_str());
		return true;
	}
	return false;
}

void WritableChip::Save(const std::string &prefix, const std::string &name_ro, const std::string &title)
{
	std::string path = hostfs::getFlashSavePath(prefix, name_ro);
	Save(path);
	INFO_LOG(FLASHROM, "Saved %s as %s", path.c_str(), title.c_str());
}

void MemChip::digest(u8 md5Digest[16])
{
	MD5Sum().add(data + write_protect_size, size - write_protect_size)
			.getDigest(md5Digest);
}

void DCFlashChip::get_sector(u32 addr, u32& offset, u32& length) const
{
	addr &= mask;
	if (type == Type::Macronix29L001MC)
	{
		length = 64_KB;
		offset = addr & ~(length - 1);
		return;
	}

	// The Dreamcast maps the upper half of an MBM29LV002TC. Its top-boot
	// sectors therefore appear as 64K, 32K, 8K, 8K and 16K locally.
	if (addr < 0x10000)
	{
		offset = 0;
		length = 64_KB;
	}
	else if (addr < 0x18000)
	{
		offset = 0x10000;
		length = 32_KB;
	}
	else if (addr < 0x1c000)
	{
		offset = addr & ~0x1fff;
		length = 8_KB;
	}
	else
	{
		offset = 0x1c000;
		length = 16_KB;
	}
}

u8 DCFlashChip::get_sector_mask(u32 addr) const
{
	addr &= mask;
	if (type == Type::Macronix29L001MC)
		return 1 << (addr / 64_KB);
	if (addr < 0x10000)
		return 1 << 0;
	if (addr < 0x18000)
		return 1 << 1;
	if (addr < 0x1a000)
		return 1 << 2;
	if (addr < 0x1c000)
		return 1 << 3;
	return 1 << 4;
}

bool DCFlashChip::is_sector_protected(u32 offset, u32 length) const
{
	if (offset < write_protect_size)
		return true;
	return type == Type::DreamcastSystem
			&& offset < 0x1c000 && offset + length > 0x1a000;
}

bool DCFlashChip::is_unlock1_address(u32 addr)
{
	return (addr & 0x7ff) == 0x555 || (addr & 0xfff) == 0xaaa;
}

bool DCFlashChip::is_unlock2_address(u32 addr)
{
	return (addr & 0x7ff) == 0x2aa || (addr & 0xfff) == 0x555;
}

bool DCFlashChip::is_selected_sector(u32 addr) const
{
	return operation_length == 0 || (erase_mask & get_sector_mask(addr)) != 0;
}

void DCFlashChip::suspend_erase()
{
	const u64 now = current_cycle();
	if (state == FS_ErasePending)
	{
		erase_remaining = operation_end > now ? operation_end - now : 0;
		erase_remaining += operation_protected ? 150_sh4us : SH4_MAIN_CLOCK - 50_sh4us;
	}
	else
	{
		erase_remaining = operation_end > now ? operation_end - now : 0;
	}
	erase_suspended = true;
	status_toggle = false;
	state = FS_EraseSuspended;
}

void DCFlashChip::resume_erase()
{
	operation_end = current_cycle() + erase_remaining;
	erase_remaining = 0;
	erase_suspended = false;
	status_toggle = false;
	state = FS_EraseBusy;
}

u64 DCFlashChip::current_cycle() const
{
	return sh4_sched_now64();
}

void DCFlashChip::finish_operation()
{
	if (state != FS_ProgramBusy && state != FS_ErasePending && state != FS_EraseBusy)
		return;
	if (current_cycle() < operation_end)
		return;
	if (state == FS_ErasePending)
	{
		state = FS_EraseBusy;
		// The erase duration includes the 50 us sector-selection window.
		operation_end += operation_protected ? 150_sh4us : SH4_MAIN_CLOCK - 50_sh4us;
		if (current_cycle() < operation_end)
			return;
	}
	if (state == FS_ProgramBusy)
	{
		if (!program_protected)
			data[program_address] &= program_data;
	}
	else if (!operation_protected)
	{
		for (u32 sector = 0; sector < size; )
		{
			u32 offset;
			u32 length;
			get_sector(sector, offset, length);
			if ((operation_length == 0 || (erase_mask & get_sector_mask(offset)) != 0)
					&& !is_sector_protected(offset, length))
				memset(data + offset, 0xff, length);
			sector = offset + length;
		}
	}
	state = idle_state();
	operation_end = 0;
}

u8 DCFlashChip::read_status(u32 addr)
{
	status_toggle = !status_toggle;
	if (state == FS_ProgramBusy)
	{
		u8 status = (~program_data) & 0x80;
		if (status_toggle)
			status |= 0x40;
		return status;
	}

	u8 status = state == FS_EraseBusy ? 0x08 : 0;
	if (status_toggle)
	{
		status |= 0x40;
		if (operation_length == 0 || (erase_mask & get_sector_mask(addr)) != 0)
			status |= 0x04;
	}
	return status;
}

u8 DCFlashChip::Read8(u32 addr)
{
	addr &= mask;
	finish_operation();
	if (state == FS_ProgramBusy || state == FS_ErasePending || state == FS_EraseBusy)
		return read_status(addr);
	if (state == FS_SelectMode)
	{
		switch (addr & 3)
		{
		case 0:	// manufacturer's code
			return type == Type::Macronix29L001MC ? 0xc2 : 0x04;
		case 1:	// device code
			return type == Type::Macronix29L001MC ? 0x51 : 0x40;
		case 2:	// sector protection verification
			DEBUG_LOG(FLASHROM, "Sector protection address %x", addr);
			return is_protected(addr) ? 1 : 0;
		default:
			WARN_LOG(FLASHROM, "SelectMode unknown address %x", addr);
			return 0;
		}
	}
	if (erase_suspended && is_selected_sector(addr))
	{
		status_toggle = !status_toggle;
		return 0x80 | (status_toggle ? 0x04 : 0);
	}
	return MemChip::Read8(addr);
}

void DCFlashChip::Write(u32 addr,u32 val,u32 sz)
{
	if (sz != 1)
	{
		if (sz == 2 || sz == 4)
		{
			// The flash is byte-wide. Wider SH-4 accesses are split by the bus
			// into consecutive little-endian byte transactions.
			for (u32 i = 0; i < sz; i++)
				Write(addr + i, val >> (i * 8), 1);
			return;
		}
		WARN_LOG(FLASHROM, "invalid access size %d addr %x", sz, addr);
		return;
	}

	addr &= mask;
	val &= 0xff;
	finish_operation();
	if ((state == FS_ErasePending || state == FS_EraseBusy)
			&& operation_length != 0 && val == 0xb0)
	{
		suspend_erase();
		return;
	}
	if (state == FS_ErasePending && val == 0x30)
	{
		u32 offset;
		u32 length;
		get_sector(addr, offset, length);
		erase_mask |= get_sector_mask(addr);
		operation_protected = operation_protected && is_sector_protected(offset, length);
		return;
	}
	if (state == FS_ProgramBusy || state == FS_ErasePending || state == FS_EraseBusy)
		return;
	if (state == FS_EraseSuspended && val == 0x30)
	{
		resume_erase();
		return;
	}
	if (state == FS_FastReset && val == 0xf0)
	{
		fast_mode = false;
		state = FS_Normal;
		return;
	}

	// The reset/read-array command is accepted from every software command
	// state except while an embedded program/erase algorithm is active.
	if (val == 0xf0 || val == 0xff)
	{
		state = idle_state();
		return;
	}

	switch(state)
	{
	case FS_Normal:
	case FS_EraseSuspended:
		switch (val)
		{
		case 0xaa:  // AMD ID select part 1
			if (is_unlock1_address(addr))
				state = FS_ReadAMDID1;
			break;
		default:
			INFO_LOG(FLASHROM, "Unknown FlashWrite mode: %x", val);
			break;
		}
		break;
	case FS_FastMode:
		if (val == 0xa0)
			state = FS_ByteProgram;
		else if (val == 0x90)
			state = FS_FastReset;
		break;
	case FS_FastReset:
		break;

	case FS_ReadAMDID1:
		if (is_unlock2_address(addr) && val == 0x55)
			state = FS_ReadAMDID2;
		else
		{
			if (val != 0xf0)
				WARN_LOG(FLASHROM, "FlashRom: ReadAMDID1 unexpected write @ %x: %x", addr, val);
			state = idle_state();
		}
		break;

	case FS_ReadAMDID2:
		if (is_unlock1_address(addr) && val == 0x80 && !erase_suspended)
			state = FS_EraseAMD1;
		else if (is_unlock1_address(addr) && val == 0xa0)
			state = FS_ByteProgram;
		else if (is_unlock1_address(addr) && val == 0x90)
			state = FS_SelectMode;
		else if (is_unlock1_address(addr) && val == 0x20
				&& type == Type::DreamcastSystem && !erase_suspended)
		{
			fast_mode = true;
			state = FS_FastMode;
		}
		else
		{
			if (val != 0xf0)
				WARN_LOG(FLASHROM, "FlashRom: ReadAMDID2 unexpected write @ %x: %x", addr, val);
			state = idle_state();
		}
		break;
	case FS_ByteProgram:
		if (erase_suspended && is_selected_sector(addr))
		{
			state = FS_EraseSuspended;
			break;
		}
		program_address = addr;
		program_data = val;
		program_protected = is_protected(addr);
		operation_end = current_cycle() + (program_protected ? 2_sh4us : 8_sh4us);
		status_toggle = false;
		state = FS_ProgramBusy;
		break;

	case FS_EraseAMD1:
		if (is_unlock1_address(addr) && val == 0xaa)
			state = FS_EraseAMD2;
		else
		{
			if (val != 0xf0)
				WARN_LOG(FLASHROM, "FlashRom: EraseAMD1 unexpected write @ %x: %x", addr, val);
			state = idle_state();
		}
		break;

	case FS_EraseAMD2:
		if (is_unlock2_address(addr) && val == 0x55)
			state = FS_EraseAMD3;
		else
		{
			if (val != 0xf0)
				WARN_LOG(FLASHROM, "FlashRom: EraseAMD2 unexpected write @ %x: %x", addr, val);
			state = idle_state();
		}
		break;

	case FS_EraseAMD3:
		if (is_unlock1_address(addr) && val == 0x10)
		{
			// chip erase
			INFO_LOG(FLASHROM, "Erasing Chip!");
			operation_address = 0;
			operation_length = 0;
			erase_mask = 0xff;
			operation_protected = false;
			operation_end = current_cycle() + 31 * (u64)SH4_MAIN_CLOCK / 10;
			status_toggle = false;
			state = FS_EraseBusy;
		}
		else if ((val & 0xff) == 0x30)
		{
			// sector erase
			u32 offset;
			u32 length;
			get_sector(addr, offset, length);
			INFO_LOG(FLASHROM, "Erase Sector %08X!", addr);
			operation_address = offset;
			operation_length = length;
			erase_mask = get_sector_mask(addr);
			operation_protected = is_sector_protected(offset, length);
			operation_end = current_cycle() + 50_sh4us;
			status_toggle = false;
			state = FS_ErasePending;
		}
		else if (val != 0xf0)
			WARN_LOG(FLASHROM, "FlashRom: EraseAMD3 unexpected write @ %x: %x", addr, val);
		if (state == FS_EraseAMD3)
			state = idle_state();
		break;
	case FS_SelectMode:
		// Only reset/read-array is valid in autoselect mode (handled above).
		break;
	default:
		WARN_LOG(FLASHROM, "FlashRom: invalid state. write @ %x: %x", addr, val);
		state = idle_state();
		break;
	}
}

int DCFlashChip::WriteBlock(u32 part_id, u32 block_id, const void *data)
{
	int offset, size;
	GetPartitionInfo(part_id, &offset, &size);

	if (!validate_header(offset, part_id))
		return 0;

	// the real system libraries allocate and write to a new physical block each
	// time a logical block is updated. the reason being that, flash memory can
	// only be programmed once, and after that the entire sector must be reset in
	// order to reprogram it. flash storage has a finite number of these erase
	// operations before its integrity deteriorates, so the libraries try to
	// minimize how often they occur by writing to a new physical block until the
	// partition is completely full
	//
	// this limitation of the original hardware isn't a problem for us, so try and
	// just update an existing logical block if it exists
	int phys_id = lookup_block(offset, size, block_id);
	if (!phys_id) {
		phys_id = alloc_block(offset, size);

		if (!phys_id)
			return 0;
	}

	// update the block's crc before writing it back out
	struct flash_user_block user;
	memcpy(&user, data, sizeof(user));
	user.block_id = block_id;
	user.crc = crc_block(&user);

	write_physical_block(offset, phys_id, &user);

	return 1;
}

int DCFlashChip::ReadBlock(u32 part_id, u32 block_id, void *data)
{
	int offset, size;
	GetPartitionInfo(part_id, &offset, &size);

	if (!validate_header(offset, part_id))
		return 0;

	int phys_id = lookup_block(offset, size, block_id);
	if (!phys_id)
		return 0;

	read_physical_block(offset, phys_id, data);

	return 1;
}

void DCFlashChip::Validate()
{
	// validate partition 0 (factory settings)
	bool valid = true;
	char sysinfo[16];

	for (u32 i = 0; i < sizeof(sysinfo); i++)
		sysinfo[i] = Read8(0x1a000 + i);
	valid = valid && memcmp(&sysinfo[5], "Dreamcast  ", 11) == 0;

	for (u32 i = 0; i < sizeof(sysinfo); i++)
		sysinfo[i] = Read8(0x1a0a0 + i);
	valid = valid && memcmp(&sysinfo[5], "Dreamcast  ", 11) == 0;

	if (!valid)
	{
		INFO_LOG(FLASHROM, "DCFlashChip::Validate resetting FLASH_PT_FACTORY");

		memcpy(sysinfo, "00000Dreamcast  ", sizeof(sysinfo));
		erase_partition(FLASH_PT_FACTORY);
		memcpy(data + 0x1a000, sysinfo, sizeof(sysinfo));
		memcpy(data + 0x1a0a0, sysinfo, sizeof(sysinfo));
	}

	// validate partition 1 (reserved)
	erase_partition(FLASH_PT_RESERVED);

	// validate partition 2 (user settings, block allocated)
	if (!validate_header(FLASH_PT_USER))
	{
		INFO_LOG(FLASHROM, "DCFlashChip::Validate resetting FLASH_PT_USER");

		erase_partition(FLASH_PT_USER);
		write_header(FLASH_PT_USER);
	}

	// validate partition 3 (game settings, block allocated)
	if (!validate_header(FLASH_PT_GAME))
	{
		INFO_LOG(FLASHROM, "DCFlashChip::Validate resetting FLASH_PT_GAME");

		erase_partition(FLASH_PT_GAME);
		write_header(FLASH_PT_GAME);
	}

	// validate partition 4 (unknown, block allocated)
	if (!validate_header(FLASH_PT_UNKNOWN))
	{
		INFO_LOG(FLASHROM, "DCFlashChip::Validate resetting FLASH_PT_UNKNOWN");

		erase_partition(FLASH_PT_UNKNOWN);
		write_header(FLASH_PT_UNKNOWN);
	}
}

int DCFlashChip::alloc_block(u32 offset, u32 size)
{
	u8 bitmap[FLASH_BLOCK_SIZE];
	int blocks = num_user_blocks(size);
	int bitmap_id = blocks;
	int phys_id = 1;
	int phys_end = 1 + blocks;

	while (phys_id < phys_end) {
		// read the next bitmap every FLASH_BITMAP_BLOCKS
		if (phys_id % FLASH_BITMAP_BLOCKS == 1) {
			read_physical_block(offset, ++bitmap_id, bitmap);
		}

		// use the first unallocated block
		if (!is_allocated(bitmap, phys_id)) {
			break;
		}
		// if the current block has been rewritten, use it
		if (lookup_block(offset, size, *(u16*)&this->data[offset + phys_id * FLASH_BLOCK_SIZE]) != phys_id)
			break;

		phys_id++;
	}

	if (phys_id >= phys_end)
	{
		WARN_LOG(FLASHROM, "Cannot allocate block in flash. Full?");
		return 0;
	}

	// mark the block as allocated
	set_allocated(bitmap, phys_id);
	write_physical_block(offset, bitmap_id, bitmap);

	return phys_id;
}

int DCFlashChip::lookup_block(u32 offset, u32 size, u32 block_id)
{
	u8 bitmap[FLASH_BLOCK_SIZE];
	int blocks = num_user_blocks(size);
	int bitmap_id = 1 + blocks;
	int phys_id = 1;
	int phys_end = bitmap_id;

	// in order to lookup a logical block, all physical blocks must be iterated.
	// since physical blocks are allocated linearly, the physical block with the
	// highest address takes precedence
	int result = 0;

	while (phys_id < phys_end) {
		// read the next bitmap every FLASH_BITMAP_BLOCKS
		if (phys_id % FLASH_BITMAP_BLOCKS == 1) {
			read_physical_block(offset, bitmap_id++, bitmap);
		}

		// being that physical blocks are allocated linearly, stop processing once
       	// the first unallocated block is hit
		if (!is_allocated(bitmap, phys_id))
			break;

		struct flash_user_block user;
		read_physical_block(offset, phys_id, &user);

		if (user.block_id == block_id)
		{
			if (!validate_crc(&user))
				WARN_LOG(FLASHROM, "flash_lookup_block physical block %d has an invalid crc", phys_id);
			else
				result = phys_id;
		}

		phys_id++;
	}

	return result;
}
