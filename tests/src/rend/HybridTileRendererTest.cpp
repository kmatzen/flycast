#include "rend/soft/HybridTileRenderer.h"
#include "rend/soft/SoftwareRasterizer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <gtest/gtest.h>
#include <limits>

using namespace rend::soft;

#ifdef TARGET_MAC
namespace {

u32 pack(Color color)
{
	return static_cast<u32>(color.r) | static_cast<u32>(color.g) << 8
			| static_cast<u32>(color.b) << 16 | static_cast<u32>(color.a) << 24;
}

hybrid::Triangle triangle(const Vertex& a, const Vertex& b, const Vertex& c,
		u32 state)
{
	hybrid::Triangle result {};
	const Vertex vertices[] { a, b, c };
	result.minX = result.minY = std::numeric_limits<s32>::max();
	result.maxX = result.maxY = std::numeric_limits<s32>::min();
	for (u32 i = 0; i < 3; ++i)
	{
		result.x[i] = static_cast<s32>(vertices[i].x * 16.f);
		result.y[i] = static_cast<s32>(vertices[i].y * 16.f);
		result.z[i] = vertices[i].z;
		result.u[i] = vertices[i].u;
		result.v[i] = vertices[i].v;
		result.color[i] = pack(vertices[i].color);
		result.offsetColor[i] = pack(vertices[i].offsetColor);
		result.minX = std::min(result.minX, static_cast<s32>(vertices[i].x));
		result.minY = std::min(result.minY, static_cast<s32>(vertices[i].y));
		result.maxX = std::max(result.maxX, static_cast<s32>(vertices[i].x - 1));
		result.maxY = std::max(result.maxY, static_cast<s32>(vertices[i].y - 1));
	}
	result.state = state;
	result.mipLevelCount = 0;
	result.shadeScale = 256;
	result.area1Index = std::numeric_limits<u32>::max();
	result.fpuSampleOffset = 8;
	result.tspSampleOffset = 8;
	result.clampMax = 0xffffffffu;
	return result;
}

void appendQuad(std::vector<hybrid::Triangle>& output, float depth, Color color,
		u32 state)
{
	const Vertex a { 0, 0, depth, color };
	const Vertex b { 2, 0, depth, color };
	const Vertex c { 0, 2, depth, color };
	const Vertex d { 2, 2, depth, color };
	output.push_back(triangle(a, b, c, state));
	output.push_back(triangle(b, d, c, state));
}

void appendTexturedQuad(std::vector<hybrid::Triangle>& output, float depth, Color color,
		u32 state, u32 width, u32 height, float uv0 = 0.f, float uv1 = 1.f)
{
	const Vertex a { 0, 0, depth, color, {}, uv0, uv0 };
	const Vertex b { 2, 0, depth, color, {}, uv1, uv0 };
	const Vertex c { 0, 2, depth, color, {}, uv0, uv1 };
	const Vertex d { 2, 2, depth, color, {}, uv1, uv1 };
	for (auto command : { triangle(a, b, c, state), triangle(b, d, c, state) })
	{
		command.textureOffset = 0;
		command.textureWidth = width;
		command.textureHeight = height;
		command.mipOffsets[0] = 0;
		command.mipLevelCount = 1;
		output.push_back(command);
	}
}

void drawQuad(SoftwareRasterizer& output, float depth, Color color,
		const PipelineState& state)
{
	const Vertex a { 0, 0, depth, color };
	const Vertex b { 2, 0, depth, color };
	const Vertex c { 0, 2, depth, color };
	const Vertex d { 2, 2, depth, color };
	output.drawTriangle(a, b, c, state);
	output.drawTriangle(b, d, c, state);
}

void drawTexturedQuad(SoftwareRasterizer& output, float depth, Color color,
		const PipelineState& state, float uv0 = 0.f, float uv1 = 1.f)
{
	const Vertex a { 0, 0, depth, color, {}, uv0, uv0 };
	const Vertex b { 2, 0, depth, color, {}, uv1, uv0 };
	const Vertex c { 0, 2, depth, color, {}, uv0, uv1 };
	const Vertex d { 2, 2, depth, color, {}, uv1, uv1 };
	output.drawTriangle(a, b, c, state);
	output.drawTriangle(b, d, c, state);
}

void expectEqual(const SoftwareRasterizer& cpu, const std::vector<u32>& gpuColors,
		const std::vector<float>& gpuDepths)
{
	ASSERT_EQ(gpuColors.size(), cpu.colorBuffer().size());
	ASSERT_EQ(gpuDepths.size(), cpu.depthBuffer().size());
	for (size_t i = 0; i < gpuColors.size(); ++i)
	{
		EXPECT_EQ(gpuColors[i], pack(cpu.colorBuffer()[i])) << "pixel=" << i;
		EXPECT_FLOAT_EQ(gpuDepths[i], cpu.depthBuffer()[i]) << "pixel=" << i;
	}
}

} // namespace

