#pragma once
#include "types.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/dyna/ngen.h"

//Translation Types
//Opcode read
#define MMU_TT_IREAD 0
//Data write
#define MMU_TT_DWRITE 1
//Data write
#define MMU_TT_DREAD 2

enum class MmuError
{
	//Translation was successful
	NONE,
	//TLB miss
	TLB_MISS,
	//TLB Multihit
	TLB_MHIT,
	//Mem is read/write protected (depends on translation type)
	PROTECTED,
	//Mem is write protected , firstwrite
	FIRSTWRITE,
	//data-Opcode read/write misaligned
	BADADDR
};

struct TLB_Entry
{
	CCN_PTEH_type Address;
	CCN_PTEL_type Data;
	CCN_PTEA_type Assistance;
};

extern TLB_Entry UTLB[64];
extern TLB_Entry ITLB[4];
extern bool mmuOn;

constexpr u32 fast_reg_lut[8] =
{
	0, 0, 0, 0	//P0-U0
	, 1		//P1
	, 1		//P2
	, 0		//P3
	, 1		//P4
};

constexpr u32 mmu_mask[4] =
{
	((0xFFFFFFFF) >> 10) << 10,	//1 kb page
	((0xFFFFFFFF) >> 12) << 12,	//4 kb page
	((0xFFFFFFFF) >> 16) << 16,	//64 kb page
	((0xFFFFFFFF) >> 20) << 20	//1 MB page
};

bool UTLB_Sync(u32 entry);
void ITLB_Sync(u32 entry);

bool mmu_match(u32 va, CCN_PTEH_type Address, CCN_PTEL_type Data);
void mmu_set_state();
void mmu_flush_table();
[[noreturn]] void mmu_raise_exception(MmuError mmu_error, u32 address, u32 am);

static inline bool mmu_enabled()
{
	return mmuOn;
}

static inline bool sh4DataAddressError(u32 vaddr, u32 size, bool write)
{
	if ((vaddr & (size - 1)) != 0)
		return true;
	if (write && size < sizeof(u32)
			&& (vaddr & 0xfc000000) == 0xe0000000)
		// Store queues accept only longword and quadword MOV stores.
		return true;

	const u32 last = vaddr + size - 1;
	if (last < vaddr)
		return true;
	if (write && (vaddr & 0xfc000000) == 0xe0000000
			&& (last & 0xfc000000) != 0xe0000000)
		return true;
	if (Sh4cntx.sr.MD != 0
			|| ((vaddr | last) & 0x80000000) == 0)
		return false;

	// User mode can write the store-queue window when SQMD is clear, but
	// cannot read it or access any other P1-P4 address.
	return !(write
			&& (vaddr & 0xfc000000) == 0xe0000000
			&& (last & 0xfc000000) == 0xe0000000
			&& CCN_MMUCR.SQMD == 0);
}

static inline bool sh4InstructionAddressError(u32 vaddr)
{
	return (vaddr & 1) != 0
			|| (Sh4cntx.sr.MD == 0 && (vaddr & 0x80000000) != 0);
}

MmuError mmu_full_lookup(u32 va, const TLB_Entry **entry, u32& rv);
MmuError mmu_instruction_lookup(u32 va, const TLB_Entry **entry, u32& rv);
template<u32 translation_type>
MmuError mmu_full_SQ(u32 va, u32& rv);

#ifdef FAST_MMU
static inline MmuError mmu_instruction_translation(u32 va, u32& rv)
{
	if (Sh4cntx.sr.MD == 0 && (va & 0x80000000) != 0)
		return MmuError::BADADDR;

	if (fast_reg_lut[va >> 29] != 0)
	{
		rv = va;
		return MmuError::NONE;
	}

	const TLB_Entry *entry;
	MmuError lookup = mmu_instruction_lookup(va, &entry, rv);
	if (lookup != MmuError::NONE)
		return lookup;

	if (Sh4cntx.sr.MD == 0 && (entry->Data.PR >> 1) == 0)
		return MmuError::PROTECTED;
	if ((rv & 0x1c000000) == 0x1c000000)
		rv |= 0xf0000000;

	return MmuError::NONE;
}
#else
MmuError mmu_instruction_translation(u32 va, u32& rv);
#endif

