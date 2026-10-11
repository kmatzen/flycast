/*
	Copyright 2020 flyinghead

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
#include <array>
#include "types.h"
#include "sh4_mem.h"
#include "modules/mmu.h"
#include "hw/sh4/sh4_if.h"
#include "serialize.h"
#include "sh4_cycles.h"

static bool cachedArea(u32 area)
{
	static const bool cached_areas[8] = {
			true, true, true, true,	// P0/U0
			true,					// P1
			false,					// P2
			true,					// P3
			false					// P4
	};
	return cached_areas[area];
}

static bool translatedArea(u32 area)
{
	static const bool translated_areas[8] = {
			true, true, true, true,	// P0/U0
			false,					// P1
			false,					// P2
			true,					// P3
			false					// P4
	};
	return translated_areas[area];
}

//
// SH4 instruction cache
//
class Sh4ICache
{
public:
	void init(Sh4Context *ctx) {
		this->ctx = ctx;
		sh4cycles.init(ctx);
	}

	u16 ReadMem(u32 address)
	{
		bool cacheOn = false;
		u32 physAddr;
		MmuError err = translateAddress(address, physAddr, cacheOn);
		if (err != MmuError::NONE)
			mmu_raise_exception(err, address, MMU_TT_IREAD);
		if (!cacheOn) {
			sh4cycles.addReadAccessCycles(physAddr, 2);
			return addrspace::readt<u16>(physAddr);
		}

		const u32 index = CCN_CCR.IIX ?
				((address >> 5) & 0x7f) | ((address >> (25 - 7)) & 0x80)
				: (address >> 5) & 0xff;
		cache_line& line = lines[index];
		const u32 tag = (physAddr >> 10) & 0x7ffff;
		if (!line.valid || tag != line.address)
		{
			// miss
			line.valid = true;
			line.address = tag;
			const u32 line_addr = physAddr & ~0x1f;
			sh4cycles.addReadAccessCycles(physAddr, 32);
			u8* const memPtr = GetMemPtr(line_addr, sizeof(line.data));
			if (memPtr != nullptr)
				memcpy(line.data, memPtr, sizeof(line.data));
			else
			{
				u32 *p = (u32 *)line.data;
				for (int i = 0; i < 32; i += 4)
					*p++ = addrspace::read32(line_addr + i);
			}
		}

		u16 data;
		memcpy(&data, &line.data[physAddr & 0x1f], sizeof(data));
		return data;
	}

	void Invalidate()
	{
		for (auto& line : lines)
			line.valid = false;
	}

	void Reset(bool hard)
	{
		if (hard)
			memset(&lines[0], 0, sizeof(lines));
	}

	void Serialize(Serializer& ser) {
		ser << lines;
	}
	void Deserialize(Deserializer& deser) {
		deser >> lines;
	}

	u32 ReadAddressArray(u32 addr)
	{
		u32 index = (addr >> 5) & 0xFF;
		return (u32)lines[index].valid | (lines[index].address << 10);
	}

	void WriteAddressArray(u32 addr, u32 data)
	{
		u32 index = (addr >> 5) & 0xFF;
		cache_line& line = lines[index];
		bool associative = (addr & 8) != 0;
		if (!associative)
		{
			line.valid = data & 1;
			line.address = (data >> 10) & 0x7ffff;
		}
		else
		{
			const u32 vaddr = data & ~0x3ff;
			bool cached;
			u32 physAddr;
			MmuError err = translateAddress(vaddr, physAddr, cached);
			if (err == MmuError::TLB_MISS)
				// Ignore the write
				return;
			if (err != MmuError::NONE)
				mmu_raise_exception(err, vaddr, MMU_TT_IREAD);

			u32 tag = (physAddr >> 10) & 0x7ffff;

			if (!line.valid || tag != line.address)
				// Ignore the write
				return;
			line.valid = data & 1;
		}
	}

	u32 ReadDataArray(u32 addr)
	{
		u32 index = (addr >> 5) & 0xFF;
		cache_line& line = lines[index];
		u32 data;
		memcpy(&data, &line.data[addr & 0x1c], sizeof(data));
		return data;
	}

	void WriteDataArray(u32 addr, u32 data)
	{
		u32 index = (addr >> 5) & 0xFF;
		cache_line& line = lines[index];
		memcpy(&line.data[addr & 0x1c], &data, sizeof(data));
	}

private:
	struct cache_line {
		bool valid;
		u32 address;
		u8 data[32];
	};

	MmuError translateAddress(u32 address, u32& physAddr, bool& cached)
	{
		// Alignment errors
		if (address & 1)
			return MmuError::BADADDR;

		const u32 area = address >> 29;
		const bool userMode = ctx->sr.MD == 0;

		if (userMode)
		{
			// kernel mem protected in user mode
			if (address & 0x80000000)
				return MmuError::BADADDR;
		}
		cached = CCN_CCR.ICE == 1 && cachedArea(area);

		if (CCN_MMUCR.AT == 0 || !translatedArea(area)
				// 7C000000 to 7FFFFFFF in P0/U0 not translated
				|| (address & 0xFC000000) == 0x7C000000)
		{
			physAddr = address;
		}
		else
		{
			const TLB_Entry *entry;
			MmuError err = mmu_instruction_lookup(address, &entry, physAddr);

			if (err != MmuError::NONE)
				return err;

			//0X  & User mode-> protection violation
			//Priv mode protection
			if (userMode)
			{
				u32 md = entry->Data.PR >> 1;
				if (md == 0)
					return MmuError::PROTECTED;
			}
			cached = cached && entry->Data.C == 1;
			if ((physAddr & 0x1c000000) == 0x1c000000)
				physAddr |= 0xf0000000;
		}
		return MmuError::NONE;
	}

	std::array<cache_line, 256> lines;
	Sh4Cycles sh4cycles;
	Sh4Context *ctx = nullptr;
};

extern Sh4ICache icache;

//
// SH4 operand cache
//
class Sh4OCache
{
public:
	void init(Sh4Context *ctx) {
		this->ctx = ctx;
		sh4cycles.init(ctx);
	}

	template<class T>
	T ReadMem(u32 address)
	{
		u32 physAddr;
		bool cacheOn = false;
		bool copyBack;
		MmuError err = translateAddress<T, MMU_TT_DREAD>(address, physAddr, cacheOn, copyBack);
		if (err != MmuError::NONE)
			mmu_raise_exception(err, address, MMU_TT_DREAD);

		if (!cacheOn) {
			sh4cycles.addReadAccessCycles(physAddr, sizeof(T));
			return addrspace::readt<T>(physAddr);
		}

		const u32 index = lineIndex(address);
		cache_line& line = lines[index];
		const u32 tag = (physAddr >> 10) & 0x7ffff;
		if (!line.valid || tag != line.address)
		{
			// miss
			const bool writeBack = line.dirty && line.valid;
			const cache_line evicted = line;
			line.address = tag;
			readCacheLine(physAddr, line);
			if (writeBack)
				// Reads have priority; drain the saved dirty line after the fill.
				doWriteBack(index, evicted);
		}

		T data;
		memcpy(&data, &line.data[physAddr & 0x1f], sizeof(data));
		return data;
	}

	template<class T>
	void WriteMem(u32 address, T data)
	{
		u32 physAddr = 0;
		bool cacheOn = false;
		bool copyBack = false;
		MmuError err = translateAddress<T, MMU_TT_DWRITE>(address, physAddr, cacheOn, copyBack);
		if (err != MmuError::NONE)
			mmu_raise_exception(err, address, MMU_TT_DWRITE);

		if (!cacheOn)
		{
			addWriteThroughCycles(physAddr, sizeof(T));
			addrspace::writet<T>(physAddr, data);
			return;
		}

		const u32 index = lineIndex(address);
		cache_line& line = lines[index];
		const u32 tag = (physAddr >> 10) & 0x7ffff;
		if (!line.valid || tag != line.address)
		{
			// miss and copy-back => read cache line
			if (copyBack)
			{
				const bool writeBack = line.dirty && line.valid;
				const cache_line evicted = line;
				line.address = tag;
				readCacheLine(physAddr, line);
				if (writeBack)
					doWriteBack(index, evicted);
			}
		}
		else if (!copyBack)
		{
			// hit and write-through => update cache
			memcpy(&line.data[physAddr & 0x1f], &data, sizeof(data));
		}
		if (copyBack)
		{
			// copy-back => update cache and mark line as dirty
			line.dirty = true;
			memcpy(&line.data[physAddr & 0x1f], &data, sizeof(data));
		}
		else
		{
			// write-through => update main ram
			addrspace::writet<T>(physAddr, data);
			addWriteThroughCycles(physAddr, sizeof(T));
		}
	}

	void Movca(u32 address, u32 data)
	{
		u32 physAddr = 0;
		bool cacheOn = false;
		bool copyBack = false;
		MmuError err = translateAddress<u32, MMU_TT_DWRITE>(address, physAddr, cacheOn, copyBack);
		if (err != MmuError::NONE)
			mmu_raise_exception(err, address, MMU_TT_DWRITE);

#ifndef STRICT_MODE
		// Fast mode does not retain cache contents, but MOVCA still performs the
		// translated longword store and its architectural exception checks.
		addWriteThroughCycles(physAddr, sizeof(data));
		addrspace::write32(physAddr, data);
		return;
#endif
		if (!cacheOn)
		{
			addWriteThroughCycles(physAddr, sizeof(data));
			addrspace::write32(physAddr, data);
			return;
		}

		const u32 index = lineIndex(address);
		cache_line& line = lines[index];
		const u32 tag = (physAddr >> 10) & 0x7ffff;
		if ((!line.valid || tag != line.address) && copyBack)
		{
			// Unlike an ordinary write miss, MOVCA allocates without fetching the
			// block. Preserve the unspecified remaining bytes and only install the
			// new tag; a dirty victim still has to be drained.
			const bool writeBack = line.dirty && line.valid;
			const cache_line evicted = line;
			line.valid = true;
			line.dirty = false;
			line.address = tag;
			if (writeBack)
				doWriteBack(index, evicted);
		}
		else if (line.valid && tag == line.address && !copyBack)
		{
			memcpy(&line.data[physAddr & 0x1f], &data, sizeof(data));
		}

		if (copyBack)
		{
			line.dirty = true;
			memcpy(&line.data[physAddr & 0x1f], &data, sizeof(data));
		}
		else
		{
			addrspace::write32(physAddr, data);
			addWriteThroughCycles(physAddr, sizeof(data));
		}
	}

	void WriteBack(u32 address, bool write_back, bool invalidate)
	{
		u32 physAddr;
		bool cached = false;
		bool copyBack;
		// OCBI is checked as a write and can raise an initial-page-write
		// exception. OCBP/OCBWB merely maintain an existing cache line and are
		// checked as reads; writing a dirty line back does not dirty the virtual
		// page.
		MmuError err = write_back
				? translateAddress<u8, MMU_TT_DREAD>(address, physAddr, cached, copyBack)
				: translateAddress<u8, MMU_TT_DWRITE>(address, physAddr, cached, copyBack);
		if (err != MmuError::NONE)
			mmu_raise_exception(err, address, write_back ? MMU_TT_DREAD : MMU_TT_DWRITE);

#ifndef STRICT_MODE
		// Cache contents are not modeled in the fast build, but the translation
		// and its architectural exceptions above still apply.
		return;
#endif
		if (!cached)
			return;

		const u32 index = lineIndex(address);
		cache_line& line = lines[index];
		const u32 tag = (physAddr >> 10) & 0x7ffff;
		if (!line.valid || tag != line.address)
			return;
		if (write_back && line.dirty)
			doWriteBack(index, line);
		line.valid = !invalidate;
		line.dirty = false;
	}

	void Prefetch(u32 address)
	{
		address &= ~0x1F;
		u32 physAddr;
		bool cached;
		bool copyBack;
		MmuError err = translateAddress<u8, MMU_TT_DREAD>(address, physAddr, cached, copyBack);
		// PREF suppresses ordinary translation and address errors, but a TLB
		// multiple hit is a reset-class exception even for this speculative
		// access.
		if (err == MmuError::TLB_MHIT)
			mmu_raise_exception(err, address, MMU_TT_DREAD);

#ifndef STRICT_MODE
		// Preserve PREF's architectural multiple-hit check without populating a
		// cache which ordinary fast-mode loads and stores do not consult.
		return;
#endif
		if (err != MmuError::NONE || !cached)
			return;

		const u32 index = lineIndex(address);
		cache_line& line = lines[index];
		const u32 tag = (physAddr >> 10) & 0x7ffff;
		if (line.valid && tag == line.address)
			return;
		const bool writeBack = line.valid && line.dirty;
		const cache_line evicted = line;
		line.address = tag;
		readCacheLine(physAddr, line);
		if (writeBack)
			doWriteBack(index, evicted);
	}

	void Invalidate()
	{
		for (auto& line : lines)
		{
			line.dirty = false;
			line.valid = false;
		}
	}

	void Reset(bool hard)
	{
		if (hard) {
			memset(&lines[0], 0, sizeof(lines));
			writeBackBufferCycles = 0;
			writeThroughBufferCycles = 0;
		}
	}

	void Serialize(Serializer& ser)
	{
		ser << lines;
		ser << writeBackBufferCycles;
		ser << writeThroughBufferCycles;
	}
	void Deserialize(Deserializer& deser)
	{
		deser >> lines;
		if (deser.version() >= Serializer::V55) {
			deser >> writeBackBufferCycles;
			deser >> writeThroughBufferCycles;
		}
		else {
			writeBackBufferCycles = 0;
			writeThroughBufferCycles = 0;
		}
	}

	u32 ReadAddressArray(u32 addr)
	{
		u32 index = (addr >> 5) & 0x1FF;
		return (u32)lines[index].valid | ((u32)lines[index].dirty << 1) | (lines[index].address << 10);
	}

	void WriteAddressArray(u32 addr, u32 data)
	{
		u32 index = (addr >> 5) & 0x1FF;
		cache_line& line = lines[index];
		bool associative = (addr & 8) != 0;
		if (!associative)
		{
			if (line.valid && line.dirty)
				doWriteBack(index, line);
			line.address = (data >> 10) & 0x7ffff;
		}
		else
		{
			u32 physAddr;
			bool cached = false;
			bool copyBack;
			MmuError err = translateAddress<u8, MMU_TT_DREAD>(data & ~0x3ff, physAddr, cached, copyBack);
			if (err == MmuError::TLB_MISS)
				// Ignore the write
				return;
			if (err != MmuError::NONE)
				mmu_raise_exception(err, data & ~0x3ff, MMU_TT_DREAD);

			u32 tag = (physAddr >> 10) & 0x7ffff;

			if (!line.valid || tag != line.address)
				// Ignore the write
				return;
			if ((data & 3) != 3 && line.dirty)
				doWriteBack(index, line);
		}
		line.valid = data & 1;
		line.dirty = (data >> 1) & 1;
	}

	u32 ReadDataArray(u32 addr)
	{
		u32 index = (addr >> 5) & 0x1FF;
		cache_line& line = lines[index];
		u32 data;
		memcpy(&data, &line.data[addr & 0x1c], sizeof(data));
		return data;
	}

	void WriteDataArray(u32 addr, u32 data)
	{
		u32 index = (addr >> 5) & 0x1FF;
		cache_line& line = lines[index];
		memcpy(&line.data[addr & 0x1c], &data, sizeof(data));
	}

	void WriteBackAll()
	{
		for (cache_line& line : lines)
		{
			if (line.valid && line.dirty)
				doWriteBack((u32)(&line - &lines[0]), line);
			line.valid = false;
			line.dirty = false;
		}
	}

private:
	struct cache_line {
		bool valid;
		bool dirty;
		u32 address;
		u8 data[32];
	};

	u32 lineIndex(u32 address)
	{
		u32 index = CCN_CCR.OIX ?
				((address >> (25 - 8)) & 0x100) | ((address >> 5) & (CCN_CCR.ORA ? 0x7f : 0xff))
				: (address >> 5) & (CCN_CCR.ORA ? 0x17f : 0x1ff);
		if (CCN_CCR.ORA && (address >> 29) == 3)
			index |= 0x80;
		return index;
	}

	void readCacheLine(u32 address, cache_line& line)
	{
		line.valid = true;
		line.dirty = false;
		const u32 line_addr = address & ~0x1f;
		sh4cycles.addReadAccessCycles(address, 32);
		u8* memPtr = GetMemPtr(line_addr, sizeof(line.data));
		if (memPtr != nullptr)
			memcpy(line.data, memPtr, sizeof(line.data));
		else
		{
			u32 *p = (u32 *)line.data;
			for (int i = 0; i < 32; i += 4)
				*p++ = addrspace::read32(line_addr + i);
		}
	}

	void doWriteBack(u32 index, const cache_line& line)
	{
		if (CCN_CCR.ORA && (index & 0x80))
			return;
		u32 line_addr = (line.address << 10) | ((index & 0x1F) << 5);
		addWriteBackCycles(line_addr);
		u8* memPtr = GetMemPtr(line_addr, sizeof(line.data));
		if (memPtr != nullptr)
			memcpy(memPtr, line.data, sizeof(line.data));
		else
		{
			u32 *p = (u32 *)line.data;
			for (int i = 0; i < 32; i += 4)
				addrspace::write32(line_addr + i, *p++);
		}
	}

	template<class T, u32 ACCESS>
	MmuError translateAddress(u32 address, u32& physAddr, bool& cached, bool& copyBack)
	{
		if (sh4DataAddressError(address, sizeof(T), ACCESS == MMU_TT_DWRITE))
			return MmuError::BADADDR;
		if (ACCESS == MMU_TT_DWRITE && (address & 0xFC000000) == 0xE0000000)
		{
			// Store queues
			u32 rv;
			MmuError lookup = mmu_full_SQ<MMU_TT_DWRITE>(address, rv);

			physAddr = address;
			return lookup;
		}
		const u32 area = address >> 29;
		const bool userMode = ctx->sr.MD == 0;

		// kernel mem protected in user mode
		if (userMode && (address & 0x80000000))
			return MmuError::BADADDR;

		cached = CCN_CCR.OCE == 1 && cachedArea(area);
		if (ACCESS == MMU_TT_DWRITE)
			// Use CCR.CB if P1 otherwise use !CCR.WT
			copyBack = area == 4 ? CCN_CCR.CB : !CCN_CCR.WT;

		if (CCN_MMUCR.AT == 0 || !translatedArea(area)
				// 7C000000 to 7FFFFFFF in P0/U0 not translated
				|| (address & 0xFC000000) == 0x7C000000)
		{
			physAddr = address;
		}
		else
		{
			const TLB_Entry *entry;
			MmuError lookup = mmu_full_lookup(address, &entry, physAddr);

			if (lookup != MmuError::NONE)
				return lookup;

			//0X  & User mode-> protection violation
			//Priv mode protection
			if (userMode)
			{
				u32 md = entry->Data.PR >> 1;
				if (md == 0)
					return MmuError::PROTECTED;
			}
			//X0 -> read only
			//X1 -> read/write , can be FW
			if (ACCESS == MMU_TT_DWRITE)
			{
				if ((entry->Data.PR & 1) == 0)
					return MmuError::PROTECTED;
				if (entry->Data.D == 0)
					return MmuError::FIRSTWRITE;
				copyBack = copyBack && entry->Data.WT == 0;
			}
			cached = cached && entry->Data.C == 1;
			if ((physAddr & 0x1C000000) == 0x1C000000)
				// map 1C000000-1FFFFFFF to P4 memory-mapped registers
				physAddr |= 0xF0000000;

		}
		return MmuError::NONE;
	}

	void addWriteBackCycles(u32 addr)
	{
		u64 now = sh4cycles.now();
		if (writeBackBufferCycles > now)
			sh4cycles.addCycles(writeBackBufferCycles - now);
		// The transfer starts only after any preceding buffered write has
		// completed.  Re-read the clock after charging that wait; using the
		// pre-wait value makes the new transfer appear to complete too early.
		writeBackBufferCycles = sh4cycles.now() + sh4cycles.writeAccessCycles(addr, 32);
	}

	void addWriteThroughCycles(u32 addr, int size)
	{
		u64 now = sh4cycles.now();
		if (writeThroughBufferCycles > now)
			sh4cycles.addCycles(writeThroughBufferCycles - now);
		int cycles = sh4cycles.writeAccessCycles(addr, std::min(size, 8));
		if (size == 32)
			sh4cycles.addCycles(cycles * 3);
		writeThroughBufferCycles = sh4cycles.now() + cycles;
	}

	std::array<cache_line, 512> lines;
	u64 writeBackBufferCycles = 0;
	u64 writeThroughBufferCycles = 0;
	Sh4Cycles sh4cycles;
	Sh4Context *ctx = nullptr;
};

extern Sh4OCache ocache;

template<class T>
T ReadCachedMem(u32 address)
{
	return ocache.ReadMem<T>(address);
}

template<class T>
void WriteCachedMem(u32 address, T data)
{
	ocache.WriteMem<T>(address, data);
}

static inline u16 IReadCachedMem(u32 address)
{
	return icache.ReadMem(address);
}