TEST(HybridTileRenderer, MatchesOracleForOpaqueDepthWinner)
{
	SoftwareRasterizer cpu(2, 2);
	PipelineState state;
	state.depthMode = DepthMode::GreaterOrEqual;
	const Color far { 20, 40, 180, 255 };
	const Color near { 220, 80, 30, 255 };
	drawQuad(cpu, 1.f, far, state);
	drawQuad(cpu, 2.f, near, state);

	std::vector<hybrid::Triangle> commands;
	const u32 gpuState = hybrid::makeState(6, true, false, false, true, 0, 1, 0);
	appendQuad(commands, 1.f, far, gpuState);
	appendQuad(commands, 2.f, near, gpuState);
	const std::vector<u32> texture { 0xffffffffu };
	hybrid::Frame frame { 2, 2, &commands, &texture };
	std::vector<u32> colors;
	std::vector<float> depths;
	ASSERT_TRUE(hybrid::render(frame, colors, depths));
	expectEqual(cpu, colors, depths);
}

TEST(HybridTileRenderer, MatchesOracleForAutosortedAlphaBlend)
{
	SoftwareRasterizer cpu(2, 2);
	PipelineState opaque;
	opaque.depthMode = DepthMode::GreaterOrEqual;
	const Color background { 20, 40, 180, 255 };
	drawQuad(cpu, 1.f, background, opaque);
	PipelineState translucent;
	translucent.depthMode = DepthMode::GreaterOrEqual;
	translucent.depthWrite = false;
	translucent.deferBlend = true;
	translucent.srcBlend = BlendFactor::SrcAlpha;
	translucent.dstBlend = BlendFactor::OneMinusSrcAlpha;
	const Color overlay { 220, 80, 30, 128 };
	drawQuad(cpu, 2.f, overlay, translucent);
	cpu.resolveDeferredFragments();

	std::vector<hybrid::Triangle> commands;
	appendQuad(commands, 1.f, background,
			hybrid::makeState(6, true, false, false, true, 0, 1, 0));
	u32 translucentState = hybrid::makeState(6, false, false, false, true, 0, 4, 5);
	translucentState |= hybrid::Autosort | (2u << hybrid::ListTypeShift);
	appendQuad(commands, 2.f, overlay, translucentState);
	const std::vector<u32> texture { 0xffffffffu };
	hybrid::Frame frame { 2, 2, &commands, &texture };
	std::vector<u32> colors;
	std::vector<float> depths;
	ASSERT_TRUE(hybrid::render(frame, colors, depths));
	expectEqual(cpu, colors, depths);
}

TEST(HybridTileRenderer, MatchesOracleForEveryBlendFactorPair)
{
	const Color background { 17, 61, 173, 83 };
	const Color overlay { 211, 97, 43, 139 };
	const std::vector<u32> texture { 0xffffffffu };
	for (u32 source = 0; source < 8; ++source)
		for (u32 destination = 0; destination < 8; ++destination)
		{
			SoftwareRasterizer cpu(2, 2);
			PipelineState opaque;
			opaque.depthMode = DepthMode::GreaterOrEqual;
			drawQuad(cpu, 1.f, background, opaque);
			PipelineState translucent;
			translucent.depthMode = DepthMode::GreaterOrEqual;
			translucent.depthWrite = false;
			translucent.srcBlend = static_cast<BlendFactor>(source);
			translucent.dstBlend = static_cast<BlendFactor>(destination);
			drawQuad(cpu, 2.f, overlay, translucent);

			std::vector<hybrid::Triangle> commands;
			appendQuad(commands, 1.f, background,
					hybrid::makeState(6, true, false, false, true, 0, 1, 0));
			u32 state = hybrid::makeState(6, false, false, false, true, 0,
					source, destination);
			state |= 2u << hybrid::ListTypeShift;
			appendQuad(commands, 2.f, overlay, state);
			hybrid::Frame frame { 2, 2, &commands, &texture };
			std::vector<u32> colors;
			std::vector<float> depths;
			ASSERT_TRUE(hybrid::render(frame, colors, depths))
					<< "source=" << source << " destination=" << destination;
			expectEqual(cpu, colors, depths);
		}
}

