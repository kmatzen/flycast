#include "rend/soft/SoftwareRasterizer.h"
#include "hw/pvr/pvr_regs.h"
#include "rend/FramebufferWrite.h"
#include "rend/FramebufferPack.h"
#include "hw/pvr/vram_map.h"

#include <cmath>
#include <gtest/gtest.h>
#include <limits>

using namespace rend::soft;

namespace {
struct FramebufferPackSink
{
	std::vector<u8> bytes;
	template<typename T> void write(T value)
	{
		for (size_t i = 0; i < sizeof(T); ++i)
			bytes.push_back(static_cast<u8>(value >> (i * 8)));
	}
};
template<typename Line> std::vector<u8> packTestPixel(FB_W_CTRL_type control)
{
	FramebufferPackSink sink;
	Line writer(control, sink);
	const u8 input[] { 0xab, 0xcd, 0xef, 0x42 };
	const u8 *cursor = input;
	writer.write(0, 1, cursor, 0);
	EXPECT_EQ(cursor, input + 4);
	return sink.bytes;
}

template<typename Line> void checkPackedTextureWrap(u16 expectedWord)
{
	std::vector<u8> storage(80, 0xa5);
	auto expected = storage;
	u8 *memory = storage.data() + 8;
	TexturePixelWriter sink(reinterpret_cast<u16 *>(memory + 62), memory, 64);
	FB_W_CTRL_type control {};
	control.fb_kval = 0x9a;
	control.fb_alpha_threshold = 0x42;
	Line writer(control, sink);
	const u8 pixels[] { 0xab, 0xcd, 0xef, 0x42, 0xab, 0xcd, 0xef, 0x42 };
	const u8 *cursor = pixels;
	writer.write(0, 2, cursor, 0);
	EXPECT_EQ(cursor, pixels + sizeof(pixels));
	sink.advance(14);
	cursor = pixels;
	writer.write(0, 2, cursor, 1);
	for (u32 offset : { 62u, 0u, 16u, 18u })
	{
		expected[8 + offset] = static_cast<u8>(expectedWord);
		expected[8 + offset + 1] = static_cast<u8>(expectedWord >> 8);
	}
	EXPECT_EQ(storage, expected); // Includes prefix/suffix guards and padding.
}
}

TEST(SoftwareRasterizer, ProductionTexturePackersWrapStoredPixelsAndPreserveGuards)
{
	checkPackedTextureWrap<FBLineWriter0555<0, 1, 2, 3, TexturePixelWriter>>(0xd73d);
	checkPackedTextureWrap<FBLineWriter565<0, 1, 2, 3, TexturePixelWriter>>(0xae7d);
	checkPackedTextureWrap<FBLineWriter4444<0, 1, 2, 3, TexturePixelWriter>>(0x4ace);
	checkPackedTextureWrap<FBLineWriter1555<0, 1, 2, 3, TexturePixelWriter>>(0xd73d);
}

TEST(SoftwareRasterizer, ProductionFramebufferPackersEmitExactStoredBytes)
{
	FB_W_CTRL_type control {};
	control.fb_kval = 0x9a;
	control.fb_alpha_threshold = 0x42;
	EXPECT_EQ((packTestPixel<FBLineWriter0555<0, 1, 2, 3, FramebufferPackSink>>(control)),
			(std::vector<u8> { 0x3d, 0xd7 }));
	EXPECT_EQ((packTestPixel<FBLineWriter565<0, 1, 2, 3, FramebufferPackSink>>(control)),
			(std::vector<u8> { 0x7d, 0xae }));
	EXPECT_EQ((packTestPixel<FBLineWriter4444<0, 1, 2, 3, FramebufferPackSink>>(control)),
			(std::vector<u8> { 0xce, 0x4a }));
	EXPECT_EQ((packTestPixel<FBLineWriter1555<0, 1, 2, 3, FramebufferPackSink>>(control)),
			(std::vector<u8> { 0x3d, 0xd7 }));
	control.fb_alpha_threshold = 0x43;
	EXPECT_EQ((packTestPixel<FBLineWriter1555<0, 1, 2, 3, FramebufferPackSink>>(control)),
			(std::vector<u8> { 0x3d, 0x57 }));
	EXPECT_EQ((packTestPixel<FBLineWriter888<0, 1, 2, 3, FramebufferPackSink>>(control)),
			(std::vector<u8> { 0xef, 0xcd, 0xab }));
	EXPECT_EQ((packTestPixel<FBLineWriter0888<0, 1, 2, 3, FramebufferPackSink>>(control)),
			(std::vector<u8> { 0xef, 0xcd, 0xab, 0x9a }));
	EXPECT_EQ((packTestPixel<FBLineWriter8888<0, 1, 2, 3, FramebufferPackSink>>(control)),
			(std::vector<u8> { 0xef, 0xcd, 0xab, 0x42 }));
}

TEST(SoftwareRasterizer, Rgb565ReadbackExpansionRequiresTruncationToPreserveStoredWords)
{
	// This tests conversion stability, not Dreamcast hardware behavior.
	// Model nearest normalized RGBA8 readback from an RGB565 attachment.
	FB_W_CTRL_type control {};
	for (u32 word = 0; word <= 0xffff; ++word)
	{
		const u8 rgba[] {
			static_cast<u8>((((word >> 11) & 31) * 255 + 15) / 31),
			static_cast<u8>((((word >> 5) & 63) * 255 + 31) / 63),
			static_cast<u8>(((word & 31) * 255 + 15) / 31), 255 };
		FramebufferPackSink sink;
		FBLineWriter565<0, 1, 2, 3, FramebufferPackSink, false> writer(control, sink);
		const u8 *cursor = rgba;
		writer.write(0, 1, cursor, 0);
		ASSERT_EQ(sink.bytes.size(), 2u);
		ASSERT_EQ(static_cast<u32>(sink.bytes[0] | (sink.bytes[1] << 8)), word);
	}
	// Captured OpenGL probe readback: a second rounding changes all channels.
	const u8 captured[] { 0xad, 0xce, 0xef, 0xff };
	const u8 *cursor = captured;
	FramebufferPackSink sink;
	FBLineWriter565<0, 1, 2, 3, FramebufferPackSink, true> rounded(control, sink);
	rounded.write(0, 1, cursor, 0);
	EXPECT_EQ(sink.bytes, (std::vector<u8> { 0x9e, 0xb6 }));
}

TEST(SoftwareRasterizer, WrapsTextureWritesAndRowAdvancesWithinVram)
{
	alignas(u16) u8 memory[8] {};
	TexturePixelWriter writer(reinterpret_cast<u16 *>(memory + 6), memory, sizeof(memory));
	writer.write(0x1234);
	writer.write(0x5678);
	EXPECT_EQ(memory[6], 0x34);
	EXPECT_EQ(memory[7], 0x12);
	EXPECT_EQ(memory[0], 0x78);
	EXPECT_EQ(memory[1], 0x56);
	writer.advance(12);
	writer.write(0xabcd);
	EXPECT_EQ(memory[6], 0xcd);
	EXPECT_EQ(memory[7], 0xab);
	alignas(u16) u8 linear[8] {};
	TexturePixelWriter staging(reinterpret_cast<u16 *>(linear), memory, sizeof(memory));
	staging.advance(4);
	staging.write(0x9abc);
	EXPECT_EQ(linear[4], 0xbc);
	EXPECT_EQ(linear[5], 0x9a);
}

TEST(SoftwareRasterizer, MapsEveryDreamcastVramByteAcrossBanksAndApertureWrap)
{
	// Independent quotient/remainder formulation: two 4 MiB banks supply
	// alternating four-byte lanes of each physical eight-byte word.
	for (u32 address = 0; address < 0x800000; ++address)
	{
		const u32 bank = address / 0x400000;
		const u32 offset = address % 0x400000;
		const u32 expected = (offset / 4) * 8 + bank * 4 + offset % 4;
		ASSERT_EQ(pvrMap32Address(address, 0x7fffff, 0x400000), expected)
				<< "address=" << address;
	}
	EXPECT_EQ(pvrMap32Address(0x3fffff, 0x7fffff, 0x400000), 0x7ffffbu);
	EXPECT_EQ(pvrMap32Address(0x400000, 0x7fffff, 0x400000), 4u);
	EXPECT_EQ(pvrMap32Address(0x7fffff, 0x7fffff, 0x400000), 0x7fffffu);
	EXPECT_EQ(pvrMap32Address(0x800000, 0x7fffff, 0x400000), 0u);
}

TEST(SoftwareRasterizer, PreservesBytePaddingAtClippedRgb888RowStart)
{
	// FB_W_LINESTRIDE is in eight-byte units, not whole RGB888 pixels.
	// 640 * 3 = 1920; stride 1928 leaves eight bytes of padding.
	// The old division by three discarded two bytes for each skipped row.
	EXPECT_EQ(framebufferWriteRowAddress(0x200000, 640, 3, 1928, 32),
			0x200000u + 32u * 1928u);
	EXPECT_NE(framebufferWriteRowAddress(0x200000, 640, 3, 1928, 32),
			0x200000u + 32u * 1926u);
	for (u32 bpp : { 2u, 3u, 4u })
		for (u32 stride = 2560; stride <= 2584; stride += 8)
			for (u32 row : { 0u, 1u, 32u, 479u })
				EXPECT_EQ(framebufferWriteRowAddress(0x200000, 640, bpp, stride, row),
						0x200000u + row * stride);
	// Preserve the existing tightly-packed fallback for zero/short stride.
	EXPECT_EQ(framebufferWriteRowAddress(0, 640, 3, 0, 32), 32u * 1920u);
}

TEST(SoftwareRasterizer, WritesClippedRgb888RowsWithoutTouchingPadding)
{
	constexpr u32 width = 5, height = 4, base = 7, stride = 16;
	std::vector<u8> source(width * height * 4);
	for (size_t i = 0; i < source.size(); ++i)
		source[i] = static_cast<u8>(i);
	std::vector<u8> actual(base + stride * height + 8, 0xa5);
	auto expected = actual;
	struct Clip { struct Point { int x, y; }; Point origin, size; };
	const Clip clip { { 1, 1 }, { 3, 2 } };
	unsigned rows = 0;
	forEachFramebufferWriteRow(width, height, source.data(), base, 3, stride, clip,
			[&](u32 address, const u8 *pixel, u32 xmin, u32 xmax, u32 y) {
				++rows;
				EXPECT_EQ(y, rows);
				for (u32 x = xmin; x < xmax; ++x, pixel += 4)
					for (int channel : { 2, 1, 0 })
						actual.at(address++) = pixel[channel];
			});
	for (u32 y = 1; y < 3; ++y)
		for (u32 x = 1; x < 4; ++x)
			for (u32 c = 0; c < 3; ++c)
				expected[base + y * stride + x * 3 + c] = source[(y * width + x) * 4 + 2 - c];
	EXPECT_EQ(rows, 2u);
	EXPECT_EQ(actual, expected); // Includes clipped pixels, padding, and guards.
}

