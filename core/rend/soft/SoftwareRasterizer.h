#pragma once

#include "types.h"

#include <array>
#include <utility>
#include <vector>

namespace rend::soft {

struct Color
{
	u8 r = 0;
	u8 g = 0;
	u8 b = 0;
	u8 a = 0;

	bool operator==(const Color& other) const
	{
		return r == other.r && g == other.g && b == other.b && a == other.a;
	}
};

struct Vertex
{
	float x = 0;
	float y = 0;
	float z = 0;
	Color color;
	Color offsetColor;
	float u = 0;
	float v = 0;
	Color color1;
	Color offsetColor1;
	float u1 = 0;
	float v1 = 0;
};

struct TextureView
{
	const Color *pixels = nullptr;
	u32 width = 0;
	u32 height = 0;
	std::array<const Color *, 11> mipLevels {};
	u8 mipLevelCount = 0;

	explicit operator bool() const { return pixels != nullptr && width != 0 && height != 0; }
};

enum class DepthMode : u8
{
	Never,
	Less,
	Equal,
	LessOrEqual,
	Greater,
	NotEqual,
	GreaterOrEqual,
	Always,
};

enum class BlendFactor : u8
{
	Zero,
	One,
	OtherColor,
	OneMinusOtherColor,
	SrcAlpha,
	OneMinusSrcAlpha,
	DstAlpha,
	OneMinusDstAlpha,
};

enum class ModifierMode : u8
{
	Xor,
	Or,
	Inclusion,
	Exclusion,
};

struct PipelineState
{
	const PipelineState *area1 = nullptr;
	DepthMode depthMode = DepthMode::GreaterOrEqual;
	bool depthWrite = true;
	bool colorWrite = true;
	BlendFactor srcBlend = BlendFactor::One;
	BlendFactor dstBlend = BlendFactor::Zero;
	TextureView texture;
	bool gouraud = true;
	bool useAlpha = true;
	bool ignoreTextureAlpha = false;
	bool useOffsetColor = false;
	bool alphaTest = false;
	u8 alphaReference = 0;
	u8 shadingInstruction = 0;
	bool bilinearFilter = false;
	bool clampU = false;
	bool clampV = false;
	bool flipU = false;
	bool flipV = false;
	bool fpuPixelCenter = true;
	bool tspPixelCenter = true;
	bool texelSampleCenter = true;
	bool textureSuperSample = false;
	bool dCalcCtrl = false;
	float nativeToHostScaleX = 1.f;
	float nativeToHostScaleY = 1.f;
	float nativeOriginX = 0.f;
	float nativeOriginY = 0.f;
	bool scissorEnabled = false;
	s32 scissorX0 = 0;
	s32 scissorY0 = 0;
	s32 scissorX1 = 0;
	s32 scissorY1 = 0;
	bool excludeClip = false;
	s32 excludeX0 = 0;
	s32 excludeY0 = 0;
	s32 excludeX1 = 0;
	s32 excludeY1 = 0;
	u8 fogMode = 2;
	Color fogRamColor;
	Color fogVertexColor;
	float fogDensity = 0;
	const u32 *fogTable = nullptr;
	bool colorClamp = false;
	Color clampMin;
	Color clampMax { 255, 255, 255, 255 };
	bool shadow = false;
	// FPU_SHAD_SCALE bit 8 selects intensity scaling instead of the
	// parameter-selection (two-volume) shadow mode.
	bool intensityShadow = true;
	bool srcSelect = false;
	bool dstSelect = false;
	bool bumpMap = false;
	// The PVR multiplies its derivative-based D value by this unsigned 2.2
	// fixed-point adjustment before selecting a mip level.
	float mipDAdjust = 1.f;
	// PVR filter modes 2 and 3 are separate trilinear passes. Pass A
	// samples the D-selected level and pass B samples the next smaller one.
	bool trilinearFilter = false;
	bool trilinearPassB = false;
	bool dither = false;
	u8 ditherDivisorR = 1;
	u8 ditherDivisorG = 1;
	u8 ditherDivisorB = 1;
	u8 ditherDivisorA = 1;
	u16 shadeScale = 256;
	bool deferBlend = false;
	bool deferArea1 = false;
	bool useArea1VertexData = false;
	bool modifierArea1Only = false;
	bool recordVisibility = false;
	bool testVisibility = false;
	// Optional render-target-sized region mask used when type-2 region
	// headers select different translucent sorting modes per tile.
	const u8 *coverageMask = nullptr;
	u32 sequence = 0;
};

class SoftwareRasterizer
{
public:
	SoftwareRasterizer(u32 width, u32 height);