TEST(HybridTileRenderer, MatchesOracleForTextureFilteringAndShading)
{
	const Color texels[] {
		{ 240, 20, 40, 32 }, { 10, 220, 60, 96 },
		{ 30, 70, 230, 160 }, { 210, 190, 20, 224 },
	};
	const Color base { 173, 109, 67, 151 };
	const std::vector<u32> texture {
		pack(texels[0]), pack(texels[1]), pack(texels[2]), pack(texels[3])
	};
	for (u32 filtering = 0; filtering < 2; ++filtering)
		for (u32 shading = 0; shading < 4; ++shading)
		{
			SoftwareRasterizer cpu(2, 2);
			PipelineState state;
			state.depthMode = DepthMode::GreaterOrEqual;
			state.texture = { texels, 2, 2 };
			state.bilinearFilter = filtering != 0;
			state.shadingInstruction = shading;
			drawTexturedQuad(cpu, 1.f, base, state);

			std::vector<hybrid::Triangle> commands;
			const u32 gpuState = hybrid::makeState(6, true, true, filtering != 0,
					true, shading, 1, 0);
			appendTexturedQuad(commands, 1.f, base, gpuState, 2, 2);
			hybrid::Frame frame { 2, 2, &commands, &texture };
			std::vector<u32> colors;
			std::vector<float> depths;
			ASSERT_TRUE(hybrid::render(frame, colors, depths))
					<< "filtering=" << filtering << " shading=" << shading;
			expectEqual(cpu, colors, depths);
		}
}

TEST(HybridTileRenderer, MatchesOracleForTextureAddressingModes)
{
	Color texels[16];
	for (u32 i = 0; i < 16; ++i)
		texels[i] = { static_cast<u8>(i * 13), static_cast<u8>(255 - i * 11),
				static_cast<u8>(i * 7), 255 };
	std::vector<u32> texture;
	for (Color color : texels)
		texture.push_back(pack(color));
	const Color white { 255, 255, 255, 255 };
	for (u32 modes = 0; modes < 16; ++modes)
	{
		SoftwareRasterizer cpu(2, 2);
		PipelineState state;
		state.depthMode = DepthMode::GreaterOrEqual;
		state.texture = { texels, 4, 4 };
		state.clampU = (modes & 1) != 0;
		state.clampV = (modes & 2) != 0;
		state.flipU = (modes & 4) != 0;
		state.flipV = (modes & 8) != 0;
		drawTexturedQuad(cpu, 1.f, white, state, -.25f, 1.75f);

		u32 gpuState = hybrid::makeState(6, true, true, false, true, 0, 1, 0);
		if (state.clampU) gpuState |= hybrid::ClampU;
		if (state.clampV) gpuState |= hybrid::ClampV;
		if (state.flipU) gpuState |= hybrid::FlipU;
		if (state.flipV) gpuState |= hybrid::FlipV;
		std::vector<hybrid::Triangle> commands;
		appendTexturedQuad(commands, 1.f, white, gpuState, 4, 4, -.25f, 1.75f);
		hybrid::Frame frame { 2, 2, &commands, &texture };
		std::vector<u32> colors;
		std::vector<float> depths;
		ASSERT_TRUE(hybrid::render(frame, colors, depths)) << "modes=" << modes;
		expectEqual(cpu, colors, depths);
	}
}