TEST(SoftwareRasterizer, IntersectsFramebufferClipBeforeFormingSourcePointers)
{
	struct Clip { struct Point { int x, y; }; Point origin, size; };
	std::vector<u8> source(4 * 4 * 4);
	unsigned calls = 0;
	forEachFramebufferWriteRow(4, 4, source.data(), 0, 3, 16,
			Clip { { -1, -1 }, { 3, 3 } },
			[&](u32 address, const u8 *pixel, u32 xmin, u32 xmax, u32 y) {
				EXPECT_EQ(xmin, 0u);
				EXPECT_EQ(xmax, 2u);
				EXPECT_EQ(y, calls++);
				EXPECT_EQ(pixel, source.data() + y * 16);
				EXPECT_EQ(address, y * 16);
			});
	EXPECT_EQ(calls, 2u);
	for (const Clip clip : { Clip { { 0, 0 }, { 0, 4 } },
			Clip { { 5, 0 }, { 3, 4 } }, Clip { { 0, 5 }, { 4, 3 } },
			Clip { { -4, 0 }, { 2, 4 } }, Clip { { 0, 0 }, { -1, 4 } } })
		forEachFramebufferWriteRow(4, 4, source.data(), 0, 3, 16, clip,
				[&](u32, const u8 *, u32, u32, u32) { ADD_FAILURE() << "Empty intersection wrote a row"; });
}

TEST(SoftwareRasterizer, TruncatesBackgroundDepthRegisterLowBits)
{
	ISP_BACKGND_D_type depth {};
	depth.i = 0x3f80000f;
	EXPECT_FLOAT_EQ(depth.get(), 1.f);
	depth.i = 0x4000001f;
	ISP_BACKGND_D_type expected {};
	expected.i = 0x40000010;
	EXPECT_FLOAT_EQ(depth.get(), expected.f);
}

TEST(SoftwareRasterizer, ImplementsAllDepthModes)
{
	EXPECT_FALSE(SoftwareRasterizer::depthTest(DepthMode::Never, 2.f, 1.f));
	EXPECT_TRUE(SoftwareRasterizer::depthTest(DepthMode::Less, 1.f, 2.f));
	EXPECT_TRUE(SoftwareRasterizer::depthTest(DepthMode::Equal, 1.f, 1.f));
	EXPECT_TRUE(SoftwareRasterizer::depthTest(DepthMode::LessOrEqual, 1.f, 1.f));
	EXPECT_TRUE(SoftwareRasterizer::depthTest(DepthMode::Greater, 2.f, 1.f));
	EXPECT_TRUE(SoftwareRasterizer::depthTest(DepthMode::NotEqual, 2.f, 1.f));
	EXPECT_TRUE(SoftwareRasterizer::depthTest(DepthMode::GreaterOrEqual, 1.f, 1.f));
	EXPECT_TRUE(SoftwareRasterizer::depthTest(DepthMode::Always, 0.f, 1.f));
}

TEST(SoftwareRasterizer, OverridesDepthModeForPunchThroughAndAutosort)
{
	EXPECT_EQ(SoftwareRasterizer::selectDepthMode(0, false, false), DepthMode::Never);
	EXPECT_EQ(SoftwareRasterizer::selectDepthMode(0, true, false), DepthMode::GreaterOrEqual);
	EXPECT_EQ(SoftwareRasterizer::selectDepthMode(0, false, true), DepthMode::GreaterOrEqual);
	EXPECT_EQ(SoftwareRasterizer::selectDepthMode(7, false, false), DepthMode::Always);
}

TEST(SoftwareRasterizer, ImplementsPvrCullingModes)
{
	const Vertex a { 0, 0, 1 };
	const Vertex b { 1, 0, 1 };
	const Vertex positive { 0, 1, 1 };
	const Vertex negative { 0, -1, 1 };
	EXPECT_FALSE(SoftwareRasterizer::cullTriangle(0, 2.f, a, b, positive));
	EXPECT_TRUE(SoftwareRasterizer::cullTriangle(1, 2.f, a, b, positive));
	EXPECT_FALSE(SoftwareRasterizer::cullTriangle(1, .5f, a, b, positive));
	EXPECT_FALSE(SoftwareRasterizer::cullTriangle(2, 0.f, a, b, positive));
	EXPECT_TRUE(SoftwareRasterizer::cullTriangle(2, 0.f, a, b, negative));
	EXPECT_TRUE(SoftwareRasterizer::cullTriangle(3, 0.f, a, b, positive));
	EXPECT_FALSE(SoftwareRasterizer::cullTriangle(3, 0.f, a, b, negative));
}

TEST(SoftwareRasterizer, SharedEdgeHasNoCracks)
{
	SoftwareRasterizer rasterizer(4, 4);
	rasterizer.clear({ 0, 0, 0, 0 });
	PipelineState state;
	state.depthMode = DepthMode::Always;
	const Color white { 255, 255, 255, 255 };
	rasterizer.drawTriangle({ 0, 0, 1, white }, { 4, 0, 1, white }, { 0, 4, 1, white }, state);
	rasterizer.drawTriangle({ 4, 0, 1, white }, { 4, 4, 1, white }, { 0, 4, 1, white }, state);
	for (u32 y = 0; y < 4; ++y)
		for (u32 x = 0; x < 4; ++x)
			EXPECT_EQ(rasterizer.pixel(x, y), white) << x << "," << y;
}

TEST(SoftwareRasterizer, InterpolatesColorAndDepth)
{
	SoftwareRasterizer rasterizer(2, 2);
	rasterizer.clear();
	PipelineState state;
	state.depthMode = DepthMode::Always;
	rasterizer.drawTriangle(
			{ 0, 0, 1, { 255, 0, 0, 255 } },
			{ 2, 0, 2, { 0, 255, 0, 255 } },
			{ 0, 2, 3, { 0, 0, 255, 255 } }, state);
	EXPECT_NEAR(rasterizer.depth(0, 0), 1.75f, 0.001f);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 73, 73, 109, 255 }));
}

TEST(SoftwareRasterizer, PreservesFlatShadingProvokingVertexWhenWindingIsNormalized)
{
	SoftwareRasterizer rasterizer(2, 2);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.gouraud = false;
	const Color red { 255, 0, 0, 255 };
	const Color green { 0, 255, 0, 255 };
	const Color blue { 0, 0, 255, 255 };
	// Negative determinant forces the rasterizer to swap vertices 1 and 2.
	rasterizer.drawTriangle({ 0, 0, 1, red }, { 0, 2, 1, green }, { 2, 0, 1, blue }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), blue);
}

TEST(SoftwareRasterizer, BlendsUsingPvrFactors)
{
	const Color source { 200, 100, 50, 128 };
	const Color destination { 20, 40, 80, 64 };
	EXPECT_EQ(SoftwareRasterizer::blend(source, destination, BlendFactor::One, BlendFactor::Zero), source);
	EXPECT_EQ(SoftwareRasterizer::blend(source, destination, BlendFactor::SrcAlpha,
			BlendFactor::OneMinusSrcAlpha), (Color { 110, 70, 65, 96 }));
}

TEST(SoftwareRasterizer, ExhaustivelyMatchesStandardAlphaBlendEquation)
{
	for (unsigned alpha = 0; alpha <= 255; ++alpha)
		for (unsigned sourceValue = 0; sourceValue <= 255; ++sourceValue)
			for (unsigned destinationValue = 0; destinationValue <= 255; ++destinationValue)
			{
				const Color source { static_cast<u8>(sourceValue), static_cast<u8>(sourceValue),
						static_cast<u8>(sourceValue), static_cast<u8>(alpha) };
				const Color destination { static_cast<u8>(destinationValue),
						static_cast<u8>(destinationValue), static_cast<u8>(destinationValue),
						static_cast<u8>(destinationValue) };
				const Color result = SoftwareRasterizer::blend(source, destination,
						BlendFactor::SrcAlpha, BlendFactor::OneMinusSrcAlpha);
				const float sourceFactor = alpha / 255.f;
				const float destinationFactor = 1.f - sourceFactor;
				const u8 expectedRgb = static_cast<u8>(std::clamp(std::lround(
						sourceValue * sourceFactor + destinationValue * destinationFactor),
						0l, 255l));
				const u8 expectedAlpha = static_cast<u8>(std::clamp(std::lround(
						alpha * sourceFactor + destinationValue * destinationFactor), 0l, 255l));
				if (!(result == Color { expectedRgb, expectedRgb, expectedRgb, expectedAlpha }))
				{
					ADD_FAILURE() << "alpha=" << alpha << " source=" << sourceValue
							<< " destination=" << destinationValue;
					return;
				}
			}
}

TEST(SoftwareRasterizer, ImplementsEveryPvrBlendFactorPair)
{
	const Color source { 211, 97, 43, 139 };
	const Color destination { 17, 61, 173, 83 };
	const auto channels = [](Color color) {
		return std::array<unsigned, 4> { color.r, color.g, color.b, color.a };
	};
	const auto sourceChannels = channels(source);
	const auto destinationChannels = channels(destination);
	const auto factor = [&](BlendFactor mode, bool sourceTerm, unsigned channel) {
		const auto& other = sourceTerm ? destinationChannels : sourceChannels;
		switch (mode)
		{
		case BlendFactor::Zero: return 0.f;
		case BlendFactor::One: return 1.f;
		case BlendFactor::OtherColor: return other[channel] / 255.f;
		case BlendFactor::OneMinusOtherColor: return 1.f - other[channel] / 255.f;
		case BlendFactor::SrcAlpha: return source.a / 255.f;
		case BlendFactor::OneMinusSrcAlpha: return 1.f - source.a / 255.f;
		case BlendFactor::DstAlpha: return destination.a / 255.f;
		case BlendFactor::OneMinusDstAlpha: return 1.f - destination.a / 255.f;
		}
		return 0.f;
	};
	for (unsigned sourceMode = 0; sourceMode < 8; ++sourceMode)
		for (unsigned destinationMode = 0; destinationMode < 8; ++destinationMode)
		{
			const auto sf = static_cast<BlendFactor>(sourceMode);
			const auto df = static_cast<BlendFactor>(destinationMode);
			const Color result = SoftwareRasterizer::blend(source, destination, sf, df);
			const auto resultChannels = channels(result);
			for (unsigned channel = 0; channel < 4; ++channel)
			{
				const long expected = std::clamp(std::lround(
						sourceChannels[channel] * factor(sf, true, channel)
						+ destinationChannels[channel] * factor(df, false, channel)),
						0l, 255l);
				EXPECT_EQ(resultChannels[channel], static_cast<unsigned>(expected))
						<< "sourceFactor=" << sourceMode << " destinationFactor="
						<< destinationMode << " channel=" << channel;
			}
		}
}

TEST(SoftwareRasterizer, ImplementsAllTextureShadingInstructions)
{
	const Color base { 100, 150, 200, 128 };
	const Color texel { 200, 100, 50, 64 };
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.texture = SoftwareRasterizer::makeTextureView(&texel, 1, 1, false);
	const Vertex v0 { 0, 0, 1, base };
	const Vertex v1 { 2, 0, 1, base };
	const Vertex v2 { 0, 2, 1, base };
	const Color expected[] {
		texel,
		{ 78, 59, 39, 64 },
		{ 125, 137, 162, 128 },
		{ 78, 59, 39, 32 },
	};
	for (u8 instruction = 0; instruction < 4; ++instruction)
	{
		SoftwareRasterizer rasterizer(1, 1);
		state.shadingInstruction = instruction;
		rasterizer.drawTriangle(v0, v1, v2, state);
		EXPECT_EQ(rasterizer.pixel(0, 0), expected[instruction])
				<< "instruction=" << static_cast<unsigned>(instruction);
	}

	SoftwareRasterizer ignoreAlpha(1, 1);
	state.shadingInstruction = 0;
	state.ignoreTextureAlpha = true;
	ignoreAlpha.drawTriangle(v0, v1, v2, state);
	EXPECT_EQ(ignoreAlpha.pixel(0, 0), (Color { 200, 100, 50, 255 }));
}