	void resize(u32 width, u32 height);
	void clear(Color color = {}, float depth = 0.f);
	void clearDepth(float depth = 0.f);
	void clearDepthRect(s32 x0, s32 y0, s32 x1, s32 y1, float depth = 0.f);
	void clearSecondary(Color color = {});
	void flushColorRect(std::vector<Color>& frameStore,
			s32 x0, s32 y0, s32 x1, s32 y1) const;
	void clearVisibility();
	void restoreDepth(const std::vector<float>& depth);
	void restoreColor(const std::vector<Color>& color);
	void clearModifierResults();
	void clearDeferredFragments();
	void resolveDeferredFragments();
	void drawTriangle(const Vertex& v0, const Vertex& v1, const Vertex& v2,
			const PipelineState& state = {});
	void drawModifierTriangle(const Vertex& v0, const Vertex& v1, const Vertex& v2,
			ModifierMode mode, const PipelineState& state = {}, bool deferred = false);
	void applyModifierShadow(u16 scale);
	void applyFramebufferDither(u32 packMode);

	u32 width() const { return width_; }
	u32 height() const { return height_; }
	const std::vector<Color>& colorBuffer() const { return colorBuffer_; }
	const std::vector<float>& depthBuffer() const { return depthBuffer_; }
	const std::vector<u8>& stencilBuffer() const { return stencilBuffer_; }
	Color pixel(u32 x, u32 y) const;
	float depth(u32 x, u32 y) const;
	void setPixel(u32 x, u32 y, Color color);

	static bool depthTest(DepthMode mode, float source, float destination);
	static DepthMode selectDepthMode(u32 encodedMode, bool punchThrough, bool autosort);
	static bool cullTriangle(u32 mode, float threshold, const Vertex& a,
			const Vertex& b, const Vertex& c);
	static Color blend(Color source, Color destination, BlendFactor sourceFactor,
			BlendFactor destinationFactor);
	static Color sampleTexture(const TextureView& texture, float u, float v,
			const PipelineState& state, float pvrD = 1.f);
	static void forceNearestFiltering(PipelineState& state);
	static TextureView makeTextureView(const Color *pixels, u32 width, u32 height,
			bool mipmapped);
	static std::vector<Color> generateMipChain(const Color *pixels, u32 dimension);
	static std::vector<Color> scaleLinear(const std::vector<Color>& source,
			u32 sourceWidth, u32 sourceHeight, u32 destinationWidth, u32 destinationHeight);
	static std::vector<Color> filterVertical(const std::vector<Color>& source,
			u32 width, u32 height, u8 outerCoefficient, u8 centerCoefficient);
	static std::vector<Color> selectInterlacedField(const std::vector<Color>& source,
			u32 width, u32 height, u32 outputHeight, bool oddField);
	static std::vector<Color> scaleHorizontalHalf(const std::vector<Color>& source,
			u32 width, u32 height);
	static std::vector<Color> scaleFramebufferForOutput(const std::vector<Color>& source,
			u32 sourceWidth, u32 sourceHeight, u32 destinationWidth, u32 destinationHeight,
			bool verticalFilter, u8 outerCoefficient, u8 centerCoefficient,
			bool interlace, bool oddField, bool horizontalHalf);
	static void ditherFramebuffer(std::vector<Color>& pixels, u32 width, u32 height,
			u32 packMode);
	static void convertFramebufferForDisplay(std::vector<Color>& pixels,
			u32 writePackMode, u8 writeKValue, u8 alphaThreshold,
			u32 readDepth, u8 concat);
	static u32 framebufferDisplayWidth(u32 writeWidth, u32 writePackMode, u32 readDepth);
	static std::vector<Color> prepareFramebufferForDisplay(const std::vector<Color>& frameStore,
			u32 width, u32 height, bool dither, u32 writePackMode,
			u8 writeKValue, u8 alphaThreshold, u32 readDepth, u8 concat);
	static void copyColorRect(const std::vector<Color>& source, std::vector<Color>& destination,
			u32 width, u32 height, s32 x0, s32 y0, s32 x1, s32 y1);
	static float fogCoefficient(float inverseDepth, float density, const u32 *table);

private:
	struct Fragment
	{
		float depth;
		u32 sequence;
		Color color;
		// Intensity volumes scale Base and Offset Color before texture
		// application, so this cannot be reconstructed by scaling color.
		Color intensityColor;
		BlendFactor srcBlend;
		BlendFactor dstBlend;
		bool srcSelect;
		bool dstSelect;
		bool dither;
		u8 ditherDivisorR;
		u8 ditherDivisorG;
		u8 ditherDivisorB;
		u8 ditherDivisorA;
		bool shadowEnabled;
		bool intensityShadow;
		u8 modifier = 0;
		bool hasArea1 = false;
		Color area1Color;
		BlendFactor area1SrcBlend = BlendFactor::One;
		BlendFactor area1DstBlend = BlendFactor::Zero;
		bool area1SrcSelect = false;
		bool area1DstSelect = false;
	};