TEST(HybridTileRenderer, MatchesOracleForPunchThroughAlphaTest)
{
	const Color texels[] {
		{ 220, 30, 20, 40 }, { 220, 30, 20, 120 },
		{ 220, 30, 20, 99 }, { 220, 30, 20, 255 },
	};
	const std::vector<u32> texture {
		pack(texels[0]), pack(texels[1]), pack(texels[2]), pack(texels[3])
	};
	const Color background { 15, 40, 180, 255 };
	const Color white { 255, 255, 255, 255 };
	SoftwareRasterizer cpu(2, 2);
	PipelineState opaque;
	opaque.depthMode = DepthMode::GreaterOrEqual;
	drawQuad(cpu, 1.f, background, opaque);
	PipelineState punch;
	punch.depthMode = DepthMode::GreaterOrEqual;
	punch.texture = { texels, 2, 2 };
	punch.alphaTest = true;
	punch.alphaReference = 100;
	drawTexturedQuad(cpu, 2.f, white, punch);

	std::vector<hybrid::Triangle> commands;
	appendQuad(commands, 1.f, background,
			hybrid::makeState(6, true, false, false, true, 0, 1, 0));
	u32 state = hybrid::makeState(6, true, true, false, true, 0, 1, 0);
	state |= hybrid::AlphaTest | (1u << hybrid::ListTypeShift);
	appendTexturedQuad(commands, 2.f, white, state, 2, 2);
	for (size_t i = 2; i < commands.size(); ++i)
		commands[i].alphaReference = 100;
	hybrid::Frame frame { 2, 2, &commands, &texture };
	std::vector<u32> colors;
	std::vector<float> depths;
	ASSERT_TRUE(hybrid::render(frame, colors, depths));
	expectEqual(cpu, colors, depths);
}

TEST(HybridTileRenderer, MatchesOracleForClampAndVertexFog)
{
	const Color base { 230, 20, 150, 240 };
	const Color offset { 40, 70, 30, 128 };
	const Color fog { 10, 210, 90, 255 };
	const Color minimum { 60, 50, 80, 100 };
	const Color maximum { 200, 180, 140, 220 };
	SoftwareRasterizer cpu(2, 2);
	PipelineState state;
	state.depthMode = DepthMode::GreaterOrEqual;
	state.useOffsetColor = true;
	state.fogMode = 1;
	state.fogVertexColor = fog;
	state.colorClamp = true;
	state.clampMin = minimum;
	state.clampMax = maximum;
	const Vertex a { 0, 0, 1, base, offset };
	const Vertex b { 2, 0, 1, base, offset };
	const Vertex c { 0, 2, 1, base, offset };
	const Vertex d { 2, 2, 1, base, offset };
	cpu.drawTriangle(a, b, c, state);
	cpu.drawTriangle(b, d, c, state);

	const u32 gpuState = hybrid::makeState(6, true, false, false, true, 0, 1, 0);
	std::vector<hybrid::Triangle> commands {
		triangle(a, b, c, gpuState), triangle(b, d, c, gpuState)
	};
	for (hybrid::Triangle& command : commands)
	{
		command.effects = hybrid::UseOffsetColor | hybrid::VertexFog | hybrid::ColorClamp;
		command.clampMin = pack(minimum);
		command.clampMax = pack(maximum);
	}
	const std::vector<u32> texture { 0xffffffffu };
	hybrid::Frame frame { 2, 2, &commands, &texture };
	frame.fogVertexColor = pack(fog);
	std::vector<u32> colors;
	std::vector<float> depths;
	ASSERT_TRUE(hybrid::render(frame, colors, depths));
	expectEqual(cpu, colors, depths);
}

TEST(HybridTileRenderer, MatchesOracleForTableFog)
{
	std::array<u32, 128> fogTable;
	for (u32 i = 0; i < fogTable.size(); ++i)
		fogTable[i] = ((255 - i) << 8) | i;
	const Color base { 210, 70, 25, 177 };
	const Color fog { 15, 90, 220, 255 };
	const std::vector<u32> texture { 0xffffffffu };
	for (float depth : { .5f, 1.f, 4.f, 32.f })
	{
		SoftwareRasterizer cpu(2, 2);
		PipelineState state;
		state.depthMode = DepthMode::GreaterOrEqual;
		state.fogMode = 0;
		state.fogRamColor = fog;
		state.fogDensity = 3.25f;
		state.fogTable = fogTable.data();
		drawQuad(cpu, depth, base, state);

		std::vector<hybrid::Triangle> commands;
		u32 gpuState = hybrid::makeState(6, true, false, false, true, 0, 1, 0);
		gpuState |= hybrid::LookupFog;
		appendQuad(commands, depth, base, gpuState);
		hybrid::Frame frame { 2, 2, &commands, &texture };
		frame.fogTable = fogTable.data();
		frame.fogColor = pack(fog);
		frame.fogDensity = 3.25f;
		std::vector<u32> colors;
		std::vector<float> depths;
		ASSERT_TRUE(hybrid::render(frame, colors, depths)) << "depth=" << depth;
		expectEqual(cpu, colors, depths);
	}
}