TEST(SoftwareRasterizer, PreservesFragmentAlphaOutsidePunchThrough)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	const Color color { 20, 40, 80, 96 };
	rasterizer.drawTriangle({ 0, 0, 1, color }, { 2, 0, 1, color },
			{ 0, 2, 1, color }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), color);

	state.alphaTest = true;
	state.alphaReference = 64;
	rasterizer.drawTriangle({ 0, 0, 1, color }, { 2, 0, 1, color },
			{ 0, 2, 1, color }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 20, 40, 80, 255 }));
}

TEST(SoftwareRasterizer, HonorsDepthWriteDisable)
{
	SoftwareRasterizer rasterizer(1, 1);
	rasterizer.clear({}, .25f);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.depthWrite = false;
	rasterizer.drawTriangle({ 0, 0, 2, {} }, { 2, 0, 2, {} }, { 0, 2, 2, {} }, state);
	EXPECT_FLOAT_EQ(rasterizer.depth(0, 0), .25f);
}

TEST(SoftwareRasterizer, RegionPassDepthClearPreservesAccumulatedColor)
{
	SoftwareRasterizer rasterizer(1, 1);
	const Color red { 255, 0, 0, 255 };
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.shadow = true;
	rasterizer.drawTriangle({ 0, 0, 2, red }, { 2, 0, 2, red }, { 0, 2, 2, red }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), red);
	EXPECT_FLOAT_EQ(rasterizer.depth(0, 0), 2.f);
	EXPECT_NE(rasterizer.stencilBuffer()[0], 0);

	// Region-array Z Clear resets the internal depth/object tag state, not
	// the tile's accumulated color from earlier render passes.
	rasterizer.clearDepth(0.f);
	EXPECT_EQ(rasterizer.pixel(0, 0), red);
	EXPECT_FLOAT_EQ(rasterizer.depth(0, 0), 0.f);
	EXPECT_EQ(rasterizer.stencilBuffer()[0], 0);
}

TEST(SoftwareRasterizer, RegionPassDepthClearIsTileLocal)
{
	SoftwareRasterizer rasterizer(2, 1);
	rasterizer.clear({}, 3.f);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.shadow = true;
	rasterizer.drawTriangle({ 0, 0, 2, {} }, { 4, 0, 2, {} }, { 0, 2, 2, {} }, state);
	ASSERT_NE(rasterizer.stencilBuffer()[0], 0);
	ASSERT_NE(rasterizer.stencilBuffer()[1], 0);
	rasterizer.clearDepthRect(1, 0, 2, 1, .5f);
	EXPECT_FLOAT_EQ(rasterizer.depth(0, 0), 2.f);
	EXPECT_FLOAT_EQ(rasterizer.depth(1, 0), .5f);
	EXPECT_NE(rasterizer.stencilBuffer()[0], 0);
	EXPECT_EQ(rasterizer.stencilBuffer()[1], 0);
}

TEST(SoftwareRasterizer, RegionCoverageMaskRestrictsFragments)
{
	SoftwareRasterizer rasterizer(2, 1);
	const u8 coverage[] { 1, 0 };
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.coverageMask = coverage;
	const Color red { 255, 0, 0, 255 };
	rasterizer.drawTriangle({ 0, 0, 1, red }, { 4, 0, 1, red }, { 0, 2, 1, red }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), red);
	EXPECT_EQ(rasterizer.pixel(1, 0), (Color {}));
}

TEST(SoftwareRasterizer, RegionCoverageMaskRestrictsModifierVolumes)
{
	SoftwareRasterizer rasterizer(2, 1);
	PipelineState polygon;
	polygon.depthMode = DepthMode::Always;
	polygon.shadow = true;
	const Color color { 200, 100, 50, 255 };
	rasterizer.drawTriangle({ 0, 0, .5f, color }, { 4, 0, .5f, color },
			{ 0, 2, .5f, color }, polygon);
	const u8 coverage[] { 1, 0 };
	PipelineState modifier;
	modifier.coverageMask = coverage;
	const Vertex v0 { 0, 0, 1.f };
	const Vertex v1 { 4, 0, 1.f };
	const Vertex v2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(v0, v1, v2, ModifierMode::Xor, modifier);
	rasterizer.drawModifierTriangle(v0, v1, v2, ModifierMode::Inclusion, modifier);
	rasterizer.applyModifierShadow(128);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 100, 50, 25, 255 }));
	EXPECT_EQ(rasterizer.pixel(1, 0), color);
}

TEST(SoftwareRasterizer, FlushesOnlySelectedFrameStoreRectangle)
{
	SoftwareRasterizer rasterizer(3, 2);
	const Color oldPixel { 1, 2, 3, 4 };
	const Color newPixel { 20, 30, 40, 50 };
	rasterizer.clear(newPixel);
	std::vector<Color> frameStore(6, oldPixel);

	// Region coordinates are exclusive at the upper edge and are safely
	// clamped to the render target, just like a partially clipped 32x32 tile.
	rasterizer.flushColorRect(frameStore, 1, -4, 8, 1);
	EXPECT_EQ(frameStore[0], oldPixel);
	EXPECT_EQ(frameStore[1], newPixel);
	EXPECT_EQ(frameStore[2], newPixel);
	EXPECT_EQ(frameStore[3], oldPixel);
	EXPECT_EQ(frameStore[4], oldPixel);
	EXPECT_EQ(frameStore[5], oldPixel);
}

TEST(SoftwareRasterizer, SamplesNearestAndBilinearTextures)
{
	const Color texels[] {
		{ 255, 0, 0, 255 }, { 0, 255, 0, 255 },
		{ 0, 0, 255, 255 }, { 255, 255, 255, 255 },
	};
	PipelineState state;
	state.texture = { texels, 2, 2 };
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, .1f, .1f, state), texels[0]);
	state.bilinearFilter = true;
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, .5f, .5f, state),
			(Color { 128, 128, 128, 255 }));
	const Color precisionTexels[] {
		{ 0, 0, 0, 255 }, { 0, 0, 0, 255 },
		{ 0, 0, 0, 255 }, { 0, 0, 17, 255 },
	};
	state.texture = { precisionTexels, 2, 2 };
	// Quantizing the two horizontal lerps before the vertical lerp produces
	// blue=2 here; the four-texel weighted result rounds once to blue=1.
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, .3f, .65f, state),
			(Color { 0, 0, 1, 255 }));
}

TEST(SoftwareRasterizer, BilinearRoundingUsesConsistentFusedInterpolation)
{
	const Color texels[] {
		{ 193, 193, 193, 193 }, { 166, 166, 166, 166 },
		{ 219, 219, 219, 219 }, { 12, 12, 12, 12 },
	};
	PipelineState state;
	state.texture = { texels, 2, 2 };
	state.bilinearFilter = true;
	state.texelSampleCenter = false;
	// Separate multiply/add rounds to 150 at this boundary. This checks
	// reference portability, not the hardware's filtering precision.
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture,
			0x1.8cfee4p-3f, 0x1.8244eep-2f, state),
			(Color { 149, 149, 149, 149 }));
}

TEST(SoftwareRasterizer, MirrorsBothTextureAxesAcrossFlipPeriods)
{
	const Color texels[] {
		{ 255, 0, 0, 255 }, { 0, 255, 0, 255 },
		{ 0, 0, 255, 255 }, { 255, 255, 255, 255 },
	};
	PipelineState state;
	state.flipU = true;
	state.texture = { texels, 4, 1 };
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, .125f, 0.f, state), texels[0]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 1.125f, 0.f, state), texels[3]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 1.375f, 0.f, state), texels[2]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, -.125f, 0.f, state), texels[0]);

	state.flipU = false;
	state.flipV = true;
	state.texture = { texels, 1, 4 };
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, .125f, state), texels[0]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 1.125f, state), texels[3]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 1.375f, state), texels[2]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, -.125f, state), texels[0]);
}

TEST(SoftwareRasterizer, SelectsMipLevelFromPvrD)
{
	EXPECT_EQ(SoftwareRasterizer::textureFootprint(3.f, 4.f, 0.f, 1.f), 5.f);
	EXPECT_EQ(SoftwareRasterizer::textureFootprint(3.f * 0x1p64f, 4.f * 0x1p64f,
			0.f, 0x1p64f), 5.f * 0x1p64f);
	const Color level0[16] { { 255, 0, 0, 255 } };
	const Color level1[4] { { 0, 255, 0, 255 } };
	const Color level2[1] { { 0, 0, 255, 255 } };
	PipelineState state;
	state.texture = { level0, 4, 4 };
	state.texture.mipLevels[0] = level0;
	state.texture.mipLevels[1] = level1;
	state.texture.mipLevels[2] = level2;
	state.texture.mipLevelCount = 3;
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, 1.f), level0[0]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, 1.99f), level0[0]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, 2.f), level1[0]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, 3.f), level2[0]);
	state.textureSuperSample = true;
	// Supersampling promotes the D-selected mip to the next higher-resolution
	// map (Dreamcast System Architecture, section 3.4.7.2).
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, 2.f), level0[0]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, 3.f), level1[0]);
	state.textureSuperSample = false;
	state.trilinearPassB = true;
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, 1.f), level1[0]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, 2.f), level2[0]);
	for (bool supersample : { false, true })
		for (bool passB : { false, true })
			for (float largeD : { 0x1p32f, 0x1p64f, std::numeric_limits<float>::max() })
			{
				state.textureSuperSample = supersample;
				state.trilinearPassB = passB;
				EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, largeD), level2[0])
						<< "D=" << largeD << " supersample=" << supersample << " passB=" << passB;
			}
}

TEST(SoftwareRasterizer, MapsPvrSmallestFirstMipStorage)
{
	std::array<Color, 21> pixels;
	pixels[0] = { 0, 0, 255, 255 };
	std::fill(pixels.begin() + 1, pixels.begin() + 5, Color { 0, 255, 0, 255 });
	std::fill(pixels.begin() + 5, pixels.end(), Color { 255, 0, 0, 255 });
	PipelineState state;
	state.texture = SoftwareRasterizer::makeTextureView(pixels.data(), 4, 4, true);
	ASSERT_EQ(state.texture.mipLevelCount, 3);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, 1.f), pixels[5]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, 2.f), pixels[1]);
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state, 3.f), pixels[0]);
}

TEST(SoftwareRasterizer, GeneratesSmallestFirstMipChain)
{
	std::array<Color, 16> source;
	for (u32 i = 0; i < source.size(); ++i)
		source[i] = { static_cast<u8>(i * 4), 0, 0, 255 };
	const std::vector<Color> pixels = SoftwareRasterizer::generateMipChain(source.data(), 4);
	ASSERT_EQ(pixels.size(), 21);
	EXPECT_EQ(pixels[0], (Color { 30, 0, 0, 255 }));
	EXPECT_EQ(pixels[1], (Color { 10, 0, 0, 255 }));
	EXPECT_EQ(pixels[2], (Color { 18, 0, 0, 255 }));
	EXPECT_EQ(pixels[3], (Color { 42, 0, 0, 255 }));
	EXPECT_EQ(pixels[4], (Color { 50, 0, 0, 255 }));
	EXPECT_TRUE(std::equal(source.begin(), source.end(), pixels.begin() + 5));
	EXPECT_TRUE(SoftwareRasterizer::generateMipChain(source.data(), 3).empty());
}