template<u32 translation_type>
MmuError mmu_data_translation(u32 va, u32& rv);
void DoMMUException(u32 addr, MmuError mmu_error, u32 access_type);

inline static bool mmu_is_translated(u32 va, u32 size)
{
#ifndef FAST_MMU
	if (va & (size - 1))
		return true;
#endif

	if (fast_reg_lut[va >> 29] != 0)
		return false;

	if ((va & 0xFC000000) == 0x7C000000)
		// On-chip RAM area isn't translated
		return false;

	return true;
}

template<typename T> T DYNACALL mmu_ReadMem(u32 adr);
u16 DYNACALL mmu_IReadMem16(u32 addr);

template<typename T> void DYNACALL mmu_WriteMem(u32 adr, T data);

void mmu_TranslateSQW(u32 adr, u32* out);

#ifdef FAST_MMU
// maps 4K virtual page number to physical address
extern u32 mmuAddressLUT[0x100000];
// Generated accesses must not consume this table without also performing the
// architecturally visible MMUCR.URC update for every UTLB access.

static inline void mmuAddressLUTFlush(bool full)
{
	if (full)
		memset(mmuAddressLUT, 0, sizeof(mmuAddressLUT) / 2);	// flush user memory
	else
	{
		constexpr u32 slotPages = (32 * 1024 * 1024) >> 12;
		memset(mmuAddressLUT, 0, slotPages * sizeof(u32));		// flush slot 0
	}
}
#endif

#if FEAT_SHREC == DYNAREC_JIT
static inline bool mmuDynarecCacheable(u32 vaddr)
{
#ifdef FAST_MMU
	if (vaddr >> 31 != 0)
		return false;

	// This is a host-side cacheability probe, not another guest UTLB access:
	// inspect the entries directly so MMUCR.URC advances only once.
	const TLB_Entry *match = nullptr;
	for (const TLB_Entry& entry : UTLB)
	{
		if (!mmu_match(vaddr, entry.Address, entry.Data))
			continue;
		if (match != nullptr)
			return false;
		match = &entry;
	}
	// The address LUT has one entry per 4 KiB. A 1 KiB mapping cannot
	// describe the other three translations in that slot.
	return match != nullptr && (match->Data.SZ0 != 0 || match->Data.SZ1 != 0);
#else
	return false;
#endif
}

static inline u32 DYNACALL mmuDynarecLookup(u32 vaddr, u32 write, u32 pc, u32 size)
{
	u32 paddr;
	MmuError rv;
	if (sh4DataAddressError(vaddr, size, write != 0))
		rv = MmuError::BADADDR;
	else if (!mmu_enabled())
		return vaddr;
	else if (write)
		rv = mmu_data_translation<MMU_TT_DWRITE>(vaddr, paddr);
	else
		rv = mmu_data_translation<MMU_TT_DREAD>(vaddr, paddr);
	if (unlikely(rv != MmuError::NONE))
	{
		Sh4cntx.pc = pc;
		DoMMUException(vaddr, rv, write ? MMU_TT_DWRITE : MMU_TT_DREAD);
		host_context_t ctx;
		sh4Dynarec->handleException(ctx);
		((void (*)())ctx.pc)();
		// not reached
		return 0;
	}
#ifdef FAST_MMU
	if (mmuDynarecCacheable(vaddr))
		mmuAddressLUT[vaddr >> 12] = paddr & ~0xfff;
#endif

	return paddr;
}

u64 DYNACALL mmuDynarecRead64(u32 vaddr, u32 pc);
void DYNACALL mmuDynarecWrite64(u32 vaddr, u32 low, u32 high, u32 pc);
#endif

void MMU_init();
void MMU_reset();
void MMU_term();

void mmu_serialize(Serializer& ser);
void mmu_deserialize(Deserializer& deser);