TEST(HybridTileRenderer, MatchesOracleForMipSelectionAndTrilinearWeights)
{
	std::array<Color, 21> texels;
	std::fill(texels.begin(), texels.begin() + 16, Color { 220, 30, 20, 240 });
	std::fill(texels.begin() + 16, texels.begin() + 20, Color { 20, 210, 50, 180 });
	texels[20] = { 30, 60, 230, 120 };
	std::vector<u32> texture;
	for (Color color : texels)
		texture.push_back(pack(color));
	TextureView textureView { texels.data(), 4, 4 };
	textureView.mipLevels[0] = texels.data();
	textureView.mipLevels[1] = texels.data() + 16;
	textureView.mipLevels[2] = texels.data() + 20;
	textureView.mipLevelCount = 3;
	const Color white { 255, 255, 255, 255 };
	struct Case { float pvrD; bool trilinear; bool passB; };
	for (const Case test : {
			Case { 1.f, false, false }, Case { 2.f, false, false },
			Case { 3.f, false, false }, Case { 1.5f, true, false },
			Case { 1.5f, true, true } })
	{
		SoftwareRasterizer cpu(2, 2);
		PipelineState state;
		state.depthMode = DepthMode::GreaterOrEqual;
		state.texture = textureView;
		state.trilinearFilter = test.trilinear;
		state.trilinearPassB = test.passB;
		// A 4-wide texture mapped over two pixels has a footprint of twice
		// the UV range, so this produces the requested PVR D value.
		drawTexturedQuad(cpu, 1.f, white, state, 0.f, test.pvrD / 2.f);

		u32 gpuState = hybrid::makeState(6, true, true, false, true, 0, 1, 0);
		if (test.trilinear) gpuState |= hybrid::Trilinear;
		if (test.passB) gpuState |= hybrid::TrilinearPassB;
		std::vector<hybrid::Triangle> commands;
		appendTexturedQuad(commands, 1.f, white, gpuState, 4, 4,
				0.f, test.pvrD / 2.f);
		for (hybrid::Triangle& command : commands)
		{
			command.mipOffsets[0] = 0;
			command.mipOffsets[1] = 16;
			command.mipOffsets[2] = 20;
			command.mipLevelCount = 3;
			command.mipNumerator = test.pvrD;
		}
		hybrid::Frame frame { 2, 2, &commands, &texture };
		std::vector<u32> colors;
		std::vector<float> depths;
		ASSERT_TRUE(hybrid::render(frame, colors, depths))
				<< "pvrD=" << test.pvrD << " passB=" << test.passB;
		expectEqual(cpu, colors, depths);
	}
}

TEST(HybridTileRenderer, MatchesOracleForTextureSupersampling)
{
	Color texels[16];
	for (u32 i = 0; i < 16; ++i)
		texels[i] = { static_cast<u8>(i * 15), static_cast<u8>(240 - i * 9),
				static_cast<u8>(i * 5), static_cast<u8>(32 + i * 11) };
	std::vector<u32> texture;
	for (Color color : texels)
		texture.push_back(pack(color));
	const Color white { 255, 255, 255, 255 };
	for (bool bilinear : { false, true })
	{
		SoftwareRasterizer cpu(2, 2);
		PipelineState state;
		state.depthMode = DepthMode::GreaterOrEqual;
		state.texture = { texels, 4, 4 };
		state.textureSuperSample = true;
		state.bilinearFilter = bilinear;
		drawTexturedQuad(cpu, 1.f, white, state);

		u32 gpuState = hybrid::makeState(6, true, true, bilinear, true, 0, 1, 0)
				| hybrid::TextureSuperSample;
		std::vector<hybrid::Triangle> commands;
		appendTexturedQuad(commands, 1.f, white, gpuState, 4, 4);
		for (hybrid::Triangle& command : commands)
		{
			command.superSampleOffsetX = 8;
			command.superSampleOffsetY = 8;
		}
		hybrid::Frame frame { 2, 2, &commands, &texture };
		std::vector<u32> colors;
		std::vector<float> depths;
		ASSERT_TRUE(hybrid::render(frame, colors, depths)) << "bilinear=" << bilinear;
		expectEqual(cpu, colors, depths);
	}
}

