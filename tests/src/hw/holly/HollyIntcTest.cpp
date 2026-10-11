#include "gtest/gtest.h"

#include "hw/aica/aica_if.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/pvr/pvr_sb_regs.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_sched.h"

class HollyIntcTest : public ::testing::Test
{
protected:
	void SetUp() override
	{
		hollyRegs.init();
		hollyRegs.reset();
		asic_reg_Reset(true);
		pvr_sb_Init();
		aica::sbInit();
	}

	void TearDown() override
	{
		aica::sbTerm();
		pvr_sb_Term();
	}

	void tick(int cycles)
	{
		Sh4cntx.sh4_sched_next -= cycles;
		sh4_sched_tick(cycles);
	}
};

TEST_F(HollyIntcTest, NormalInterruptMasksIgnoreUnimplementedSources)
{
	hollyRegs.write<u32>(SB_IML2NRM_addr, 0xffffffff);
	hollyRegs.write<u32>(SB_IML4NRM_addr, 0xffffffff);
	hollyRegs.write<u32>(SB_IML6NRM_addr, 0xffffffff);

	EXPECT_EQ(0x003fffffu, SB_IML2NRM);
	EXPECT_EQ(0x003fffffu, SB_IML4NRM);
	EXPECT_EQ(0x003fffffu, SB_IML6NRM);
}

TEST_F(HollyIntcTest, PvrDmaSoftwareStartRequiresEnableAndSoftwareMode)
{
	SB_PDEN = 0;
	SB_PDTSEL = 0;
	hollyRegs.write<u32>(SB_PDST_addr, 1);
	EXPECT_EQ(0u, SB_PDST);

	SB_PDEN = 1;
	SB_PDTSEL = 1;
	hollyRegs.write<u32>(SB_PDST_addr, 1);
	EXPECT_EQ(0u, SB_PDST);
}

TEST_F(HollyIntcTest, DisablingPvrDmaClearsActiveStatus)
{
	SB_PDEN = 1;
	SB_PDST = 1;

	hollyRegs.write<u32>(SB_PDEN_addr, 0);

	EXPECT_EQ(0u, SB_PDEN);
	EXPECT_EQ(0u, SB_PDST);
}

TEST_F(HollyIntcTest, UnselectedInterruptDoesNotTriggerPvrDma)
{
	SB_PDEN = 1;
	SB_PDTSEL = 1;
	SB_PDTNRM = 1u << (u8)holly_SCANINT1;

	asic_RaiseInterrupt(holly_SCANINT2);

	EXPECT_EQ(0u, SB_PDST);
}

TEST_F(HollyIntcTest, SelectedInterruptTriggersPvrDmaValidation)
{
	hollyRegs.write<u32>(SB_PDSTAP_addr, 0x04000000);
	hollyRegs.write<u32>(SB_PDSTAR_addr, 0x0c000000);
	SB_PDLEN = 0x20;
	SB_PDAPRO = 0x0505; // Allow 0x0d000000-0x0d0fffff, excluding source.
	SB_PDEN = 1;
	SB_PDTSEL = 1;
	SB_PDTNRM = 1u << (u8)holly_SCANINT1;
	SB_ISTERR = 0;

	asic_RaiseInterrupt(holly_SCANINT1);

	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_PVR_OVERRUN));
	EXPECT_EQ(0u, SB_PDST);
}

