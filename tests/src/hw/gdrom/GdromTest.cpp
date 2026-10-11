#include <gtest/gtest.h>

#include "hw/gdrom/gdrom_if.h"
#include "hw/gdrom/gdromv3.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_core.h"
#include "reios/gdrom_hle.h"

TEST(GdromPioBuffer, PreservesOddLengthTransfers)
{
	PioBuffer buffer;
	u8 *data = buffer.fill(3);
	data[0] = 0x12;
	data[1] = 0x34;
	data[2] = 0x56;

	EXPECT_EQ(3u, buffer.getSize());
	EXPECT_EQ(0x3412u, buffer.read());
	EXPECT_EQ(1u, buffer.getRemaining());
	EXPECT_EQ(0x0056u, buffer.read());
	EXPECT_TRUE(buffer.atEnd());
}

TEST(GdromPioBuffer, AcceptsFullByteCountRange)
{
	PioBuffer buffer;
	u8 *data = buffer.fill(PioBuffer::Capacity);
	data[0] = 0x78;
	data[1] = 0x56;
	data[PioBuffer::Capacity - 2] = 0x34;
	data[PioBuffer::Capacity - 1] = 0x12;

	EXPECT_EQ(PioBuffer::Capacity, buffer.getRemaining());
	EXPECT_EQ(0x5678u, buffer.read());
	for (u32 i = sizeof(u16); i < PioBuffer::Capacity - sizeof(u16); i += sizeof(u16))
		buffer.read();
	EXPECT_EQ(0x1234u, buffer.read());
	EXPECT_TRUE(buffer.atEnd());
}

TEST(GdromPioBuffer, StoresOnlyRequestedOddByte)
{
	PioBuffer buffer;
	buffer.resetSize(3);
	buffer.write(0x3412);
	buffer.write(0x7856);

	ASSERT_TRUE(buffer.atEnd());
	EXPECT_EQ(0x12, buffer.data()[0]);
	EXPECT_EQ(0x34, buffer.data()[1]);
	EXPECT_EQ(0x56, buffer.data()[2]);
}

TEST(GdromTiming, UsesConfiguredReadSpeed)
{
	EXPECT_EQ(150_KB, gdromReadRate(1));
	EXPECT_EQ(300_KB, gdromReadRate(2));
	EXPECT_EQ(600_KB, gdromReadRate(3));
	EXPECT_EQ(900_KB, gdromReadRate(4));
	EXPECT_EQ(1200_KB, gdromReadRate(5));
	EXPECT_EQ(1500_KB, gdromReadRate(6));
	EXPECT_EQ(1800_KB, gdromReadRate(0));
	EXPECT_EQ(1800_KB, gdromReadRate(7));
}

TEST(GdromDma, AccountsForRootBusTransferUnits)
{
	EXPECT_EQ(0u, gdromDmaBusLength(0));
	EXPECT_EQ(32u, gdromDmaBusLength(1));
	EXPECT_EQ(32u, gdromDmaBusLength(32));
	EXPECT_EQ(64u, gdromDmaBusLength(33));
}

TEST(GdromTaskFile, DeviceHeadUsesLunInsteadOfSlaveSelect)
{
	const u32 previous = ReadMem_gdrom(GD_DRVSEL, 1);

	WriteMem_gdrom(GD_DRVSEL, 0xff, 1);
	EXPECT_EQ(0xafu, ReadMem_gdrom(GD_DRVSEL, 1));

	WriteMem_gdrom(GD_DRVSEL, previous, 1);
}

TEST(GdromTaskFile, SoftResetClearsOptionalLun)
{
	WriteMem_gdrom(GD_DRVSEL, 0xaf, 1);
	ASSERT_EQ(0xafu, ReadMem_gdrom(GD_DRVSEL, 1));

	WriteMem_gdrom(GD_COMMAND_Write, ATA_SOFT_RESET, 1);

	EXPECT_EQ(0xa0u, ReadMem_gdrom(GD_DRVSEL, 1));
}

TEST(GdromTaskFile, SoftResetExposesDiagnosticErrorSignature)
{
	WriteMem_gdrom(GD_COMMAND_Write, ATA_SOFT_RESET, 1);

	EXPECT_EQ(0x00u, ReadMem_gdrom(GD_STATUS_Read, 1));
	EXPECT_EQ(0x01u, ReadMem_gdrom(GD_ERROR_Read, 1));
	// Register reads must not synthesize or consume pending packet sense.
	EXPECT_EQ(0x01u, ReadMem_gdrom(GD_ERROR_Read, 1));
}

