#include "rend/texconv.h"

#include <gtest/gtest.h>
#include <array>
#include <vector>

namespace {

// Reference Morton address for a 2x2 block. PVR interleaves Y then X while
// both dimensions have bits, then appends the remaining rectangular axis.
u32 referenceTwiddledBlock(u32 blockX, u32 blockY, u32 blocksX, u32 blocksY)
{
	u32 address = 0;
	u32 shift = 0;
	while (blocksX > 1 || blocksY > 1)
	{
		if (blocksY > 1)
		{
			address |= (blockY & 1) << shift++;
			blockY >>= 1;
			blocksY >>= 1;
		}
		if (blocksX > 1)
		{
			address |= (blockX & 1) << shift++;
			blockX >>= 1;
			blocksX >>= 1;
		}
	}
	return address;
}

u16 coordinate565(u32 x, u32 y)
{
	return static_cast<u16>(((x * 3 + y) & 31) << 11
			| ((x + y * 5) & 63) << 5 | ((x * 7 + y * 11) & 31));
}

} // namespace

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

TEST(TextureDecode, DetwiddlesSquareAndRectangularTexturesAgainstReference)
{
	constexpr std::array<std::array<u32, 2>, 6> dimensions {{
		{ 2, 2 }, { 4, 8 }, { 8, 4 }, { 8, 8 }, { 16, 8 }, { 8, 16 },
	}};
	for (const auto& size : dimensions)
	{
		const u32 width = size[0];
		const u32 height = size[1];
		std::vector<u16> native(static_cast<size_t>(width) * height);
		for (u32 y = 0; y < height; ++y)
			for (u32 x = 0; x < width; ++x)
			{
				const u32 block = referenceTwiddledBlock(x / 2, y / 2,
						width / 2, height / 2);
				const u32 within = (x & 1) * 2 + (y & 1);
				native[block * 4 + within] = coordinate565(x, y);
			}

		PixelBuffer<u32> decoded;
		decoded.init(width, height);
		opengl::pvrTexInfo[1].TW32(&decoded,
				reinterpret_cast<const u8 *>(native.data()), width, height);
		for (u32 y = 0; y < height; ++y)
			for (u32 x = 0; x < width; ++x)
				EXPECT_EQ(*decoded.data(x, y),
						(Unpacker565_32<RGBAPacker>::unpack(coordinate565(x, y))))
						<< width << 'x' << height << " at " << x << ',' << y;
	}
}

TEST(TextureDecode, DetwiddlesSquareVqIndicesAgainstReference)
{
	constexpr u32 width = 16;
	constexpr u32 height = 16;
	constexpr u32 blocksX = width / 2;
	constexpr u32 blocksY = height / 2;
	std::array<u16, VQ_CODEBOOK_SIZE / sizeof(u16)> codebook {};
	std::array<u8, blocksX * blocksY> indices {};
	for (u32 by = 0; by < blocksY; ++by)
		for (u32 bx = 0; bx < blocksX; ++bx)
		{
			const u8 entry = static_cast<u8>(1 + by * blocksX + bx);
			indices[referenceTwiddledBlock(bx, by, blocksX, blocksY)] = entry;
			for (u32 within = 0; within < 4; ++within)
				codebook[entry * 4 + within] = coordinate565(
						bx * 2 + within / 2, by * 2 + within % 2);
		}
	vq_codebook = reinterpret_cast<const u8 *>(codebook.data());

	PixelBuffer<u32> decoded;
	decoded.init(width, height);
	opengl::pvrTexInfo[1].VQ32(&decoded, indices.data(), width, height);
	for (u32 y = 0; y < height; ++y)
		for (u32 x = 0; x < width; ++x)
			EXPECT_EQ(*decoded.data(x, y),
					(Unpacker565_32<RGBAPacker>::unpack(coordinate565(x, y))))
					<< "at " << x << ',' << y;
}

TEST(TextureDecode, ConvertsDocumentedYuv422Coefficients)
{
	// Chroma deltas of 32 make every documented coefficient integral:
	// R = Y + 11/8 V, G = Y - 11/32 U - 22/32 V,
	// B = Y + 110/64 U.
	const std::array<u8, 8> native { 160, 100, 128, 50,
			128, 100, 160, 50 };
	PixelBuffer<u32> decoded;
	decoded.init(4, 1);
	opengl::pvrTexInfo[3].PL32(&decoded, native.data(), 4, 1);

	EXPECT_EQ(*decoded.data(0, 0), RGBAPacker::pack(100, 89, 155, 255));
	EXPECT_EQ(*decoded.data(1, 0), RGBAPacker::pack(50, 39, 105, 255));
	EXPECT_EQ(*decoded.data(2, 0), RGBAPacker::pack(144, 78, 100, 255));
	EXPECT_EQ(*decoded.data(3, 0), RGBAPacker::pack(94, 28, 50, 255));
}

TEST(TextureDecode, DecodesTwiddledYuv422WordOrder)
{
	const std::array<u16, 4> native {
		static_cast<u16>(10 << 8 | 128),
		static_cast<u16>(30 << 8 | 128),
		static_cast<u16>(20 << 8 | 128),
		static_cast<u16>(40 << 8 | 128),
	};
	PixelBuffer<u32> decoded;
	decoded.init(2, 2);
	opengl::pvrTexInfo[3].TW32(&decoded,
			reinterpret_cast<const u8 *>(native.data()), 2, 2);

	EXPECT_EQ(*decoded.data(0, 0), 0xff0a0a0au);
	EXPECT_EQ(*decoded.data(1, 0), 0xff141414u);
	EXPECT_EQ(*decoded.data(0, 1), 0xff1e1e1eu);
	EXPECT_EQ(*decoded.data(1, 1), 0xff282828u);
}

TEST(TextureDecode, DecodesPalettedBytesInTwiddledMicrotileOrder)
{
	palette_index = 256;
	for (u32 i = 0; i < 8; ++i)
		palette32_ram[palette_index + i] = 0xb0000000u | i;
	const std::array<u8, 8> native { 0, 1, 2, 3, 4, 5, 6, 7 };
	PixelBuffer<u32> decoded;
	decoded.init(2, 4);
	opengl::pvrTexInfo[6].TW32(&decoded, native.data(), 2, 4);

	constexpr std::array<u8, 8> expected { 0, 2, 1, 3, 4, 6, 5, 7 };
	for (u32 y = 0; y < 4; ++y)
		for (u32 x = 0; x < 2; ++x)
			EXPECT_EQ(*decoded.data(x, y), 0xb0000000u | expected[y * 2 + x]);
}
