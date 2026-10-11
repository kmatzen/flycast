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
#pragma once
#include "types.h"
#include "serialize.h"
#include <cmath>

struct MemChip
{
	u8* data;
	u32 size;
	u32 mask;

protected:
	u32 write_protect_size;
	std::string load_filename;

	MemChip(u32 size, u32 write_protect_size = 0)
	{
		this->data = new u8[size]();
		this->size = size;
		this->mask = size - 1; // must be power of 2
		this->write_protect_size = write_protect_size;
	}

public:
	virtual ~MemChip()
	{
		delete[] data;
	}

	virtual u8 Read8(u32 addr)
	{
		return data[addr & mask];
	}

	u32 Read(u32 addr, u32 sz)
	{
		addr &= mask;

		u32 rv = 0;

		for (u32 i = 0; i < sz; i++)
			rv |= Read8(addr + i) << (i * 8);

		return rv;
	}

	bool Load(const std::string& file);
	bool Load(const std::string &prefix, const std::string &names_ro,
			const std::string &title);
	void Load(const u8 *data, size_t size);
	void digest(u8 md5Digest[16]);

	virtual void Reset() {}
	virtual void Serialize(Serializer& ser) const { }
	virtual void Deserialize(Deserializer& deser) { }
};

struct WritableChip : MemChip
{
protected:
	WritableChip(u32 size, u32 write_protect_size = 0)
		: MemChip(size, write_protect_size) {}

public:
	virtual void Write(u32 addr, u32 data, u32 size) = 0;
	void Save(const std::string& file);
	void Save(const std::string &prefix, const std::string &name_ro,
			const std::string &title);
};

struct RomChip : MemChip
{
	RomChip(u32 sz) : MemChip(sz) {}
};

struct SRamChip : WritableChip
{
	SRamChip(u32 sz, u32 write_protect_size = 0) : WritableChip(sz, write_protect_size) {}

	void Write(u32 addr, u32 val, u32 sz) override
	{
		addr &= mask;
		if (addr < write_protect_size)
			return;
		switch (sz)
		{
		case 1:
			data[addr] = (u8)val;
			return;
		case 2:
			*(u16 *)&data[addr] = (u16)val;
			return;
		case 4:
			*(u32 *)&data[addr] = val;
			return;
		default:
			die("invalid access size");
		}
	}

	void Serialize(Serializer& ser) const override
	{
		ser.serialize(&this->data[write_protect_size], size - write_protect_size);
	}
	void Deserialize(Deserializer& deser) override
	{
		deser.deserialize(&this->data[write_protect_size], size - write_protect_size);
	}
};

//
// Flash block handling code borrowed from redream (https://github.com/inolen/redream)
//

// magic cookie every block-allocated partition begins with
#define FLASH_MAGIC_COOKIE "KATANA_FLASH____"

#define FLASH_BLOCK_SIZE 0x40
// each bitmap is 64 bytes in length, and each byte can record the state of 8
// physical blocks (one per bit), therefore, each bitmap can represent up to
// 512 physical blocks. these 512 blocks are each 64-bytes in length, meaning
// each partition would need partition_size / 32768 bitmap blocks to represent
// all of its physical blocks
#define FLASH_BITMAP_BLOCKS (FLASH_BLOCK_SIZE * 8)
#define FLASH_BITMAP_BYTES (FLASH_BITMAP_BLOCKS * 64)

// flash partitions
#define FLASH_PT_FACTORY 0
#define FLASH_PT_RESERVED 1
#define FLASH_PT_USER 2
#define FLASH_PT_GAME 3
#define FLASH_PT_UNKNOWN 4
#define FLASH_PT_NUM 5

// flash logical blocks
#define FLASH_USER_SYSCFG 0x05
#define FLASH_USER_INET 0x80
#define FLASH_USER_ISP1 0xC0
#define FLASH_USER_ISP2 0xC6
#define FLASH_USER_BBA 0xE0

struct flash_syscfg_block {
  u16 block_id;
  // last set time (seconds since 1/1/1950 00:00)
  u16 time_lo;
  u16 time_hi;
	// in 15 mins increment, from -48 (West) to +52 (East), unused
  int8_t time_zone;
  u8 lang;
  u8 mono;
  u8 autostart;
  u8 unknown2[4];
  u8 reserved[50];
};
static_assert(sizeof(flash_syscfg_block) == 64);

struct flash_isp1_block {
	u16 block_id;
	u8 _unknown[4];
	char sega[4];
	char username[28];
	char password[16];
	char phone[8];
	u16 crc;
};
static_assert(sizeof(flash_isp1_block) == 64);

