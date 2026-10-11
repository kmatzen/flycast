/*
	Copyright 2023 flyinghead

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
#include "gtest/gtest.h"
#include "types.h"
#include "hw/mem/addrspace.h"
#include "emulator.h"
#include "hw/sh4/modules/mmu.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_cache.h"
#include "hw/sh4/sh4_interrupts.h"
#include "hw/sh4/dyna/blockmanager.h"

class MmuTest : public ::testing::Test {
protected:
	void SetUp() override
	{
		if (!addrspace::reserve())
			die("addrspace::reserve failed");
		emu.init();
		emu.dc_reset(true);
		CCN_MMUCR.AT = 1;
		MMU_reset();
	}
};

TEST_F(MmuTest, ManualResetRetainsCacheLines)
{
	constexpr u32 icAddress = 0xf0000020;
	constexpr u32 ocAddress = 0xf4000040;
	constexpr u32 icLine = 0x12345001; // tag plus valid bit
	constexpr u32 ocLine = 0x23456003; // tag plus valid and dirty bits
	icache.WriteAddressArray(icAddress, icLine);
	ocache.WriteAddressArray(ocAddress, ocLine);

	emu.dc_reset(false);

	EXPECT_EQ(icLine, icache.ReadAddressArray(icAddress));
	// O-cache tag RAM stores physical address bits 28:10. Reserved bit 29
	// reads back as zero even if it was set in the address-array write.
	EXPECT_EQ(ocLine & 0x1ffffc03, ocache.ReadAddressArray(ocAddress));
}

TEST_F(MmuTest, TestUntranslated)
{
	EXPECT_TRUE(mmu_enabled());
	u32 pa;
	// P1
	MmuError err = mmu_data_translation<MMU_TT_DREAD>(0x80000000, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0x80000000u, pa);
	err = mmu_instruction_translation(0x80000002, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0x80000002u, pa);
	// Privileged instruction fetches in P4 are not an instruction-address
	// error; only odd addresses and upper-half fetches in user mode are.
	err = mmu_instruction_translation(0xFF00008A, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0xFF00008Au, pa);

	// P2
	err = mmu_data_translation<MMU_TT_DWRITE>(0xA0001234, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0xA0001234u, pa);

	// P4
	err = mmu_data_translation<MMU_TT_DREAD>(0xFF0000CC, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0xFF0000CCu, pa);

	// 7C000000 to 7FFFFFFF in P0/U0 not translated
	err = mmu_data_translation<MMU_TT_DREAD>(0x7D000088, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0x7D000088u, pa);

	// SQ write
	UTLB[0].Address.VPN = 0xE2000000 >> 10;
	UTLB[0].Data.SZ0 = 1;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.PR = 3;
	UTLB[0].Data.D = 1;
	UTLB_Sync(0);
	err = mmu_data_translation<MMU_TT_DWRITE>(0xE2000004, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0xE2000004, pa);
}

TEST_F(MmuTest, AtBitEnablesCpuTranslationWithoutGuestHeuristics)
{
	EXPECT_TRUE(mmu_enabled());
	EXPECT_THROW(ReadMem32(0x02000000), SH4ThrownException);
	EXPECT_EQ(0x02000000u, CCN_TEA);
}

TEST_F(MmuTest, TestTranslated)
{
	u32 pa;
	// U0
	UTLB[0].Address.VPN = 0x02000000 >> 10;
	UTLB[0].Data.SZ0 = 1;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.PR = 3;
	UTLB[0].Data.D = 1;
	UTLB[0].Data.PPN = 0x0C000000 >> 10;
	UTLB_Sync(0);
	MmuError err = mmu_data_translation<MMU_TT_DREAD>(0x02000044, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0x0C000044u, pa);

	err = mmu_data_translation<MMU_TT_DWRITE>(0x02000045, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0x0C000045u, pa);

	err = mmu_instruction_translation(0x02000046, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0x0C000046u, pa);

	// ASID match
	UTLB[0].Address.ASID = 13;
	CCN_PTEH.ASID = 13;
	UTLB_Sync(0);
	err = mmu_data_translation<MMU_TT_DWRITE>(0x02000222, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0x0C000222u, pa);
	err = mmu_instruction_translation(0x02000232, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0x0C000232u, pa);

	// Shared entry
	UTLB[0].Data.SH = 1;
	CCN_PTEH.ASID = 14;
	UTLB_Sync(0);
	err = mmu_data_translation<MMU_TT_DWRITE>(0x02000222, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0x0C000222u, pa);

	// 1C000000-1FFFFFF mapped to P4
	UTLB[0].Data.PPN = 0x1C000000 >> 10;
	UTLB_Sync(0);
	err = mmu_data_translation<MMU_TT_DWRITE>(0x02000222, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0xFC000222u, pa);
	memset(ITLB, 0, sizeof(ITLB));
	err = mmu_instruction_translation(0x02000222, pa);
	ASSERT_EQ(MmuError::NONE, err);
	ASSERT_EQ(0xFC000222u, pa);
}

TEST_F(MmuTest, TestMiss)
{
	u32 pa;
	UTLB[0].Address.VPN = 0x02000000 >> 10;
	UTLB[0].Data.SZ0 = 1;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.PR = 3;
	UTLB[0].Data.D = 1;
	UTLB[0].Data.PPN = 0x0C000000 >> 10;
	UTLB_Sync(0);
	// no match
	MmuError err = mmu_data_translation<MMU_TT_DREAD>(0x02100044, pa);
	ASSERT_EQ(MmuError::TLB_MISS, err);

	// entry not valid
	UTLB[0].Data.V = 0;
	UTLB_Sync(0);
	err = mmu_data_translation<MMU_TT_DREAD>(0x02000044, pa);
	ASSERT_EQ(MmuError::TLB_MISS, err);

	// asid mismatch
	UTLB[0].Data.V = 1;
	UTLB[0].Address.ASID = 13;
	CCN_PTEH.ASID = 14;
	UTLB_Sync(0);
	err = mmu_data_translation<MMU_TT_DREAD>(0x02000044, pa);
	ASSERT_EQ(MmuError::TLB_MISS, err);
}

TEST_F(MmuTest, AllPageSizes)
{
	struct PageCase {
		u32 size;
		u32 offset;
	};
	const PageCase cases[] = {
		{ 0, 0x234 },	// 1 KiB
		{ 1, 0xabc },	// 4 KiB
		{ 2, 0x5678 },	// 64 KiB
		{ 3, 0xabcde },	// 1 MiB
	};

	for (const PageCase& page : cases)
	{
		MMU_reset();
		const u32 mask = mmu_mask[page.size];
		const u32 virtualBase = 0x02000000 & mask;
		const u32 physicalBase = 0x0c000000 & mask;
		UTLB[0].Address.VPN = virtualBase >> 10;
		UTLB[0].Data.PPN = physicalBase >> 10;
		UTLB[0].Data.SZ0 = page.size & 1;
		UTLB[0].Data.SZ1 = page.size >> 1;
		UTLB[0].Data.V = 1;
		UTLB[0].Data.PR = 3;
		UTLB[0].Data.D = 1;
		UTLB_Sync(0);

		u32 pa;
		ASSERT_EQ(MmuError::NONE,
				mmu_data_translation<MMU_TT_DREAD>(virtualBase + page.offset, pa));
		EXPECT_EQ(physicalBase + page.offset, pa);
	}
}

TEST_F(MmuTest, UtlbLookupAdvancesReplacementCounter)
{
	CCN_MMUCR.URB = 63;
	CCN_MMUCR.URC = 4;
	u32 pa;
	EXPECT_EQ(MmuError::TLB_MISS,
			mmu_full_lookup(0x02000000, nullptr, pa));
	EXPECT_EQ(5u, CCN_MMUCR.URC);
}

#if FEAT_SHREC == DYNAREC_JIT
TEST_F(MmuTest, DynarecLookupAdvancesReplacementCounter)
{
	constexpr u32 virtualAddress = 0x02000040;
	UTLB[0].Address.VPN = virtualAddress >> 10;
	UTLB[0].Data.PPN = 0x0c000000 >> 10;
	UTLB[0].Data.SZ0 = 1;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.PR = 3;
	UTLB[0].Data.D = 1;
	UTLB_Sync(0);
	mmuOn = true;
	CCN_MMUCR.URB = 63;
	CCN_MMUCR.URC = 4;

	EXPECT_EQ(0x0c000040u,
			mmuDynarecLookup(virtualAddress, false, 0x8c010000, sizeof(u32)));
	EXPECT_EQ(5u, CCN_MMUCR.URC);
}

TEST_F(MmuTest, DynarecDispatchHandlesFaultingExceptionVector)
{
	Sh4cntx.pc = 0x04000000;
	Sh4cntx.vbr = 0x02000000;
	Sh4cntx.sr.BL = 0;
	Sh4cntx.sr.MD = 1;

	// Both the requested PC and the normal TLB-miss vector lack mappings.
	// The second exception is blocked and therefore performs a manual reset;
	// dispatch must then translate the fixed P2 reset vector.
	(void)bm_GetCodeByVAddr(Sh4cntx.pc);

	EXPECT_EQ(Sh4Ex_ManualReset, CCN_EXPEVT);
	EXPECT_EQ(0xa0000000u, Sh4cntx.pc);
}

TEST_F(MmuTest, DynarecDispatchHandlesInvalidNoMmuExceptionVector)
{
	CCN_MMUCR.AT = 0;
	mmu_set_state();
	Sh4cntx.pc = 1;
	Sh4cntx.vbr = 0x8c000001; // keeps the first exception vector misaligned
	Sh4cntx.sr.BL = 0;
	Sh4cntx.sr.MD = 1;

	(void)bm_GetCodeByVAddr(Sh4cntx.pc);

	EXPECT_EQ(Sh4Ex_ManualReset, CCN_EXPEVT);
	EXPECT_EQ(0xa0000000u, Sh4cntx.pc);

	CCN_MMUCR.AT = 1;
	mmu_set_state();
}
#endif

TEST_F(MmuTest, LookupMasksIgnoredPpnBits)
{
	constexpr u32 virtualAddress = 0x02045678;
	UTLB[0].Address.VPN = 0x020abc00 >> 10;
	UTLB[0].Data.PPN = 0x0c0abc00 >> 10;
	UTLB[0].Data.SZ0 = 1;
	UTLB[0].Data.SZ1 = 1; // 1 MiB
	UTLB[0].Data.V = 1;
	const u32 storedVpn = UTLB[0].Address.VPN;
	const u32 storedPpn = UTLB[0].Data.PPN;
	UTLB_Sync(0);
	EXPECT_EQ(storedVpn, UTLB[0].Address.VPN);
	EXPECT_EQ(storedPpn, UTLB[0].Data.PPN);

	u32 pa;
	EXPECT_EQ(MmuError::NONE,
			mmu_full_lookup(virtualAddress, nullptr, pa));
	EXPECT_EQ(0x0c045678u, pa);
}

TEST_F(MmuTest, FlushInvalidatesGuestTlbEntries)
{
	UTLB[0].Data.V = 1;
	ITLB[0].Data.V = 1;
#ifdef FAST_MMU
	constexpr u32 virtualAddress = 0x02000000;
	mmuAddressLUT[virtualAddress >> 12] = 0x0c000000;
#endif

	mmu_flush_table();

	EXPECT_EQ(0u, UTLB[0].Data.V);
	EXPECT_EQ(0u, ITLB[0].Data.V);
#ifdef FAST_MMU
	EXPECT_EQ(0u, mmuAddressLUT[virtualAddress >> 12]);
#endif
}

TEST_F(MmuTest, TlbArraysIgnoreNonLongwordAccesses)
{
	ITLB[0].Address.reg_data = 0x12345012;
	ITLB[0].Data.reg_data = 0x0c0001f4;
	UTLB[0].Address.reg_data = 0x23456034;
	UTLB[0].Data.reg_data = 0x0d0001f4;

	addrspace::write8(0xf2000000, 0xff);
	addrspace::write16(0xf3000000, 0xffff);
	addrspace::write8(0xf6000000, 0xff);
	addrspace::write16(0xf7000000, 0xffff);

	EXPECT_EQ(0x12345012u, ITLB[0].Address.reg_data);
	EXPECT_EQ(0x0c0001f4u, ITLB[0].Data.reg_data);
	EXPECT_EQ(0x23456034u, UTLB[0].Address.reg_data);
	EXPECT_EQ(0x0d0001f4u, UTLB[0].Data.reg_data);
	EXPECT_EQ(0u, addrspace::read8(0xf2000000));
	EXPECT_EQ(0u, addrspace::read16(0xf3000000));
	EXPECT_EQ(0u, addrspace::read8(0xf6000000));
	EXPECT_EQ(0u, addrspace::read16(0xf7000000));
}

TEST_F(MmuTest, TlbDataArrayTwoReadsAssistanceData)
{
	ITLB[2].Data.reg_data = 0x0c0001f4;
	ITLB[2].Assistance.reg_data = 0xb;
	UTLB[37].Data.reg_data = 0x0d0001f4;
	UTLB[37].Assistance.reg_data = 0xd;

	EXPECT_EQ(0x0c0001f4u & 0x1ffffddau, addrspace::read32(0xf3000200));
	EXPECT_EQ(0xbu, addrspace::read32(0xf3800200));
	EXPECT_EQ(0x0d0001f4u, addrspace::read32(0xf7002500));
	EXPECT_EQ(0xdu, addrspace::read32(0xf7802500));
}

TEST_F(MmuTest, TlbDataArraysMaskReservedFields)
{
	addrspace::write32(0xf3000000, 0xffffffff);
	addrspace::write32(0xf7000000, 0xffffffff);
	addrspace::write32(0xf3800000, 0xffffffff);
	addrspace::write32(0xf7800000, 0xffffffff);

	EXPECT_EQ(0x1ffffddau, ITLB[0].Data.reg_data);
	EXPECT_EQ(0x1ffffdffu, UTLB[0].Data.reg_data);
	EXPECT_EQ(0xfu, ITLB[0].Assistance.reg_data);
	EXPECT_EQ(0xfu, UTLB[0].Assistance.reg_data);
	EXPECT_EQ(0x1ffffddau, addrspace::read32(0xf3000000));
	EXPECT_EQ(0x1ffffdffu, addrspace::read32(0xf7000000));
}

TEST_F(MmuTest, TlbAddressArrayReadsMaskReservedFields)
{
	ITLB[0].Address.reg_data = 0xffffffff;
	ITLB[0].Data.V = 0;
	UTLB[0].Address.reg_data = 0xffffffff;
	UTLB[0].Data.V = 0;
	UTLB[0].Data.D = 0;

	EXPECT_EQ(0xfffffcffu, addrspace::read32(0xf2000000));
	EXPECT_EQ(0xfffffcffu, addrspace::read32(0xf6000000));
}

TEST_F(MmuTest, AssociativeUtlbWriteIsAtomicOnMultipleHit)
{
	constexpr u32 virtualAddress = 0x02000000;
	for (u32 i = 0; i < 2; i++)
	{
		UTLB[i].Address.VPN = virtualAddress >> 10;
		UTLB[i].Data.V = 1;
		UTLB[i].Data.D = 1;
		UTLB[i].Data.SZ0 = 1;
	}
	const u32 data = virtualAddress | 0x100; // V=1, D=0

	EXPECT_THROW(addrspace::write32(0xf6000080, data), SH4ThrownException);
	EXPECT_EQ(1u, UTLB[0].Data.D);
	EXPECT_EQ(1u, UTLB[1].Data.D);
	EXPECT_EQ(virtualAddress, CCN_TEA);
	EXPECT_EQ(virtualAddress >> 10, CCN_PTEH.VPN);
}

TEST_F(MmuTest, TlbMultipleHitUsesResetVector)
{
	Sh4cntx.vbr = 0x8c010000;
	Sh4cntx.spc = 0x12345678;
	Sh4cntx.gbr = 0x23456789;
	Sh4cntx.dbr = 0x3456789a;
	Sh4cntx.fpscr.full = 0x003fffff;
	Sh4cntx.old_fpscr = Sh4cntx.fpscr;
	CCN_TEA = 0x456789ab;
	CCN_BASRA = 0x5a;
	CCN_BASRB = 0xa5;
	UBC_BARA = 0x56789abc;
	UBC_BRCR = 0xc481;
	BSC_BCR1.full = 0x6789abcd;
	CPG_STBCR = 0x68;
	TMU_TCPR2 = 0x789abcde;
	CCN_CCR.ORA = 1;
	addrspace::write32(0x7c000000, 0x89abcdef);
	CCN_MMUCR.AT = 1;
	UTLB[3].Data.V = 1;
	Sh4cntx.sr.setFull(0x10008303); // reset-class faults are accepted with BL set

	Do_Exception(0x8c020000, Sh4Ex_TlbMultiHit);

	EXPECT_EQ(Sh4Ex_TlbMultiHit, CCN_EXPEVT);
	EXPECT_EQ(0u, Sh4cntx.vbr);
	EXPECT_EQ(0xa0000000u, Sh4cntx.pc);
	EXPECT_EQ(0x12345678u, Sh4cntx.spc);
	EXPECT_EQ(0x700003f3u, Sh4cntx.sr.getFull());
	EXPECT_EQ(0x00040001u, Sh4cntx.fpscr.full);
	EXPECT_EQ(0x23456789u, Sh4cntx.gbr);
	EXPECT_EQ(0x3456789au, Sh4cntx.dbr);
	EXPECT_EQ(0x456789abu, CCN_TEA);
	EXPECT_EQ(0x5au, CCN_BASRA);
	EXPECT_EQ(0xa5u, CCN_BASRB);
	EXPECT_EQ(0x56789abcu, UBC_BARA);
	EXPECT_EQ(0xc481u, UBC_BRCR);
	EXPECT_EQ(0x6789abcdu, BSC_BCR1.full);
	EXPECT_EQ(0x68u, CPG_STBCR);
	EXPECT_EQ(0x789abcdeu, TMU_TCPR2);
	CCN_CCR.ORA = 1;
	EXPECT_EQ(0x89abcdefu, addrspace::read32(0x7c000000));
	EXPECT_EQ(0u, CCN_MMUCR.reg_data);
	EXPECT_EQ(0u, UTLB[3].Data.V);
}

TEST_F(MmuTest, BlockedGeneralExceptionCausesManualReset)
{
	Sh4cntx.vbr = 0x8c010000;
	Sh4cntx.spc = 0x12345678;
	Sh4cntx.gbr = 0x23456789;
	Sh4cntx.dbr = 0x3456789a;
	Sh4cntx.fpscr.full = 0x003fffff;
	Sh4cntx.old_fpscr = Sh4cntx.fpscr;
	CCN_TEA = 0x456789ab;
	CCN_BASRA = 0x5a;
	CCN_BASRB = 0xa5;
	UBC_BARA = 0x56789abc;
	UBC_BRCR = 0xc481;
	BSC_BCR1.full = 0x6789abcd;
	CPG_STBCR = 0x68;
	TMU_TCPR2 = 0x789abcde;
	CCN_MMUCR.AT = 1;
	UTLB[3].Data.V = 1;
	Sh4cntx.sr.setFull(0x10008303);

	Do_Exception(0x8c020000, Sh4Ex_AddressErrorRead);

	EXPECT_EQ(Sh4Ex_ManualReset, CCN_EXPEVT);
	EXPECT_EQ(0u, Sh4cntx.vbr);
	EXPECT_EQ(0xa0000000u, Sh4cntx.pc);
	EXPECT_EQ(0x12345678u, Sh4cntx.spc);
	EXPECT_EQ(0x700003f3u, Sh4cntx.sr.getFull());
	EXPECT_EQ(0x00040001u, Sh4cntx.fpscr.full);
	EXPECT_EQ(0x23456789u, Sh4cntx.gbr);
	EXPECT_EQ(0x3456789au, Sh4cntx.dbr);
	EXPECT_EQ(0x456789abu, CCN_TEA);
	EXPECT_EQ(0x5au, CCN_BASRA);
	EXPECT_EQ(0xa5u, CCN_BASRB);
	EXPECT_EQ(0x56789abcu, UBC_BARA);
	EXPECT_EQ(0xc481u, UBC_BRCR);
	EXPECT_EQ(0x6789abcdu, BSC_BCR1.full);
	EXPECT_EQ(0x68u, CPG_STBCR);
	EXPECT_EQ(0x789abcdeu, TMU_TCPR2);
	EXPECT_EQ(0u, CCN_MMUCR.reg_data);
	EXPECT_EQ(0u, UTLB[3].Data.V);
}

TEST_F(MmuTest, AssociativeUtlbWriteUpdatesOnlyItlbValidity)
{
	constexpr u32 virtualAddress = 0x02000000;
	UTLB[0].Address.VPN = virtualAddress >> 10;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.D = 1;
	UTLB[0].Data.SZ0 = 1;
	ITLB[0] = UTLB[0];
	ITLB[0].Data.D = 1;

	addrspace::write32(0xf6000080, virtualAddress); // clear V and D

	EXPECT_EQ(0u, UTLB[0].Data.V);
	EXPECT_EQ(0u, UTLB[0].Data.D);
	EXPECT_EQ(0u, ITLB[0].Data.V);
	EXPECT_EQ(1u, ITLB[0].Data.D);
}

TEST_F(MmuTest, AssociativeUtlbWriteUsesPtehAsid)
{
	constexpr u32 virtualAddress = 0x02000000;
	UTLB[0].Address.VPN = virtualAddress >> 10;
	UTLB[0].Address.ASID = 7;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.D = 1;
	UTLB[0].Data.SZ0 = 1;
	CCN_PTEH.ASID = 7;

	// Bits 7:0 in the associative write value do not supply the comparison
	// ASID; the hardware uses PTEH.ASID. Deliberately write a different ASID.
	addrspace::write32(0xf6000080, virtualAddress | 9);

	EXPECT_EQ(0u, UTLB[0].Data.V);
	EXPECT_EQ(0u, UTLB[0].Data.D);
}

TEST_F(MmuTest, DoublewordAccessRequiresQuadwordAlignment)
{
	constexpr u32 virtualBase = 0x02000000;
	constexpr u32 physicalBase = 0x0c000000;
	UTLB[0].Address.VPN = virtualBase >> 10;
	UTLB[0].Data.PPN = physicalBase >> 10;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.PR = 3;
	UTLB[0].Data.D = 1;
	UTLB_Sync(0);

	EXPECT_THROW(mmu_ReadMem<u64>(virtualBase + 0x3fc), SH4ThrownException);
	EXPECT_THROW(mmu_WriteMem<u64>(virtualBase + 0x3fc,
			0xfedcba9876543210ull), SH4ThrownException);

	addrspace::write32(physicalBase + 0x3f8, 0x44332211);
	addrspace::write32(physicalBase + 0x3fc, 0x88776655);
	CCN_MMUCR.URB = 63;
	CCN_MMUCR.URC = 4;
	EXPECT_EQ(0x8877665544332211ull, mmu_ReadMem<u64>(virtualBase + 0x3f8));
	EXPECT_EQ(5u, CCN_MMUCR.URC);
#if FEAT_SHREC == DYNAREC_JIT
	CCN_MMUCR.URC = 4;
	EXPECT_EQ(0x8877665544332211ull,
			mmuDynarecRead64(virtualBase + 0x3f8, 0x8c010000));
	EXPECT_EQ(5u, CCN_MMUCR.URC);
	CCN_MMUCR.URC = 4;
	mmuDynarecWrite64(virtualBase + 0x3f8, 0x89abcdef, 0x01234567,
			0x8c010000);
	EXPECT_EQ(5u, CCN_MMUCR.URC);
	EXPECT_EQ(0x89abcdefu, addrspace::read32(physicalBase + 0x3f8));
	EXPECT_EQ(0x01234567u, addrspace::read32(physicalBase + 0x3fc));
#endif
}

#ifdef STRICT_MODE
TEST_F(MmuTest, CachedQuadwordUsesOneTranslation)
{
	constexpr u32 virtualAddress = 0x020003f8;
	constexpr u32 physicalAddress = 0x0c0003f8;
	UTLB[0].Address.VPN = virtualAddress >> 10;
	UTLB[0].Data.PPN = physicalAddress >> 10;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.PR = 3;
	UTLB[0].Data.D = 1;
	UTLB[0].Data.C = 1;
	UTLB_Sync(0);
	CCN_CCR.OCE = 1;
	CCN_MMUCR.URB = 63;
	CCN_MMUCR.URC = 4;
	addrspace::write32(physicalAddress, 0x44332211);
	addrspace::write32(physicalAddress + 4, 0x88776655);

	EXPECT_EQ(0x8877665544332211ull, ocache.ReadMem<u64>(virtualAddress));
	EXPECT_EQ(5u, CCN_MMUCR.URC);
}

TEST_F(MmuTest, CachedStoreQueueRejectsByteAndWordStores)
{
	p_sh4rcb->cntx.sr.MD = 1;
	EXPECT_THROW(ocache.WriteMem<u8>(0xe0000000, 1), SH4ThrownException);
	EXPECT_THROW(ocache.WriteMem<u16>(0xe0000000, 1), SH4ThrownException);
}
#endif

TEST_F(MmuTest, StoreQueueTranslationSupportsAllPageSizes)
{
	constexpr u32 virtualBase = 0xe0000000;
	constexpr u32 physicalBase = 0x0c000000;
	constexpr u32 offsets[] = { 0x220, 0x8a0, 0x5620, 0xabcc0 };

	for (u32 size = 0; size < std::size(offsets); size++)
	{
		CCN_MMUCR.AT = 1;
		MMU_reset();
		const u32 mask = mmu_mask[size];
		UTLB[0].Address.VPN = (virtualBase & mask) >> 10;
		UTLB[0].Data.PPN = (physicalBase & mask) >> 10;
		UTLB[0].Data.SZ0 = size & 1;
		UTLB[0].Data.SZ1 = size >> 1;
		UTLB[0].Data.V = 1;
		UTLB[0].Data.PR = 3;
		UTLB[0].Data.D = 1;
		UTLB_Sync(0);

		u32 translated;
		mmu_TranslateSQW(virtualBase + offsets[size], &translated);
		EXPECT_EQ((physicalBase & mask) + offsets[size], translated);
		EXPECT_TRUE(mmu_enabled());

		UTLB[0].Data.V = 0;
		UTLB_Sync(0);
		const TLB_Entry *entry = nullptr;
		u32 lookupAddress = 0;
		EXPECT_EQ(MmuError::TLB_MISS,
				mmu_full_lookup(virtualBase + offsets[size], &entry, lookupAddress));
		EXPECT_THROW(mmu_TranslateSQW(virtualBase + offsets[size], &translated),
				SH4ThrownException);
	}
}

TEST_F(MmuTest, StoreQueuesUseTheirOwnQacrArea)
{
	CCN_MMUCR.AT = 0;
	CCN_QACR0.Area = 0;
	CCN_QACR1.Area = 3;
	setSqwHandler();

	constexpr u32 destination = 0xe0000020; // store queue 1
	constexpr u32 physical = 0x0c000020;
	u32 *queue = reinterpret_cast<u32 *>(Sh4cntx.sq_buffer[1].data);
	for (u32 i = 0; i < sizeof(SQBuffer) / sizeof(u32); i++)
	{
		queue[i] = 0x12340000 + i;
		addrspace::write32(physical + i * sizeof(u32), 0);
	}

	Sh4cntx.cycle_counter = 100;
	Sh4cntx.doSqWrite(destination, &Sh4cntx);
	EXPECT_LT(Sh4cntx.cycle_counter, 100);
	for (u32 i = 0; i < sizeof(SQBuffer) / sizeof(u32); i++)
		EXPECT_EQ(0x12340000u + i,
				addrspace::read32(physical + i * sizeof(u32)));
}

TEST_F(MmuTest, SharedAndPrivilegedSingleVirtualModeEntries)
{
	UTLB[0].Address.VPN = 0x02000000 >> 10;
	UTLB[0].Address.ASID = 7;
	UTLB[0].Data.PPN = 0x0c000000 >> 10;
	UTLB[0].Data.SZ0 = 1;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.PR = 3;
	UTLB[0].Data.D = 1;
	CCN_PTEH.ASID = 9;
	UTLB_Sync(0);

	u32 pa;
	EXPECT_EQ(MmuError::TLB_MISS,
			mmu_data_translation<MMU_TT_DREAD>(0x02000040, pa));
	UTLB[0].Data.SH = 1;
	EXPECT_EQ(MmuError::NONE,
			mmu_data_translation<MMU_TT_DREAD>(0x02000040, pa));

	UTLB[0].Data.SH = 0;
	p_sh4rcb->cntx.sr.MD = 1;
	CCN_MMUCR.SV = 1;
	EXPECT_EQ(MmuError::NONE,
			mmu_data_translation<MMU_TT_DREAD>(0x02000040, pa));
}

#ifdef FAST_MMU
TEST_F(MmuTest, UtlbChangesInvalidateDynarecTranslations)
{
	constexpr u32 virtualAddress = 0x02000000;
	mmuAddressLUT[virtualAddress >> 12] = 0x0c000000;
	UTLB[0].Address.VPN = virtualAddress >> 10;
	UTLB[0].Data.SZ0 = 1;
	UTLB[0].Data.V = 1;
	UTLB_Sync(0);
	EXPECT_EQ(0u, mmuAddressLUT[virtualAddress >> 12]);
}

#if FEAT_SHREC == DYNAREC_JIT
TEST_F(MmuTest, OneKiBPagesAreNotDynarecCacheable)
{
	constexpr u32 virtualAddress = 0x02000400;
	UTLB[0].Address.VPN = virtualAddress >> 10;
	UTLB[0].Data.PPN = 0x0c000800 >> 10;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.PR = 3;
	UTLB[0].Data.D = 1;
	UTLB_Sync(0);
	EXPECT_FALSE(mmuDynarecCacheable(virtualAddress));

	UTLB[0].Data.SZ0 = 1; // 4 KiB
	UTLB_Sync(0);
	EXPECT_TRUE(mmuDynarecCacheable(virtualAddress));
}
#endif

TEST_F(MmuTest, TranslationContextChangesInvalidateDynarecTranslations)
{
	constexpr u32 virtualAddress = 0x04000000;
	mmuOn = true;

	mmuAddressLUT[virtualAddress >> 12] = 0x0c000000;
	p_sh4rcb->cntx.old_sr.MD = 1;
	p_sh4rcb->cntx.sr.MD = 0;
	UpdateSR();
	EXPECT_EQ(0u, mmuAddressLUT[virtualAddress >> 12]);

	mmuAddressLUT[virtualAddress >> 12] = 0x0c000000;
	addrspace::write32(0xff000000, CCN_PTEH.reg_data ^ 1); // ASID change
	EXPECT_EQ(0u, mmuAddressLUT[virtualAddress >> 12]);

	mmuAddressLUT[virtualAddress >> 12] = 0x0c000000;
	CCN_MMUCR_type mmucr = CCN_MMUCR;
	mmucr.SV ^= 1;
	addrspace::write32(0xff000010, mmucr.reg_data);
	EXPECT_EQ(0u, mmuAddressLUT[virtualAddress >> 12]);
}
#endif

TEST_F(MmuTest, TestErrors)
{
	u32 pa;
	MmuError err;

	p_sh4rcb->cntx.sr.MD = 1;
	EXPECT_FALSE(sh4InstructionAddressError(0xff00008a));
	EXPECT_TRUE(sh4InstructionAddressError(0xff00008b));
	EXPECT_TRUE(sh4DataAddressError(0xfffffffcu, sizeof(u64), false));
	EXPECT_TRUE(sh4DataAddressError(0xfffffffcu, sizeof(u64), true));
	EXPECT_TRUE(sh4DataAddressError(0xe0000000, sizeof(u8), true));
	EXPECT_TRUE(sh4DataAddressError(0xe0000000, sizeof(u16), true));
	EXPECT_FALSE(sh4DataAddressError(0xe0000000, sizeof(u32), true));
	EXPECT_FALSE(sh4DataAddressError(0xe0000000, sizeof(u64), true));
	EXPECT_TRUE(sh4DataAddressError(0xe3fffffc, sizeof(u64), true));

	// unaligned address
	EXPECT_THROW(mmu_IReadMem16(0xFF00008B), SH4ThrownException);
	EXPECT_THROW(mmu_ReadMem<u32>(0x80000046), SH4ThrownException);
	EXPECT_THROW(mmu_ReadMem<u64>(0x80000044), SH4ThrownException);
	EXPECT_THROW(mmu_ReadMem<u16>(0x80000045), SH4ThrownException);

	// Protection violation
	p_sh4rcb->cntx.sr.MD = 0;
	UTLB[0].Address.VPN = 0x04000000 >> 10;
	UTLB[0].Data.SZ0 = 1;
	UTLB[0].Data.V = 1;
	UTLB[0].Data.PR = 0;
	UTLB[0].Data.D = 1;
	UTLB[0].Data.PPN = 0x0A000000 >> 10;
	// no access in user mode
	err = mmu_data_translation<MMU_TT_DREAD>(0x04000040, pa);
	ASSERT_EQ(MmuError::PROTECTED, err);
	err = mmu_data_translation<MMU_TT_DWRITE>(0x04000040, pa);
	ASSERT_EQ(MmuError::PROTECTED, err);
	err = mmu_instruction_translation(0x04000042, pa);
	ASSERT_EQ(MmuError::PROTECTED, err);
	// read-only access in priv mode
	p_sh4rcb->cntx.sr.MD = 1;
	err = mmu_data_translation<MMU_TT_DREAD>(0x04000040, pa);
	ASSERT_EQ(MmuError::NONE, err);
	err = mmu_data_translation<MMU_TT_DWRITE>(0x04000040, pa);
	ASSERT_EQ(MmuError::PROTECTED, err);
	// read-only access in user & priv mode
	UTLB[0].Data.PR = 2;
	p_sh4rcb->cntx.sr.MD = 0;
	err = mmu_data_translation<MMU_TT_DWRITE>(0x04000040, pa);
	ASSERT_EQ(MmuError::PROTECTED, err);
	p_sh4rcb->cntx.sr.MD = 1;
	err = mmu_data_translation<MMU_TT_DWRITE>(0x04000040, pa);
	ASSERT_EQ(MmuError::PROTECTED, err);
	UTLB[0].Data.PR = 3;

	// kernel address in user mode
	p_sh4rcb->cntx.sr.MD = 0;
	EXPECT_TRUE(sh4InstructionAddressError(0xff00008a));
	err = mmu_data_translation<MMU_TT_DWRITE>(0xA4000004, pa);
	ASSERT_EQ(MmuError::BADADDR, err);
	err = mmu_instruction_translation(0xA4000006, pa);
	ASSERT_EQ(MmuError::BADADDR, err);

	// multiple hits
	memset(ITLB, 0, sizeof(ITLB));
	UTLB[1].Address.VPN = 0x04000000 >> 10;
	UTLB[1].Data.SZ1 = 1;
	UTLB[1].Data.V = 1;
	UTLB[1].Data.PR = 3;
	UTLB[1].Data.D = 1;
	UTLB[1].Data.PPN = 0x0C000000 >> 10;
	err = mmu_data_translation<MMU_TT_DREAD>(0x04000040, pa);
	ASSERT_EQ(MmuError::TLB_MHIT, err);
	err = mmu_instruction_translation(0x04000042, pa);
	ASSERT_EQ(MmuError::TLB_MHIT, err);
	UTLB[1].Data.V = 0;

	// first write
	UTLB[0].Data.D = 0;
	err = mmu_data_translation<MMU_TT_DWRITE>(0x04000224, pa);
	ASSERT_EQ(MmuError::FIRSTWRITE, err);
}

TEST_F(MmuTest, NoMmuCpuHandlersStillValidateAddresses)
{
	mmuOn = false;
	SetMemoryHandlers();
	p_sh4rcb->cntx.sr.MD = 1;
	EXPECT_THROW(ReadMem32(0x0c000002), SH4ThrownException);
	EXPECT_THROW(WriteMem8(0xe0000000, 1), SH4ThrownException);
	EXPECT_THROW(WriteMem16(0xe0000000, 1), SH4ThrownException);
	EXPECT_NO_THROW((void)IReadMem16(0xf0000000));

	p_sh4rcb->cntx.sr.MD = 0;
	EXPECT_THROW(ReadMem32(0x8c000000), SH4ThrownException);
	EXPECT_THROW(IReadMem16(0xe0000000), SH4ThrownException);

	mmuOn = true;
	SetMemoryHandlers();
}

TEST_F(MmuTest, OperandCacheWriteThroughBufferTiming)
{
	Sh4Context& ctx = p_sh4rcb->cntx;
	ocache.init(&ctx);
	ocache.Reset(true);
	CCN_CCR.OCE = 0;
	ctx.cycle_counter = SH4_TIMESLICE;

	// An uncached write is handed to the write-through buffer immediately.
	// Each following write must wait for the preceding SDRAM transfer before
	// it can enter the buffer. The default BSC configuration takes 14 SH-4
	// cycles for this longword transfer.
	ocache.WriteMem<u32>(0xAC000000, 1);
	EXPECT_EQ(SH4_TIMESLICE, ctx.cycle_counter);
	ocache.WriteMem<u32>(0xAC000004, 2);
	EXPECT_EQ(SH4_TIMESLICE - 14, ctx.cycle_counter);
	ocache.WriteMem<u32>(0xAC000008, 3);
	EXPECT_EQ(SH4_TIMESLICE - 28, ctx.cycle_counter);
}

TEST_F(MmuTest, AssociativeOperandCacheInvalidationWritesBackDirtyLine)
{
	constexpr u32 physicalAddress = 0x0c000000;
	constexpr u32 value = 0x89abcdef;
	CCN_MMUCR.AT = 0;
	ocache.Reset(true);
	addrspace::write32(physicalAddress, 0);

	// Install a valid, dirty line and populate its first longword through the
	// memory-mapped OC arrays.
	addrspace::write32(0xf4000000, physicalAddress | 3);
	addrspace::write32(0xf5000000, value);
	EXPECT_EQ(0u, addrspace::read32(physicalAddress));

	// Associative write with both U and V clear must write the old dirty line
	// back before invalidating it.
	addrspace::write32(0xf4000008, physicalAddress);
	EXPECT_EQ(value, addrspace::read32(physicalAddress));
	EXPECT_EQ(0u, addrspace::read32(0xf4000000) & 3);
}

#ifdef STRICT_MODE
TEST_F(MmuTest, OperandCacheWriteBackBufferTiming)
{
	Sh4Context& ctx = p_sh4rcb->cntx;
	ocache.init(&ctx);
	ocache.Reset(true);
	CCN_CCR.OCE = 1;
	CCN_CCR.CB = 1;

	// Populate three different dirty cache lines without evicting any of them.
	ocache.WriteMem<u32>(0x8c000000, 1);
	ocache.WriteMem<u32>(0x8c000020, 2);
	ocache.WriteMem<u32>(0x8c000040, 3);
	ctx.cycle_counter = SH4_TIMESLICE;

	// The first write-back enters the buffer immediately. Each following
	// 32-byte RAM transfer waits for the previous four-beat, 14-cycle transfer.
	ocache.WriteBack(0x8c000000, true, false);
	EXPECT_EQ(SH4_TIMESLICE, ctx.cycle_counter);
	ocache.WriteBack(0x8c000020, true, false);
	EXPECT_EQ(SH4_TIMESLICE - 14, ctx.cycle_counter);
	ocache.WriteBack(0x8c000040, true, false);
	EXPECT_EQ(SH4_TIMESLICE - 28, ctx.cycle_counter);
}
#endif

TEST_F(MmuTest, OperandCacheHardResetClearsWriteBuffers)
{
	Sh4Context& ctx = p_sh4rcb->cntx;
	ocache.init(&ctx);
	ocache.Reset(true);
	CCN_CCR.OCE = 0;
	ctx.cycle_counter = SH4_TIMESLICE;

	ocache.WriteMem<u32>(0xAC000000, 1);
	ocache.Reset(true);
	ocache.WriteMem<u32>(0xAC000004, 2);
	EXPECT_EQ(SH4_TIMESLICE, ctx.cycle_counter);
}