TEST(SoftwareRasterizer, LinearlyScalesRenderToTextureOutput)
{
	const std::vector<Color> source {
		{ 255, 0, 0, 255 }, { 0, 255, 0, 255 },
		{ 0, 0, 255, 255 }, { 255, 255, 255, 255 },
	};
	const std::vector<Color> scaled = SoftwareRasterizer::scaleLinear(source, 2, 2, 1, 1);
	ASSERT_EQ(scaled.size(), 1);
	EXPECT_EQ(scaled[0], (Color { 128, 128, 128, 255 }));
}

TEST(SoftwareRasterizer, AppliesPvrThreeLineVerticalFilter)
{
	const std::vector<Color> source {
		{ 0, 0, 0, 0 }, { 100, 100, 100, 100 }, { 200, 200, 200, 200 },
	};
	const std::vector<Color> filtered = SoftwareRasterizer::filterVertical(
			source, 1, 3, 0x40, 0x80);
	ASSERT_EQ(filtered.size(), 3);
	EXPECT_EQ(filtered[0], (Color { 25, 25, 25, 25 }));
	EXPECT_EQ(filtered[1], (Color { 100, 100, 100, 100 }));
	EXPECT_EQ(filtered[2], (Color { 175, 175, 175, 175 }));
}

TEST(SoftwareRasterizer, UsesProgrammedPvrVerticalScaleFactor)
{
	const std::vector<Color> source {
		{ 0, 0, 0, 0 }, { 64, 64, 64, 64 }, { 128, 128, 128, 128 },
		{ 192, 192, 192, 192 }, { 255, 255, 255, 255 },
	};
	const auto scaled = SoftwareRasterizer::scaleVerticalPvr(source, 1, 5, 4, 0x500);
	ASSERT_EQ(scaled.size(), 4u);
	// A 1.25 source-line step samples 0, 1.25, 2.5, and 3.75. A generic
	// destination-size ratio would instead shift every sample by 1/8 line.
	EXPECT_EQ(scaled[0], (Color { 0, 0, 0, 0 }));
	EXPECT_EQ(scaled[1], (Color { 80, 80, 80, 80 }));
	EXPECT_EQ(scaled[2], (Color { 160, 160, 160, 160 }));
	EXPECT_EQ(scaled[3], (Color { 239, 239, 239, 239 }));

	const auto flickerFree = SoftwareRasterizer::scaleVerticalPvr(
			source, 1, 5, 4, 0x401);
	EXPECT_EQ(flickerFree.front(), source.front());
	EXPECT_EQ(flickerFree[1], source[1]);
}

TEST(SoftwareRasterizer, SelectsPvrInterlaceFieldLines)
{
	const std::vector<Color> source {
		{ 0, 0, 0, 0 }, { 10, 10, 10, 10 },
		{ 20, 20, 20, 20 }, { 30, 30, 30, 30 },
	};
	const std::vector<Color> even = SoftwareRasterizer::selectInterlacedField(
			source, 1, 4, 2, false);
	const std::vector<Color> odd = SoftwareRasterizer::selectInterlacedField(
			source, 1, 4, 2, true);
	ASSERT_EQ(even.size(), 2);
	ASSERT_EQ(odd.size(), 2);
	EXPECT_EQ(even[0], source[0]);
	EXPECT_EQ(even[1], source[2]);
	EXPECT_EQ(odd[0], source[1]);
	EXPECT_EQ(odd[1], source[3]);
}

TEST(SoftwareRasterizer, PvrHorizontalScalerAveragesPixelPairs)
{
	const std::vector<Color> source {
		{ 0, 0, 0, 0 }, { 11, 21, 31, 41 },
		{ 20, 40, 60, 80 }, { 40, 80, 120, 160 },
		{ 255, 255, 255, 255 },
	};
	const std::vector<Color> scaled = SoftwareRasterizer::scaleHorizontalHalf(source, 5, 1);
	ASSERT_EQ(scaled.size(), 2);
	EXPECT_EQ(scaled[0], (Color { 5, 10, 15, 20 }));
	EXPECT_EQ(scaled[1], (Color { 30, 60, 90, 120 }));
}

TEST(SoftwareRasterizer, ComposesPvrFramebufferScalerStages)
{
	const std::vector<Color> source {
		{ 0, 0, 0, 255 }, { 10, 10, 10, 255 },
		{ 20, 20, 20, 255 }, { 40, 40, 40, 255 },
		{ 50, 50, 50, 255 }, { 60, 60, 60, 255 },
		{ 60, 60, 60, 255 }, { 80, 80, 80, 255 },
	};
	const auto scaled = SoftwareRasterizer::scaleFramebufferForOutput(source,
			2, 4, 1, 2, false, 0, 0, true, true, true);
	ASSERT_EQ(scaled.size(), 2u);
	// Odd-field selection chooses source rows 1 and 3, then the X scaler
	// averages each pair.
	EXPECT_EQ(scaled[0], (Color { 30, 30, 30, 255 }));
	EXPECT_EQ(scaled[1], (Color { 70, 70, 70, 255 }));
}

TEST(SoftwareRasterizer, HonorsBothPvrDCalculationModes)
{
	std::array<Color, 16> level0;
	std::array<Color, 4> level1;
	level0.fill({ 255, 0, 0, 255 });
	level1.fill({ 0, 255, 0, 255 });
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.texture = { level0.data(), 4, 4 };
	state.texture.mipLevels[0] = level0.data();
	state.texture.mipLevels[1] = level1.data();
	state.texture.mipLevelCount = 2;
	const Vertex v0 { 10, 10, 1, {}, {}, 0, 0 };
	const Vertex v1 { 14, 10, 2, {}, {}, 1, 0 };
	const Vertex v2 { 10, 14, 1, {}, {}, 1, 1 };

	SoftwareRasterizer originMode(16, 16);
	originMode.drawTriangle(v0, v1, v2, state);
	EXPECT_EQ(originMode.pixel(10, 10), level1[0]);

	state.dCalcCtrl = true;
	SoftwareRasterizer firstVertexMode(16, 16);
	firstVertexMode.drawTriangle(v0, v1, v2, state);
	EXPECT_EQ(firstVertexMode.pixel(10, 10), level0[0]);
}

TEST(SoftwareRasterizer, DecodesLegalMipDAdjustAsUnsignedTwoPointTwo)
{
	EXPECT_FLOAT_EQ(SoftwareRasterizer::decodeMipDAdjust(0), 1.f);
	for (u32 encoded = 1; encoded <= 15; ++encoded)
		EXPECT_FLOAT_EQ(SoftwareRasterizer::decodeMipDAdjust(encoded), encoded * .25f);
	EXPECT_FLOAT_EQ(SoftwareRasterizer::decodeMipDAdjust(0x1f), 3.75f);
}

TEST(SoftwareRasterizer, KeepsMipSelectionInNativePvrPixelUnitsWhenUpscaled)
{
	std::array<Color, 16> level0;
	std::array<Color, 4> level1;
	level0.fill({ 255, 0, 0, 255 });
	level1.fill({ 0, 255, 0, 255 });
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.fpuPixelCenter = false;
	state.tspPixelCenter = false;
	state.texture = { level0.data(), 4, 4 };
	state.texture.mipLevels[0] = level0.data();
	state.texture.mipLevels[1] = level1.data();
	state.texture.mipLevelCount = 2;

	SoftwareRasterizer native(4, 4);
	native.drawTriangle({ 0, 0, 1, {}, {}, 0, 0 }, { 4, 0, 1, {}, {}, 2.5f, 0 },
			{ 0, 4, 1, {}, {}, 0, 0 }, state);
	EXPECT_EQ(native.pixel(0, 0), level1[0]);

	state.mipDAdjust = .5f;
	SoftwareRasterizer adjusted(4, 4);
	adjusted.drawTriangle({ 0, 0, 1, {}, {}, 0, 0 }, { 4, 0, 1, {}, {}, 2.5f, 0 },
			{ 0, 4, 1, {}, {}, 0, 0 }, state);
	EXPECT_EQ(adjusted.pixel(0, 0), level0[0]);
	state.mipDAdjust = 1.f;

	state.nativeToHostScaleX = 2.f;
	state.nativeToHostScaleY = 2.f;
	SoftwareRasterizer upscaled(8, 8);
	upscaled.drawTriangle({ 0, 0, 1, {}, {}, 0, 0 }, { 8, 0, 1, {}, {}, 2.5f, 0 },
			{ 0, 8, 1, {}, {}, 0, 0 }, state);
	EXPECT_EQ(upscaled.pixel(0, 0), level1[0]);
}

TEST(SoftwareRasterizer, HonorsHalfOffsetSamplingControls)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.tspPixelCenter = false;
	const Color red { 255, 0, 0, 255 };
	rasterizer.drawTriangle({ 0, 0, 1, red }, { 2, 0, 1, { 0, 255, 0, 255 } },
			{ 0, 2, 1, { 0, 0, 255, 255 } }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), red);

	const Color texels[] {
		{ 255, 0, 0, 255 }, { 0, 255, 0, 255 },
		{ 0, 0, 255, 255 }, { 255, 255, 255, 255 },
	};
	state.texture = { texels, 2, 2 };
	state.texelSampleCenter = true;
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, .25f, .25f, state), texels[0]);
	state.texelSampleCenter = false;
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, .25f, .25f, state), texels[3]);
	state.bilinearFilter = true;
	state.texelSampleCenter = true;
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, .25f, .25f, state), texels[0]);
	// At (0, 0), half-texel sampling straddles all four texture edges. The
	// PVR wraps those adjacent texels unless the corresponding UV axis is
	// clamped (Dreamcast System Architecture, section 3.4.7.2.2).
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state),
			(Color { 128, 128, 128, 255 }));
	state.clampU = true;
	state.clampV = true;
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, 0.f, 0.f, state), texels[0]);
	state.clampU = false;
	state.clampV = false;
	state.texelSampleCenter = false;
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, .25f, .25f, state),
			(Color { 128, 128, 128, 255 }));
}

TEST(SoftwareRasterizer, AveragesFourTextureSuperSamples)
{
	const Color texels[4] {
		{ 255, 0, 0, 255 }, { 0, 255, 0, 255 },
		{ 0, 0, 255, 255 }, { 255, 255, 255, 255 },
	};
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.fpuPixelCenter = false;
	state.tspPixelCenter = false;
	state.textureSuperSample = true;
	state.texture = { texels, 2, 2 };
	const Color white { 255, 255, 255, 255 };
	rasterizer.drawTriangle({ 0, 0, 1, white, {}, 0, 0 },
			{ 1, 0, 1, white, {}, 1, 0 }, { 0, 1, 1, white, {}, 0, 1 }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 128, 128, 128, 255 }));
}