struct flash_isp2_block {
	u16 block_id;
	char sega[4];
	char username[28];
	char password[16];
	char phone[12];
	u16 crc;
};
static_assert(sizeof(flash_isp2_block) == 64);

#pragma pack(push,1)
struct flash_bba_block {
	u16 block_id;
	char sega[4];
	u32 unk1;
	u32 ipAddress;
	u32 netmask;
	u32 broadcast;
	u32 dns1;
	u32 dns2;
	u32 gateway;
	char unk2[28];
	u16 crc;
};
#pragma pack(pop)
static_assert(sizeof(flash_bba_block) == 64);

// header block in block-allocated partition
struct flash_header_block {
  char magic[16];
  u8 part_id;
  u8 version;
  u8 reserved[46];
};
static_assert(sizeof(flash_header_block) == 64);

// user block in block-allocated partition
struct flash_user_block {
  u16 block_id;
  u8 data[60];
  u16 crc;
};
static_assert(sizeof(flash_user_block) == 64);

// Macronix 29LV160TMC
// AtomisWave uses a custom 29L001mc model
struct DCFlashChip : WritableChip
{
	enum class Type
	{
		DreamcastSystem,
		Macronix29L001MC,
	};

	DCFlashChip(u32 sz, u32 write_protect_size = 0, Type type = Type::DreamcastSystem)
		: WritableChip(sz, write_protect_size), state(FS_Normal), type(type)
	{
		memset(data, 0xff, size);
	}

	enum FlashState
	{
		FS_Normal,
		FS_ReadAMDID1,
		FS_ReadAMDID2,
		FS_ByteProgram,
		FS_EraseAMD1,
		FS_EraseAMD2,
		FS_EraseAMD3,
		FS_SelectMode,
		FS_ProgramBusy,
		FS_ErasePending,
		FS_EraseBusy,
		FS_EraseSuspended,
		FS_FastMode,
		FS_FastReset,
	};

	FlashState state;
	void Reset() override
	{
		//reset the flash chip state
		state = FS_Normal;
		operation_end = 0;
		erase_suspended = false;
		erase_remaining = 0;
		fast_mode = false;
	}
	
	void Write(u32 addr,u32 val,u32 sz) override;
	u8 Read8(u32 addr) override;

	int WriteBlock(u32 part_id, u32 block_id, const void *data);
	int ReadBlock(u32 part_id, u32 block_id, void *data);

	void GetPartitionInfo(int part_id, int *offset, int *size)
	{
		switch (part_id)
		{
		case FLASH_PT_FACTORY:
			*offset = 0x1a000;
			*size = 8_KB;
			break;
		case FLASH_PT_RESERVED:
			*offset = 0x18000;
			*size = 8_KB;
			break;
		case FLASH_PT_USER:
			*offset = 0x1c000;
			*size = 16_KB;
			break;
		case FLASH_PT_GAME:
			*offset = 0x10000;
			*size = 32_KB;
			break;
		case FLASH_PT_UNKNOWN:
			*offset = 0x00000;
			*size = 64_KB;
			break;
		default:
			*offset = 0;
			*size = 0;
			die("unknown partition");
			break;
		}
	}

	void Validate();

private:
	Type type;
	u32 operation_address = 0;
	u32 operation_length = 0;
	u8 erase_mask = 0;
	u32 program_address = 0;
	u8 program_data = 0xff;
	u64 operation_end = 0;
	bool operation_protected = false;
	bool program_protected = false;
	bool status_toggle = false;
	bool erase_suspended = false;
	u64 erase_remaining = 0;
	bool fast_mode = false;

	bool is_protected(u32 addr) const
	{
		addr &= mask;
		return addr < write_protect_size
				|| (type == Type::DreamcastSystem && (addr & 0x1e000) == 0x1a000);
	}
	void get_sector(u32 addr, u32& offset, u32& length) const;
	u8 get_sector_mask(u32 addr) const;
	bool is_sector_protected(u32 offset, u32 length) const;
	void finish_operation();
	u8 read_status(u32 addr);
	virtual u64 current_cycle() const;
	static bool is_unlock1_address(u32 addr);
	static bool is_unlock2_address(u32 addr);
	FlashState idle_state() const
	{
		return erase_suspended ? FS_EraseSuspended : fast_mode ? FS_FastMode : FS_Normal;
	}
	bool is_selected_sector(u32 addr) const;
	void suspend_erase();
	void resume_erase();