TEST(HybridTileRenderer, MatchesOracleForSecondaryAccumulation)
{
	const Color stored { 40, 80, 120, 160 };
	const Color ignored { 230, 20, 10, 255 };
	SoftwareRasterizer cpu(2, 2);
	PipelineState store;
	store.depthMode = DepthMode::Always;
	store.dstSelect = true;
	drawQuad(cpu, 1.f, stored, store);
	PipelineState load;
	load.depthMode = DepthMode::Always;
	load.srcSelect = true;
	drawQuad(cpu, 2.f, ignored, load);

	std::vector<hybrid::Triangle> commands;
	u32 storeState = hybrid::makeState(7, true, false, false, true, 0, 1, 0);
	appendQuad(commands, 1.f, stored, storeState);
	commands[0].effects |= hybrid::DestinationSelect;
	commands[1].effects |= hybrid::DestinationSelect;
	u32 loadState = hybrid::makeState(7, true, false, false, true, 0, 1, 0);
	appendQuad(commands, 2.f, ignored, loadState);
	commands[2].effects |= hybrid::SourceSelect;
	commands[3].effects |= hybrid::SourceSelect;
	const std::vector<u32> texture { 0xffffffffu };
	hybrid::Frame frame { 2, 2, &commands, &texture };
	std::vector<u32> colors;
	std::vector<float> depths;
	std::vector<u32> secondary;
	ASSERT_TRUE(hybrid::render(frame, colors, depths, nullptr, &secondary));
	expectEqual(cpu, colors, depths);
	ASSERT_EQ(secondary.size(), cpu.secondaryBuffer().size());
	for (size_t i = 0; i < secondary.size(); ++i)
		EXPECT_EQ(secondary[i], pack(cpu.secondaryBuffer()[i])) << "pixel=" << i;
}

TEST(HybridTileRenderer, MatchesOracleForModifierAreaSelection)
{
	const Color red { 230, 20, 30, 255 };
	const Color green { 20, 220, 60, 255 };
	const Vertex a { 0, 0, .5f, red };
	const Vertex b { 2, 0, .5f, red };
	const Vertex c { 0, 2, .5f, red };
	const Vertex d { 2, 2, .5f, red };
	SoftwareRasterizer cpu(2, 2);
	PipelineState geometry;
	geometry.depthMode = DepthMode::Always;
	geometry.colorWrite = false;
	geometry.shadow = true;
	cpu.drawTriangle(a, b, c, geometry);
	cpu.drawTriangle(b, d, c, geometry);
	const Vertex ma { 0, 0, 1.f };
	const Vertex mb { 2, 0, 1.f };
	const Vertex mc { 0, 2, 1.f };
	const Vertex md { 2, 2, 1.f };
	cpu.drawModifierTriangle(ma, mb, mc, ModifierMode::Xor);
	cpu.drawModifierTriangle(mb, md, mc, ModifierMode::Xor);
	cpu.drawModifierTriangle(ma, mb, mc, ModifierMode::Inclusion);
	cpu.drawModifierTriangle(mb, md, mc, ModifierMode::Inclusion);
	cpu.restoreDepth(std::vector<float>(4, 0.f));
	PipelineState area1;
	area1.depthMode = DepthMode::Always;
	PipelineState area0 = area1;
	area0.area1 = &area1;
	Vertex ca = a, cb = b, cc = c, cd = d;
	ca.color1 = cb.color1 = cc.color1 = cd.color1 = green;
	cpu.drawTriangle(ca, cb, cc, area0);
	cpu.drawTriangle(cb, cd, cc, area0);

	const u32 primaryState = hybrid::makeState(7, true, false, false, true, 0, 1, 0);
	const u32 areaState = primaryState | hybrid::Area1Only;
	std::vector<hybrid::Triangle> commands;
	for (const auto& vertices : {
			std::array<Vertex, 3> { ca, cb, cc },
			std::array<Vertex, 3> { cb, cd, cc } })
	{
		const u32 primary = static_cast<u32>(commands.size());
		commands.push_back(triangle(vertices[0], vertices[1], vertices[2], primaryState));
		Vertex aa = vertices[0], ab = vertices[1], ac = vertices[2];
		aa.color = aa.color1;
		ab.color = ab.color1;
		ac.color = ac.color1;
		commands.push_back(triangle(aa, ab, ac, areaState));
		commands[primary].area1Index = primary + 1;
		commands[primary + 1].effects |= hybrid::ModifierArea1Only;
	}
	const std::vector<u32> texture { 0xffffffffu };
	const std::vector<u8> modifierMask(4, 1);
	hybrid::Frame frame { 2, 2, &commands, &texture };
	frame.modifierMask = &modifierMask;
	std::vector<u32> colors;
	std::vector<float> depths;
	ASSERT_TRUE(hybrid::render(frame, colors, depths));
	expectEqual(cpu, colors, depths);
}