TEST(SoftwareRasterizer, AppliesPunchThroughAlphaReference)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.alphaTest = true;
	state.alphaReference = 128;
	const Color transparentRed { 255, 0, 0, 127 };
	rasterizer.drawTriangle({ 0, 0, 1, transparentRed }, { 2, 0, 1, transparentRed },
			{ 0, 2, 1, transparentRed }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color {}));
}

TEST(SoftwareRasterizer, AppliesPunchThroughAfterBilinearAlphaFiltering)
{
	const Color mixedAlpha[4] {
		{ 255, 0, 0, 255 }, { 0, 255, 0, 255 },
		{ 0, 0, 255, 255 }, { 255, 255, 255, 0 },
	};
	Color opaque[4];
	std::copy(std::begin(mixedAlpha), std::end(mixedAlpha), std::begin(opaque));
	opaque[3].a = 255;
	const Color white { 255, 255, 255, 255 };
	const Vertex a { 0, 0, 1, white, {}, .5f, .5f };
	const Vertex b { 2, 0, 1, white, {}, .5f, .5f };
	const Vertex c { 0, 2, 1, white, {}, .5f, .5f };

	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.alphaTest = true;
	state.alphaReference = 255;
	state.bilinearFilter = true;
	state.shadingInstruction = 0;
	state.texture = { mixedAlpha, 2, 2 };
	SoftwareRasterizer rejected(1, 1);
	rejected.drawTriangle(a, b, c, state);
	// Holly2 alpha-tests the filtered result. One transparent contributor
	// lowers the four-texel average below fully opaque, leaving a hole.
	EXPECT_EQ(rejected.pixel(0, 0), (Color {}));

	state.texture = { opaque, 2, 2 };
	SoftwareRasterizer accepted(1, 1);
	accepted.drawTriangle(a, b, c, state);
	EXPECT_EQ(accepted.pixel(0, 0), (Color { 128, 128, 128, 255 }));
}

TEST(SoftwareRasterizer, AppliesInclusiveExclusiveScissor)
{
	SoftwareRasterizer rasterizer(3, 3);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.scissorEnabled = true;
	state.scissorX0 = 1;
	state.scissorY0 = 1;
	state.scissorX1 = 2;
	state.scissorY1 = 2;
	const Color white { 255, 255, 255, 255 };
	rasterizer.drawTriangle({ 0, 0, 1, white }, { 4, 0, 1, white }, { 0, 4, 1, white }, state);
	EXPECT_EQ(rasterizer.pixel(1, 1), white);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color {}));
	EXPECT_EQ(rasterizer.pixel(2, 1), (Color {}));
}

TEST(SoftwareRasterizer, InterpolatesPvrFogTableEntries)
{
	u32 table[128] {};
	reinterpret_cast<u8 *>(table)[0] = 10;
	reinterpret_cast<u8 *>(table)[1] = 20;
	EXPECT_NEAR(SoftwareRasterizer::fogCoefficient(1.f, 1.f, table), 20.f / 255.f, .0001f);
	EXPECT_NEAR(SoftwareRasterizer::fogCoefficient(1.f, 1.03125f, table), 15.f / 255.f, .0001f);
}

TEST(SoftwareRasterizer, SelectsFogTableAddressFromIeeeExponentAndMantissa)
{
	u32 table[128] {};
	for (u32 index = 0; index < 128; ++index)
	{
		u8 *entry = reinterpret_cast<u8 *>(&table[index]);
		entry[1] = static_cast<u8>(index);
		entry[0] = static_cast<u8>(std::min(127u, index + 1));
	}
	EXPECT_FLOAT_EQ(SoftwareRasterizer::fogCoefficient(1.f, 1.f, table), 0.f);
	EXPECT_FLOAT_EQ(SoftwareRasterizer::fogCoefficient(2.f, 1.f, table), 16.f / 255.f);
	EXPECT_FLOAT_EQ(SoftwareRasterizer::fogCoefficient(4.f, 1.f, table), 32.f / 255.f);
	const float belowTwo = std::nextafter(2.f, 1.f);
	const float coefficient = SoftwareRasterizer::fogCoefficient(belowTwo, 1.f, table);
	EXPECT_GE(coefficient, 15.f / 255.f);
	EXPECT_LT(coefficient, 16.f / 255.f);
}

TEST(SoftwareRasterizer, MatchesPvrFogAddressBoundariesAcrossAllExponentBands)
{
	u32 table[128] {};
	for (u32 index = 0; index < 128; ++index)
	{
		u8 *entry = reinterpret_cast<u8 *>(&table[index]);
		entry[1] = static_cast<u8>(index);
		entry[0] = static_cast<u8>(std::min(127u, index + 1));
	}

	// The PVR clamps 1/W to [1, 255.9999]. Each exact table address is
	// 2^exponent * (16 + mantissa) / 16, with seven exponent/mantissa bits.
	EXPECT_FLOAT_EQ(SoftwareRasterizer::fogCoefficient(.25f, 1.f, table), 0.f);
	for (u32 exponent = 0; exponent < 8; ++exponent)
	{
		for (u32 mantissa = 0; mantissa < 16; ++mantissa)
		{
			const u32 index = exponent * 16 + mantissa;
			const float z = std::ldexp((16.f + mantissa) / 16.f,
					static_cast<int>(exponent));
			EXPECT_FLOAT_EQ(SoftwareRasterizer::fogCoefficient(z, 1.f, table),
					index / 255.f) << "index=" << index;
		}
	}
	EXPECT_FLOAT_EQ(SoftwareRasterizer::fogCoefficient(1024.f, 1.f, table),
			127.f / 255.f);
}

TEST(SoftwareRasterizer, InterpolatesWithinEveryPvrFogMantissaCell)
{
	u32 table[128] {};
	for (u32 index = 0; index < 128; ++index)
	{
		u8 *entry = reinterpret_cast<u8 *>(&table[index]);
		entry[1] = static_cast<u8>(index);
		entry[0] = static_cast<u8>(std::min(127u, index + 1));
	}

	for (u32 exponent = 0; exponent < 8; ++exponent)
	{
		for (u32 mantissa = 0; mantissa < 16; ++mantissa)
		{
			const u32 index = exponent * 16 + mantissa;
			if (index == 127)
				continue;
			const float midpoint = std::ldexp((16.5f + mantissa) / 16.f,
					static_cast<int>(exponent));
			EXPECT_NEAR(SoftwareRasterizer::fogCoefficient(midpoint, 1.f, table),
					(index + .5f) / 255.f, 1e-6f) << "index=" << index;
		}
	}
}

TEST(SoftwareRasterizer, AppliesColorClampBeforeFog)
{
	SoftwareRasterizer rasterizer(1, 1);
	u32 table[128] {};
	reinterpret_cast<u8 *>(table)[0] = 128;
	reinterpret_cast<u8 *>(table)[1] = 128;
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.colorClamp = true;
	state.clampMax = { 100, 100, 100, 100 };
	state.fogMode = 0;
	state.fogRamColor = { 255, 255, 255, 255 };
	state.fogDensity = 1.f;
	state.fogTable = table;
	const Color input { 200, 200, 200, 200 };
	rasterizer.drawTriangle({ 0, 0, 1, input }, { 2, 0, 1, input },
			{ 0, 2, 1, input }, state);
	// The PVR clamps the TSP result to 100 first, then applies 128/255 fog.
	// Applying fog before the clamp would leave RGB at 100 instead of 178.
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 178, 178, 178, 100 }));
}

TEST(SoftwareRasterizer, AppliesModifierVolumeShadow)
{
	SoftwareRasterizer rasterizer(1, 1);
	rasterizer.clear({}, 0.f);
	PipelineState polygon;
	polygon.depthMode = DepthMode::Always;
	polygon.shadow = true;
	const Color color { 200, 100, 50, 255 };
	rasterizer.drawTriangle({ 0, 0, .5f, color }, { 2, 0, .5f, color },
			{ 0, 2, .5f, color }, polygon);
	const Vertex v0 { 0, 0, 1.f };
	const Vertex v1 { 2, 0, 1.f };
	const Vertex v2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(v0, v1, v2, ModifierMode::Xor);
	rasterizer.drawModifierTriangle(v0, v1, v2, ModifierMode::Inclusion);
	rasterizer.applyModifierShadow(128);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 100, 50, 25, 255 }));
	EXPECT_EQ(rasterizer.stencilBuffer()[0] & 3, 0);
}

TEST(SoftwareRasterizer, ModifierVolumeHonorsSceneDepth)
{
	SoftwareRasterizer rasterizer(1, 1);
	rasterizer.clear({}, 2.f);
	const Vertex v0 { 0, 0, 1.f };
	const Vertex v1 { 2, 0, 1.f };
	const Vertex v2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(v0, v1, v2, ModifierMode::Xor);
	EXPECT_EQ(rasterizer.stencilBuffer()[0] & 2, 0);
}

TEST(SoftwareRasterizer, ModifierVolumeSummaryIgnoresSceneDepth)
{
	SoftwareRasterizer rasterizer(1, 1);
	rasterizer.clear({}, 2.f);
	const Vertex front0 { 0, 0, 3.f };
	const Vertex front1 { 2, 0, 3.f };
	const Vertex front2 { 0, 2, 3.f };
	rasterizer.drawModifierTriangle(front0, front1, front2, ModifierMode::Or);
	ASSERT_EQ(rasterizer.stencilBuffer()[0] & 3, 2);

	// Inclusion/exclusion are stencil summary operations. The PVR does not
	// depth-test their geometry, so a summary triangle behind the scene must
	// still collapse the accumulated bit into the final modifier result.
	const Vertex behind0 { 0, 0, 1.f };
	const Vertex behind1 { 2, 0, 1.f };
	const Vertex behind2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(behind0, behind1, behind2, ModifierMode::Inclusion);
	EXPECT_EQ(rasterizer.stencilBuffer()[0] & 3, 1);
}

TEST(SoftwareRasterizer, AppliesFramebufferDitherMatrix)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.dither = true;
	state.ditherDivisorR = state.ditherDivisorG = state.ditherDivisorB = 2;
	const Color color { 10, 20, 30, 40 };
	rasterizer.drawTriangle({ 0, 0, 1, color }, { 2, 0, 1, color }, { 0, 2, 1, color }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 12, 22, 32, 41 }));
}

TEST(SoftwareRasterizer, DithersFinalFramebufferAfterBlending)
{
	SoftwareRasterizer rasterizer(1, 1);
	rasterizer.clear({ 0, 0, 0, 0 });
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.srcBlend = BlendFactor::One;
	state.dstBlend = BlendFactor::One;
	const Color source { 2, 2, 2, 0 };
	for (int layer = 0; layer < 2; ++layer)
		rasterizer.drawTriangle({ 0, 0, 1, source }, { 2, 0, 1, source },
				{ 0, 2, 1, source }, state);
	ASSERT_EQ(rasterizer.pixel(0, 0), (Color { 4, 4, 4, 0 }));
	std::vector<Color> pixels = rasterizer.colorBuffer();
	SoftwareRasterizer::ditherFramebuffer(pixels, 1, 1, 1);
	EXPECT_EQ(pixels[0], (Color { 6, 5, 6, 1 }));
	// At (0,0), RGB565 dither adds (2,1,2). Applying it to each
	// source before additive blending would produce (8,6,8), crossing
	// the red and blue 5-bit packing boundary incorrectly for this model.
	const u16 packed = (pixels[0].r >> 3) << 11
			| (pixels[0].g >> 2) << 5 | (pixels[0].b >> 3);
	EXPECT_EQ(packed, 0x0020);
	EXPECT_NE(packed, 0x0821);
}

