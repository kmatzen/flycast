#include "rend/texconv.h"

#include <gtest/gtest.h>

TEST(TextureDecode, ExpandsNative16BitChannelsExactly)
{
	EXPECT_EQ((Unpacker1555_32<RGBAPacker>::unpack(0xfc00)), 0xff0000ffu);
	EXPECT_EQ((Unpacker1555_32<RGBAPacker>::unpack(0x03e0)), 0x0000ff00u);
	EXPECT_EQ((Unpacker1555_32<RGBAPacker>::unpack(0x001f)), 0x00ff0000u);
	EXPECT_EQ((Unpacker1555_32<RGBAPacker>::unpack(0x4210)), 0x00848484u);

	EXPECT_EQ((Unpacker565_32<RGBAPacker>::unpack(0xf800)), 0xff0000ffu);
	EXPECT_EQ((Unpacker565_32<RGBAPacker>::unpack(0x07e0)), 0xff00ff00u);
	EXPECT_EQ((Unpacker565_32<RGBAPacker>::unpack(0x001f)), 0xffff0000u);
	EXPECT_EQ((Unpacker565_32<RGBAPacker>::unpack(0x8410)), 0xff848284u);

	EXPECT_EQ((Unpacker4444_32<RGBAPacker>::unpack(0xf123)), 0xff332211u);
	EXPECT_EQ((Unpacker4444_32<RGBAPacker>::unpack(0x8ace)), 0x88eeccaau);
}

TEST(TextureDecode, ExpandsNative8888AndDirectXChannelOrder)
{
	EXPECT_EQ((Unpacker8888<RGBAPacker>::unpack(0x80402010)), 0x80102040u);
	EXPECT_EQ((Unpacker8888<BGRAPacker>::unpack(0x80402010)), 0x80402010u);
	EXPECT_EQ((Unpacker1555_32<BGRAPacker>::unpack(0xfc00)), 0xffff0000u);
	EXPECT_EQ((Unpacker565_32<BGRAPacker>::unpack(0x001f)), 0xff0000ffu);
}

TEST(TextureDecode, ReordersNative16BitPackedLayouts)
{
	EXPECT_EQ(Unpacker1555::unpack(0xfc00), 0xf801u);
	EXPECT_EQ(Unpacker1555::unpack(0x03e0), 0x07c0u);
	EXPECT_EQ(Unpacker4444::unpack(0xf123), 0x123fu);
	EXPECT_EQ(Unpacker4444::unpack(0x8ace), 0xace8u);
}

TEST(TextureDecode, UsesSmallestFirstMipStorage)
{
	PixelBuffer<u32> pixels;
	pixels.init(8, 8, true);
	for (u32 level = 0; level < 4; ++level)
	{
		pixels.set_mipmap(level);
		const u32 dimension = 1u << level;
		for (u32 y = 0; y < dimension; ++y)
			for (u32 x = 0; x < dimension; ++x)
				pixels.prel(x, y, 0x10000000u * level + y * dimension + x);
	}

	pixels.set_mipmap(0);
	EXPECT_EQ(*pixels.data(), 0u);
	pixels.set_mipmap(1);
	EXPECT_EQ(*pixels.data(1, 1), 0x10000003u);
	pixels.set_mipmap(2);
	EXPECT_EQ(*pixels.data(3, 3), 0x2000000fu);
	pixels.set_mipmap(3);
	EXPECT_EQ(*pixels.data(7, 7), 0x3000003fu);
}