	class FragmentList
	{
	public:
		bool empty() const { return !inlineUsed_ && multiple_.empty(); }
		bool multiple() const { return !multiple_.empty(); }

		void clear()
		{
			inlineUsed_ = false;
			multiple_.clear();
		}

		void push_back(const Fragment& fragment)
		{
			if (!multiple_.empty())
			{
				multiple_.push_back(fragment);
				return;
			}
			if (!inlineUsed_)
			{
				inline_ = fragment;
				inlineUsed_ = true;
				return;
			}
			multiple_.push_back(inline_);
			multiple_.push_back(fragment);
			inlineUsed_ = false;
		}

		Fragment& back() { return multiple_.empty() ? inline_ : multiple_.back(); }
		const Fragment& back() const { return multiple_.empty() ? inline_ : multiple_.back(); }

		template<typename Visitor>
		void forEach(Visitor&& visitor)
		{
			if (inlineUsed_)
				visitor(inline_);
			else
				for (Fragment& fragment : multiple_)
					visitor(fragment);
		}

		template<typename Visitor>
		void forEach(Visitor&& visitor) const
		{
			if (inlineUsed_)
				visitor(inline_);
			else
				for (const Fragment& fragment : multiple_)
					visitor(fragment);
		}

		template<typename Visitor>
		void forEachReverseUntil(Visitor&& visitor)
		{
			if (inlineUsed_)
			{
				visitor(inline_);
				return;
			}
			for (auto it = multiple_.rbegin(); it != multiple_.rend(); ++it)
				if (visitor(*it))
					break;
		}

		template<typename Compare>
		void stableSort(Compare&& compare)
		{
			if (!multiple_.empty())
				std::stable_sort(multiple_.begin(), multiple_.end(),
						std::forward<Compare>(compare));
		}

	private:
		Fragment inline_ {};
		bool inlineUsed_ = false;
		std::vector<Fragment> multiple_;
	};

	u32 width_ = 0;
	u32 height_ = 0;
	std::vector<Color> colorBuffer_;
	std::vector<float> depthBuffer_;
	std::vector<u8> stencilBuffer_;
	std::vector<Color> secondaryBuffer_;
	std::vector<u32> visibilityBuffer_;
	std::vector<FragmentList> deferredFragments_;
};

} // namespace rend::soft