TEST(SoftwareRasterizer, QuantizesFramebufferAndAppendsDisplayConcatBits)
{
	std::vector<Color> pixels { { 0xab, 0xcd, 0xef, 0x42 } };
	SoftwareRasterizer::convertFramebufferForDisplay(pixels, 1, 0, 0, 1, 5);
	EXPECT_EQ(pixels[0], (Color { 0xad, 0xcd, 0xed, 0xff }));

	pixels[0] = { 0xab, 0xcd, 0xef, 0x42 };
	SoftwareRasterizer::convertFramebufferForDisplay(pixels, 3, 0, 0, 0, 2);
	EXPECT_EQ(pixels[0], (Color { 0xaa, 0xca, 0xea, 0xff }));

	pixels[0] = { 0xab, 0xcd, 0xef, 0x42 };
	SoftwareRasterizer::convertFramebufferForDisplay(pixels, 6, 0, 0, 3, 7);
	EXPECT_EQ(pixels[0], (Color { 0xab, 0xcd, 0xef, 0xff }));

	// KRGB's K bit becomes the high red bit when the same stored word is
	// interpreted as 565 by the display controller.
	pixels[0] = { 0, 0, 0, 0x42 };
	SoftwareRasterizer::convertFramebufferForDisplay(pixels, 0, 0x80, 0, 1, 5);
	EXPECT_EQ(pixels[0], (Color { 0x85, 0x01, 0x05, 0xff }));

	// Likewise, a 4444 RTT word read as displayable 0555 crosses channel
	// boundaries; it is not equivalent to independently truncating RGB.
	pixels[0] = { 0xab, 0xcd, 0xef, 0x42 };
	SoftwareRasterizer::convertFramebufferForDisplay(pixels, 2, 0, 0, 0, 2);
	EXPECT_EQ(pixels[0], (Color { 0x92, 0xb2, 0x72, 0xff }));

	// The display controller groups the stored bytes independently of the
	// writer. Two 565 pixels therefore become one 0888 display pixel.
	pixels = { { 0x11, 0x22, 0x33, 0x44 }, { 0xaa, 0xbb, 0xcc, 0xdd } };
	SoftwareRasterizer::convertFramebufferForDisplay(pixels, 1, 0, 0, 3, 0);
	ASSERT_EQ(pixels.size(), 1u);
	EXPECT_EQ(pixels[0], (Color { 0xd9, 0x11, 0x06, 0xff }));
	EXPECT_EQ(SoftwareRasterizer::framebufferDisplayWidth(640, 1, 3), 320u);

	// Conversely, one 0888 stored pixel is displayed as two 565 pixels.
	pixels = { { 0x12, 0x34, 0x56, 0x78 } };
	SoftwareRasterizer::convertFramebufferForDisplay(pixels, 5, 0x9a, 0, 1, 0);
	ASSERT_EQ(pixels.size(), 2u);
	EXPECT_EQ(pixels[0], (Color { 0x30, 0x88, 0xb0, 0xff }));
	EXPECT_EQ(pixels[1], (Color { 0x98, 0x40, 0x90, 0xff }));
}

TEST(SoftwareRasterizer, PacksFramebufferAlphaThresholdInclusivelyForEveryBytePair)
{
	// Sega Dreamcast/Dev.Box System Architecture, FB_W_CTRL (p. 343):
	// ARGB1555 stores alpha=1 when pixel alpha >= fb_alpha_threshold.
	// Read the stored word as RGB565 so bit 15 is observable as red bit 7;
	// a normal 0555 scanout would hide exactly the bit under test.
	for (unsigned threshold = 0; threshold < 256; ++threshold)
	{
		std::vector<Color> pixels;
		for (unsigned alpha = 0; alpha < 256; ++alpha)
			pixels.push_back({ 0, 0, 0, static_cast<u8>(alpha) });
		SoftwareRasterizer::convertFramebufferForDisplay(pixels, 3, 0,
				static_cast<u8>(threshold), 1, 0);
		ASSERT_EQ(pixels.size(), 256u);
		for (unsigned alpha = 0; alpha < 256; ++alpha)
			EXPECT_EQ(pixels[alpha], (Color {
					static_cast<u8>(alpha >= threshold ? 128 : 0), 0, 0, 255 }))
					<< "alpha=" << alpha << " threshold=" << threshold;
	}
}

TEST(SoftwareRasterizer, PacksFramebufferKBitIndependentlyOfPixelAlpha)
{
	// KRGB0555 takes only K[7], not pixel alpha or K's lower seven bits.
	for (unsigned k = 0; k < 256; ++k)
	{
		std::vector<Color> pixels { { 0, 0, 0, 0 }, { 0, 0, 0, 255 } };
		SoftwareRasterizer::convertFramebufferForDisplay(pixels, 0,
				static_cast<u8>(k), 0, 1, 0);
		for (const Color& pixel : pixels)
			EXPECT_EQ(pixel, (Color { static_cast<u8>(k & 128), 0, 0, 255 }))
					<< "K=" << k;
	}
}

TEST(SoftwareRasterizer, RebuildsPresentationFromUnditheredFrameStore)
{
	const std::vector<Color> frameStore { { 100, 100, 100, 100 } };
	const auto first = SoftwareRasterizer::prepareFramebufferForDisplay(
			frameStore, 1, 1, true, 1, 0, 0, 1, 0);
	const auto second = SoftwareRasterizer::prepareFramebufferForDisplay(
			frameStore, 1, 1, true, 1, 0, 0, 1, 0);
	EXPECT_EQ(first, second);
	EXPECT_EQ(frameStore[0], (Color { 100, 100, 100, 100 }));
	EXPECT_EQ(first[0], (Color { 96, 100, 96, 255 }));
}

TEST(SoftwareRasterizer, DecodesBumpMapNormalIntoAlpha)
{
	SoftwareRasterizer rasterizer(1, 1);
	const Color texel { 0, 0, 0, 255 };
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.texture = { &texel, 1, 1 };
	state.bumpMap = true;
	state.useOffsetColor = true;
	const Color base { 255, 255, 255, 255 };
	const Color offset { 255, 0, 0, 0 };
	rasterizer.drawTriangle({ 0, 0, 1, base, offset }, { 2, 0, 1, base, offset },
			{ 0, 2, 1, base, offset }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0).r, 255);
	EXPECT_EQ(rasterizer.pixel(0, 0).g, 255);
	EXPECT_EQ(rasterizer.pixel(0, 0).b, 255);
	EXPECT_GE(rasterizer.pixel(0, 0).a, 250);
}

TEST(SoftwareRasterizer, UsesPvrBumpMapAngleEncoding)
{
	SoftwareRasterizer rasterizer(1, 1);
	// ARGB 0x8000 encodes S = 0x80 and R = 0x00. The PVR interprets
	// S as pi/2 * 128/256, not 128/255.
	const Color texel { 0, 0, 0, 136 };
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.texture = { &texel, 1, 1 };
	state.bumpMap = true;
	state.useOffsetColor = true;
	const Color base { 255, 255, 255, 255 };
	// K1 = 0, K2 = 1, K3 = 0, Q = 0: alpha = sin(S).
	const Color bumpParameters { 255, 0, 0, 0 };
	rasterizer.drawTriangle({ 0, 0, 1, base, bumpParameters },
			{ 2, 0, 1, base, bumpParameters }, { 0, 2, 1, base, bumpParameters }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0).a, 180);
}

TEST(SoftwareRasterizer, SupportsSecondaryAccumulationBuffer)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.dstSelect = true;
	const Color red { 255, 0, 0, 255 };
	rasterizer.drawTriangle({ 0, 0, 1, red }, { 2, 0, 1, red }, { 0, 2, 1, red }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color {}));
	state.dstSelect = false;
	state.srcSelect = true;
	const Color green { 0, 255, 0, 255 };
	rasterizer.drawTriangle({ 0, 0, 1, green }, { 2, 0, 1, green }, { 0, 2, 1, green }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), red);
}

TEST(SoftwareRasterizer, PreservesSecondaryAccumulationAcrossDeferredBatches)
{
	SoftwareRasterizer rasterizer(1, 1);
	const Color stored { 40, 80, 120, 160 };
	PipelineState store;
	store.depthMode = DepthMode::Always;
	store.deferBlend = true;
	store.dstSelect = true;
	store.srcBlend = BlendFactor::One;
	store.dstBlend = BlendFactor::Zero;
	rasterizer.drawTriangle({ 0, 0, 1, stored }, { 2, 0, 1, stored }, { 0, 2, 1, stored }, store);
	rasterizer.resolveDeferredFragments();

	rasterizer.clearDeferredFragments();
	PipelineState load;
	load.depthMode = DepthMode::Always;
	load.deferBlend = true;
	load.srcSelect = true;
	load.srcBlend = BlendFactor::One;
	load.dstBlend = BlendFactor::Zero;
	rasterizer.drawTriangle({ 0, 0, 1 }, { 2, 0, 1 }, { 0, 2, 1 }, load);
	rasterizer.resolveDeferredFragments();
	EXPECT_EQ(rasterizer.pixel(0, 0), stored);
}

TEST(SoftwareRasterizer, GeometryPassWritesDepthWithoutColor)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.colorWrite = false;
	const Color red { 255, 0, 0, 255 };
	rasterizer.drawTriangle({ 0, 0, 2, red }, { 2, 0, 2, red }, { 0, 2, 2, red }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color {}));
	EXPECT_FLOAT_EQ(rasterizer.depth(0, 0), 2.f);
}

TEST(SoftwareRasterizer, ShadesOnlyOpaqueIspWinner)
{
	SoftwareRasterizer rasterizer(1, 1);
	rasterizer.clearVisibility();
	const Color red { 255, 0, 0, 255 };
	const Color green { 0, 255, 0, 255 };
	PipelineState geometry;
	geometry.depthMode = DepthMode::GreaterOrEqual;
	geometry.colorWrite = false;
	geometry.recordVisibility = true;
	geometry.sequence = 1;
	rasterizer.drawTriangle({ 0, 0, 1, red }, { 2, 0, 1, red }, { 0, 2, 1, red }, geometry);
	geometry.sequence = 2;
	rasterizer.drawTriangle({ 0, 0, 2, green }, { 2, 0, 2, green }, { 0, 2, 2, green }, geometry);

	rasterizer.clearDepth();
	PipelineState color;
	color.depthMode = DepthMode::GreaterOrEqual;
	color.testVisibility = true;
	color.srcBlend = BlendFactor::One;
	color.dstBlend = BlendFactor::One;
	color.sequence = 1;
	rasterizer.drawTriangle({ 0, 0, 1, red }, { 2, 0, 1, red }, { 0, 2, 1, red }, color);
	color.sequence = 2;
	rasterizer.drawTriangle({ 0, 0, 2, green }, { 2, 0, 2, green }, { 0, 2, 2, green }, color);
	EXPECT_EQ(rasterizer.pixel(0, 0), green);
}