TEST_F(HollyIntcTest, LatchedInterruptOnlyTriggersDmaOnSignalEdges)
{
	hollyRegs.write<u32>(SB_PDSTAP_addr, 0x04000000);
	hollyRegs.write<u32>(SB_PDSTAR_addr, 0x0c000000);
	SB_PDLEN = 0x20;
	SB_PDAPRO = 0x0505;
	SB_PDEN = 1;
	SB_PDTSEL = 1;
	SB_PDTNRM = 1u << (u8)holly_SCANINT1;

	asic_RaiseInterrupt(holly_SCANINT1);
	ASSERT_NE(0u, SB_ISTERR & (1u << (u8)holly_PVR_OVERRUN));
	SB_ISTERR = 0;

	// The status bit is still latched, so another source notification does not
	// create another rising edge on the masked DMA trigger line.
	asic_RaiseInterrupt(holly_SCANINT1);
	EXPECT_EQ(0u, SB_ISTERR & (1u << (u8)holly_PVR_OVERRUN));

	hollyRegs.write<u32>(SB_ISTNRM_addr, 1u << (u8)holly_SCANINT1);
	asic_RaiseInterrupt(holly_SCANINT1);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_PVR_OVERRUN));
}

TEST_F(HollyIntcTest, UnmaskingLatchedStatusRaisesDmaTrigger)
{
	hollyRegs.write<u32>(SB_PDSTAP_addr, 0x04000000);
	hollyRegs.write<u32>(SB_PDSTAR_addr, 0x0c000000);
	SB_PDLEN = 0x20;
	SB_PDAPRO = 0x0505;
	SB_PDEN = 1;
	SB_PDTSEL = 1;
	SB_PDTNRM = 0;
	SB_ISTERR = 0;

	asic_RaiseInterrupt(holly_SCANINT1);
	EXPECT_EQ(0u, SB_ISTERR & (1u << (u8)holly_PVR_OVERRUN));

	hollyRegs.write<u32>(SB_PDTNRM_addr, 1u << (u8)holly_SCANINT1);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_PVR_OVERRUN));
}

TEST_F(HollyIntcTest, G2DmaSoftwareStartIsIgnoredInInterruptMode)
{
	SB_E1STAR = 0x0c000000;
	SB_E1STAG = 0x01000000;
	SB_E1TSEL = 2;
	SB_E1ST = 0;
	hollyRegs.write<u32>(SB_E1EN_addr, 1);

	hollyRegs.write<u32>(SB_E1ST_addr, 1);

	EXPECT_EQ(0u, SB_E1ST);
}

TEST_F(HollyIntcTest, G2SuspendRequestRequiresSuspendModeAndIsWriteOnly)
{
	SB_ADTSEL = 0;
	SB_ADSUSP = 0;
	hollyRegs.write<u32>(SB_ADSUSP_addr, 1);
	EXPECT_EQ(0u, SB_ADSUSP);

	SB_ADTSEL = 4;
	hollyRegs.write<u32>(SB_ADSUSP_addr, 1);
	EXPECT_NE(0u, SB_ADSUSP & 1); // Retained internally to gate the DMA engine.
	EXPECT_EQ(0u, hollyRegs.read<u32>(SB_ADSUSP_addr) & 1);
}

TEST_F(HollyIntcTest, G2DmaAddressesLatchIllegalAddressErrors)
{
	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_ADSTAR_addr, 0x00800000);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_AICA_ILLADDR));

	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_ADSTAG_addr, 0x0c000000);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_AICA_ILLADDR));

	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_ADSTAR_addr, 0x0c000000);
	hollyRegs.write<u32>(SB_ADSTAG_addr, 0x00800000);
	EXPECT_EQ(0u, SB_ISTERR);
}

TEST_F(HollyIntcTest, G2DmaRejectsAnotherChannelsDeviceWindow)
{
	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_E1STAG_addr, 0x03000000);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_EXT1_ILLADDR));

	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_E2STAG_addr, 0x01000000);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_EXT2_ILLADDR));

	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_DDSTAG_addr, 0x00800000);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_DEV_ILLADDR));

	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_E1STAG_addr, 0x01000000);
	hollyRegs.write<u32>(SB_E2STAG_addr, 0x03000000);
	hollyRegs.write<u32>(SB_DDSTAG_addr, 0x14000000);
	EXPECT_EQ(0u, SB_ISTERR);
}

