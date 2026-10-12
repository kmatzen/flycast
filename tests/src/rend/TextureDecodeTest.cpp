#include "rend/texconv.h"

#include <gtest/gtest.h>
#include <array>

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

TEST(TextureDecode, DetwiddlesNativeMortonBlocks)
{
	std::array<u16, 16> native {};
	constexpr std::array<u16, 4> colors { 0xf800, 0x07e0, 0x001f, 0xffff };
	for (u32 block = 0; block < colors.size(); ++block)
		for (u32 texel = 0; texel < 4; ++texel)
			native[block * 4 + texel] = colors[block];

	PixelBuffer<u32> decoded;
	decoded.init(4, 4);
	opengl::pvrTexInfo[1].TW32(&decoded,
			reinterpret_cast<const u8 *>(native.data()), 4, 4);

	EXPECT_EQ(*decoded.data(0, 0), 0xff0000ffu);
	EXPECT_EQ(*decoded.data(3, 0), 0xffff0000u);
	EXPECT_EQ(*decoded.data(0, 3), 0xff00ff00u);
	EXPECT_EQ(*decoded.data(3, 3), 0xffffffffu);
}

TEST(TextureDecode, ExpandsVectorQuantizedCodebookEntriesInTwiddledOrder)
{
	std::array<u16, VQ_CODEBOOK_SIZE / sizeof(u16)> codebook {};
	constexpr std::array<u16, 4> colors { 0xf800, 0x07e0, 0x001f, 0xffff };
	constexpr std::array<u8, 4> entries { 10, 11, 12, 13 };
	for (u32 block = 0; block < entries.size(); ++block)
	{
		u16 *entry = &codebook[entries[block] * 4];
		for (u32 texel = 0; texel < 4; ++texel)
			entry[texel] = colors[block];
	}
	vq_codebook = reinterpret_cast<const u8 *>(codebook.data());
	const std::array<u8, 4> indices { 10, 11, 12, 13 };

	PixelBuffer<u32> decoded;
	decoded.init(4, 4);
	opengl::pvrTexInfo[1].VQ32(&decoded, indices.data(), 4, 4);

	EXPECT_EQ(*decoded.data(0, 0), 0xff0000ffu);
	EXPECT_EQ(*decoded.data(3, 0), 0xffff0000u);
	EXPECT_EQ(*decoded.data(0, 3), 0xff00ff00u);
	EXPECT_EQ(*decoded.data(3, 3), 0xffffffffu);
}

TEST(TextureDecode, DecodesPalettedNibblesThroughSelectedBank)
{
	palette_index = 32;
	for (u32 i = 0; i < 16; ++i)
		palette32_ram[palette_index + i] = 0xa0000000u | i;
	const std::array<u8, 8> native { 0x10, 0x32, 0x54, 0x76,
			0x98, 0xba, 0xdc, 0xfe };

	PixelBuffer<u32> decoded;
	decoded.init(4, 4);
	opengl::pvrTexInfo[5].TW32(&decoded, native.data(), 4, 4);

	constexpr std::array<u8, 16> expected {
		0, 2, 8, 10,
		1, 3, 9, 11,
		4, 6, 12, 14,
		5, 7, 13, 15,
	};
	for (u32 y = 0; y < 4; ++y)
		for (u32 x = 0; x < 4; ++x)
			EXPECT_EQ(*decoded.data(x, y), 0xa0000000u | expected[y * 4 + x]);
}

TEST(TextureDecode, ConvertsPlanarYuv422WithSharedChroma)
{
	const std::array<u8, 8> native { 128, 10, 128, 20,
			128, 30, 128, 40 };
	PixelBuffer<u32> decoded;
	decoded.init(4, 1);
	opengl::pvrTexInfo[3].PL32(&decoded, native.data(), 4, 1);

	EXPECT_EQ(*decoded.data(0, 0), 0xff0a0a0au);
	EXPECT_EQ(*decoded.data(1, 0), 0xff141414u);
	EXPECT_EQ(*decoded.data(2, 0), 0xff1e1e1eu);
	EXPECT_EQ(*decoded.data(3, 0), 0xff282828u);
}