	int crc_block(struct flash_user_block *block)
	{
		const u8 *buf = (const u8 *)block;
		int size = 62;
		int n = 0xffff;

		for (int i = 0; i < size; i++) {
			n ^= (buf[i] << 8);

			for (int c = 0; c < 8; c++) {
				if (n & 0x8000) {
					n = (n << 1) ^ 4129;
				} else {
					n = (n << 1);
				}
			}
		}

		return (~n) & 0xffff;
	}

	int validate_crc(struct flash_user_block *user)
	{
		return user->crc == crc_block(user);
	}

	inline int num_physical_blocks(u32 size)
	{
		return size / FLASH_BLOCK_SIZE;
	}

	inline int num_bitmap_blocks(u32 size)
	{
		return (int)std::ceil(size / (float)FLASH_BITMAP_BYTES);
	}

	inline int num_user_blocks(u32 size)
	{
		return num_physical_blocks(size) - num_bitmap_blocks(size) - 1;
	}

	inline int is_allocated(const u8 *bitmap, u32 phys_id)
	{
		int index = (phys_id - 1) % FLASH_BITMAP_BLOCKS;
		return (bitmap[index / 8] & (0x80 >> (index % 8))) == 0x0;
	}

	inline void set_allocated(u8 *bitmap, u32 phys_id)
	{
		int index = (phys_id - 1) % FLASH_BITMAP_BLOCKS;
		bitmap[index / 8] &= ~(0x80 >> (index % 8));
	}

	void write_physical_block(u32 offset, u32 phys_id, const void *data)
	{
		memcpy(&this->data[offset + phys_id * FLASH_BLOCK_SIZE], data, FLASH_BLOCK_SIZE);
	}

	void read_physical_block(u32 offset, u32 phys_id, void *data)
	{
		memcpy(data, &this->data[offset + phys_id * FLASH_BLOCK_SIZE], FLASH_BLOCK_SIZE);
	}

	int validate_header(u32 offset, u32 part_id)
	{
		struct flash_header_block header;
		read_physical_block(offset, 0, &header);

		if (memcmp(header.magic, FLASH_MAGIC_COOKIE, sizeof(header.magic)) != 0)
			return 0;

		if (header.part_id != part_id)
			return 0;

		return 1;
	}

	int validate_header(u32 part_id)
	{
		int offset, size;
		GetPartitionInfo(part_id, &offset, &size);

		return validate_header(offset, part_id);
	}

	int alloc_block(u32 offset, u32 size);
	int lookup_block(u32 offset, u32 size, u32 block_id);

	void Serialize(Serializer& ser) const override
	{
		ser << state;
		ser << operation_address;
		ser << operation_length;
		ser << erase_mask;
		ser << program_address;
		ser << program_data;
		ser << operation_end;
		ser << operation_protected;
		ser << program_protected;
		ser << status_toggle;
		ser << erase_suspended;
		ser << erase_remaining;
		ser << fast_mode;
		ser.serialize(&this->data[write_protect_size], size - write_protect_size);
	}

	void Deserialize(Deserializer& deser) override
	{
		deser >> state;
		if (deser.version() >= Deserializer::V86)
		{
			deser >> operation_address;
			deser >> operation_length;
			deser >> erase_mask;
			deser >> program_address;
			deser >> program_data;
			deser >> operation_end;
			deser >> operation_protected;
			deser >> program_protected;
			deser >> status_toggle;
			deser >> erase_suspended;
			deser >> erase_remaining;
			deser >> fast_mode;
		}
		else
		{
			operation_address = 0;
			operation_length = 0;
			erase_mask = 0;
			program_address = 0;
			program_data = 0xff;
			operation_end = 0;
			operation_protected = false;
			program_protected = false;
			status_toggle = false;
			erase_suspended = false;
			erase_remaining = 0;
			fast_mode = false;
		}
		deser.deserialize(&this->data[write_protect_size], size - write_protect_size);
	}

	void erase_partition(u32 part_id)
	{
		int offset, size;
		GetPartitionInfo(part_id, &offset, &size);

		memset(data + offset, 0xFF, size);
	}

	void write_header(int part_id)
	{
		int offset, size;
		GetPartitionInfo(part_id, &offset, &size);

		struct flash_header_block header;
		memset(&header, 0xff, sizeof(header));
		memcpy(header.magic, FLASH_MAGIC_COOKIE, sizeof(header.magic));
		header.part_id = part_id;
		header.version = 0;

		write_physical_block(offset, 0, &header);
	}
};