TEST_F(HollyIntcTest, EnablingAicaDmaRevalidatesAddresses)
{
	SB_ADSTAR = 0x00800000;
	SB_ADSTAG = 0x00800000;
	SB_ISTERR = 0;

	hollyRegs.write<u32>(SB_ADEN_addr, 1);

	EXPECT_EQ(0u, SB_ADEN);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_AICA_ILLADDR));
}

TEST_F(HollyIntcTest, G2DmaProtectionRejectsWrappingTransfer)
{
	SB_ADSTAR = 0x0fffffe0;
	SB_ADSTAG = 0x00800000;
	SB_ADLEN = 0x40;
	SB_ADDIR = 0;
	SB_ADTSEL = 0;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	SB_G2APRO = 0x7f7f;
	SB_ISTERR = 0;

	hollyRegs.write<u32>(SB_ADST_addr, 1);

	EXPECT_EQ(0u, SB_ADST);
	EXPECT_EQ(0u, SB_ADEN);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_AICA_OVERRUN));
}

TEST_F(HollyIntcTest, G2DmaProtectionDoesNotRejectTextureMemory)
{
	SB_ADSTAR = 0x04000000;
	SB_ADSTAG = 0x00800000;
	SB_ADLEN = 0x20;
	SB_ADDIR = 0;
	SB_ADTSEL = 0;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	SB_G2APRO = 0x7f00; // Default system-memory protection range.
	SB_ISTERR = 0;

	hollyRegs.write<u32>(SB_ADST_addr, 1);

	EXPECT_EQ(1u, SB_ADST);
	EXPECT_EQ(0u, SB_ISTERR & (1u << (u8)holly_AICA_OVERRUN));
	hollyRegs.write<u32>(SB_ADEN_addr, 0);
}

TEST_F(HollyIntcTest, G2DmaRejectsTransferCrossingTextureWindow)
{
	SB_ADSTAR = 0x057fffe0;
	SB_ADSTAG = 0x00800000;
	SB_ADLEN = 0x40;
	SB_ADDIR = 0;
	SB_ADTSEL = 0;
	hollyRegs.write<u32>(SB_ADEN_addr, 1);
	SB_ISTERR = 0;

	hollyRegs.write<u32>(SB_ADST_addr, 1);

	EXPECT_EQ(0u, SB_ADST);
	EXPECT_EQ(0u, SB_ADEN);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_AICA_OVERRUN));
}

TEST_F(HollyIntcTest, DisablingAicaDmaCancelsActiveStatus)
{
	SB_ADEN = 1;
	SB_ADST = 1;
	SB_ADSUSP = 0;

	hollyRegs.write<u32>(SB_ADEN_addr, 0);

	EXPECT_EQ(0u, SB_ADEN);
	EXPECT_EQ(0u, SB_ADST);
	EXPECT_NE(0u, SB_ADSUSP & 0x10);
}

TEST_F(HollyIntcTest, G2SuspendReadbackReportsLiveStopStatus)
{
	SB_ADTSEL = 4;
	SB_ADEN = 0;
	SB_ADST = 0;
	SB_ADSUSP = 0;
	EXPECT_EQ(0x10u, hollyRegs.read<u32>(SB_ADSUSP_addr));

	// Starting a transfer clears stop status without exposing the write-only
	// request bit. An accepted suspend request makes stop status visible again.
	SB_ADST = 1;
	EXPECT_EQ(0u, hollyRegs.read<u32>(SB_ADSUSP_addr));
	SB_ADSUSP = 1;
	EXPECT_EQ(0x10u, hollyRegs.read<u32>(SB_ADSUSP_addr));

	SB_ADTSEL = 0;
	SB_ADSUSP = 0x10;
	EXPECT_EQ(0u, hollyRegs.read<u32>(SB_ADSUSP_addr));
}