TEST(SoftwareRasterizer, PunchThroughHoleExposesNextVisiblePolygon)
{
	SoftwareRasterizer rasterizer(1, 1);
	rasterizer.clearVisibility();
	const Color opaqueRed { 255, 0, 0, 255 };
	const Color transparentGreen { 0, 255, 0, 0 };
	PipelineState geometry;
	geometry.depthMode = DepthMode::GreaterOrEqual;
	geometry.colorWrite = false;
	geometry.recordVisibility = true;
	geometry.alphaTest = true;
	geometry.alphaReference = 128;
	geometry.sequence = 1;
	rasterizer.drawTriangle({ 0, 0, 1, opaqueRed }, { 2, 0, 1, opaqueRed },
			{ 0, 2, 1, opaqueRed }, geometry);
	geometry.sequence = 2;
	rasterizer.drawTriangle({ 0, 0, 2, transparentGreen }, { 2, 0, 2, transparentGreen },
			{ 0, 2, 2, transparentGreen }, geometry);
	EXPECT_FLOAT_EQ(rasterizer.depth(0, 0), 1.f);

	rasterizer.clearDepth();
	PipelineState color = geometry;
	color.colorWrite = true;
	color.recordVisibility = false;
	color.testVisibility = true;
	color.sequence = 1;
	rasterizer.drawTriangle({ 0, 0, 1, opaqueRed }, { 2, 0, 1, opaqueRed },
			{ 0, 2, 1, opaqueRed }, color);
	color.sequence = 2;
	rasterizer.drawTriangle({ 0, 0, 2, transparentGreen }, { 2, 0, 2, transparentGreen },
			{ 0, 2, 2, transparentGreen }, color);
	EXPECT_EQ(rasterizer.pixel(0, 0), opaqueRed);
}

TEST(SoftwareRasterizer, PunchThroughEqualDepthSelectsLaterAcceptedPolygon)
{
	SoftwareRasterizer rasterizer(1, 1);
	rasterizer.clearVisibility();
	const Color red { 255, 0, 0, 255 };
	const Color green { 0, 255, 0, 255 };
	const Color transparentBlue { 0, 0, 255, 0 };
	PipelineState geometry;
	geometry.depthMode = DepthMode::GreaterOrEqual;
	geometry.colorWrite = false;
	geometry.recordVisibility = true;
	geometry.alphaTest = true;
	geometry.alphaReference = 128;
	geometry.sequence = 1;
	rasterizer.drawTriangle({ 0, 0, 1, red }, { 2, 0, 1, red },
			{ 0, 2, 1, red }, geometry);
	geometry.sequence = 2;
	rasterizer.drawTriangle({ 0, 0, 1, green }, { 2, 0, 1, green },
			{ 0, 2, 1, green }, geometry);
	geometry.sequence = 3;
	rasterizer.drawTriangle({ 0, 0, 1, transparentBlue }, { 2, 0, 1, transparentBlue },
			{ 0, 2, 1, transparentBlue }, geometry);

	rasterizer.clearDepth();
	PipelineState color = geometry;
	color.colorWrite = true;
	color.recordVisibility = false;
	color.testVisibility = true;
	for (const auto& fragment : { std::pair { 1u, red }, std::pair { 2u, green },
			std::pair { 3u, transparentBlue } })
	{
		color.sequence = fragment.first;
		rasterizer.drawTriangle({ 0, 0, 1, fragment.second },
				{ 2, 0, 1, fragment.second }, { 0, 2, 1, fragment.second }, color);
	}
	EXPECT_EQ(rasterizer.pixel(0, 0), green);
}

TEST(SoftwareRasterizer, PunchThroughAreaOneRejectPreservesAreaZero)
{
	SoftwareRasterizer rasterizer(1, 1);
	const Color red { 255, 0, 0, 255 };
	const Color transparentGreen { 0, 255, 0, 0 };
	PipelineState area0;
	area0.depthMode = DepthMode::GreaterOrEqual;
	area0.alphaTest = true;
	area0.alphaReference = 128;
	rasterizer.drawTriangle({ 0, 0, 1, red }, { 2, 0, 1, red },
			{ 0, 2, 1, red }, area0);

	PipelineState shadowed;
	shadowed.depthMode = DepthMode::GreaterOrEqual;
	shadowed.shadow = true;
	shadowed.colorWrite = false;
	rasterizer.drawTriangle({ 0, 0, 1, red }, { 2, 0, 1, red },
			{ 0, 2, 1, red }, shadowed);
	rasterizer.drawModifierTriangle({ 0, 0, 2 }, { 2, 0, 2 }, { 0, 2, 2 },
			ModifierMode::Xor);
	rasterizer.drawModifierTriangle({ 0, 0, 2 }, { 2, 0, 2 }, { 0, 2, 2 },
			ModifierMode::Inclusion);

	PipelineState area1 = area0;
	area1.useArea1VertexData = true;
	area1.modifierArea1Only = true;
	Vertex area1v0 { 0, 0, 1 };
	Vertex area1v1 { 2, 0, 1 };
	Vertex area1v2 { 0, 2, 1 };
	area1v0.color1 = area1v1.color1 = area1v2.color1 = transparentGreen;
	rasterizer.drawTriangle(area1v0, area1v1, area1v2, area1);
	EXPECT_EQ(rasterizer.pixel(0, 0), red);

	const Color blue { 0, 0, 255, 200 };
	area1v0.color1 = area1v1.color1 = area1v2.color1 = blue;
	rasterizer.drawTriangle(area1v0, area1v1, area1v2, area1);
	// Accepted Punch Through output is opaque even when its shading alpha is
	// merely above PT_ALPHA_REF rather than already equal to 0xff.
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 0, 0, 255, 255 }));
}

TEST(SoftwareRasterizer, SelectsAreaOneInsideModifierVolume)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState geometry;
	geometry.depthMode = DepthMode::Always;
	geometry.colorWrite = false;
	geometry.shadow = true;
	const Color red { 255, 0, 0, 255 };
	const Color green { 0, 255, 0, 255 };
	Vertex v0 { 0, 0, .5f, red };
	Vertex v1 { 2, 0, .5f, red };
	Vertex v2 { 0, 2, .5f, red };
	v0.color1 = v1.color1 = v2.color1 = green;
	rasterizer.drawTriangle(v0, v1, v2, geometry);
	const Vertex m0 { 0, 0, 1.f };
	const Vertex m1 { 2, 0, 1.f };
	const Vertex m2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Xor);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Inclusion);
	rasterizer.restoreDepth(std::vector<float>(1, 0.f));
	PipelineState area1;
	area1.depthMode = DepthMode::Always;
	PipelineState color = area1;
	color.area1 = &area1;
	rasterizer.drawTriangle(v0, v1, v2, color);
	EXPECT_EQ(rasterizer.pixel(0, 0), green);
}

TEST(SoftwareRasterizer, ShadesSingleVolumePolygonBeforeBlending)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState geometry;
	geometry.depthMode = DepthMode::Always;
	geometry.colorWrite = false;
	geometry.shadow = true;
	const Color color { 200, 100, 50, 255 };
	const Vertex v0 { 0, 0, .5f, color };
	const Vertex v1 { 2, 0, .5f, color };
	const Vertex v2 { 0, 2, .5f, color };
	rasterizer.drawTriangle(v0, v1, v2, geometry);
	const Vertex m0 { 0, 0, 1.f };
	const Vertex m1 { 2, 0, 1.f };
	const Vertex m2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Xor);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Inclusion);
	rasterizer.restoreDepth(std::vector<float>(1, 0.f));
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.shadeScale = 128;
	rasterizer.drawTriangle(v0, v1, v2, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 100, 50, 25, 255 }));
}

TEST(SoftwareRasterizer, IntensityVolumeScalesBaseAndOffsetBeforeTexture)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState geometry;
	geometry.depthMode = DepthMode::Always;
	geometry.colorWrite = false;
	geometry.shadow = true;
	const Color base { 200, 100, 50, 255 };
	Vertex v0 { 0, 0, .5f, base };
	Vertex v1 { 2, 0, .5f, base };
	Vertex v2 { 0, 2, .5f, base };
	v0.offsetColor = v1.offsetColor = v2.offsetColor = { 40, 20, 10, 0 };
	rasterizer.drawTriangle(v0, v1, v2, geometry);
	const Vertex m0 { 0, 0, 1.f };
	const Vertex m1 { 2, 0, 1.f };
	const Vertex m2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Xor);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Inclusion);
	rasterizer.restoreDepth(std::vector<float>(1, 0.f));

	const Color texel { 100, 120, 140, 255 };
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.texture = SoftwareRasterizer::makeTextureView(&texel, 1, 1, false);
	state.shadingInstruction = 0; // Texture replace: Base Color has no RGB contribution.
	state.useOffsetColor = true;
	state.shadeScale = 128;
	rasterizer.drawTriangle(v0, v1, v2, state);
	// Texture RGB is unchanged; only Offset Color is halved before addition.
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 120, 130, 145, 255 }));
}

TEST(SoftwareRasterizer, DoesNotIntensityShadeParameterSelectionPolygonWithoutAreaOne)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState geometry;
	geometry.depthMode = DepthMode::Always;
	geometry.colorWrite = false;
	geometry.shadow = true;
	const Color color { 200, 100, 50, 255 };
	const Vertex v0 { 0, 0, .5f, color };
	const Vertex v1 { 2, 0, .5f, color };
	const Vertex v2 { 0, 2, .5f, color };
	rasterizer.drawTriangle(v0, v1, v2, geometry);
	const Vertex m0 { 0, 0, 1.f };
	const Vertex m1 { 2, 0, 1.f };
	const Vertex m2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Xor);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Inclusion);
	rasterizer.restoreDepth(std::vector<float>(1, 0.f));
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.intensityShadow = false;
	state.shadeScale = 128;
	rasterizer.drawTriangle(v0, v1, v2, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), color);
}

TEST(SoftwareRasterizer, ResolvesTranslucentFragmentsFarToNear)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.depthWrite = false;
	state.deferBlend = true;
	state.srcBlend = BlendFactor::SrcAlpha;
	state.dstBlend = BlendFactor::OneMinusSrcAlpha;
	const Color red { 200, 0, 0, 128 };
	const Color green { 0, 200, 0, 128 };
	state.sequence = 2;
	rasterizer.drawTriangle({ 0, 0, 2, red }, { 2, 0, 2, red }, { 0, 2, 2, red }, state);
	state.sequence = 1;
	rasterizer.drawTriangle({ 0, 0, 1, green }, { 2, 0, 1, green }, { 0, 2, 1, green }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color {}));
	rasterizer.resolveDeferredFragments();
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 100, 50, 0, 96 }));
}

TEST(SoftwareRasterizer, UsesSequenceToBreakEqualDepthTies)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.deferBlend = true;
	state.srcBlend = BlendFactor::SrcAlpha;
	state.dstBlend = BlendFactor::OneMinusSrcAlpha;
	const Color red { 200, 0, 0, 128 };
	const Color green { 0, 200, 0, 128 };
	state.sequence = 2;
	rasterizer.drawTriangle({ 0, 0, 1, red }, { 2, 0, 1, red }, { 0, 2, 1, red }, state);
	state.sequence = 1;
	rasterizer.drawTriangle({ 0, 0, 1, green }, { 2, 0, 1, green }, { 0, 2, 1, green }, state);
	rasterizer.resolveDeferredFragments();
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 100, 50, 0, 96 }));
}

