/*
	Copyright 2019 flyinghead

	This file is part of reicast.

    reicast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    reicast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with reicast.  If not, see <https://www.gnu.org/licenses/>.
 */
#include "mmu.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_core.h"
#include "types.h"
#include "stdclass.h"

#ifdef FAST_MMU

#include "hw/sh4/sh4_mem.h"

extern TLB_Entry UTLB[64];

//#define TRACE_WINCE_SYSCALLS

#include "wince.h"

static MmuError find_entry(u32 address, const TLB_Entry **ret_entry)
{
	*ret_entry = nullptr;
	for (const TLB_Entry& entry : UTLB)
	{
		if (!mmu_match(address, entry.Address, entry.Data))
			continue;
		if (*ret_entry != nullptr)
			return MmuError::TLB_MHIT;
		*ret_entry = &entry;
	}
	return *ret_entry == nullptr ? MmuError::TLB_MISS : MmuError::NONE;
}

#if 0
static void dump_table()
{
	static int iter = 1;
	char filename[128];
	snprintf(filename, sizeof(filename), "mmutable%03d", iter++);
	FILE *f = fopen(filename, "wb");
	if (f == NULL)
		return;
	fwrite(full_table, sizeof(full_table[0]), full_table_size, f);
	fclose(f);
}

int main(int argc, char *argv[])
{
	FILE *f = fopen(argv[1], "rb");
	if (f == NULL)
	{
		perror(argv[1]);
		return 1;
	}
	full_table_size = fread(full_table, sizeof(full_table[0]), std::size(full_table), f);
	fclose(f);
	printf("Loaded %d entries\n", full_table_size);
	std::vector<u32> addrs;
	std::vector<u32> asids;
	for (int i = 0; i < full_table_size; i++)
	{
		u32 sz = full_table[i].entry.Data.SZ1 * 2 + full_table[i].entry.Data.SZ0;
		u32 mask = sz == 3 ? 1_MB : sz == 2 ? 64_KB : sz == 1 ? 4_KB : 1_KB;
		mask--;
		addrs.push_back(((full_table[i].entry.Address.VPN << 10) & mmu_mask[sz]) | (random() * mask / RAND_MAX));
		asids.push_back(full_table[i].entry.Address.ASID);
//		printf("%08x -> %08x sz %d ASID %d SH %d\n", full_table[i].entry.Address.VPN << 10, full_table[i].entry.Data.PPN << 10,
//				full_table[i].entry.Data.SZ1 * 2 + full_table[i].entry.Data.SZ0,
//				full_table[i].entry.Address.ASID, full_table[i].entry.Data.SH);
		u16 bucket = bucket_index(full_table[i].entry.Address.VPN << 10, full_table[i].entry.Data.SZ1 * 2 + full_table[i].entry.Data.SZ0);
		full_table[i].next_entry = entry_buckets[bucket];
		entry_buckets[bucket] = &full_table[i];
	}
	for (int i = 0; i < full_table_size / 10; i++)
	{
		addrs.push_back(random());
		asids.push_back(666);
	}
	u64 start = getTimeMs();
	int success = 0;
	const int loops = 100000;
	for (int i = 0; i < loops; i++)
	{
		for (int j = 0; j < addrs.size(); j++)
		{
			u32 addr = addrs[j];
			CCN_PTEH.ASID = asids[j];
			const TLB_Entry *p;
			if (find_entry(addr, &p))
				success++;
		}
	}
	u64 end = getTimeMs();
	printf("Lookup time: %f ms. Success rate %f max_len %d\n", ((double)end - start) / addrs.size(), (double)success / addrs.size() / loops, 0/*max_length*/);
}
#endif

bool UTLB_Sync(u32)
{
	// Translated dynarec accesses cache 4 KiB pages in this LUT. Any UTLB
	// mutation can invalidate one or more cached pages (including the old
	// mapping of this slot), so discard all user mappings. Do not normalize
	// VPN or PPN here: ignored large-page bits remain visible through the
	// memory-mapped TLB arrays even though lookup masks them.
	mmuAddressLUTFlush(true);

	return true;
}

void ITLB_Sync(u32 entry)
{
}