TEST_F(HollyIntcTest, PvrDmaAddressesLatchIllegalAddressErrors)
{
	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_PDSTAP_addr, 0x08000000);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_PVR_ILLADDR));

	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_PDSTAR_addr, 0x04000000);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_PVR_ILLADDR));

	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_PDSTAP_addr, 0x04000000);
	hollyRegs.write<u32>(SB_PDSTAR_addr, 0x0c000000);
	EXPECT_EQ(0u, SB_ISTERR);

	// Reserved upper bits do not turn cached or mirrored addresses into valid
	// physical PVR-DMA addresses.
	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_PDSTAP_addr, 0xa4000000);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_PVR_ILLADDR));

	SB_ISTERR = 0;
	hollyRegs.write<u32>(SB_PDSTAR_addr, 0x8c000000);
	EXPECT_NE(0u, SB_ISTERR & (1u << (u8)holly_PVR_ILLADDR));
}

TEST_F(HollyIntcTest, PvrDmaProtectionRequiresSecurityCode)
{
	SB_PDAPRO = 0x1234;
	hollyRegs.write<u32>(SB_PDAPRO_addr, 0x00005678);
	EXPECT_EQ(0x1234u, SB_PDAPRO);

	hollyRegs.write<u32>(SB_PDAPRO_addr, 0x67024567);
	EXPECT_EQ(0x4567u, SB_PDAPRO);
}

TEST_F(HollyIntcTest, G1SystemModeIsReadOnly)
{
	SB_G1SYSM = 1;
	hollyRegs.write<u32>(SB_G1SYSM_addr, 0xffffffff);
	EXPECT_EQ(1u, SB_G1SYSM);
}

TEST_F(HollyIntcTest, G2ModemTimingRegistersRetainTheirEightBitFields)
{
	// MDMTO accepts the documented disabled-wait value, while MDMW can
	// represent every 40 ns step through its complete eight-bit field.
	hollyRegs.write<u32>(SB_G2MDMTO_addr, 0xffffffff);
	hollyRegs.write<u32>(SB_G2MDMW_addr, 0xffffff17);

	EXPECT_EQ(0xffu, SB_G2MDMTO);
	EXPECT_EQ(0x17u, SB_G2MDMW);
}

TEST_F(HollyIntcTest, TaFifoRemainingReflectsSynchronousConsumption)
{
	// Backing storage is not the status source: the current synchronous TA
	// model has no queued entries when the SH4 can perform this read.
	SB_TFREM = 0;
	EXPECT_EQ(8u, hollyRegs.read<u32>(SB_TFREM_addr));
	hollyRegs.write<u32>(SB_TFREM_addr, 3);
	EXPECT_EQ(8u, hollyRegs.read<u32>(SB_TFREM_addr));
}

TEST_F(HollyIntcTest, SortDmaStopRequestWaitsForTransferBoundary)
{
	SB_SDST = 0;
	SB_SDDIV = 17;

	hollyRegs.write<u32>(SB_SDST_addr, 1);
	EXPECT_EQ(1u, SB_SDST);
	EXPECT_EQ(0u, SB_SDDIV);

	hollyRegs.write<u32>(SB_SDST_addr, 0);
	// Hardware retains the active indication until the in-flight object has
	// stopped; the scheduler clears it at the next object boundary.
	EXPECT_EQ(1u, SB_SDST);

	SB_ISTNRM = 0;
	tick(1);
	EXPECT_EQ(0u, SB_SDST);
	EXPECT_EQ(0u, SB_ISTNRM & (1u << (u8)holly_PVR_SortDMA));
}

TEST_F(HollyIntcTest, ExternalInterruptMasksIgnoreUnimplementedSources)
{
	hollyRegs.write<u32>(SB_IML2EXT_addr, 0xffffffff);
	hollyRegs.write<u32>(SB_IML4EXT_addr, 0xffffffff);
	hollyRegs.write<u32>(SB_IML6EXT_addr, 0xffffffff);

	EXPECT_EQ(0x0000000fu, SB_IML2EXT);
	EXPECT_EQ(0x0000000fu, SB_IML4EXT);
	EXPECT_EQ(0x0000000fu, SB_IML6EXT);
}
