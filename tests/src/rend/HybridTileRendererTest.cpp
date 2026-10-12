#include "rend/soft/HybridTileRenderer.h"
#include "rend/soft/SoftwareRasterizer.h"

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
#endif