TEST(SoftwareRasterizer, AppliesModifierVolumesPerTranslucentFragment)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState translucent;
	translucent.depthMode = DepthMode::Always;
	translucent.depthWrite = false;
	translucent.deferBlend = true;
	translucent.shadow = true;
	translucent.shadeScale = 128;
	const Color color { 200, 100, 50, 255 };
	rasterizer.drawTriangle({ 0, 0, .5f, color }, { 2, 0, .5f, color },
			{ 0, 2, .5f, color }, translucent);
	const Vertex m0 { 0, 0, 1.f };
	const Vertex m1 { 2, 0, 1.f };
	const Vertex m2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Xor, {}, true);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Inclusion, {}, true);
	rasterizer.resolveDeferredFragments();
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 100, 50, 25, 255 }));
}

TEST(SoftwareRasterizer, ModifierExclusionClearsOnlyOverlappingVolumes)
{
	const Color color { 200, 100, 50, 255 };
	const Vertex polygon[3] {
		{ 0, 0, .5f, color }, { 2, 0, .5f, color }, { 0, 2, .5f, color }
	};
	const Vertex modifier[3] {
		{ 0, 0, 1.f }, { 2, 0, 1.f }, { 0, 2, 1.f }
	};

	SoftwareRasterizer retained(1, 1);
	PipelineState geometry;
	geometry.depthMode = DepthMode::Always;
	geometry.shadow = true;
	retained.drawTriangle(polygon[0], polygon[1], polygon[2], geometry);
	retained.drawModifierTriangle(modifier[0], modifier[1], modifier[2], ModifierMode::Xor);
	retained.drawModifierTriangle(modifier[0], modifier[1], modifier[2], ModifierMode::Inclusion);
	// The exclusion result is inverted before AND, so no new intersection
	// retains the prior summary.
	retained.drawModifierTriangle(modifier[0], modifier[1], modifier[2], ModifierMode::Exclusion);
	retained.applyModifierShadow(128);
	EXPECT_EQ(retained.pixel(0, 0), (Color { 100, 50, 25, 255 }));

	SoftwareRasterizer excluded(1, 1);
	excluded.drawTriangle(polygon[0], polygon[1], polygon[2], geometry);
	excluded.drawModifierTriangle(modifier[0], modifier[1], modifier[2], ModifierMode::Xor);
	excluded.drawModifierTriangle(modifier[0], modifier[1], modifier[2], ModifierMode::Inclusion);
	// A new intersection makes the inverted result zero and clears the summary.
	excluded.drawModifierTriangle(modifier[0], modifier[1], modifier[2], ModifierMode::Xor);
	excluded.drawModifierTriangle(modifier[0], modifier[1], modifier[2], ModifierMode::Exclusion);
	excluded.applyModifierShadow(128);
	EXPECT_EQ(excluded.pixel(0, 0), color);
}

TEST(SoftwareRasterizer, DeferredModifierExclusionUsesPerFragmentSummary)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState translucent;
	translucent.depthMode = DepthMode::Always;
	translucent.depthWrite = false;
	translucent.deferBlend = true;
	translucent.shadow = true;
	translucent.shadeScale = 128;
	const Color color { 200, 100, 50, 255 };
	rasterizer.drawTriangle({ 0, 0, .5f, color }, { 2, 0, .5f, color },
			{ 0, 2, .5f, color }, translucent);
	const Vertex m0 { 0, 0, 1.f };
	const Vertex m1 { 2, 0, 1.f };
	const Vertex m2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Xor, {}, true);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Inclusion, {}, true);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Xor, {}, true);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Exclusion, {}, true);
	rasterizer.resolveDeferredFragments();
	EXPECT_EQ(rasterizer.pixel(0, 0), color);
}

TEST(SoftwareRasterizer, DeferredModifierVolumeRejectsEqualDepth)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState translucent;
	translucent.depthMode = DepthMode::Always;
	translucent.depthWrite = false;
	translucent.deferBlend = true;
	translucent.shadow = true;
	translucent.shadeScale = 128;
	const Color color { 200, 100, 50, 255 };
	rasterizer.drawTriangle({ 0, 0, 1.f, color }, { 2, 0, 1.f, color },
			{ 0, 2, 1.f, color }, translucent);
	const Vertex m0 { 0, 0, 1.f };
	const Vertex m1 { 2, 0, 1.f };
	const Vertex m2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Xor, {}, true);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Inclusion, {}, true);
	rasterizer.resolveDeferredFragments();
	EXPECT_EQ(rasterizer.pixel(0, 0), color);
}

TEST(SoftwareRasterizer, DeferredIntensityVolumeDoesNotScaleReplacedTexture)
{
	SoftwareRasterizer rasterizer(1, 1);
	const Color texel { 80, 120, 160, 255 };
	PipelineState translucent;
	translucent.depthMode = DepthMode::Always;
	translucent.depthWrite = false;
	translucent.deferBlend = true;
	translucent.shadow = true;
	translucent.shadeScale = 128;
	translucent.texture = SoftwareRasterizer::makeTextureView(&texel, 1, 1, false);
	translucent.shadingInstruction = 0;
	const Color base { 200, 100, 50, 255 };
	rasterizer.drawTriangle({ 0, 0, .5f, base }, { 2, 0, .5f, base },
			{ 0, 2, .5f, base }, translucent);
	const Vertex m0 { 0, 0, 1.f };
	const Vertex m1 { 2, 0, 1.f };
	const Vertex m2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Xor, {}, true);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Inclusion, {}, true);
	rasterizer.resolveDeferredFragments();
	EXPECT_EQ(rasterizer.pixel(0, 0), texel);
}

TEST(SoftwareRasterizer, DoesNotIntensityShadeDeferredParameterSelectionFragment)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState translucent;
	translucent.depthMode = DepthMode::Always;
	translucent.depthWrite = false;
	translucent.deferBlend = true;
	translucent.shadow = true;
	translucent.intensityShadow = false;
	translucent.shadeScale = 128;
	const Color color { 200, 100, 50, 255 };
	rasterizer.drawTriangle({ 0, 0, .5f, color }, { 2, 0, .5f, color },
			{ 0, 2, .5f, color }, translucent);
	const Vertex m0 { 0, 0, 1.f };
	const Vertex m1 { 2, 0, 1.f };
	const Vertex m2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Xor, {}, true);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Inclusion, {}, true);
	rasterizer.resolveDeferredFragments();
	EXPECT_EQ(rasterizer.pixel(0, 0), color);
}

TEST(SoftwareRasterizer, SelectsAreaOnePerTranslucentFragment)
{
	SoftwareRasterizer rasterizer(1, 1);
	const Color red { 255, 0, 0, 255 };
	const Color green { 0, 255, 0, 255 };
	Vertex v0 { 0, 0, .5f, red };
	Vertex v1 { 2, 0, .5f, red };
	Vertex v2 { 0, 2, .5f, red };
	v0.color1 = v1.color1 = v2.color1 = green;
	PipelineState area0;
	area0.depthMode = DepthMode::Always;
	area0.depthWrite = false;
	area0.deferBlend = true;
	area0.shadow = true;
	area0.sequence = 7;
	rasterizer.drawTriangle(v0, v1, v2, area0);
	PipelineState area1 = area0;
	area1.deferArea1 = true;
	area1.useArea1VertexData = true;
	rasterizer.drawTriangle(v0, v1, v2, area1);
	const Vertex m0 { 0, 0, 1.f };
	const Vertex m1 { 2, 0, 1.f };
	const Vertex m2 { 0, 2, 1.f };
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Xor, {}, true);
	rasterizer.drawModifierTriangle(m0, m1, m2, ModifierMode::Inclusion, {}, true);
	rasterizer.resolveDeferredFragments();
	EXPECT_EQ(rasterizer.pixel(0, 0), green);
}

TEST(SoftwareRasterizer, AppliesTrilinearPassWeight)
{
	SoftwareRasterizer rasterizer(1, 1);
	std::array<Color, 16> level0;
	std::array<Color, 4> level1;
	level0.fill({ 200, 100, 40, 128 });
	level1.fill({ 200, 100, 40, 128 });
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.fpuPixelCenter = false;
	state.tspPixelCenter = false;
	state.texture = { level0.data(), 4, 4 };
	state.texture.mipLevels[0] = level0.data();
	state.texture.mipLevels[1] = level1.data();
	state.texture.mipLevelCount = 2;
	state.trilinearFilter = true;
	const Color white { 255, 255, 255, 255 };
	// du/dx * texture width = 1.25, so pass A is weighted by .75.
	rasterizer.drawTriangle({ 0, 0, 1, white, {}, 0, 0 },
			{ 1, 0, 1, white, {}, .3125f, 0 }, { 0, 1, 1, white, {}, 0, 0 }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color { 150, 75, 30, 96 }));

	SoftwareRasterizer magnified(1, 1);
	// D = .5 still selects the full-size map, but its .5 fractional part
	// weights trilinear Pass A. Clamping D to one would incorrectly leave
	// the source color unscaled.
	magnified.drawTriangle({ 0, 0, 1, white, {}, 0, 0 },
			{ 1, 0, 1, white, {}, .125f, 0 }, { 0, 1, 1, white, {}, 0, 0 }, state);
	EXPECT_EQ(magnified.pixel(0, 0), (Color { 100, 50, 20, 64 }));
}

TEST(SoftwareRasterizer, ForceNearestDisablesEveryTextureFilteringStage)
{
	Color texels[5] {};
	PipelineState state;
	state.texture = SoftwareRasterizer::makeTextureView(texels, 2, 2, true);
	state.bilinearFilter = true;
	state.textureSuperSample = true;
	state.trilinearFilter = true;
	ASSERT_GT(state.texture.mipLevelCount, 1);

	SoftwareRasterizer::forceNearestFiltering(state);

	EXPECT_FALSE(state.bilinearFilter);
	EXPECT_FALSE(state.textureSuperSample);
	EXPECT_FALSE(state.trilinearFilter);
	EXPECT_EQ(state.texture.mipLevelCount, 1);
}

TEST(SoftwareRasterizer, RejectsInvalidAndSafelyClampsExtremeCoordinates)
{
	SoftwareRasterizer rasterizer(1, 1);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	const Color red { 255, 0, 0, 255 };
	const float infinity = std::numeric_limits<float>::infinity();
	rasterizer.drawTriangle({ infinity, 0, 1, red }, { 2, 0, 1, red },
			{ 0, 2, 1, red }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color {}));

	rasterizer.drawTriangle({ -1.e30f, -1.e30f, 1, red }, { 1.e30f, -1.e30f, 1, red },
			{ 0, 1.e30f, 1, red }, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), red);

	rasterizer.clear();
	rasterizer.drawModifierTriangle({ infinity, 0, 1 }, { 2, 0, 1 }, { 0, 2, 1 },
			ModifierMode::Xor, state);
	EXPECT_EQ(rasterizer.pixel(0, 0), (Color {}));

	const Color texels[] { red, { 0, 255, 0, 255 } };
	state.texture = { texels, 2, 1 };
	state.flipU = true;
	EXPECT_EQ(SoftwareRasterizer::sampleTexture(state.texture, infinity, 0, state, infinity), red);
	EXPECT_NO_FATAL_FAILURE(SoftwareRasterizer::sampleTexture(state.texture, 1.e30f, 0, state));
}