TEST(HybridTileRenderer, MatchesOracleForModifierIntensityShadow)
{
	const Color base { 200, 100, 50, 255 };
	const Vertex a { 0, 0, .5f, base };
	const Vertex b { 2, 0, .5f, base };
	const Vertex c { 0, 2, .5f, base };
	const Vertex d { 2, 2, .5f, base };
	SoftwareRasterizer cpu(2, 2);
	PipelineState geometry;
	geometry.depthMode = DepthMode::Always;
	geometry.colorWrite = false;
	geometry.shadow = true;
	cpu.drawTriangle(a, b, c, geometry);
	cpu.drawTriangle(b, d, c, geometry);
	const Vertex ma { 0, 0, 1.f };
	const Vertex mb { 2, 0, 1.f };
	const Vertex mc { 0, 2, 1.f };
	const Vertex md { 2, 2, 1.f };
	cpu.drawModifierTriangle(ma, mb, mc, ModifierMode::Xor);
	cpu.drawModifierTriangle(mb, md, mc, ModifierMode::Xor);
	cpu.drawModifierTriangle(ma, mb, mc, ModifierMode::Inclusion);
	cpu.drawModifierTriangle(mb, md, mc, ModifierMode::Inclusion);
	cpu.restoreDepth(std::vector<float>(4, 0.f));
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.shadeScale = 128;
	cpu.drawTriangle(a, b, c, state);
	cpu.drawTriangle(b, d, c, state);

	u32 gpuState = hybrid::makeState(7, true, false, false, true, 0, 1, 0)
			| hybrid::IntensityShadow;
	std::vector<hybrid::Triangle> commands {
		triangle(a, b, c, gpuState), triangle(b, d, c, gpuState)
	};
	for (hybrid::Triangle& command : commands)
		command.shadeScale = 128;
	const std::vector<u32> texture { 0xffffffffu };
	const std::vector<u8> modifierMask(4, 1);
	hybrid::Frame frame { 2, 2, &commands, &texture };
	frame.modifierMask = &modifierMask;
	std::vector<u32> colors;
	std::vector<float> depths;
	ASSERT_TRUE(hybrid::render(frame, colors, depths));
	expectEqual(cpu, colors, depths);
}

TEST(HybridTileRenderer, MatchesOracleForEveryDepthMode)
{
	const Color background { 15, 50, 170, 255 };
	const Color source { 230, 70, 25, 255 };
	const std::vector<u32> initialColors(4, pack(background));
	const std::vector<float> initialDepths(4, 1.f);
	const std::vector<u32> texture { 0xffffffffu };
	for (u32 mode = 0; mode < 8; ++mode)
		for (float sourceDepth : { .5f, 1.f, 2.f })
		{
			SoftwareRasterizer cpu(2, 2);
			cpu.clear(background, 1.f);
			PipelineState state;
			state.depthMode = static_cast<DepthMode>(mode);
			drawQuad(cpu, sourceDepth, source, state);

			std::vector<hybrid::Triangle> commands;
			appendQuad(commands, sourceDepth, source,
					hybrid::makeState(mode, true, false, false, true, 0, 1, 0));
			hybrid::Frame frame { 2, 2, &commands, &texture, &initialColors,
					&initialDepths };
			std::vector<u32> colors;
			std::vector<float> depths;
			ASSERT_TRUE(hybrid::render(frame, colors, depths))
					<< "mode=" << mode << " sourceDepth=" << sourceDepth;
			expectEqual(cpu, colors, depths);
		}
}