//Do a full lookup on the UTLB entry's
MmuError mmu_full_lookup(u32 va, const TLB_Entry** tlb_entry_ret, u32& rv)
{
	CCN_MMUCR.URC++;
	if (CCN_MMUCR.URB == CCN_MMUCR.URC)
		CCN_MMUCR.URC = 0;

	const TLB_Entry *localEntry;
	if (tlb_entry_ret == nullptr)
		tlb_entry_ret = &localEntry;

	MmuError lookup = find_entry(va, tlb_entry_ret);
	if (lookup == MmuError::NONE)
	{
		u32 mask = mmu_mask[(*tlb_entry_ret)->Data.SZ1 * 2 + (*tlb_entry_ret)->Data.SZ0];
		rv = (((*tlb_entry_ret)->Data.PPN << 10) & mask) | (va & ~mask);

		return MmuError::NONE;
	}
	if (lookup == MmuError::TLB_MHIT)
		return lookup;

#ifdef USE_WINCE_HACK
	// WinCE hack
	TLB_Entry& entry = UTLB[CCN_MMUCR.URC];
	if (wince_resolve_address(va, entry))
	{
		CCN_PTEL.reg_data = entry.Data.reg_data;
		CCN_PTEA.reg_data = entry.Assistance.reg_data;
		CCN_PTEH.reg_data = entry.Address.reg_data;

		*tlb_entry_ret = &entry;

		u32 sz = entry.Data.SZ1 * 2 + entry.Data.SZ0;
		rv = ((entry.Data.PPN << 10) & mmu_mask[sz])
				| (va & ~mmu_mask[sz]);

		p_sh4rcb->cntx.cycle_counter -= 164;

		return MmuError::NONE;
	}
#endif

	return MmuError::TLB_MISS;
}

template<u32 translation_type>
MmuError mmu_full_SQ(u32 va, u32& rv)
{
	if ((va & 3) || (CCN_MMUCR.SQMD == 1 && Sh4cntx.sr.MD == 0))
		return MmuError::BADADDR;

	if (!CCN_MMUCR.AT)
	{
		const u32 area = ((va >> 5) & 1 ? CCN_QACR1.Area : CCN_QACR0.Area) << 26;
		rv = area | (va & 0x03ffffe0);
		return MmuError::NONE;
	}

	const TLB_Entry *entry;
	MmuError lookup = mmu_full_lookup(va, &entry, rv);

	if (lookup != MmuError::NONE)
		return lookup;

	rv &= ~31;//lower 5 bits are forced to 0
	if (Sh4cntx.sr.MD == 0 && (entry->Data.PR >> 1) == 0)
		return MmuError::PROTECTED;
	if (translation_type == MMU_TT_DWRITE)
	{
		if ((entry->Data.PR & 1) == 0)
			return MmuError::PROTECTED;
		if (entry->Data.D == 0)
			return MmuError::FIRSTWRITE;
	}

	return MmuError::NONE;
}
template MmuError mmu_full_SQ<MMU_TT_DREAD>(u32 va, u32& rv);
template MmuError mmu_full_SQ<MMU_TT_DWRITE>(u32 va, u32& rv);

template<u32 translation_type>
MmuError mmu_data_translation(u32 va, u32& rv)
{
	if (translation_type == MMU_TT_DWRITE && (va & 0xFC000000) == 0xE0000000)
	{
		MmuError lookup = mmu_full_SQ<MMU_TT_DWRITE>(va, rv);
		if (lookup != MmuError::NONE)
			return lookup;

		rv = va;	// SQ writes are translated when the queue is written back.
		return MmuError::NONE;
	}

	if (Sh4cntx.sr.MD == 0 && (va & 0x80000000) != 0)
		return MmuError::BADADDR;

	if (fast_reg_lut[va >> 29] != 0)
	{
		rv = va;
		return MmuError::NONE;
	}

	if ((va & 0xFC000000) == 0x7C000000)
	{
		// On-chip RAM area isn't translated
		rv = va;
		return MmuError::NONE;
	}

	const TLB_Entry *entry;
	MmuError lookup = mmu_full_lookup(va, &entry, rv);
	if (lookup != MmuError::NONE)
		return lookup;

	if (Sh4cntx.sr.MD == 0 && (entry->Data.PR >> 1) == 0)
		return MmuError::PROTECTED;
	if (translation_type == MMU_TT_DWRITE)
	{
		if ((entry->Data.PR & 1) == 0)
			return MmuError::PROTECTED;
		if (entry->Data.D == 0)
			return MmuError::FIRSTWRITE;
	}

	if ((rv & 0x1C000000) == 0x1C000000)
		// map 1C000000-1FFFFFFF to P4 memory-mapped registers
		rv |= 0xF0000000;
#ifdef TRACE_WINCE_SYSCALLS
	if (unresolved_unicode_string != 0 && lookup == MmuError::NONE)
	{
		if (va == unresolved_unicode_string)
		{
			unresolved_unicode_string = 0;
			printf("RESOLVED %s\n", get_unicode_string(va).c_str());
		}
	}
#endif

	return MmuError::NONE;
}
template MmuError mmu_data_translation<MMU_TT_DREAD>(u32 va, u32& rv);
template MmuError mmu_data_translation<MMU_TT_DWRITE>(u32 va, u32& rv);

void mmu_flush_table()
{
	for (TLB_Entry& entry : ITLB)
		entry.Data.V = 0;
	for (TLB_Entry& entry : UTLB)
		entry.Data.V = 0;
	mmuAddressLUTFlush(true);
}
#endif 	// FAST_MMU
