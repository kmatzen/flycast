#include "SoftwareRasterizer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif
#ifdef TARGET_MAC
#include <dispatch/dispatch.h>
#endif

namespace rend::soft {
namespace {

// The PVR rasterizes on a 1/16-pixel grid. Keeping edge equations integral
// makes shared-edge ownership deterministic on every host architecture.
constexpr s32 SUBPIXEL_BITS = 4;
constexpr s32 SUBPIXEL_SCALE = 1 << SUBPIXEL_BITS;
constexpr s32 CENTER_SAMPLE_OFFSET = SUBPIXEL_SCALE / 2;
// Keep edge products comfortably inside s64 even for corrupt or deliberately
// hostile TA data. This still permits coordinates over four million pixels
// outside the render target.
constexpr s32 MAX_FIXED_COORDINATE = 1 << 26;

struct Point
{
	s32 x;
	s32 y;
};

s32 toFixed(float value)
{
	const double fixed = std::floor(static_cast<double>(value) * SUBPIXEL_SCALE + .5);
	return static_cast<s32>(std::clamp(fixed,
			-static_cast<double>(MAX_FIXED_COORDINATE),
			static_cast<double>(MAX_FIXED_COORDINATE)));
}

s64 edge(const Point& a, const Point& b, s32 x, s32 y)
{
	return (static_cast<s64>(b.x) - a.x) * (static_cast<s64>(y) - a.y)
			- (static_cast<s64>(b.y) - a.y) * (static_cast<s64>(x) - a.x);
}

bool isTopLeft(const Point& a, const Point& b)
{
	const s64 dx = static_cast<s64>(b.x) - a.x;
	const s64 dy = static_cast<s64>(b.y) - a.y;
	return dy < 0 || (dy == 0 && dx > 0);
}

bool hasFinitePosition(const Vertex& vertex)
{
	return std::isfinite(vertex.x) && std::isfinite(vertex.y) && std::isfinite(vertex.z);
}

u8 clampByte(float value)
{
	return static_cast<u8>(std::clamp(std::lround(value), 0l, 255l));
}

float wrapCoordinate(float coordinate, bool clamp, bool flip)
{
	if (!std::isfinite(coordinate))
		return 0.f;
	if (clamp)
		return std::clamp(coordinate, 0.f, 1.f);
	// Texture coordinates produced by ordinary Dreamcast polygons are already
	// in the fundamental repeat interval. Avoid a floor operation for every
	// axis and every (potentially supersampled) fragment in that common case.
	if (coordinate >= 0.f && coordinate < 1.f)
		return coordinate;
	if (flip)
	{
		float period = std::floor(coordinate);
		float fraction = coordinate - period;
		const bool periodFitsInteger = period > static_cast<float>(std::numeric_limits<s64>::min())
				&& period < static_cast<float>(std::numeric_limits<s64>::max());
		if (periodFitsInteger)
		{
			if ((static_cast<s64>(period) & 1) != 0)
				fraction = 1.f - fraction;
			return fraction;
		}
		if (std::fmod(std::abs(period), 2.f) >= 1.f)
			fraction = 1.f - fraction;
		return fraction;
	}
	return coordinate - std::floor(coordinate);
}

u32 wrapTexel(s32 coordinate, u32 size, bool clamp, bool flip)
{
	if (size <= 1)
		return 0;
	if (clamp)
		return static_cast<u32>(std::clamp<s32>(coordinate, 0, static_cast<s32>(size) - 1));
	// Native PVR textures are powers of two. Masking is exactly equivalent to
	// signed modulo with positive wrap, but avoids integer division for every
	// point sample (and eight times per bilinear sample).
	if ((size & (size - 1)) == 0)
	{
		if (!flip)
			return static_cast<u32>(coordinate) & (size - 1);
		const u32 periodMask = size * 2 - 1;
		const u32 wrapped = static_cast<u32>(coordinate) & periodMask;
		return wrapped < size ? wrapped : periodMask - wrapped;
	}

	const s32 signedSize = static_cast<s32>(size);
	if (flip)
	{
		const s32 period = signedSize * 2;
		s32 wrapped = coordinate % period;
		if (wrapped < 0)
			wrapped += period;
		if (wrapped >= signedSize)
			wrapped = period - 1 - wrapped;
		return static_cast<u32>(wrapped);
	}

	s32 wrapped = coordinate % signedSize;
	if (wrapped < 0)
		wrapped += signedSize;
	return static_cast<u32>(wrapped);
}

std::array<u16, 4> factorNumerator(BlendFactor blendFactor, Color source, Color destination,
		bool sourceTerm)
{
	const Color other = sourceTerm ? destination : source;
	switch (blendFactor)
	{
	case BlendFactor::Zero: return { 0, 0, 0, 0 };
	case BlendFactor::One: return { 255, 255, 255, 255 };
	case BlendFactor::OtherColor:
		return { other.r, other.g, other.b, other.a };
	case BlendFactor::OneMinusOtherColor:
		return { static_cast<u16>(255 - other.r), static_cast<u16>(255 - other.g),
				static_cast<u16>(255 - other.b), static_cast<u16>(255 - other.a) };
	case BlendFactor::SrcAlpha:
		return { source.a, source.a, source.a, source.a };
	case BlendFactor::OneMinusSrcAlpha:
		return { static_cast<u16>(255 - source.a), static_cast<u16>(255 - source.a),
				static_cast<u16>(255 - source.a), static_cast<u16>(255 - source.a) };
	case BlendFactor::DstAlpha:
		return { destination.a, destination.a, destination.a, destination.a };
	case BlendFactor::OneMinusDstAlpha:
		return { static_cast<u16>(255 - destination.a), static_cast<u16>(255 - destination.a),
				static_cast<u16>(255 - destination.a), static_cast<u16>(255 - destination.a) };
	}
	return {};
}

u8 blendChannel(u8 source, u8 destination, u16 sourceFactor, u16 destinationFactor)
{
	// All PVR blend factors are exact 8-bit fractions. The largest numerator
	// is 2 * 255 * 255, so this stays comfortably within an unsigned integer.
	// Division by the odd value 255 can never produce an exact half-integer;
	// adding 127 therefore matches round-to-nearest without a tie case.
	return static_cast<u8>(std::min<u32>(255,
			(static_cast<u32>(source) * sourceFactor
					+ static_cast<u32>(destination) * destinationFactor + 127) / 255));
}

} // namespace

SoftwareRasterizer::SoftwareRasterizer(u32 width, u32 height)
{
	resize(width, height);
}

void SoftwareRasterizer::resize(u32 width, u32 height)
{
	width_ = width;
	height_ = height;
	colorBuffer_.resize(static_cast<size_t>(width) * height);
	depthBuffer_.resize(static_cast<size_t>(width) * height);
	stencilBuffer_.resize(static_cast<size_t>(width) * height);
	secondaryBuffer_.resize(static_cast<size_t>(width) * height);
	visibilityBuffer_.resize(static_cast<size_t>(width) * height);
	deferredFragments_.resize(static_cast<size_t>(width) * height);
	clearVisibility();
	clearDeferredFragments();
}

void SoftwareRasterizer::clear(Color color, float depth)
{
	std::fill(colorBuffer_.begin(), colorBuffer_.end(), color);
	std::fill(depthBuffer_.begin(), depthBuffer_.end(), depth);
	std::fill(stencilBuffer_.begin(), stencilBuffer_.end(), 0);
	std::fill(secondaryBuffer_.begin(), secondaryBuffer_.end(), Color {});
	clearVisibility();
	clearDeferredFragments();
}

void SoftwareRasterizer::clearDepth(float depth)
{
	std::fill(depthBuffer_.begin(), depthBuffer_.end(), depth);
	std::fill(stencilBuffer_.begin(), stencilBuffer_.end(), 0);
}

void SoftwareRasterizer::clearDepthRect(s32 x0, s32 y0, s32 x1, s32 y1, float depth)
{
	x0 = std::clamp<s32>(x0, 0, width_);
	y0 = std::clamp<s32>(y0, 0, height_);
	x1 = std::clamp<s32>(x1, 0, width_);
	y1 = std::clamp<s32>(y1, 0, height_);
	if (x0 >= x1 || y0 >= y1)
		return;
	for (s32 y = y0; y < y1; ++y)
	{
		const size_t begin = static_cast<size_t>(y) * width_ + x0;
		const size_t end = static_cast<size_t>(y) * width_ + x1;
		std::fill(depthBuffer_.begin() + begin, depthBuffer_.begin() + end, depth);
		std::fill(stencilBuffer_.begin() + begin, stencilBuffer_.begin() + end, 0);
	}
}

void SoftwareRasterizer::clearSecondary(Color color)
{
	std::fill(secondaryBuffer_.begin(), secondaryBuffer_.end(), color);
}

void SoftwareRasterizer::flushColorRect(std::vector<Color>& frameStore,
		s32 x0, s32 y0, s32 x1, s32 y1) const
{
	copyColorRect(colorBuffer_, frameStore, width_, height_, x0, y0, x1, y1);
}

void SoftwareRasterizer::clearVisibility()
{
	std::fill(visibilityBuffer_.begin(), visibilityBuffer_.end(), std::numeric_limits<u32>::max());
}

void SoftwareRasterizer::restoreDepth(const std::vector<float>& depth)
{
	if (depth.size() == depthBuffer_.size())
		depthBuffer_ = depth;
}

void SoftwareRasterizer::clearModifierResults()
{
	for (u8& stencil : stencilBuffer_)
		stencil &= 0xfc;
}

void SoftwareRasterizer::clearDeferredFragments()
{
	for (auto& fragments : deferredFragments_)
		fragments.clear();
}

void SoftwareRasterizer::resolveDeferredFragments()
{
	static constexpr u8 ditherTable[16] {
		5, 13, 7, 15, 9, 1, 11, 3, 6, 14, 4, 12, 10, 2, 8, 0
	};
	auto resolveRow = [&](u32 y)
	{
		const size_t rowOffset = static_cast<size_t>(y) * width_;
		for (u32 x = 0; x < width_; ++x)
		{
			const size_t offset = rowOffset + x;
			auto& fragments = deferredFragments_[offset];
			if (fragments.empty())
				continue;
			fragments.stableSort([](const Fragment& left, const Fragment& right) {
				return left.depth < right.depth
						|| (left.depth == right.depth && left.sequence < right.sequence);
			});
			Color finalColor = colorBuffer_[offset];
			Color secondary = secondaryBuffer_[offset];
			fragments.forEach([&](const Fragment& fragment) {
				const bool area1 = fragment.shadowEnabled && (fragment.modifier & 2) != 0
						&& fragment.hasArea1;
				Color fragmentColor = area1 ? fragment.area1Color : fragment.color;
				if (!area1 && fragment.shadowEnabled && fragment.intensityShadow
						&& (fragment.modifier & 2) != 0)
					fragmentColor = fragment.intensityColor;
				const bool srcSelect = area1 ? fragment.area1SrcSelect : fragment.srcSelect;
				const bool dstSelect = area1 ? fragment.area1DstSelect : fragment.dstSelect;
				const BlendFactor srcBlend = area1 ? fragment.area1SrcBlend : fragment.srcBlend;
				const BlendFactor dstBlend = area1 ? fragment.area1DstBlend : fragment.dstBlend;
				const Color source = srcSelect ? secondary : fragmentColor;
				const Color destination = dstSelect ? secondary : finalColor;
				const Color result = blend(source, destination, srcBlend, dstBlend);
				if (dstSelect)
					secondary = result;
				else
					finalColor = result;
			});
			const Fragment& last = fragments.back();
			if (last.dither)
			{
				const u8 dither = ditherTable[(y & 3) * 4 + (x & 3)];
				finalColor.r = static_cast<u8>(std::min(255,
						finalColor.r + dither / last.ditherDivisorR));
				finalColor.g = static_cast<u8>(std::min(255,
						finalColor.g + dither / last.ditherDivisorG));
				finalColor.b = static_cast<u8>(std::min(255,
						finalColor.b + dither / last.ditherDivisorB));
				finalColor.a = static_cast<u8>(std::min(255,
						finalColor.a + 1 / last.ditherDivisorA));
			}
			colorBuffer_[offset] = finalColor;
			secondaryBuffer_[offset] = secondary;
		}
	};
#ifdef TARGET_MAC
	if (height_ >= 16)
	{
		struct RowContext
		{
			decltype(resolveRow) *resolve;
		} rowContext { &resolveRow };
		dispatch_apply_f(height_, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
				&rowContext, [](void *opaque, size_t row) {
					auto& context = *static_cast<RowContext *>(opaque);
					(*context.resolve)(static_cast<u32>(row));
				});
	}
	else
#endif
		for (u32 y = 0; y < height_; ++y)
			resolveRow(y);
}

Color SoftwareRasterizer::pixel(u32 x, u32 y) const
{
	return colorBuffer_.at(static_cast<size_t>(y) * width_ + x);
}

float SoftwareRasterizer::depth(u32 x, u32 y) const
{
	return depthBuffer_.at(static_cast<size_t>(y) * width_ + x);
}

void SoftwareRasterizer::setPixel(u32 x, u32 y, Color color)
{
	colorBuffer_.at(static_cast<size_t>(y) * width_ + x) = color;
}

bool SoftwareRasterizer::depthTest(DepthMode mode, float source, float destination)
{
	switch (mode)
	{
	case DepthMode::Never: return false;
	case DepthMode::Less: return source < destination;
	case DepthMode::Equal: return source == destination;
	case DepthMode::LessOrEqual: return source <= destination;
	case DepthMode::Greater: return source > destination;
	case DepthMode::NotEqual: return source != destination;
	case DepthMode::GreaterOrEqual: return source >= destination;
	case DepthMode::Always: return true;
	}
	return false;
}

DepthMode SoftwareRasterizer::selectDepthMode(u32 encodedMode, bool punchThrough,
		bool autosort)
{
	// Holly2 ignores the ISP instruction for Punch Through, and the span
	// sorter does the same for autosorted translucent polygons.
	return punchThrough || autosort ? DepthMode::GreaterOrEqual
			: static_cast<DepthMode>(encodedMode & 7);
}

bool SoftwareRasterizer::cullTriangle(u32 mode, float threshold, const Vertex& a,
		const Vertex& b, const Vertex& c)
{
	if (mode == 0)
		return false;
	const float determinant = (b.x - a.x) * (c.y - a.y)
			- (b.y - a.y) * (c.x - a.x);
	if (!std::isfinite(determinant) || std::abs(determinant) < threshold)
		return true;
	if (mode == 1)
		return false;
	return mode == 2 ? determinant < 0.f : determinant > 0.f;
}

Color SoftwareRasterizer::blend(Color source, Color destination, BlendFactor sourceFactor,
		BlendFactor destinationFactor)
{
	if (sourceFactor == BlendFactor::One && destinationFactor == BlendFactor::Zero)
		return source;
	if (sourceFactor == BlendFactor::Zero && destinationFactor == BlendFactor::One)
		return destination;
	if (sourceFactor == BlendFactor::SrcAlpha
			&& destinationFactor == BlendFactor::OneMinusSrcAlpha)
	{
		const u16 sourceAlpha = source.a;
		const u16 inverseSourceAlpha = 255 - source.a;
#if defined(__ARM_NEON)
		u32 sourcePacked;
		u32 destinationPacked;
		std::memcpy(&sourcePacked, &source, sizeof(sourcePacked));
		std::memcpy(&destinationPacked, &destination, sizeof(destinationPacked));
		const uint16x4_t sourceChannels = vget_low_u16(vmovl_u8(
				vreinterpret_u8_u32(vdup_n_u32(sourcePacked))));
		const uint16x4_t destinationChannels = vget_low_u16(vmovl_u8(
				vreinterpret_u8_u32(vdup_n_u32(destinationPacked))));
		uint32x4_t numerators = vmull_n_u16(sourceChannels, sourceAlpha);
		numerators = vmlal_n_u16(numerators, destinationChannels, inverseSourceAlpha);
		numerators = vaddq_u32(numerators, vdupq_n_u32(127));
		// Numerators are below 2^24, so conversion to float is exact. Since 255
		// is odd, the quotient cannot be a half-integer; truncating the biased
		// quotient therefore reproduces the scalar round-to-nearest equation.
		uint32x4_t blended = vcvtq_u32_f32(vmulq_n_f32(
				vcvtq_f32_u32(numerators), 1.f / 255.f));
		blended = vminq_u32(blended, vdupq_n_u32(255));
		const uint8x8_t bytes = vmovn_u16(vcombine_u16(vmovn_u32(blended),
				vdup_n_u16(0)));
		const u32 packed = vget_lane_u32(vreinterpret_u32_u8(bytes), 0);
		Color result;
		std::memcpy(&result, &packed, sizeof(result));
		return result;
#else
		return {
			blendChannel(source.r, destination.r, sourceAlpha, inverseSourceAlpha),
			blendChannel(source.g, destination.g, sourceAlpha, inverseSourceAlpha),
			blendChannel(source.b, destination.b, sourceAlpha, inverseSourceAlpha),
			blendChannel(source.a, destination.a, sourceAlpha, inverseSourceAlpha),
		};
#endif
	}
	const auto sf = factorNumerator(sourceFactor, source, destination, true);
	const auto df = factorNumerator(destinationFactor, source, destination, false);
	return {
		blendChannel(source.r, destination.r, sf[0], df[0]),
		blendChannel(source.g, destination.g, sf[1], df[1]),
		blendChannel(source.b, destination.b, sf[2], df[2]),
		blendChannel(source.a, destination.a, sf[3], df[3]),
	};
}

#if defined(__GNUC__) || defined(__clang__)
__attribute__((always_inline))
#endif
Color SoftwareRasterizer::sampleTexture(const TextureView& texture, float u, float v,
		const PipelineState& state, float pvrD)
{
	if (!texture)
		return { 255, 255, 255, 255 };
	const Color *pixels = texture.pixels;
	u32 width = texture.width;
	u32 height = texture.height;
	if (!std::isfinite(pvrD))
		pvrD = 1.f;
	if (texture.mipLevelCount > 1)
	{
		// Unlike conventional logarithmic LOD, PVR D selects the full-size
		// map for 0 < D < 2, the half-size map for 2 <= D < 3, and so on.
		float selected = std::max(0.f, std::floor(std::max(pvrD, 1.f)) - 1.f);
		// Supersampling takes four spatial samples and, for a mipmapped
		// texture, promotes the D-selected map to the next higher resolution.
		if (state.textureSuperSample)
			selected = std::max(0.f, selected - 1.f);
		const u32 level = std::min<u32>(texture.mipLevelCount - 1,
				static_cast<u32>(selected) + (state.trilinearPassB ? 1u : 0u));
		pixels = texture.mipLevels[level];
		width = std::max(1u, width >> level);
		height = std::max(1u, height >> level);
	}
	u = wrapCoordinate(u, state.clampU, state.flipU);
	v = wrapCoordinate(v, state.clampV, state.flipV);
	const float sampleAdjustment = state.texelSampleCenter ? .5f : 0.f;
	const float texelX = u * width - sampleAdjustment;
	const float texelY = v * height - sampleAdjustment;
	auto fetch = [&](s32 x, s32 y) {
		const u32 sx = wrapTexel(x, width, state.clampU, state.flipU);
		const u32 sy = wrapTexel(y, height, state.clampV, state.flipV);
		return pixels[static_cast<size_t>(sy) * width + sx];
	};
	if (!state.bilinearFilter)
	{
		// With the texel-center half offset enabled, normalized texel centers
		// are at (n + .5) / size and point sampling selects floor(u * size).
		// In corner mode centers are at n / size, so ties are rounded upward.
		return fetch(static_cast<s32>(std::floor(texelX + .5f)),
				static_cast<s32>(std::floor(texelY + .5f)));
	}

	const s32 x0 = static_cast<s32>(std::floor(texelX));
	const s32 y0 = static_cast<s32>(std::floor(texelY));
	const u32 sx0 = wrapTexel(x0, width, state.clampU, state.flipU);
	const u32 sx1 = wrapTexel(x0 + 1, width, state.clampU, state.flipU);
	const u32 sy0 = wrapTexel(y0, height, state.clampV, state.flipV);
	const u32 sy1 = wrapTexel(y0 + 1, height, state.clampV, state.flipV);
	const Color c00 = pixels[static_cast<size_t>(sy0) * width + sx0];
	const Color c10 = pixels[static_cast<size_t>(sy0) * width + sx1];
	const Color c01 = pixels[static_cast<size_t>(sy1) * width + sx0];
	const Color c11 = pixels[static_cast<size_t>(sy1) * width + sx1];
	const float tx = texelX - x0;
	const float ty = texelY - y0;
#if defined(__ARM_NEON)
	auto channels = [](Color color) {
		u32 packed;
		std::memcpy(&packed, &color, sizeof(packed));
		const uint8x8_t bytes = vreinterpret_u8_u32(vdup_n_u32(packed));
		return vcvtq_f32_u32(vmovl_u16(vget_low_u16(vmovl_u8(bytes))));
	};
	const float32x4_t f00 = channels(c00);
	const float32x4_t f10 = channels(c10);
	const float32x4_t f01 = channels(c01);
	const float32x4_t f11 = channels(c11);
	const float32x4_t top = vfmaq_n_f32(f00, vsubq_f32(f10, f00), tx);
	const float32x4_t bottom = vfmaq_n_f32(f01, vsubq_f32(f11, f01), tx);
	float32x4_t interpolated = vfmaq_n_f32(top, vsubq_f32(bottom, top), ty);
	interpolated = vmaxq_f32(vdupq_n_f32(0.f),
			vminq_f32(vdupq_n_f32(255.f), interpolated));
	const uint16x4_t rounded = vmovn_u32(vcvtaq_u32_f32(interpolated));
	const uint8x8_t packedBytes = vmovn_u16(vcombine_u16(rounded, vdup_n_u16(0)));
	const u32 packed = vget_lane_u32(vreinterpret_u32_u8(packedBytes), 0);
	Color result;
	std::memcpy(&result, &packed, sizeof(result));
	return result;
#else
	auto bilinear = [&](auto member) {
		const float top = c00.*member + (c10.*member - c00.*member) * tx;
		const float bottom = c01.*member + (c11.*member - c01.*member) * tx;
		return clampByte(top + (bottom - top) * ty);
	};
	return { bilinear(&Color::r), bilinear(&Color::g), bilinear(&Color::b),
		bilinear(&Color::a) };
#endif
}

void SoftwareRasterizer::forceNearestFiltering(PipelineState& state)
{
	state.bilinearFilter = false;
	state.textureSuperSample = false;
	state.trilinearFilter = false;
	state.texture.mipLevelCount = std::min<u8>(1, state.texture.mipLevelCount);
}

TextureView SoftwareRasterizer::makeTextureView(const Color *pixels, u32 width, u32 height,
		bool mipmapped)
{
	TextureView result;
	result.width = width;
	result.height = height;
	if (pixels == nullptr || width == 0 || height == 0)
		return result;
	if (!mipmapped)
	{
		result.pixels = pixels;
		result.mipLevels[0] = pixels;
		result.mipLevelCount = 1;
		return result;
	}
	// PVR mipmapped textures are square. They are stored smallest first,
	// while renderer LoD zero denotes the largest level.
	u32 maxLevel = 0;
	for (u32 dimension = width; dimension > 1; dimension >>= 1)
		++maxLevel;
	result.mipLevelCount = static_cast<u8>(std::min<size_t>(maxLevel + 1,
			result.mipLevels.size()));
	for (u32 lod = 0; lod < result.mipLevelCount; ++lod)
	{
		const u32 smallestIndex = maxLevel - lod;
		const size_t offset = smallestIndex == 0 ? 0
				: ((static_cast<size_t>(1) << (2 * smallestIndex)) - 1) / 3;
		result.mipLevels[lod] = pixels + offset;
	}
	result.pixels = result.mipLevels[0];
	return result;
}

std::vector<Color> SoftwareRasterizer::generateMipChain(const Color *source, u32 dimension)
{
	if (source == nullptr || dimension == 0 || (dimension & (dimension - 1)) != 0)
		return {};
	const size_t pixelCount = static_cast<size_t>(dimension) * dimension;
	const size_t totalPixels = pixelCount > 1
			? pixelCount + (pixelCount - 1) / 3 : pixelCount;
	std::vector<Color> pixels(totalPixels);
	u32 sourceDimension = dimension;
	size_t sourceOffset = pixelCount > 1 ? (pixelCount - 1) / 3 : 0;
	std::copy_n(source, pixelCount, pixels.data() + sourceOffset);
	while (sourceDimension > 1)
	{
		const u32 destinationDimension = sourceDimension / 2;
		const size_t destinationPixels = static_cast<size_t>(destinationDimension)
				* destinationDimension;
		const size_t destinationOffset = destinationDimension == 1 ? 0
				: (destinationPixels - 1) / 3;
		for (u32 y = 0; y < destinationDimension; ++y)
			for (u32 x = 0; x < destinationDimension; ++x)
			{
				const Color& c00 = pixels[sourceOffset
						+ static_cast<size_t>(y * 2) * sourceDimension + x * 2];
				const Color& c10 = pixels[sourceOffset
						+ static_cast<size_t>(y * 2) * sourceDimension + x * 2 + 1];
				const Color& c01 = pixels[sourceOffset
						+ static_cast<size_t>(y * 2 + 1) * sourceDimension + x * 2];
				const Color& c11 = pixels[sourceOffset
						+ static_cast<size_t>(y * 2 + 1) * sourceDimension + x * 2 + 1];
				auto average = [&](auto member) {
					return static_cast<u8>((static_cast<u32>(c00.*member) + c10.*member
							+ c01.*member + c11.*member + 2) / 4);
				};
				pixels[destinationOffset + static_cast<size_t>(y) * destinationDimension + x] = {
					average(&Color::r), average(&Color::g), average(&Color::b), average(&Color::a)
				};
			}
		sourceDimension = destinationDimension;
		sourceOffset = destinationOffset;
	}
	return pixels;
}

std::vector<Color> SoftwareRasterizer::scaleLinear(const std::vector<Color>& source,
		u32 sourceWidth, u32 sourceHeight, u32 destinationWidth, u32 destinationHeight)
{
	if (sourceWidth == 0 || sourceHeight == 0 || destinationWidth == 0 || destinationHeight == 0
			|| source.size() < static_cast<size_t>(sourceWidth) * sourceHeight)
		return {};
	std::vector<Color> destination(static_cast<size_t>(destinationWidth) * destinationHeight);
	for (u32 y = 0; y < destinationHeight; ++y)
	{
		const float sourceY = (y + .5f) * sourceHeight / destinationHeight - .5f;
		const s32 y0 = static_cast<s32>(std::floor(sourceY));
		const s32 y1 = y0 + 1;
		const float fy = sourceY - y0;
		for (u32 x = 0; x < destinationWidth; ++x)
		{
			const float sourceX = (x + .5f) * sourceWidth / destinationWidth - .5f;
			const s32 x0 = static_cast<s32>(std::floor(sourceX));
			const s32 x1 = x0 + 1;
			const float fx = sourceX - x0;
			auto fetch = [&](s32 sx, s32 sy) -> const Color& {
				const u32 clampedX = static_cast<u32>(std::clamp<s32>(sx, 0, sourceWidth - 1));
				const u32 clampedY = static_cast<u32>(std::clamp<s32>(sy, 0, sourceHeight - 1));
				return source[static_cast<size_t>(clampedY) * sourceWidth + clampedX];
			};
			const Color& c00 = fetch(x0, y0);
			const Color& c10 = fetch(x1, y0);
			const Color& c01 = fetch(x0, y1);
			const Color& c11 = fetch(x1, y1);
			auto channel = [&](auto member) {
				const float top = c00.*member + (c10.*member - c00.*member) * fx;
				const float bottom = c01.*member + (c11.*member - c01.*member) * fx;
				return clampByte(top + (bottom - top) * fy);
			};
			destination[static_cast<size_t>(y) * destinationWidth + x] = {
				channel(&Color::r), channel(&Color::g), channel(&Color::b), channel(&Color::a)
			};
		}
	}
	return destination;
}

std::vector<Color> SoftwareRasterizer::filterVertical(const std::vector<Color>& source,
		u32 width, u32 height, u8 outerCoefficient, u8 centerCoefficient)
{
	if (width == 0 || height == 0
			|| source.size() < static_cast<size_t>(width) * height)
		return {};
	std::vector<Color> destination(static_cast<size_t>(width) * height);
	for (u32 y = 0; y < height; ++y)
	{
		const u32 previousY = y == 0 ? 0 : y - 1;
		const u32 nextY = std::min(y + 1, height - 1);
		for (u32 x = 0; x < width; ++x)
		{
			const Color& previous = source[static_cast<size_t>(previousY) * width + x];
			const Color& center = source[static_cast<size_t>(y) * width + x];
			const Color& next = source[static_cast<size_t>(nextY) * width + x];
			auto filtered = [&](auto member) {
				const u32 sum = (static_cast<u32>(previous.*member) + next.*member)
						* outerCoefficient + static_cast<u32>(center.*member) * centerCoefficient;
				return static_cast<u8>(std::min(255u, sum >> 8));
			};
			destination[static_cast<size_t>(y) * width + x] = {
				filtered(&Color::r), filtered(&Color::g), filtered(&Color::b), filtered(&Color::a)
			};
		}
	}
	return destination;
}

std::vector<Color> SoftwareRasterizer::selectInterlacedField(
		const std::vector<Color>& source, u32 width, u32 height, u32 outputHeight,
		bool oddField)
{
	if (width == 0 || height == 0 || outputHeight == 0
			|| source.size() < static_cast<size_t>(width) * height)
		return {};
	std::vector<Color> destination(static_cast<size_t>(width) * outputHeight);
	for (u32 y = 0; y < outputHeight; ++y)
	{
		const u32 sourceY = std::min(y * 2 + static_cast<u32>(oddField), height - 1);
		std::copy_n(source.data() + static_cast<size_t>(sourceY) * width, width,
				destination.data() + static_cast<size_t>(y) * width);
	}
	return destination;
}

std::vector<Color> SoftwareRasterizer::scaleHorizontalHalf(
		const std::vector<Color>& source, u32 width, u32 height)
{
	if (width < 2 || height == 0
			|| source.size() < static_cast<size_t>(width) * height)
		return {};
	const u32 outputWidth = width / 2;
	std::vector<Color> destination(static_cast<size_t>(outputWidth) * height);
	for (u32 y = 0; y < height; ++y)
		for (u32 x = 0; x < outputWidth; ++x)
		{
			const Color& left = source[static_cast<size_t>(y) * width + x * 2];
			const Color& right = source[static_cast<size_t>(y) * width + x * 2 + 1];
			auto average = [&](auto member) {
				return static_cast<u8>((static_cast<u32>(left.*member) + right.*member) >> 1);
			};
			destination[static_cast<size_t>(y) * outputWidth + x] = {
				average(&Color::r), average(&Color::g), average(&Color::b), average(&Color::a)
			};
		}
	return destination;
}

std::vector<Color> SoftwareRasterizer::scaleFramebufferForOutput(
		const std::vector<Color>& source, u32 sourceWidth, u32 sourceHeight,
		u32 destinationWidth, u32 destinationHeight, bool verticalFilter,
		u8 outerCoefficient, u8 centerCoefficient, bool interlace, bool oddField,
		bool horizontalHalf)
{
	if (sourceWidth == 0 || sourceHeight == 0 || destinationWidth == 0
			|| destinationHeight == 0
			|| source.size() < static_cast<size_t>(sourceWidth) * sourceHeight)
		return {};
	std::vector<Color> current(source.begin(),
			source.begin() + static_cast<size_t>(sourceWidth) * sourceHeight);
	u32 width = sourceWidth;
	u32 height = sourceHeight;
	if (verticalFilter)
		current = filterVertical(current, width, height, outerCoefficient, centerCoefficient);
	if (interlace)
	{
		current = selectInterlacedField(current, width, height, destinationHeight, oddField);
		height = destinationHeight;
	}
	if (horizontalHalf && width >= 2)
	{
		current = scaleHorizontalHalf(current, width, height);
		width /= 2;
	}
	if (width != destinationWidth || height != destinationHeight)
		current = scaleLinear(current, width, height, destinationWidth, destinationHeight);
	return current;
}

void SoftwareRasterizer::ditherFramebuffer(std::vector<Color>& pixels, u32 width, u32 height,
		u32 packMode)
{
	if (packMode > 3 || pixels.size() < static_cast<size_t>(width) * height)
		return;
	static constexpr u8 ditherTable[16] {
		5, 13, 7, 15, 9, 1, 11, 3, 6, 14, 4, 12, 10, 2, 8, 0
	};
	u8 divisorR = 1;
	u8 divisorG = 1;
	u8 divisorB = 1;
	if (packMode == 0 || packMode == 3)
		divisorR = divisorG = divisorB = 2;
	else if (packMode == 1)
	{
		divisorR = divisorB = 2;
		divisorG = 4;
	}
	for (u32 y = 0; y < height; ++y)
		for (u32 x = 0; x < width; ++x)
		{
			Color& color = pixels[static_cast<size_t>(y) * width + x];
			const u8 dither = ditherTable[(y & 3) * 4 + (x & 3)];
			color.r = static_cast<u8>(std::min(255, color.r + dither / divisorR));
			color.g = static_cast<u8>(std::min(255, color.g + dither / divisorG));
			color.b = static_cast<u8>(std::min(255, color.b + dither / divisorB));
			// The PVR's alpha dither input is a constant one; only RGB uses
			// the spatial 4x4 matrix (matching the hardware renderer paths).
			color.a = static_cast<u8>(std::min(255, color.a + 1));
		}
}

void SoftwareRasterizer::convertFramebufferForDisplay(std::vector<Color>& pixels,
		u32 writePackMode, u8 writeKValue, u8 alphaThreshold, u32 readDepth, u8 concat)
{
	static constexpr u8 writeBytesPerPixel[8] { 2, 2, 2, 2, 3, 4, 4, 0 };
	static constexpr u8 readBytesPerPixel[4] { 2, 2, 3, 4 };
	writePackMode &= 7;
	readDepth &= 3;
	const u32 writeBpp = writeBytesPerPixel[writePackMode];
	const u32 readBpp = readBytesPerPixel[readDepth];
	if (writeBpp == 0)
	{
		pixels.clear();
		return;
	}
	std::vector<u8> bytes;
	bytes.reserve(pixels.size() * writeBpp);
	auto append16 = [&](u16 value) {
		bytes.push_back(static_cast<u8>(value));
		bytes.push_back(static_cast<u8>(value >> 8));
	};
	for (const Color& color : pixels)
	{
		switch (writePackMode)
		{
		case 0:
			append16(static_cast<u16>(((writeKValue & 0x80) << 8)
					| ((color.r >> 3) << 10) | ((color.g >> 3) << 5) | (color.b >> 3)));
			break;
		case 1:
			append16(static_cast<u16>(((color.r >> 3) << 11)
					| ((color.g >> 2) << 5) | (color.b >> 3)));
			break;
		case 2:
			append16(static_cast<u16>(((color.a >> 4) << 12) | ((color.r >> 4) << 8)
					| ((color.g >> 4) << 4) | (color.b >> 4)));
			break;
		case 3:
			append16(static_cast<u16>((color.a >= alphaThreshold ? 0x8000 : 0)
					| ((color.r >> 3) << 10) | ((color.g >> 3) << 5) | (color.b >> 3)));
			break;
		case 4:
			bytes.insert(bytes.end(), { color.b, color.g, color.r });
			break;
		case 5:
			bytes.insert(bytes.end(), { color.b, color.g, color.r, writeKValue });
			break;
		case 6:
			bytes.insert(bytes.end(), { color.b, color.g, color.r, color.a });
			break;
		}
	}
	concat &= 7;
	std::vector<Color> decoded(bytes.size() / readBpp);
	for (size_t i = 0; i < decoded.size(); ++i)
	{
		const u8 *source = &bytes[i * readBpp];
		Color& color = decoded[i];
		if (readDepth <= 1)
		{
			const u16 stored = static_cast<u16>(source[0] | (source[1] << 8));
			if (readDepth == 0)
			{
				color.r = static_cast<u8>(((stored >> 10) & 0x1f) << 3) | concat;
				color.g = static_cast<u8>(((stored >> 5) & 0x1f) << 3) | concat;
				color.b = static_cast<u8>((stored & 0x1f) << 3) | concat;
			}
			else
			{
				color.r = static_cast<u8>(((stored >> 11) & 0x1f) << 3) | concat;
				color.g = static_cast<u8>(((stored >> 5) & 0x3f) << 2) | (concat & 3);
				color.b = static_cast<u8>((stored & 0x1f) << 3) | concat;
			}
		}
		else
		{
			color.b = source[0];
			color.g = source[1];
			color.r = source[2];
		}
		color.a = 255;
	}
	pixels = std::move(decoded);
}

u32 SoftwareRasterizer::framebufferDisplayWidth(u32 writeWidth, u32 writePackMode,
		u32 readDepth)
{
	static constexpr u8 writeBytesPerPixel[8] { 2, 2, 2, 2, 3, 4, 4, 0 };
	static constexpr u8 readBytesPerPixel[4] { 2, 2, 3, 4 };
	return writeWidth * writeBytesPerPixel[writePackMode & 7]
			/ readBytesPerPixel[readDepth & 3];
}

std::vector<Color> SoftwareRasterizer::prepareFramebufferForDisplay(
		const std::vector<Color>& frameStore, u32 width, u32 height, bool dither,
		u32 writePackMode, u8 writeKValue, u8 alphaThreshold, u32 readDepth, u8 concat)
{
	std::vector<Color> result = frameStore;
	if (dither && writePackMode <= 3)
		ditherFramebuffer(result, width, height, writePackMode);
	std::vector<Color> display;
	display.reserve(static_cast<size_t>(framebufferDisplayWidth(width, writePackMode, readDepth))
			* height);
	for (u32 y = 0; y < height; ++y)
	{
		const auto first = result.begin() + std::min<size_t>(static_cast<size_t>(y) * width,
				result.size());
		const auto last = result.begin() + std::min<size_t>(static_cast<size_t>(y + 1) * width,
				result.size());
		std::vector<Color> row(first, last);
		convertFramebufferForDisplay(row, writePackMode, writeKValue, alphaThreshold,
				readDepth, concat);
		display.insert(display.end(), row.begin(), row.end());
	}
	return display;
}

void SoftwareRasterizer::copyColorRect(const std::vector<Color>& source,
		std::vector<Color>& destination, u32 width, u32 height,
		s32 x0, s32 y0, s32 x1, s32 y1)
{
	const size_t pixelCount = static_cast<size_t>(width) * height;
	if (source.size() < pixelCount)
		return;
	if (destination.size() != pixelCount)
		destination.resize(pixelCount);
	x0 = std::clamp<s32>(x0, 0, width);
	y0 = std::clamp<s32>(y0, 0, height);
	x1 = std::clamp<s32>(x1, x0, width);
	y1 = std::clamp<s32>(y1, y0, height);
	for (s32 y = y0; y < y1; ++y)
	{
		const size_t offset = static_cast<size_t>(y) * width + x0;
		std::copy(source.begin() + offset, source.begin() + offset + (x1 - x0),
				destination.begin() + offset);
	}
}

void SoftwareRasterizer::applyFramebufferDither(u32 packMode)
{
	ditherFramebuffer(colorBuffer_, width_, height_, packMode);
}

float SoftwareRasterizer::fogCoefficient(float inverseDepth, float density, const u32 *table)
{
	if (table == nullptr)
		return 0.f;
	const float z = std::clamp(density * inverseDepth, 1.f, 255.9999f);
	// The CORE does not calculate a logarithm: it concatenates the low three
	// exponent bits with the high four IEEE-754 mantissa bits. The remaining
	// 19 mantissa bits are the interpolation fraction. Extracting those fields
	// directly also makes power-of-two transitions deterministic.
	u32 bits;
	std::memcpy(&bits, &z, sizeof(bits));
	const u32 exponent = ((bits >> 23) & 0xff) - 127;
	const u32 mantissa = bits & 0x7fffff;
	const u32 index = std::min(127u, (exponent << 4) | (mantissa >> 19));
	const float fraction = (mantissa & 0x7ffff) / 524288.f;
	const u8 *entry = reinterpret_cast<const u8 *>(&table[index]);
	return (entry[1] + (entry[0] - entry[1]) * fraction) / 255.f;
}

void SoftwareRasterizer::drawTriangle(const Vertex& in0, const Vertex& in1, const Vertex& in2,
		const PipelineState& state)
{
	if (width_ == 0 || height_ == 0)
		return;
	if (!hasFinitePosition(in0) || !hasFinitePosition(in1) || !hasFinitePosition(in2))
		return;

	Vertex vertices[3] { in0, in1, in2 };
	// Flat-shaded PVR triangles use the final submitted vertex. Preserve it
	// when winding normalization swaps the other interpolation vertices.
	const Vertex flatVertex = in2;
	Point points[3] {
		{ toFixed(in0.x), toFixed(in0.y) },
		{ toFixed(in1.x), toFixed(in1.y) },
		{ toFixed(in2.x), toFixed(in2.y) },
	};
	s64 area = edge(points[0], points[1], points[2].x, points[2].y);
	if (area == 0)
		return;
	if (area < 0)
	{
		std::swap(points[1], points[2]);
		std::swap(vertices[1], vertices[2]);
		area = -area;
	}

	const s32 fpuSampleOffset = state.fpuPixelCenter ? CENTER_SAMPLE_OFFSET : 0;
	const s32 tspSampleOffset = state.tspPixelCenter ? CENTER_SAMPLE_OFFSET : 0;
	s32 minX = std::max(0, static_cast<s32>(std::ceil((std::min({ points[0].x, points[1].x, points[2].x }) - fpuSampleOffset) / static_cast<float>(SUBPIXEL_SCALE))));
	s32 minY = std::max(0, static_cast<s32>(std::ceil((std::min({ points[0].y, points[1].y, points[2].y }) - fpuSampleOffset) / static_cast<float>(SUBPIXEL_SCALE))));
	const s32 targetMaxX = static_cast<s32>(std::min<u32>(width_ - 1, MAX_FIXED_COORDINATE / SUBPIXEL_SCALE));
	const s32 targetMaxY = static_cast<s32>(std::min<u32>(height_ - 1, MAX_FIXED_COORDINATE / SUBPIXEL_SCALE));
	s32 maxX = std::min(targetMaxX, (std::max({ points[0].x, points[1].x, points[2].x }) - fpuSampleOffset) / SUBPIXEL_SCALE);
	s32 maxY = std::min(targetMaxY, (std::max({ points[0].y, points[1].y, points[2].y }) - fpuSampleOffset) / SUBPIXEL_SCALE);
	if (state.scissorEnabled)
	{
		minX = std::max(minX, state.scissorX0);
		minY = std::max(minY, state.scissorY0);
		maxX = std::min(maxX, state.scissorX1 - 1);
		maxY = std::min(maxY, state.scissorY1 - 1);
	}
	if (minX > maxX || minY > maxY)
		return;

	const bool topLeft0 = isTopLeft(points[1], points[2]);
	const bool topLeft1 = isTopLeft(points[2], points[0]);
	const bool topLeft2 = isTopLeft(points[0], points[1]);
	const double inverseArea = 1.0 / static_cast<double>(area);
	const s64 edgeStepX0 = -(static_cast<s64>(points[2].y) - points[1].y) * SUBPIXEL_SCALE;
	const s64 edgeStepX1 = -(static_cast<s64>(points[0].y) - points[2].y) * SUBPIXEL_SCALE;
	const s64 edgeStepX2 = -(static_cast<s64>(points[1].y) - points[0].y) * SUBPIXEL_SCALE;
	const s64 edgeStepY0 = (static_cast<s64>(points[2].x) - points[1].x) * SUBPIXEL_SCALE;
	const s64 edgeStepY1 = (static_cast<s64>(points[0].x) - points[2].x) * SUBPIXEL_SCALE;
	const s64 edgeStepY2 = (static_cast<s64>(points[1].x) - points[0].x) * SUBPIXEL_SCALE;
	const float barycentricDx[3] {
		static_cast<float>(edgeStepX0 * inverseArea),
		static_cast<float>(edgeStepX1 * inverseArea),
		static_cast<float>(edgeStepX2 * inverseArea),
	};
	const float barycentricDy[3] {
		static_cast<float>(edgeStepY0 * inverseArea),
		static_cast<float>(edgeStepY1 * inverseArea),
		static_cast<float>(edgeStepY2 * inverseArea),
	};
	const float qDxHost = vertices[0].z * barycentricDx[0]
			+ vertices[1].z * barycentricDx[1] + vertices[2].z * barycentricDx[2];
	const float qDyHost = vertices[0].z * barycentricDy[0]
			+ vertices[1].z * barycentricDy[1] + vertices[2].z * barycentricDy[2];
	auto attributeStep = [&](bool area1, bool u, const float *step) {
		const float a0 = u ? (area1 ? vertices[0].u1 : vertices[0].u)
				: (area1 ? vertices[0].v1 : vertices[0].v);
		const float a1 = u ? (area1 ? vertices[1].u1 : vertices[1].u)
				: (area1 ? vertices[1].v1 : vertices[1].v);
		const float a2 = u ? (area1 ? vertices[2].u1 : vertices[2].u)
				: (area1 ? vertices[2].v1 : vertices[2].v);
		return a0 * vertices[0].z * step[0] + a1 * vertices[1].z * step[1]
				+ a2 * vertices[2].z * step[2];
	};
	const float uqDxHost[2] { attributeStep(false, true, barycentricDx),
		attributeStep(true, true, barycentricDx) };
	const float uqDyHost[2] { attributeStep(false, true, barycentricDy),
		attributeStep(true, true, barycentricDy) };
	const float vqDxHost[2] { attributeStep(false, false, barycentricDx),
		attributeStep(true, false, barycentricDx) };
	const float vqDyHost[2] { attributeStep(false, false, barycentricDy),
		attributeStep(true, false, barycentricDy) };
	struct MipGradient
	{
		bool valid = false;
		float numerator = 0.f;
	};
	// With D calculation control clear, q, uq and vq are evaluated at the
	// native origin. Their derivative numerators are therefore invariant over
	// the triangle; only 1 / q^2 changes from pixel to pixel.
	auto prepareMipGradient = [&](const PipelineState& fragmentState, unsigned areaIndex) {
		MipGradient result;
		if (fragmentState.texture.mipLevelCount <= 1 || fragmentState.dCalcCtrl)
			return result;
		const s32 sampleXFixed = minX * SUBPIXEL_SCALE + tspSampleOffset;
		const s32 sampleYFixed = minY * SUBPIXEL_SCALE + tspSampleOffset;
		const float q0 = static_cast<float>(edge(points[1], points[2], sampleXFixed, sampleYFixed) * inverseArea);
		const float q1 = static_cast<float>(edge(points[2], points[0], sampleXFixed, sampleYFixed) * inverseArea);
		const float q2 = static_cast<float>(edge(points[0], points[1], sampleXFixed, sampleYFixed) * inverseArea);
		const float q = vertices[0].z * q0 + vertices[1].z * q1 + vertices[2].z * q2;
		const float u0 = areaIndex ? vertices[0].u1 : vertices[0].u;
		const float u1 = areaIndex ? vertices[1].u1 : vertices[1].u;
		const float u2 = areaIndex ? vertices[2].u1 : vertices[2].u;
		const float v0 = areaIndex ? vertices[0].v1 : vertices[0].v;
		const float v1 = areaIndex ? vertices[1].v1 : vertices[1].v;
		const float v2 = areaIndex ? vertices[2].v1 : vertices[2].v;
		const float uq = u0 * vertices[0].z * q0 + u1 * vertices[1].z * q1
				+ u2 * vertices[2].z * q2;
		const float vq = v0 * vertices[0].z * q0 + v1 * vertices[1].z * q1
				+ v2 * vertices[2].z * q2;
		const float sampleX = sampleXFixed / static_cast<float>(SUBPIXEL_SCALE)
				- fragmentState.nativeOriginX;
		const float sampleY = sampleYFixed / static_cast<float>(SUBPIXEL_SCALE)
				- fragmentState.nativeOriginY;
		const float referenceQ = q - qDxHost * sampleX - qDyHost * sampleY;
		const float referenceUQ = uq - uqDxHost[areaIndex] * sampleX
				- uqDyHost[areaIndex] * sampleY;
		const float referenceVQ = vq - vqDxHost[areaIndex] * sampleX
				- vqDyHost[areaIndex] * sampleY;
		const float dqDx = qDxHost * fragmentState.nativeToHostScaleX;
		const float dqDy = qDyHost * fragmentState.nativeToHostScaleY;
		const float duqDx = uqDxHost[areaIndex] * fragmentState.nativeToHostScaleX;
		const float duqDy = uqDyHost[areaIndex] * fragmentState.nativeToHostScaleY;
		const float dvqDx = vqDxHost[areaIndex] * fragmentState.nativeToHostScaleX;
		const float dvqDy = vqDyHost[areaIndex] * fragmentState.nativeToHostScaleY;
		const float dxU = (duqDx * referenceQ - dqDx * referenceUQ)
				* fragmentState.texture.width;
		const float dxV = (dvqDx * referenceQ - dqDx * referenceVQ)
				* fragmentState.texture.height;
		const float dyU = (duqDy * referenceQ - dqDy * referenceUQ)
				* fragmentState.texture.width;
		const float dyV = (dvqDy * referenceQ - dqDy * referenceVQ)
				* fragmentState.texture.height;
		result.valid = true;
		result.numerator = std::sqrt(std::max(dxU * dxU + dxV * dxV,
				dyU * dyU + dyV * dyV)) * fragmentState.mipDAdjust;
		return result;
	};
	const PipelineState& area1State = state.useArea1VertexData || state.area1 == nullptr
			? state : *state.area1;
	const MipGradient mipGradients[2] {
		prepareMipGradient(state, 0),
		prepareMipGradient(area1State, 1),
	};
	struct SuperSampleEdgeOffset
	{
		s64 x[3];
		s64 y[3];
	};
	auto prepareSuperSampleEdgeOffset = [&](const PipelineState& fragmentState) {
		const s32 x = static_cast<s32>(std::lround(
				CENTER_SAMPLE_OFFSET * fragmentState.nativeToHostScaleX));
		const s32 y = static_cast<s32>(std::lround(
				CENTER_SAMPLE_OFFSET * fragmentState.nativeToHostScaleY));
		return SuperSampleEdgeOffset {
			{ edgeStepX0 / SUBPIXEL_SCALE * x, edgeStepX1 / SUBPIXEL_SCALE * x,
					edgeStepX2 / SUBPIXEL_SCALE * x },
			{ edgeStepY0 / SUBPIXEL_SCALE * y, edgeStepY1 / SUBPIXEL_SCALE * y,
					edgeStepY2 / SUBPIXEL_SCALE * y },
		};
	};
	const SuperSampleEdgeOffset superSampleEdgeOffsets[2] {
		prepareSuperSampleEdgeOffset(state),
		prepareSuperSampleEdgeOffset(area1State),
	};
	const float vertexZ[3] {
		vertices[0].z, vertices[1].z, vertices[2].z,
	};
	const float vertexUZ[2][3] {
		{ vertices[0].u * vertexZ[0], vertices[1].u * vertexZ[1],
				vertices[2].u * vertexZ[2] },
		{ vertices[0].u1 * vertexZ[0], vertices[1].u1 * vertexZ[1],
				vertices[2].u1 * vertexZ[2] },
	};
	const float vertexVZ[2][3] {
		{ vertices[0].v * vertexZ[0], vertices[1].v * vertexZ[1],
				vertices[2].v * vertexZ[2] },
		{ vertices[0].v1 * vertexZ[0], vertices[1].v1 * vertexZ[1],
				vertices[2].v1 * vertexZ[2] },
	};
	struct WeightedColor
	{
		float r;
		float g;
		float b;
		float a;
	};
	auto weightColor = [](const Color& color, float z) {
		return WeightedColor {
			color.r * z, color.g * z, color.b * z, color.a * z,
		};
	};
	const WeightedColor vertexColorZ[2][2][3] {
		{
			{ weightColor(vertices[0].color, vertexZ[0]),
					weightColor(vertices[1].color, vertexZ[1]),
					weightColor(vertices[2].color, vertexZ[2]) },
			{ weightColor(vertices[0].offsetColor, vertexZ[0]),
					weightColor(vertices[1].offsetColor, vertexZ[1]),
					weightColor(vertices[2].offsetColor, vertexZ[2]) },
		},
		{
			{ weightColor(vertices[0].color1, vertexZ[0]),
					weightColor(vertices[1].color1, vertexZ[1]),
					weightColor(vertices[2].color1, vertexZ[2]) },
			{ weightColor(vertices[0].offsetColor1, vertexZ[0]),
					weightColor(vertices[1].offsetColor1, vertexZ[1]),
					weightColor(vertices[2].offsetColor1, vertexZ[2]) },
		},
	};

	auto rasterizeRow = [&](s32 y)
	{
		const s32 sampleY = y * SUBPIXEL_SCALE + fpuSampleOffset;
		const s32 firstSampleX = minX * SUBPIXEL_SCALE + fpuSampleOffset;
		s64 w0 = edge(points[1], points[2], firstSampleX, sampleY);
		s64 w1 = edge(points[2], points[0], firstSampleX, sampleY);
		s64 w2 = edge(points[0], points[1], firstSampleX, sampleY);
		const s32 shadeSampleY = y * SUBPIXEL_SCALE + tspSampleOffset;
		const s32 firstShadeSampleX = minX * SUBPIXEL_SCALE + tspSampleOffset;
		s64 shadeW0 = edge(points[1], points[2], firstShadeSampleX, shadeSampleY);
		s64 shadeW1 = edge(points[2], points[0], firstShadeSampleX, shadeSampleY);
		s64 shadeW2 = edge(points[0], points[1], firstShadeSampleX, shadeSampleY);
		for (s32 x = minX; x <= maxX; ++x,
				w0 += edgeStepX0, w1 += edgeStepX1, w2 += edgeStepX2,
				shadeW0 += edgeStepX0, shadeW1 += edgeStepX1, shadeW2 += edgeStepX2)
		{
			if (state.excludeClip && x >= state.excludeX0 && x < state.excludeX1
					&& y >= state.excludeY0 && y < state.excludeY1)
				continue;
			if (w0 < 0 || (w0 == 0 && !topLeft0)
					|| w1 < 0 || (w1 == 0 && !topLeft1)
					|| w2 < 0 || (w2 == 0 && !topLeft2))
				continue;

			const float b0 = static_cast<float>(w0 * inverseArea);
			const float b1 = static_cast<float>(w1 * inverseArea);
			const float b2 = static_cast<float>(w2 * inverseArea);
			const float z = vertices[0].z * b0 + vertices[1].z * b1 + vertices[2].z * b2;
			const size_t offset = static_cast<size_t>(y) * width_ + static_cast<u32>(x);
			if (state.coverageMask != nullptr && state.coverageMask[offset] == 0)
				continue;
			if (!depthTest(state.depthMode, z, depthBuffer_[offset]))
				continue;
			if (state.testVisibility && visibilityBuffer_[offset] != state.sequence)
				continue;
			const bool modifierArea1 = (stencilBuffer_[offset] & 0x81) == 0x81;
			if (state.modifierArea1Only && !modifierArea1)
				continue;
			const bool area1 = state.useArea1VertexData
					|| (state.area1 != nullptr && modifierArea1);
			const PipelineState& fragmentState = area1 && !state.useArea1VertexData
					? *state.area1 : state;
			const float s0 = static_cast<float>(shadeW0 * inverseArea);
			const float s1 = static_cast<float>(shadeW1 * inverseArea);
			const float s2 = static_cast<float>(shadeW2 * inverseArea);
			const float shadeZ = vertices[0].z * s0 + vertices[1].z * s1 + vertices[2].z * s2;

			const float inverseZ = shadeZ == 0.f ? 0.f : 1.f / shadeZ;
			auto interpolate = [&](bool offsetColor, auto colorMember, auto weightedMember) {
				if (!fragmentState.gouraud)
				{
					const Color& flatColor = area1
							? (offsetColor ? flatVertex.offsetColor1 : flatVertex.color1)
							: (offsetColor ? flatVertex.offsetColor : flatVertex.color);
					return static_cast<float>(flatColor.*colorMember);
				}
				const WeightedColor* colors = vertexColorZ[static_cast<unsigned>(area1)]
						[static_cast<unsigned>(offsetColor)];
				return ((colors[0].*weightedMember) * s0
						+ (colors[1].*weightedMember) * s1
						+ (colors[2].*weightedMember) * s2) * inverseZ;
			};
			Color source {
				clampByte(interpolate(false, &Color::r, &WeightedColor::r)),
				clampByte(interpolate(false, &Color::g, &WeightedColor::g)),
				clampByte(interpolate(false, &Color::b, &WeightedColor::b)),
				clampByte(interpolate(false, &Color::a, &WeightedColor::a)),
			};
			if (!fragmentState.useAlpha)
				source.a = 255;
			const bool needsIntensitySource = fragmentState.intensityShadow
					&& (state.deferBlend ? state.shadow
							: (!area1 && state.area1 == nullptr
									&& (stencilBuffer_[offset] & 0x81) == 0x81));
			Color intensitySource = source;
			auto scaleIntensityRgb = [&](Color& color) {
				color.r = static_cast<u8>((color.r * fragmentState.shadeScale + 128) / 256);
				color.g = static_cast<u8>((color.g * fragmentState.shadeScale + 128) / 256);
				color.b = static_cast<u8>((color.b * fragmentState.shadeScale + 128) / 256);
			};
			if (needsIntensitySource)
				scaleIntensityRgb(intensitySource);
			// Table fog is only consumed by lookup fog and lookup-fog alpha.
			// Vertex fog uses the offset alpha, while fog mode 2 bypasses fog
			// entirely, so avoid the exponent/mantissa lookup for those pixels.
			const float fog = fragmentState.fogMode == 0 || fragmentState.fogMode == 3
					? fogCoefficient(shadeZ, fragmentState.fogDensity, fragmentState.fogTable)
					: 0.f;
			if (fragmentState.fogMode == 3)
			{
				source.r = fragmentState.fogRamColor.r;
				source.g = fragmentState.fogRamColor.g;
				source.b = fragmentState.fogRamColor.b;
				source.a = clampByte(fog * 255.f);
				if (needsIntensitySource)
					intensitySource = source;
			}
			float pvrD = 1.f;
			if (fragmentState.texture)
			{
				const unsigned areaIndex = static_cast<unsigned>(area1);
				auto projectedAt = [&](s64 edgeOffset0, s64 edgeOffset1, s64 edgeOffset2) {
					const float q0 = static_cast<float>((shadeW0 + edgeOffset0) * inverseArea);
					const float q1 = static_cast<float>((shadeW1 + edgeOffset1) * inverseArea);
					const float q2 = static_cast<float>((shadeW2 + edgeOffset2) * inverseArea);
					const float q = vertexZ[0] * q0 + vertexZ[1] * q1 + vertexZ[2] * q2;
					const float uq = vertexUZ[areaIndex][0] * q0
						 + vertexUZ[areaIndex][1] * q1 + vertexUZ[areaIndex][2] * q2;
					const float vq = vertexVZ[areaIndex][0] * q0
						 + vertexVZ[areaIndex][1] * q1 + vertexVZ[areaIndex][2] * q2;
					const float reciprocal = q == 0.f ? 0.f : 1.f / q;
					return std::array<float, 5> {
						uq * reciprocal, vq * reciprocal, q, uq, vq,
					};
				};
				const float uq = vertexUZ[areaIndex][0] * s0
						+ vertexUZ[areaIndex][1] * s1 + vertexUZ[areaIndex][2] * s2;
				const float vq = vertexVZ[areaIndex][0] * s0
						+ vertexVZ[areaIndex][1] * s1 + vertexVZ[areaIndex][2] * s2;
				const float u = uq * inverseZ;
				const float v = vq * inverseZ;
				if (fragmentState.texture.mipLevelCount > 1)
				{
					const float denominator = shadeZ * shadeZ;
					const float reciprocalDenominator = denominator == 0.f ? 0.f : 1.f / denominator;
					if (mipGradients[areaIndex].valid)
						pvrD = std::max(0.f,
								mipGradients[areaIndex].numerator * reciprocalDenominator);
					else
					{
						const float dqDx = qDxHost * fragmentState.nativeToHostScaleX;
						const float dqDy = qDyHost * fragmentState.nativeToHostScaleY;
						const float duqDx = uqDxHost[areaIndex] * fragmentState.nativeToHostScaleX;
						const float duqDy = uqDyHost[areaIndex] * fragmentState.nativeToHostScaleY;
						const float dvqDx = vqDxHost[areaIndex] * fragmentState.nativeToHostScaleX;
						const float dvqDy = vqDyHost[areaIndex] * fragmentState.nativeToHostScaleY;
						const float referenceQ = vertexZ[0];
						const float referenceUQ = vertexUZ[areaIndex][0];
						const float referenceVQ = vertexVZ[areaIndex][0];
						const float dxU = (duqDx * referenceQ - dqDx * referenceUQ)
								* reciprocalDenominator * fragmentState.texture.width;
						const float dxV = (dvqDx * referenceQ - dqDx * referenceVQ)
								* reciprocalDenominator * fragmentState.texture.height;
						const float dyU = (duqDy * referenceQ - dqDy * referenceUQ)
								* reciprocalDenominator * fragmentState.texture.width;
						const float dyV = (dvqDy * referenceQ - dqDy * referenceVQ)
								* reciprocalDenominator * fragmentState.texture.height;
						const float footprint = std::sqrt(std::max(dxU * dxU + dxV * dxV,
								dyU * dyU + dyV * dyV));
						pvrD = std::max(0.f, footprint * fragmentState.mipDAdjust);
					}
				}
				Color texel = sampleTexture(fragmentState.texture, u, v, fragmentState, pvrD);
				if (fragmentState.textureSuperSample)
				{
					const SuperSampleEdgeOffset& offsets = superSampleEdgeOffsets[areaIndex];
					const auto uv10 = projectedAt(offsets.x[0], offsets.x[1], offsets.x[2]);
					const auto uv01 = projectedAt(offsets.y[0], offsets.y[1], offsets.y[2]);
					const auto uv11 = projectedAt(offsets.x[0] + offsets.y[0],
							offsets.x[1] + offsets.y[1], offsets.x[2] + offsets.y[2]);
					const Color samples[3] {
						sampleTexture(fragmentState.texture, uv10[0], uv10[1], fragmentState, pvrD),
						sampleTexture(fragmentState.texture, uv01[0], uv01[1], fragmentState, pvrD),
						sampleTexture(fragmentState.texture, uv11[0], uv11[1], fragmentState, pvrD),
					};
					texel = {
						static_cast<u8>((static_cast<u32>(texel.r) + samples[0].r + samples[1].r + samples[2].r + 2) / 4),
						static_cast<u8>((static_cast<u32>(texel.g) + samples[0].g + samples[1].g + samples[2].g + 2) / 4),
						static_cast<u8>((static_cast<u32>(texel.b) + samples[0].b + samples[1].b + samples[2].b + 2) / 4),
						static_cast<u8>((static_cast<u32>(texel.a) + samples[0].a + samples[1].a + samples[2].a + 2) / 4),
					};
				}
				if (fragmentState.bumpMap)
				{
					constexpr float PI = 3.14159265358979323846f;
					const float offsetA = interpolate(true, &Color::a, &WeightedColor::a) / 255.f;
					const float offsetR = interpolate(true, &Color::r, &WeightedColor::r) / 255.f;
					const float offsetG = interpolate(true, &Color::g, &WeightedColor::g) / 255.f;
					const float offsetB = interpolate(true, &Color::b, &WeightedColor::b) / 255.f;
					auto angleFraction = [](float parameter) {
						// PVR angle parameters use value / 256, except that the 0xff
						// encoding represents 256 exactly.
						return parameter >= 255.f ? 1.f : parameter / 256.f;
					};
					const float sParameter = (texel.a * 15.f * 16.f + texel.r * 15.f) / 255.f;
					const float rParameter = (texel.g * 15.f * 16.f + texel.b * 15.f) / 255.f;
					const float s = PI * .5f * angleFraction(sParameter);
					const float r = 2.f * PI * angleFraction(rParameter);
					const float q = 2.f * PI * angleFraction(offsetB * 255.f);
					texel = { 255, 255, 255, clampByte(std::clamp(offsetA + offsetR * std::sin(s)
							+ offsetG * std::cos(s) * std::cos(r - q), 0.f, 1.f) * 255.f) };
				}
				else if (fragmentState.ignoreTextureAlpha)
					texel.a = 255;
				auto applyTexture = [&](Color& color) {
					switch (fragmentState.shadingInstruction)
					{
					case 0: color = texel; break;
					case 1:
						color = { clampByte(color.r * texel.r / 255.f), clampByte(color.g * texel.g / 255.f),
							clampByte(color.b * texel.b / 255.f), texel.a };
						break;
					case 2:
						color = { clampByte((color.r * (255 - texel.a) + texel.r * texel.a) / 255.f),
							clampByte((color.g * (255 - texel.a) + texel.g * texel.a) / 255.f),
							clampByte((color.b * (255 - texel.a) + texel.b * texel.a) / 255.f), color.a };
						break;
					case 3:
						color = { clampByte(color.r * texel.r / 255.f), clampByte(color.g * texel.g / 255.f),
							clampByte(color.b * texel.b / 255.f), clampByte(color.a * texel.a / 255.f) };
						break;
					}
				};
				applyTexture(source);
				if (needsIntensitySource)
					applyTexture(intensitySource);
				if (fragmentState.useOffsetColor && !fragmentState.bumpMap)
				{
					const Color offsetColor { clampByte(interpolate(true, &Color::r, &WeightedColor::r)),
						clampByte(interpolate(true, &Color::g, &WeightedColor::g)),
						clampByte(interpolate(true, &Color::b, &WeightedColor::b)), 0 };
					source.r = clampByte(source.r + offsetColor.r);
					source.g = clampByte(source.g + offsetColor.g);
					source.b = clampByte(source.b + offsetColor.b);
					if (needsIntensitySource)
					{
						Color intensityOffset = offsetColor;
						scaleIntensityRgb(intensityOffset);
						intensitySource.r = clampByte(intensitySource.r + intensityOffset.r);
						intensitySource.g = clampByte(intensitySource.g + intensityOffset.g);
						intensitySource.b = clampByte(intensitySource.b + intensityOffset.b);
					}
				}
			}
			if (!state.deferBlend && !area1 && fragmentState.intensityShadow
					&& state.area1 == nullptr
					&& (stencilBuffer_[offset] & 0x81) == 0x81)
				source = intensitySource;
			auto finishColor = [&](Color& color) {
				if (fragmentState.colorClamp)
				{
					color.r = std::clamp(color.r, fragmentState.clampMin.r, fragmentState.clampMax.r);
					color.g = std::clamp(color.g, fragmentState.clampMin.g, fragmentState.clampMax.g);
					color.b = std::clamp(color.b, fragmentState.clampMin.b, fragmentState.clampMax.b);
					color.a = std::clamp(color.a, fragmentState.clampMin.a, fragmentState.clampMax.a);
				}
			if (fragmentState.fogMode == 0)
			{
				color.r = clampByte(color.r + (fragmentState.fogRamColor.r - color.r) * fog);
				color.g = clampByte(color.g + (fragmentState.fogRamColor.g - color.g) * fog);
				color.b = clampByte(color.b + (fragmentState.fogRamColor.b - color.b) * fog);
			}
			else if (fragmentState.fogMode == 1 && fragmentState.useOffsetColor
					&& !fragmentState.bumpMap)
			{
				const float vertexFog = interpolate(true, &Color::a, &WeightedColor::a) / 255.f;
				color.r = clampByte(color.r + (fragmentState.fogVertexColor.r - color.r) * vertexFog);
				color.g = clampByte(color.g + (fragmentState.fogVertexColor.g - color.g) * vertexFog);
				color.b = clampByte(color.b + (fragmentState.fogVertexColor.b - color.b) * vertexFog);
			}
			if (fragmentState.trilinearFilter)
			{
				const float fraction = pvrD - std::floor(pvrD);
				const float weight = fragmentState.trilinearPassB ? fraction : 1.f - fraction;
				color.r = clampByte(color.r * weight);
				color.g = clampByte(color.g * weight);
				color.b = clampByte(color.b * weight);
				color.a = clampByte(color.a * weight);
			}
			};
			finishColor(source);
			if (needsIntensitySource)
				finishColor(intensitySource);
			if (fragmentState.alphaTest && source.a < fragmentState.alphaReference)
				continue;
			if (fragmentState.alphaTest)
				source.a = 255;
			if (state.recordVisibility)
				visibilityBuffer_[offset] = state.sequence;
			if (state.colorWrite && fragmentState.dither && !state.deferBlend)
			{
				static constexpr u8 ditherTable[16] {
					5, 13, 7, 15, 9, 1, 11, 3, 6, 14, 4, 12, 10, 2, 8, 0
				};
				const u8 dither = ditherTable[(y & 3) * 4 + (x & 3)];
				source.r = static_cast<u8>(std::min(255, source.r + dither / fragmentState.ditherDivisorR));
				source.g = static_cast<u8>(std::min(255, source.g + dither / fragmentState.ditherDivisorG));
				source.b = static_cast<u8>(std::min(255, source.b + dither / fragmentState.ditherDivisorB));
				source.a = static_cast<u8>(std::min(255, source.a + 1 / fragmentState.ditherDivisorA));
			}
			if (state.colorWrite)
			{
				if (state.deferBlend)
				{
					if (state.deferArea1)
					{
						auto& fragments = deferredFragments_[offset];
						fragments.forEachReverseUntil([&](Fragment& fragment) {
							if (fragment.sequence == state.sequence && fragment.depth == z)
							{
								fragment.hasArea1 = true;
								fragment.area1Color = source;
								fragment.area1SrcBlend = fragmentState.srcBlend;
								fragment.area1DstBlend = fragmentState.dstBlend;
								fragment.area1SrcSelect = fragmentState.srcSelect;
								fragment.area1DstSelect = fragmentState.dstSelect;
								return true;
							}
							return false;
						});
					}
					else
					{
						Fragment fragment {};
						fragment.depth = z;
						fragment.sequence = state.sequence;
						fragment.color = source;
						fragment.intensityColor = intensitySource;
						fragment.srcBlend = fragmentState.srcBlend;
						fragment.dstBlend = fragmentState.dstBlend;
						fragment.srcSelect = fragmentState.srcSelect;
						fragment.dstSelect = fragmentState.dstSelect;
						fragment.dither = fragmentState.dither;
						fragment.ditherDivisorR = fragmentState.ditherDivisorR;
						fragment.ditherDivisorG = fragmentState.ditherDivisorG;
						fragment.ditherDivisorB = fragmentState.ditherDivisorB;
						fragment.ditherDivisorA = fragmentState.ditherDivisorA;
						fragment.shadowEnabled = state.shadow;
						fragment.intensityShadow = fragmentState.intensityShadow;
						deferredFragments_[offset].push_back(fragment);
					}
				}
				else
				{
					const Color blendSource = fragmentState.srcSelect ? secondaryBuffer_[offset] : source;
					const Color blendDestination = fragmentState.dstSelect ? secondaryBuffer_[offset] : colorBuffer_[offset];
					const Color result = blend(blendSource, blendDestination, fragmentState.srcBlend, fragmentState.dstBlend);
					if (fragmentState.dstSelect)
						secondaryBuffer_[offset] = result;
					else
						colorBuffer_[offset] = result;
				}
			}
			if (!state.deferArea1)
				stencilBuffer_[offset] = (stencilBuffer_[offset] & 0x7f) | (state.shadow ? 0x80 : 0);
			if (!state.deferArea1 && state.depthWrite)
				depthBuffer_[offset] = z;
		}
	};
#ifdef TARGET_MAC
	const size_t rowCount = static_cast<size_t>(maxY - minY + 1);
	if (rowCount >= 16)
	{
		struct RowContext
		{
			decltype(rasterizeRow) *rasterize;
			s32 firstRow;
		} rowContext { &rasterizeRow, minY };
		dispatch_apply_f(rowCount,
				dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0),
				&rowContext, [](void *opaque, size_t row) {
					auto& context = *static_cast<RowContext *>(opaque);
					(*context.rasterize)(context.firstRow + static_cast<s32>(row));
				});
	}
	else
#endif
	{
		for (s32 y = minY; y <= maxY; ++y)
			rasterizeRow(y);
	}
}