TEST(HybridTileRenderer, MatchesOracleForExclusiveClipAndDither)
{
	const Color color { 93, 126, 159, 191 };
	SoftwareRasterizer cpu(2, 2);
	const Color clear { 0, 0, 0, 255 };
	cpu.clear(clear);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.excludeClip = true;
	state.excludeX0 = 1;
	state.excludeY0 = 0;
	state.excludeX1 = 2;
	state.excludeY1 = 1;
	state.dither = true;
	state.ditherDivisorR = 8;
	state.ditherDivisorG = 4;
	state.ditherDivisorB = 2;
	state.ditherDivisorA = 1;
	drawQuad(cpu, 1.f, color, state);

	std::vector<hybrid::Triangle> commands;
	appendQuad(commands, 1.f, color,
			hybrid::makeState(7, true, false, false, true, 0, 1, 0));
	for (hybrid::Triangle& command : commands)
	{
		command.effects |= hybrid::ExcludeClip | hybrid::Dither;
		command.excludeX0 = 1;
		command.excludeY0 = 0;
		command.excludeX1 = 2;
		command.excludeY1 = 1;
		command.ditherDivisors = 8u | (4u << 8) | (2u << 16) | (1u << 24);
	}
	const std::vector<u32> texture { 0xffffffffu };
	const std::vector<u32> initialColors(4, pack(clear));
	hybrid::Frame frame { 2, 2, &commands, &texture, &initialColors };
	std::vector<u32> colors;
	std::vector<float> depths;
	ASSERT_TRUE(hybrid::render(frame, colors, depths));
	expectEqual(cpu, colors, depths);
}

TEST(HybridTileRenderer, MatchesOracleForBumpMapping)
{
	const Color texel { 0, 0, 0, 136 };
	const Color base { 255, 255, 255, 255 };
	const Color bumpParameters { 255, 0, 0, 0 };
	const Vertex a { 0, 0, 1, base, bumpParameters, 0, 0 };
	const Vertex b { 2, 0, 1, base, bumpParameters, 1, 0 };
	const Vertex c { 0, 2, 1, base, bumpParameters, 0, 1 };
	const Vertex d { 2, 2, 1, base, bumpParameters, 1, 1 };
	SoftwareRasterizer cpu(2, 2);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.texture = { &texel, 1, 1 };
	state.bumpMap = true;
	state.useOffsetColor = true;
	cpu.drawTriangle(a, b, c, state);
	cpu.drawTriangle(b, d, c, state);

	u32 gpuState = hybrid::makeState(7, true, true, false, true, 0, 1, 0);
	std::vector<hybrid::Triangle> commands {
		triangle(a, b, c, gpuState), triangle(b, d, c, gpuState)
	};
	for (hybrid::Triangle& command : commands)
	{
		command.textureWidth = command.textureHeight = 1;
		command.mipOffsets[0] = 0;
		command.mipLevelCount = 1;
		command.effects |= hybrid::UseOffsetColor | hybrid::BumpMap;
	}
	const std::vector<u32> texture { pack(texel) };
	hybrid::Frame frame { 2, 2, &commands, &texture };
	std::vector<u32> colors;
	std::vector<float> depths;
	ASSERT_TRUE(hybrid::render(frame, colors, depths));
	expectEqual(cpu, colors, depths);
}

TEST(HybridTileRenderer, MatchesOracleForLookupFogAlpha)
{
	std::array<u32, 128> fogTable;
	for (u32 i = 0; i < fogTable.size(); ++i)
		fogTable[i] = ((i * 2) << 8) | std::min(255u, i * 2 + 1);
	const Color base { 210, 70, 25, 177 };
	const Color fog { 15, 90, 220, 255 };
	SoftwareRasterizer cpu(2, 2);
	PipelineState state;
	state.depthMode = DepthMode::Always;
	state.fogMode = 3;
	state.fogRamColor = fog;
	state.fogDensity = 2.5f;
	state.fogTable = fogTable.data();
	drawQuad(cpu, 3.f, base, state);

	std::vector<hybrid::Triangle> commands;
	appendQuad(commands, 3.f, base,
			hybrid::makeState(7, true, false, false, true, 0, 1, 0));
	for (hybrid::Triangle& command : commands)
		command.effects |= hybrid::LookupFogAlpha;
	const std::vector<u32> texture { 0xffffffffu };
	hybrid::Frame frame { 2, 2, &commands, &texture };
	frame.fogTable = fogTable.data();
	frame.fogColor = pack(fog);
	frame.fogDensity = 2.5f;
	std::vector<u32> colors;
	std::vector<float> depths;
	ASSERT_TRUE(hybrid::render(frame, colors, depths));
	expectEqual(cpu, colors, depths);
}
#endif