TEST(GdromTaskFile, SoftResetRestoresInterruptEnable)
{
	const u32 gdromInterrupt = 1u << (u8)holly_GDROM_CMD;
	SB_ISTEXT &= ~gdromInterrupt;
	SB_GDEN = 1;
	SB_GDST = 1;
	WriteMem_gdrom(GD_DEVCTRL_Write, 2, 1); // nIEN
	WriteMem_gdrom(GD_COMMAND_Write, ATA_EXEC_DIAG, 1);
	EXPECT_EQ(0u, SB_ISTEXT & gdromInterrupt);

	WriteMem_gdrom(GD_COMMAND_Write, ATA_SOFT_RESET, 1);
	EXPECT_EQ(1u, SB_GDEN); // The drive reset does not alter the G1 enable latch.
	EXPECT_EQ(1u, SB_GDST); // Nor does it terminate the independent G1 engine.
	SB_ISTEXT &= ~gdromInterrupt;
	WriteMem_gdrom(GD_COMMAND_Write, ATA_EXEC_DIAG, 1);
	EXPECT_NE(0u, SB_ISTEXT & gdromInterrupt);

	ReadMem_gdrom(GD_STATUS_Read, 1);
	SB_GDST = 0;
	SB_GDEN = 0;
}

TEST(GdromTaskFile, RejectsReservedPioDefaultMode)
{
	WriteMem_gdrom(GD_COMMAND_Write, ATA_SOFT_RESET, 1);
	WriteMem_gdrom(GD_FEATURES_Write, 3, 1);
	WriteMem_gdrom(GD_SECTCNT_Write, 7, 1); // PIO default class, reserved mode 7
	WriteMem_gdrom(GD_COMMAND_Write, ATA_SET_FEATURES, 1);

	EXPECT_NE(0u, ReadMem_gdrom(GD_STATUS_Read, 1) & 1);
	EXPECT_NE(0u, ReadMem_gdrom(GD_ERROR_Read, 1) & 4);
}

TEST(GdromTaskFile, AcceptsDefinedPioDefaultMode)
{
	WriteMem_gdrom(GD_COMMAND_Write, ATA_SOFT_RESET, 1);
	WriteMem_gdrom(GD_FEATURES_Write, 3, 1);
	WriteMem_gdrom(GD_SECTCNT_Write, 1, 1); // PIO default class, defined mode 1
	WriteMem_gdrom(GD_COMMAND_Write, ATA_SET_FEATURES, 1);

	EXPECT_EQ(0u, ReadMem_gdrom(GD_STATUS_Read, 1) & 1);
	EXPECT_EQ(0u, ReadMem_gdrom(GD_ERROR_Read, 1) & 4);
}

TEST(GdromTaskFile, PacketRejectsReservedFeatureBitsBeforeCdb)
{
	WriteMem_gdrom(GD_COMMAND_Write, ATA_SOFT_RESET, 1);
	WriteMem_gdrom(GD_FEATURES_Write, 0x80, 1);
	WriteMem_gdrom(GD_COMMAND_Write, ATA_SPI_PACKET, 1);

	const u32 status = ReadMem_gdrom(GD_STATUS_Read, 1);
	EXPECT_NE(0u, status & 1); // CHECK
	EXPECT_EQ(0u, status & 8); // DRQ: no packet requested
	EXPECT_NE(0u, ReadMem_gdrom(GD_ERROR_Read, 1) & 4); // ABRT

	WriteMem_gdrom(GD_COMMAND_Write, ATA_SOFT_RESET, 1);
}

TEST(GdromHle, ResetRaisesUnitAttention)
{
	const u32 oldR0 = Sh4cntx.r[0];
	const u32 oldR6 = Sh4cntx.r[6];
	const u32 oldR7 = Sh4cntx.r[7];
	const int oldSense = sns_key;
	const int oldAsc = sns_asc;
	const int oldAscq = sns_ascq;

	Sh4cntx.r[6] = SYSCALL_GDROM;
	Sh4cntx.r[7] = GDROM_RESET;
	gdrom_hle_op();

	EXPECT_EQ(GDC_OK, (s32)Sh4cntx.r[0]);
	EXPECT_EQ(6, sns_key);
	EXPECT_EQ(0x29, sns_asc);
	EXPECT_EQ(0, sns_ascq);

	Sh4cntx.r[0] = oldR0;
	Sh4cntx.r[6] = oldR6;
	Sh4cntx.r[7] = oldR7;
	sns_key = oldSense;
	sns_asc = oldAsc;
	sns_ascq = oldAscq;
}