void SoftwareRasterizer::drawModifierTriangle(const Vertex& in0, const Vertex& in1, const Vertex& in2,
		ModifierMode mode, const PipelineState& state, bool deferred)
{
	if (width_ == 0 || height_ == 0)
		return;
	if (!hasFinitePosition(in0) || !hasFinitePosition(in1) || !hasFinitePosition(in2))
		return;
	Vertex vertices[3] { in0, in1, in2 };
	Point points[3] {
		{ toFixed(in0.x), toFixed(in0.y) },
		{ toFixed(in1.x), toFixed(in1.y) },
		{ toFixed(in2.x), toFixed(in2.y) },
	};
	s64 area = edge(points[0], points[1], points[2].x, points[2].y);
	if (area == 0)
		return;
	if (area < 0)
	{
		std::swap(points[1], points[2]);
		std::swap(vertices[1], vertices[2]);
		area = -area;
	}
	const s32 sampleOffset = state.fpuPixelCenter ? CENTER_SAMPLE_OFFSET : 0;
	s32 minX = std::max(0, static_cast<s32>(std::ceil((std::min({ points[0].x, points[1].x, points[2].x }) - sampleOffset) / static_cast<float>(SUBPIXEL_SCALE))));
	s32 minY = std::max(0, static_cast<s32>(std::ceil((std::min({ points[0].y, points[1].y, points[2].y }) - sampleOffset) / static_cast<float>(SUBPIXEL_SCALE))));
	const s32 targetMaxX = static_cast<s32>(std::min<u32>(width_ - 1, MAX_FIXED_COORDINATE / SUBPIXEL_SCALE));
	const s32 targetMaxY = static_cast<s32>(std::min<u32>(height_ - 1, MAX_FIXED_COORDINATE / SUBPIXEL_SCALE));
	s32 maxX = std::min(targetMaxX, (std::max({ points[0].x, points[1].x, points[2].x }) - sampleOffset) / SUBPIXEL_SCALE);
	s32 maxY = std::min(targetMaxY, (std::max({ points[0].y, points[1].y, points[2].y }) - sampleOffset) / SUBPIXEL_SCALE);
	if (state.scissorEnabled)
	{
		minX = std::max(minX, state.scissorX0);
		minY = std::max(minY, state.scissorY0);
		maxX = std::min(maxX, state.scissorX1 - 1);
		maxY = std::min(maxY, state.scissorY1 - 1);
	}
	if (minX > maxX || minY > maxY)
		return;
	const bool topLeft0 = isTopLeft(points[1], points[2]);
	const bool topLeft1 = isTopLeft(points[2], points[0]);
	const bool topLeft2 = isTopLeft(points[0], points[1]);
	const double inverseArea = 1.0 / static_cast<double>(area);
	for (s32 y = minY; y <= maxY; ++y)
		for (s32 x = minX; x <= maxX; ++x)
		{
			if (state.excludeClip && x >= state.excludeX0 && x < state.excludeX1
					&& y >= state.excludeY0 && y < state.excludeY1)
				continue;
			const s32 sampleX = x * SUBPIXEL_SCALE + sampleOffset;
			const s32 sampleY = y * SUBPIXEL_SCALE + sampleOffset;
			const s64 w0 = edge(points[1], points[2], sampleX, sampleY);
			const s64 w1 = edge(points[2], points[0], sampleX, sampleY);
			const s64 w2 = edge(points[0], points[1], sampleX, sampleY);
			if (w0 < 0 || (w0 == 0 && !topLeft0) || w1 < 0 || (w1 == 0 && !topLeft1)
					|| w2 < 0 || (w2 == 0 && !topLeft2))
				continue;
			const size_t offset = static_cast<size_t>(y) * width_ + static_cast<u32>(x);
			if (state.coverageMask != nullptr && state.coverageMask[offset] == 0)
				continue;
			if (deferred)
			{
				const float z = static_cast<float>((vertices[0].z * w0 + vertices[1].z * w1
						+ vertices[2].z * w2) * inverseArea);
				deferredFragments_[offset].forEach([&](Fragment& fragment) {
					if (!fragment.shadowEnabled)
						return;
					if (mode == ModifierMode::Xor || mode == ModifierMode::Or)
					{
						if (z <= fragment.depth)
							return;
						if (mode == ModifierMode::Xor)
							fragment.modifier ^= 1;
						else
							fragment.modifier |= 1;
					}
					else if (mode == ModifierMode::Inclusion)
					{
						const u8 previous = fragment.modifier;
						fragment.modifier &= ~1;
						if ((previous & 3) == 1)
							fragment.modifier |= 2;
					}
					else
					{
						const u8 previous = fragment.modifier;
						fragment.modifier &= ~3;
						// Exclusion uses cumulative AND the inverted volume result:
						// retain area 1 only when it was already set (bit 1) and this
						// volume did not contain the fragment (bit 0 clear).
						if ((previous & 3) == 2)
							fragment.modifier |= 2;
					}
				});
				continue;
			}
			if (mode == ModifierMode::Xor || mode == ModifierMode::Or)
			{
				const float z = static_cast<float>((vertices[0].z * w0 + vertices[1].z * w1
						+ vertices[2].z * w2) * inverseArea);
				if (z <= depthBuffer_[offset])
					continue;
				if (mode == ModifierMode::Xor)
					stencilBuffer_[offset] ^= 2;
				else
					stencilBuffer_[offset] |= 2;
			}
			else if (mode == ModifierMode::Inclusion)
				stencilBuffer_[offset] = (stencilBuffer_[offset] & 0xfc)
						| ((stencilBuffer_[offset] & 3) >= 1 ? 1 : 0);
			else
				stencilBuffer_[offset] = (stencilBuffer_[offset] & 0xfc)
						// Bit 0 is the cumulative result and bit 1 is the current
						// volume intersection. Exclusion ANDs with its inverse.
						| ((stencilBuffer_[offset] & 3) == 1 ? 1 : 0);
		}
}

void SoftwareRasterizer::applyModifierShadow(u16 scale)
{
	for (size_t i = 0; i < stencilBuffer_.size(); ++i)
	{
		if ((stencilBuffer_[i] & 0x81) == 0x81)
		{
			colorBuffer_[i].r = static_cast<u8>((colorBuffer_[i].r * scale + 128) / 256);
			colorBuffer_[i].g = static_cast<u8>((colorBuffer_[i].g * scale + 128) / 256);
			colorBuffer_[i].b = static_cast<u8>((colorBuffer_[i].b * scale + 128) / 256);
		}
		stencilBuffer_[i] &= 0xfc;
	}
}

} // namespace rend::soft
