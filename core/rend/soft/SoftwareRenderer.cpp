#include "SoftwareRenderer.h"

#include "SoftwareRasterizer.h"
#include "cfg/option.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/TexCache.h"
#include "rend/transform_matrix.h"
#ifdef USE_OPENGL
#include "rend/gles/quad.h"
#endif

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>

namespace rend::soft {
namespace {

class SoftwareTexture final : public BaseTextureCacheData
{
public:
	SoftwareTexture(TSP tsp, TCW tcw, int area) : BaseTextureCacheData(tsp, tcw, area) {}
	SoftwareTexture(SoftwareTexture&& other)
		: BaseTextureCacheData(std::move(other)), pixels(std::move(other.pixels)),
		  uploadedWidth(other.uploadedWidth), uploadedHeight(other.uploadedHeight),
		  mipmapped(other.mipmapped) {}

	std::string GetId() override { return std::to_string(startAddress); }
	bool Force32BitTexture(TextureType) const override { return true; }
	void UploadToGPU(int uploadWidth, int uploadHeight, const u8 *source,
			bool mipmapsRequested, bool mipmapsIncluded) override
	{
		uploadedWidth = uploadWidth;
		uploadedHeight = uploadHeight;
		const size_t pixelCount = static_cast<size_t>(uploadWidth) * uploadHeight;
		// Native PVR mipmapped textures are square powers of two. Custom
		// replacements that do not preserve that invariant cannot use the PVR
		// smallest-first layout represented by TextureView.
		const bool canBuildMipChain = uploadWidth == uploadHeight && uploadWidth != 0
				&& (uploadWidth & (uploadWidth - 1)) == 0;
		mipmapped = mipmapsIncluded || (mipmapsRequested && canBuildMipChain);
		const size_t totalPixels = mipmapped && pixelCount > 1
				? pixelCount + (pixelCount - 1) / 3 : pixelCount;
		pixels.resize(totalPixels);
		if (mipmapsIncluded || !mipmapped)
		{
			std::memcpy(pixels.data(), source, totalPixels * sizeof(Color));
			return;
		}

		// PixelBuffer supplies included Dreamcast mipmaps smallest first. Match
		// that layout so makeTextureView can treat generated chains identically.
		pixels = SoftwareRasterizer::generateMipChain(
				reinterpret_cast<const Color *>(source), uploadWidth);
	}

	TextureView view() const
	{
		return SoftwareRasterizer::makeTextureView(pixels.data(), uploadedWidth,
				uploadedHeight, mipmapped);
	}
	std::vector<Color> pixels;
	u32 uploadedWidth = 0;
	u32 uploadedHeight = 0;
	bool mipmapped = false;
};

class SoftwareTextureCache final : public BaseTextureCache<SoftwareTexture> {};

Color convertColor(const u8 color[4])
{
	return { color[0], color[1], color[2], color[3] };
}

BlendFactor convertBlend(u32 factor)
{
	return static_cast<BlendFactor>(factor & 7);
}

class SoftwareRenderer final : public Renderer
{
public:
	bool Init() override
	{
		BaseTextureCacheData::SetDirectXColorOrder(false);
#ifdef USE_OPENGL
		// The software rasterizer still uses OpenGL to present its completed
		// framebuffer. Initialize the shared GL capability/shader state before
		// constructing the presentation quad, just as the GL renderers do.
		findGLVersion();
		presentTexture = glcache.GenTexture();
		glcache.BindTexture(GL_TEXTURE_2D, presentTexture);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_A, GL_ONE);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		quadDrawer = std::make_unique<GlQuadDrawer>();
#endif
		return true;
	}

	void Term() override
	{
#ifdef USE_OPENGL
		quadDrawer.reset();
		if (presentTexture != 0)
			glcache.DeleteTextures(1, &presentTexture);
		presentTexture = 0;
#endif
		textureCache.Clear();
		screenRasterizer.reset();
		framebufferRasterizer.reset();
		rttRasterizer.reset();
		displayRasterizer = nullptr;
		rasterizer = nullptr;
		rawFrameStore.clear();
		presentationBuffer.clear();
		presentationWidth = presentationHeight = 0;
		activeCoverageMask = nullptr;
		backgroundCoverageMask = nullptr;
		frameReady = false;
		rendContext = nullptr;
	}

	void Process(TA_context *ctx) override
	{
		rendContext = &ctx->rend;
		// Texture pointers are attached to polygon parameters during parsing.
		// Resetting the cache afterwards would leave those pointers dangling.
		if (resetTextureCache)
		{
			textureCache.Clear();
			resetTextureCache = false;
		}
		textureCache.CollectCleanup();
		ta_parse(ctx, false);
	}

	bool Render() override
	{
		if (rendContext == nullptr)
			return false;
		const u32 width = std::max(1u, rendContext->framebufferWidth);
		const u32 height = std::max(1u, rendContext->framebufferHeight);
		auto& targetRasterizer = rendContext->isRTT ? rttRasterizer : screenRasterizer;
		if (!targetRasterizer || targetRasterizer->width() != width
				|| targetRasterizer->height() != height)
			targetRasterizer = std::make_unique<SoftwareRasterizer>(width, height);
		rasterizer = targetRasterizer.get();
		if (!rendContext->isRTT)
			displayRasterizer = rasterizer;
		std::vector<Color> previousFrameStore;
		if (!rendContext->isRTT)
		{
			if (rawFrameStore.size() == rasterizer->colorBuffer().size())
				previousFrameStore = rawFrameStore;
			else
				previousFrameStore = rasterizer->colorBuffer();
		}
		if (rendContext->isRTT)
			rasterizer->clear({ 0, 0, 0, 0 }, 0.f);
		else if (rendContext->clearFramebuffer)
			rasterizer->clear({ static_cast<u8>(rendContext->vo_border_col.red() * 255.f),
				static_cast<u8>(rendContext->vo_border_col.green() * 255.f),
				static_cast<u8>(rendContext->vo_border_col.blue() * 255.f), 255 }, 0.f);
		else
			rasterizer->clearDepth(0.f);
		// The PVR secondary accumulation buffer starts clear for each render
		// invocation and persists while all lists/passes for the tile are run.
		rasterizer->clearSecondary();
		// The region header controls when each tile's accumulation buffer is
		// committed to external frame-store memory. Keep that visible image
		// separate from later, deliberately unflushed accumulation results.
		std::vector<Color> frameStore;
		if (!rendContext->isRTT
				&& previousFrameStore.size() == rasterizer->colorBuffer().size())
			frameStore = std::move(previousFrameStore);
		else
			frameStore = rasterizer->colorBuffer();
		matrices.CalcMatrices(rendContext, width, height);
		std::vector<std::vector<RegionInfo>> passRegions;
		passRegions.reserve(rendContext->render_passes.size());
		size_t tileCount = 0;
		for (const RenderPass& pass : rendContext->render_passes)
		{
			passRegions.push_back(getPassRegions(pass));
			tileCount = std::max(tileCount, passRegions.back().size());
		}
		auto renderRegions = [&](size_t passIndex, const std::vector<RegionInfo>& regions)
		{
			regionClipEnabled = !regions.empty() && !config::PerStripSorting;
			if (regionClipEnabled)
			{
				regionClipX0 = regions.front().x0;
				regionClipY0 = regions.front().y0;
				regionClipX1 = regions.front().x1;
				regionClipY1 = regions.front().y1;
				for (const RegionInfo& region : regions)
				{
					regionClipX0 = std::min(regionClipX0, region.x0);
					regionClipY0 = std::min(regionClipY0, region.y0);
					regionClipX1 = std::max(regionClipX1, region.x1);
					regionClipY1 = std::max(regionClipY1, region.y1);
				}
			}
			const RenderPass& pass = rendContext->render_passes[passIndex];
			const RenderPass previous = passIndex == 0
					? RenderPass {} : rendContext->render_passes[passIndex - 1];
			std::vector<u8> opaqueMask = makeRegionMask(regions,
					[](const RegionInfo& region) { return region.opaquePresent; });
			const std::vector<u8> allRegionMask = makeRegionMask(regions,
					[](const RegionInfo&) { return true; });
			std::vector<u8> punchThroughMask = makeRegionMask(regions,
					[](const RegionInfo& region) { return region.punchThroughPresent; });
			std::vector<u8> opaqueModifierMask = makeRegionMask(regions,
					[](const RegionInfo& region) { return region.opaqueModifierPresent; });
			if (config::PerStripSorting)
			{
				opaqueMask.clear();
				punchThroughMask.clear();
				opaqueModifierMask.clear();
			}
			clearPassDepth(pass, regions);
			backgroundCoverageMask = config::PerStripSorting ? nullptr : maskPointer(allRegionMask);
			const bool hasOpaqueModifiers = config::ModifierVolumes
					&& pass.mvo_count > previous.mvo_count;
			if (hasOpaqueModifiers)
			{
				const std::vector<float> previousDepth = rasterizer->depthBuffer();
				rasterizer->clearVisibility();
				activeCoverageMask = maskPointer(opaqueMask);
				drawList(rendContext->global_param_op, previous.op_count, pass.op_count,
						false, false, false, false, false, true, false);
				activeCoverageMask = maskPointer(punchThroughMask);
				drawList(rendContext->global_param_pt, previous.pt_count, pass.pt_count,
						true, false, false, false, false, true, false);
				activeCoverageMask = maskPointer(opaqueModifierMask);
				drawModifierVolumes(rendContext->global_param_mvo, previous.mvo_count, pass.mvo_count);
				rasterizer->restoreDepth(previousDepth);
				activeCoverageMask = maskPointer(opaqueMask);
				drawList(rendContext->global_param_op, previous.op_count, pass.op_count,
						false, false, true, false, false, false, true);
				activeCoverageMask = maskPointer(punchThroughMask);
				drawList(rendContext->global_param_pt, previous.pt_count, pass.pt_count,
						true, false, true, false, false, false, true);
			}
			else
			{
				activeCoverageMask = maskPointer(opaqueMask);
				drawList(rendContext->global_param_op, previous.op_count, pass.op_count,
						false, false, true);
				activeCoverageMask = maskPointer(punchThroughMask);
				drawList(rendContext->global_param_pt, previous.pt_count, pass.pt_count,
						true, false, true);
			}
			rasterizer->clearModifierResults();
			drawTranslucentPass(passIndex, previous, pass, regions);
			std::vector<RegionInfo> passCommittedRegions;
			for (const RegionInfo& region : regions)
				if (region.flushAccumulate)
				{
					rasterizer->flushColorRect(frameStore,
							region.x0, region.y0, region.x1, region.y1);
					passCommittedRegions.push_back(region);
				}
			if (regions.empty())
			{
				// With no usable region headers, preserve the full-target fallback
				// promised below. Previously the lists rendered, but no accumulation
				// pixels were ever committed to the local or VRAM frame store.
				RegionInfo fullTarget { 0, 0,
						static_cast<s32>(rasterizer->width()),
						static_cast<s32>(rasterizer->height()), false, true,
						pass.autosort, false, true, true, true, true, true };
				rasterizer->flushColorRect(frameStore, fullTarget.x0, fullTarget.y0,
						fullTarget.x1, fullTarget.y1);
				passCommittedRegions.push_back(fullTarget);
			}
			// Hardware commits tiles as region-array passes complete. Writing here,
			// rather than once after the whole render, lets a texture used by a
			// later pass observe framebuffer feedback through the VRAM cache locks.
			if (rendContext->isRTT)
				writeFramebufferToVram(frameStore, passCommittedRegions);
			else if (config::EmulateFramebuffer)
				writeDisplayFramebufferToVram(frameStore, passCommittedRegions);
			backgroundCoverageMask = nullptr;
			regionClipEnabled = false;
		};
		if (tileCount == 0)
		{
			// Preserve the old full-target fallback for a malformed or truncated
			// region array that contains none of the requested pass headers.
			for (size_t passIndex = 0; passIndex < rendContext->render_passes.size(); ++passIndex)
				renderRegions(passIndex, {});
		}
		else
		{
			// Region coverage makes tiles independent within each pass. Batch each
			// pass's regions so its primitive lists are transformed and traversed
			// once, while clear/flush rectangles and list-presence masks continue to
			// follow the individual region headers. Pass order and the VRAM commit
			// between passes remain observable to later framebuffer-texture reads.
			for (size_t passIndex = 0; passIndex < passRegions.size(); ++passIndex)
				renderRegions(passIndex, passRegions[passIndex]);
		}
		if (rendContext->isRTT)
		{
			rasterizer = screenRasterizer.get();
		}
		else if (config::EmulateFramebuffer)
		{
			rawFrameStore.clear();
			presentationBuffer.clear();
			presentationWidth = presentationHeight = 0;
		}
		else
		{
			rawFrameStore = std::move(frameStore);
			FramebufferInfo info;
			// Render jobs may execute after the emulated CPU has already begun
			// programming the following frame. Scan out using the register image
			// captured with this STARTRENDER, never a mixture of later live state.
			info.fb_r_size = rendContext->fb_R_SIZE;
			info.fb_r_ctrl = rendContext->fb_R_CTRL;
			info.spg_control = rendContext->spg_control;
			info.spg_status = rendContext->spg_status;
			info.fb_r_sof1 = rendContext->fb_R_SOF1;
			info.fb_r_sof2 = rendContext->fb_R_SOF2;
			info.vo_control = rendContext->vo_control;
			info.vo_border_col = rendContext->vo_border_col;
			int displayWidth = 0;
			int displayHeight = 0;
			getDCFramebufferReadSize(info, displayWidth, displayHeight);
			presentationWidth = std::max(1, displayWidth);
			presentationHeight = std::max(1, displayHeight);
			if (info.fb_r_ctrl.fb_enable == 0 || info.vo_control.blank_video == 1)
			{
				const Color border {
						static_cast<u8>(info.vo_border_col.red() * 255.f),
						static_cast<u8>(info.vo_border_col.green() * 255.f),
						static_cast<u8>(info.vo_border_col.blue() * 255.f), 255 };
				presentationBuffer.assign(static_cast<size_t>(presentationWidth)
						* presentationHeight, border);
			}
			else
			{
				PixelBuffer<u32> pixels;
				ReadFramebuffer(info, pixels, displayWidth, displayHeight);
				presentationWidth = std::max(1, displayWidth);
				presentationHeight = std::max(1, displayHeight);
				presentationBuffer.resize(static_cast<size_t>(presentationWidth)
						* presentationHeight);
				for (u32 y = 0; y < presentationHeight; ++y)
					for (u32 x = 0; x < presentationWidth; ++x)
					{
						const u32 value = *pixels.data(x, y);
						presentationBuffer[static_cast<size_t>(y) * presentationWidth + x] = {
								static_cast<u8>(value), static_cast<u8>(value >> 8),
								static_cast<u8>(value >> 16), static_cast<u8>(value >> 24) };
					}
			}
		}
		activeCoverageMask = nullptr;
		if (!rendContext->isRTT)
		{
			frameReady = true;
			frameAspectRatio = getOutputFramebufferAspectRatio();
			clearLastFrame = false;
		}
		return !rendContext->isRTT;
	}

	void RenderFramebuffer(const FramebufferInfo& info) override
	{
		rawFrameStore.clear();
		presentationBuffer.clear();
		presentationWidth = presentationHeight = 0;
		int width = 0;
		int height = 0;
		if (info.fb_r_ctrl.fb_enable == 0 || info.vo_control.blank_video == 1)
		{
			getDCFramebufferReadSize(info, width, height);
			const u32 rasterWidth = std::max(1, width);
			const u32 rasterHeight = std::max(1, height);
			if (!framebufferRasterizer || framebufferRasterizer->width() != rasterWidth
					|| framebufferRasterizer->height() != rasterHeight)
				framebufferRasterizer = std::make_unique<SoftwareRasterizer>(rasterWidth, rasterHeight);
			rasterizer = framebufferRasterizer.get();
			displayRasterizer = rasterizer;
			rasterizer->clear({ static_cast<u8>(info.vo_border_col.red() * 255.f),
				static_cast<u8>(info.vo_border_col.green() * 255.f),
				static_cast<u8>(info.vo_border_col.blue() * 255.f), 255 });
			frameReady = true;
			clearLastFrame = false;
			frameAspectRatio = getDCFramebufferAspectRatio();
			rendContext = nullptr;
			return;
		}
		ReadFramebuffer(info, framebufferPixels, width, height);
		if (!framebufferRasterizer || framebufferRasterizer->width() != static_cast<u32>(width)
				|| framebufferRasterizer->height() != static_cast<u32>(height))
			framebufferRasterizer = std::make_unique<SoftwareRasterizer>(width, height);
		rasterizer = framebufferRasterizer.get();
		displayRasterizer = rasterizer;
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				const u32 value = *framebufferPixels.data(x, y);
				rasterizer->setPixel(x, y, {
					static_cast<u8>(value), static_cast<u8>(value >> 8),
					static_cast<u8>(value >> 16), static_cast<u8>(value >> 24) });
			}
		frameReady = true;
		clearLastFrame = false;
		frameAspectRatio = getDCFramebufferAspectRatio();
		rendContext = nullptr;
	}

	bool RenderLastFrame() override
	{
		if (!displayRasterizer || clearLastFrame)
			return false;
		return drawFrame();
	}

	bool Present() override
	{
		if (!frameReady)
			return false;
		frameReady = false;
	#ifndef LIBRETRO
		imguiDriver->setFrameRendered();
	#endif
		return true;
	}

	bool drawFrame()
	{
		if (!displayRasterizer)
			return false;
	#ifdef USE_OPENGL
	#ifndef TARGET_IPHONE
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
	#endif
		const bool prepared = !presentationBuffer.empty();
		const auto& pixels = prepared
				? presentationBuffer : displayRasterizer->colorBuffer();
		const u32 pixelWidth = prepared ? presentationWidth : displayRasterizer->width();
		const u32 pixelHeight = prepared ? presentationHeight : displayRasterizer->height();
		glActiveTexture(GL_TEXTURE0);
		glcache.BindTexture(GL_TEXTURE_2D, presentTexture);
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pixelWidth, pixelHeight, 0,
				GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
		const GLint filter = config::LinearInterpolation ? GL_LINEAR : GL_NEAREST;
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
		glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		glViewport(0, 0, settings.display.width, settings.display.height);
		glcache.ClearColor(VO_BORDER_COL.red(), VO_BORDER_COL.green(), VO_BORDER_COL.blue(), 1.f);
		glClear(GL_COLOR_BUFFER_BIT);
		int dx = 0;
		int dy = 0;
		getWindowboxDimensions(settings.display.width, settings.display.height,
				frameAspectRatio, dx, dy, config::Rotate90);
		glViewport(dx, dy, settings.display.width - dx * 2, settings.display.height - dy * 2);
		glcache.Disable(GL_BLEND);
		glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
		quadDrawer->draw(presentTexture, config::Rotate90, true);
		glCheck();
#endif
		return true;
	}

	bool GetLastFrame(std::vector<u8>& data, int& width, int& height) override
	{
		if (!displayRasterizer || clearLastFrame)
			return false;
		const bool prepared = !presentationBuffer.empty();
		const int sourceWidth = prepared ? presentationWidth : displayRasterizer->width();
		const int sourceHeight = prepared ? presentationHeight : displayRasterizer->height();
		const int orientedWidth = config::Rotate90 ? sourceHeight : sourceWidth;
		const int orientedHeight = config::Rotate90 ? sourceWidth : sourceHeight;
		const float aspectRatio = frameAspectRatio;
		if (width == 0 && height == 0)
		{
			width = orientedWidth;
			height = orientedHeight;
			const int squarePixelWidth = aspectRatio * height;
			if (width > squarePixelWidth)
				height = width / aspectRatio;
			else
				width = squarePixelWidth;
		}
		else if (width == 0)
			width = std::max(1, static_cast<int>(height * aspectRatio));
		else if (height == 0)
			height = std::max(1, static_cast<int>(width / aspectRatio));
		data.resize(static_cast<size_t>(width) * height * 3);
		const auto& source = prepared ? presentationBuffer : displayRasterizer->colorBuffer();
		for (int y = 0; y < height; ++y)
			for (int x = 0; x < width; ++x)
			{
				const int orientedX = x * orientedWidth / width;
				const int orientedY = y * orientedHeight / height;
				const int sourceX = config::Rotate90 ? orientedY : orientedX;
				const int sourceY = config::Rotate90 ? sourceHeight - 1 - orientedX : orientedY;
				const Color color = source[static_cast<size_t>(sourceY) * sourceWidth + sourceX];
				u8 *destination = &data[(static_cast<size_t>(y) * width + x) * 3];
				destination[0] = color.r;
				destination[1] = color.g;
				destination[2] = color.b;
			}
		return true;
	}

	BaseTextureCacheData *GetTexture(TSP tsp, TCW tcw, int area) override
	{
		SoftwareTexture *texture = textureCache.getTextureCacheData(tsp, tcw, area);
		if (texture->NeedsUpdate())
			texture->Update();
		return texture;
	}

private:
	struct RegionInfo
	{
		s32 x0;
		s32 y0;
		s32 x1;
		s32 y1;
		bool zClear;
		bool flushAccumulate;
		bool autosort;
		bool modifierListsShared;
		bool opaquePresent;
		bool opaqueModifierPresent;
		bool translucentPresent;
		bool translucentModifierPresent;
		bool punchThroughPresent;
	};

	std::vector<RegionInfo> getPassRegions(const RenderPass& pass) const
	{
		std::vector<RegionInfo> regions;
		if (rendContext->region_array.empty())
			return regions;
		const bool type1 = ((rendContext->fpu_param_cfg >> 21) & 1) == 0;
		// Region headers are interleaved by physical TA pass for every tile.
		// The parser deliberately omits empty continuation passes, so the
		// compacted render_passes.size() is not a valid interleave stride.
		RegionArrayTile firstTile;
		firstTile.full = rendContext->region_array[0].words[0];
		size_t passCount = 1;
		while (passCount < MAX_PASSES
				&& passCount < rendContext->region_array.size() && !firstTile.LastRegion)
		{
			RegionArrayTile candidate;
			candidate.full = rendContext->region_array[passCount].words[0];
			if (candidate.X != firstTile.X || candidate.Y != firstTile.Y)
				break;
			++passCount;
			if (candidate.LastRegion)
				break;
		}
		// A retained render pass is authoritative evidence that its physical
		// region-array slot exists. Keep the coordinate scan for omitted trailing
		// passes, but never let malformed or unusual first-tile headers infer a
		// stride too small to address a pass that the TA parser actually decoded.
		for (const RenderPass& renderPass : rendContext->render_passes)
			passCount = std::max(passCount,
					static_cast<size_t>(renderPass.region_array_pass) + 1);
		if (pass.region_array_pass >= passCount)
			return regions;
		const glm::mat4& viewport = matrices.GetViewportMatrix();
		for (size_t header = 0; header < rendContext->region_array.size(); ++header)
		{
			const RegionArrayEntry& entry = rendContext->region_array[header];
			RegionArrayTile tile;
			tile.full = entry.words[0];
			if (header % passCount == pass.region_array_pass)
			{
				const glm::vec4 first = viewport * glm::vec4(
						tile.X * 32.f, tile.Y * 32.f, 0.f, 1.f);
				const glm::vec4 last = viewport * glm::vec4(
						(tile.X + 1) * 32.f, (tile.Y + 1) * 32.f, 0.f, 1.f);
				regions.push_back({
					static_cast<s32>(std::lround(std::min(first.x, last.x))),
					static_cast<s32>(std::lround(std::min(first.y, last.y))),
					static_cast<s32>(std::lround(std::max(first.x, last.x))),
					static_cast<s32>(std::lround(std::max(first.y, last.y))),
					!tile.NoZClear,
					!tile.FlushAccumulate,
					type1 ? (rendContext->isp_feed_cfg & 1) == 0 : !tile.PreSort,
					entry.words[2] == entry.words[4],
					(entry.words[1] & 0x80000000) == 0,
					(entry.words[2] & 0x80000000) == 0,
					(entry.words[3] & 0x80000000) == 0,
					(entry.words[4] & 0x80000000) == 0,
					!type1 && (entry.words[5] & 0x80000000) == 0,
				});
			}
			if (tile.LastRegion)
				break;
		}
		return regions;
	}

	void fillRegionMask(std::vector<u8>& mask, const RegionInfo& region, u8 value) const
	{
		const s32 x0 = std::clamp<s32>(region.x0, 0, rasterizer->width());
		const s32 y0 = std::clamp<s32>(region.y0, 0, rasterizer->height());
		const s32 x1 = std::clamp<s32>(region.x1, 0, rasterizer->width());
		const s32 y1 = std::clamp<s32>(region.y1, 0, rasterizer->height());
		for (s32 y = y0; y < y1; ++y)
			std::fill(mask.begin() + static_cast<size_t>(y) * rasterizer->width() + x0,
					mask.begin() + static_cast<size_t>(y) * rasterizer->width() + x1, value);
	}

	template<typename Predicate>
	std::vector<u8> makeRegionMask(const std::vector<RegionInfo>& regions,
			Predicate included) const
	{
		if (regions.empty())
			return {};
		std::vector<u8> mask(static_cast<size_t>(rasterizer->width())
				* rasterizer->height());
		for (const RegionInfo& region : regions)
			if (included(region))
				fillRegionMask(mask, region, 1);
		return mask;
	}

	const std::vector<u8> *maskPointer(const std::vector<u8>& mask) const
	{
		return mask.empty() ? nullptr : &mask;
	}

	void clearPassDepth(const RenderPass& pass, const std::vector<RegionInfo>& regions)
	{
		for (const RegionInfo& region : regions)
			if (region.zClear)
			{
				rasterizer->clearDepthRect(region.x0, region.y0, region.x1, region.y1, 0.f);
			}
		// Preserve the parser's first-tile behavior for malformed or truncated
		// region arrays where the requested pass header could not be found.
		if (regions.empty() && pass.z_clear)
			rasterizer->clearDepth(0.f);
	}

	void drawAutosortedTranslucent(size_t passIndex, const RenderPass& previous,
			const RenderPass& pass, const std::vector<RegionInfo>& regions,
			const std::vector<u8> *coverageMask)
	{
		activeCoverageMask = coverageMask;
		rasterizer->clearDeferredFragments();
		drawList(rendContext->global_param_tr, previous.tr_count, pass.tr_count,
				false, true, true, true);
		if (config::ModifierVolumes)
		{
			if (!regions.empty())
			{
				const std::vector<u8> sharedMask = makeRegionMask(regions,
						[](const RegionInfo& region) {
							return region.autosort && region.translucentPresent
									&& region.modifierListsShared
									&& region.opaqueModifierPresent;
						});
				const std::vector<u8> separateMask = makeRegionMask(regions,
						[](const RegionInfo& region) {
							return region.autosort && region.translucentPresent
									&& !region.modifierListsShared
									&& region.translucentModifierPresent;
						});
				activeCoverageMask = &sharedMask;
				drawModifierVolumes(rendContext->global_param_mvo, previous.mvo_count,
						pass.mvo_count, true);
				activeCoverageMask = &separateMask;
				drawModifierVolumes(rendContext->global_param_mvo_tr, previous.mvo_tr_count,
						pass.mvo_tr_count, true);
			}
			else if (pass.mv_op_tr_shared)
			{
				activeCoverageMask = coverageMask;
				drawModifierVolumes(rendContext->global_param_mvo, previous.mvo_count,
						pass.mvo_count, true);
			}
			else
			{
				activeCoverageMask = coverageMask;
				drawModifierVolumes(rendContext->global_param_mvo_tr, previous.mvo_tr_count,
						pass.mvo_tr_count, true);
			}
		}
		rasterizer->resolveDeferredFragments();
		activeCoverageMask = coverageMask;
		if (passIndex + 1 < rendContext->render_passes.size())
			drawList(rendContext->global_param_tr, previous.tr_count, pass.tr_count,
					false, true, false, false, true);
	}

	void drawTranslucentPass(size_t passIndex, const RenderPass& previous,
			const RenderPass& pass, const std::vector<RegionInfo>& regions)
	{
		if (config::PerStripSorting)
		{
			activeCoverageMask = nullptr;
			drawList(rendContext->global_param_tr, previous.tr_count, pass.tr_count,
					false, false, true);
			activeCoverageMask = nullptr;
			return;
		}
		const bool hasAutosort = std::any_of(regions.begin(), regions.end(),
				[](const RegionInfo& region) {
					return region.translucentPresent && region.autosort;
				});
		const bool hasPresort = std::any_of(regions.begin(), regions.end(),
				[](const RegionInfo& region) {
					return region.translucentPresent && !region.autosort;
				});
		const std::vector<u8> translucentMask = makeRegionMask(regions,
				[](const RegionInfo& region) { return region.translucentPresent; });
		if (regions.empty() || !hasAutosort || !hasPresort)
		{
			if (regions.empty() ? pass.autosort : hasAutosort)
				drawAutosortedTranslucent(passIndex, previous, pass, regions,
						maskPointer(translucentMask));
			else
			{
				activeCoverageMask = maskPointer(translucentMask);
				drawList(rendContext->global_param_tr, previous.tr_count, pass.tr_count,
						false, false, true);
			}
			activeCoverageMask = nullptr;
			return;
		}

		std::vector<u8> autosortMask(static_cast<size_t>(rasterizer->width())
				* rasterizer->height());
		std::vector<u8> presortMask(autosortMask.size());
		for (const RegionInfo& region : regions)
			if (region.translucentPresent)
				fillRegionMask(region.autosort ? autosortMask : presortMask, region, 1);
		drawAutosortedTranslucent(passIndex, previous, pass, regions, &autosortMask);
		activeCoverageMask = &presortMask;
		drawList(rendContext->global_param_tr, previous.tr_count, pass.tr_count,
				false, false, true);
		activeCoverageMask = nullptr;
	}

	Rect scaleRegionClip(const RegionInfo& region, u32 sourceWidth, u32 sourceHeight,
			u32 outputWidth, u32 outputHeight, const Rect& outputClip) const
	{
		const s32 x0 = static_cast<s32>(std::floor(
				static_cast<double>(region.x0) * outputWidth / sourceWidth));
		const s32 y0 = static_cast<s32>(std::floor(
				static_cast<double>(region.y0) * outputHeight / sourceHeight));
		const s32 x1 = static_cast<s32>(std::ceil(
				static_cast<double>(region.x1) * outputWidth / sourceWidth));
		const s32 y1 = static_cast<s32>(std::ceil(
				static_cast<double>(region.y1) * outputHeight / sourceHeight));
		const s32 clipX0 = std::max(x0, outputClip.origin.x);
		const s32 clipY0 = std::max(y0, outputClip.origin.y);
		const s32 clipX1 = std::min(x1, outputClip.origin.x + outputClip.size.x);
		const s32 clipY1 = std::min(y1, outputClip.origin.y + outputClip.size.y);
		return { { clipX0, clipY0 },
			{ std::max(0, clipX1 - clipX0), std::max(0, clipY1 - clipY0) } };
	}

	void writeFramebufferToVram(const std::vector<Color>& source,
			const std::vector<RegionInfo>& committedRegions)
	{
		const u32 sourceWidth = rasterizer->width();
		const u32 sourceHeight = rasterizer->height();
		if (sourceWidth == 0 || sourceHeight == 0)
			return;
		glm::ivec2 scaledSize;
		Rect finalClip;
		getWriteFBToVramParams(*rendContext, scaledSize, finalClip);
		if (scaledSize.x <= 0 || scaledSize.y <= 0)
			return;
		std::vector<Color> filtered;
		std::vector<Color> field;
		std::vector<Color> horizontal;
		std::vector<Color> scaled;
		std::vector<Color> ditheredOutput;
		const Color *output = source.data();
		const std::vector<Color> *scaleSource = &source;
		u32 scaleSourceWidth = sourceWidth;
		u32 scaleSourceHeight = sourceHeight;
		if (rendContext->scaler_ctl.vscalefactor > 1024
				&& scaledSize.y < static_cast<int>(sourceHeight))
		{
			filtered = SoftwareRasterizer::filterVertical(source, sourceWidth, sourceHeight,
					static_cast<u8>(rendContext->y_coeff),
					static_cast<u8>(rendContext->y_coeff >> 8));
			scaleSource = &filtered;
			output = filtered.data();
		}
		if (rendContext->scaler_ctl.interlace
				&& rendContext->scaler_ctl.vscalefactor == 2048)
		{
			field = SoftwareRasterizer::selectInterlacedField(*scaleSource,
					sourceWidth, sourceHeight, scaledSize.y,
					rendContext->scaler_ctl.fieldselect != 0);
			scaleSource = &field;
			scaleSourceHeight = scaledSize.y;
			output = field.data();
		}
		if (rendContext->scaler_ctl.hscale && sourceWidth >= 2)
		{
			horizontal = SoftwareRasterizer::scaleHorizontalHalf(*scaleSource,
					sourceWidth, scaleSourceHeight);
			scaleSource = &horizontal;
			scaleSourceWidth = sourceWidth / 2;
			output = horizontal.data();
		}
		if (scaledSize.x != static_cast<int>(scaleSourceWidth)
				|| scaledSize.y != static_cast<int>(scaleSourceHeight))
		{
			scaled = SoftwareRasterizer::scaleLinear(*scaleSource, scaleSourceWidth, scaleSourceHeight,
					scaledSize.x, scaledSize.y);
			output = scaled.data();
		}
		const bool dithered = rendContext->fb_W_CTRL.fb_dither
				&& rendContext->fb_W_CTRL.fb_packmode <= 3;
		if (dithered)
		{
			ditheredOutput.assign(output,
					output + static_cast<size_t>(scaledSize.x) * scaledSize.y);
			SoftwareRasterizer::ditherFramebuffer(ditheredOutput, scaledSize.x, scaledSize.y,
					rendContext->fb_W_CTRL.fb_packmode);
			output = ditheredOutput.data();
		}
		u16 *destination = reinterpret_cast<u16 *>(
				&vram[rendContext->fb_W_SOF1 & VRAM_MASK]);
		for (const RegionInfo& region : committedRegions)
		{
			const Rect regionClip = scaleRegionClip(region, sourceWidth, sourceHeight,
					scaledSize.x, scaledSize.y, finalClip);
			if (regionClip.size.x == 0 || regionClip.size.y == 0)
				continue;
			WriteTextureToVRam<0, 1, 2, 3>(scaledSize.x, scaledSize.y,
					reinterpret_cast<const u8 *>(output), destination,
					rendContext->fb_W_CTRL, rendContext->fb_W_LINESTRIDE * 8,
					regionClip,
					// The PVR truncates channels when packing its framebuffer. Dithered
					// pixels have already had the matrix added, but undithered output
					// must use the same truncating quantizer rather than host rounding.
					true);
		}
	}

	void writeDisplayFramebufferToVram(const std::vector<Color>& source,
			const std::vector<RegionInfo>& committedRegions)
	{
		glm::ivec2 scaledSize;
		Rect finalClip;
		getWriteFBToVramParams(*rendContext, scaledSize, finalClip);
		if (scaledSize.x <= 0 || scaledSize.y <= 0)
			return;
		const u32 sourceWidth = rasterizer->width();
		const u32 sourceHeight = rasterizer->height();
		std::vector<Color> filtered;
		std::vector<Color> field;
		std::vector<Color> horizontal;
		std::vector<Color> scaled;
		const Color *output = source.data();
		const std::vector<Color> *scaleSource = &source;
		u32 scaleSourceWidth = sourceWidth;
		u32 scaleSourceHeight = sourceHeight;
		if (rendContext->scaler_ctl.vscalefactor > 1024
				&& scaledSize.y < static_cast<int>(sourceHeight))
		{
			filtered = SoftwareRasterizer::filterVertical(source, sourceWidth, sourceHeight,
					static_cast<u8>(rendContext->y_coeff),
					static_cast<u8>(rendContext->y_coeff >> 8));
			scaleSource = &filtered;
			output = filtered.data();
		}
		if (rendContext->scaler_ctl.interlace
				&& rendContext->scaler_ctl.vscalefactor == 2048)
		{
			field = SoftwareRasterizer::selectInterlacedField(*scaleSource,
					sourceWidth, sourceHeight, scaledSize.y,
					rendContext->scaler_ctl.fieldselect != 0);
			scaleSource = &field;
			scaleSourceHeight = scaledSize.y;
			output = field.data();
		}
		if (rendContext->scaler_ctl.hscale && sourceWidth >= 2)
		{
			horizontal = SoftwareRasterizer::scaleHorizontalHalf(*scaleSource,
					sourceWidth, scaleSourceHeight);
			scaleSource = &horizontal;
			scaleSourceWidth = sourceWidth / 2;
			output = horizontal.data();
		}
		if (scaledSize.x != static_cast<int>(scaleSourceWidth)
				|| scaledSize.y != static_cast<int>(scaleSourceHeight))
		{
			scaled = SoftwareRasterizer::scaleLinear(*scaleSource, scaleSourceWidth, scaleSourceHeight,
					scaledSize.x, scaledSize.y);
			output = scaled.data();
		}
		if (rendContext->fb_W_CTRL.fb_dither
				&& rendContext->fb_W_CTRL.fb_packmode <= 3)
		{
			if (scaled.empty())
				scaled.assign(output,
						output + static_cast<size_t>(scaledSize.x) * scaledSize.y);
			SoftwareRasterizer::ditherFramebuffer(scaled, scaledSize.x, scaledSize.y,
					rendContext->fb_W_CTRL.fb_packmode);
			output = scaled.data();
		}
		for (const RegionInfo& region : committedRegions)
		{
			const Rect regionClip = scaleRegionClip(region, sourceWidth, sourceHeight,
					scaledSize.x, scaledSize.y, finalClip);
			if (regionClip.size.x == 0 || regionClip.size.y == 0)
				continue;
			WriteFramebuffer<0, 1, 2, 3>(scaledSize.x, scaledSize.y,
					reinterpret_cast<const u8 *>(output), rendContext->fb_W_SOF1 & VRAM_MASK,
					rendContext->fb_W_CTRL, rendContext->fb_W_LINESTRIDE * 8, regionClip);
		}
	}

	TextureView currentTexture(BaseTextureCacheData *textureData)
	{
		if (textureData == nullptr)
			return {};
		auto *texture = static_cast<SoftwareTexture *>(textureData);
		if (texture->NeedsUpdate())
			texture->Update();
		return texture->view();
	}

	void configureState(const PolyParam& poly, PipelineState& state, bool punchThrough, bool sorting)
	{
		state.coverageMask = activeCoverageMask == nullptr ? nullptr : activeCoverageMask->data();
		state.depthMode = SoftwareRasterizer::selectDepthMode(poly.isp.DepthMode,
				punchThrough, sorting);
		state.depthWrite = punchThrough || (!sorting && !poly.isp.ZWriteDis);
		state.srcBlend = convertBlend(poly.tsp.SrcInstr);
		state.dstBlend = convertBlend(poly.tsp.DstInstr);
		if (punchThrough)
		{
			// Punch Through pixels are opaque writes. TSP blend and secondary
			// accumulation selections are not part of this list's processing.
			state.srcBlend = BlendFactor::One;
			state.dstBlend = BlendFactor::Zero;
		}
		state.gouraud = poly.pcw.Gouraud;
		state.useAlpha = poly.tsp.UseAlpha;
		state.ignoreTextureAlpha = poly.tsp.IgnoreTexA || poly.tcw.PixelFmt == Pixel565;
		state.useOffsetColor = poly.pcw.Offset;
		state.alphaTest = punchThrough;
		state.alphaReference = rendContext->pt_alpha_ref & 0xff;
		state.shadingInstruction = poly.tsp.ShadInstr;
		if (poly.pcw.Texture)
			state.texture = currentTexture(poly.texture);
		const bool hasMipChain = state.texture.mipLevelCount > 1;
		const bool validTrilinear = poly.tsp.FilterMode > 1 && hasMipChain && !punchThrough;
		// The PVR processes an invalid trilinear request (no mip chain) as
		// point sampled, not as ordinary bilinear filtering.
		state.bilinearFilter = poly.tsp.FilterMode == 1 || validTrilinear;
		state.clampU = poly.tsp.ClampU;
		state.clampV = poly.tsp.ClampV;
		state.flipU = poly.tsp.FlipU;
		state.flipV = poly.tsp.FlipV;
		state.fpuPixelCenter = (rendContext->half_offset & 1) != 0;
		state.tspPixelCenter = (rendContext->half_offset & 2) != 0;
		state.texelSampleCenter = (rendContext->half_offset & 4) != 0;
		state.textureSuperSample = poly.tsp.SupSample;
		if (config::TextureFiltering == 1)
		{
			// Match the hardware backends' force-nearest option. Besides selecting
			// point samples, hide the mip chain so the scalar path can skip the
			// derivative calculation that is irrelevant to nearest filtering.
			state.bilinearFilter = false;
			state.textureSuperSample = false;
			state.texture.mipLevelCount = std::min<u8>(1, state.texture.mipLevelCount);
		}
		state.dCalcCtrl = poly.isp.DCalcCtrl;
		const glm::mat4& viewport = matrices.GetViewportMatrix();
		state.nativeToHostScaleX = std::abs(viewport[0][0]);
		state.nativeToHostScaleY = std::abs(viewport[1][1]);
		state.nativeOriginX = viewport[3][0];
		state.nativeOriginY = viewport[3][1];
		state.fogMode = config::Fog ? poly.tsp.FogCtrl : 2;
		state.fogRamColor = { static_cast<u8>(rendContext->fog_col_ram.red() * 255.f),
			static_cast<u8>(rendContext->fog_col_ram.green() * 255.f), static_cast<u8>(rendContext->fog_col_ram.blue() * 255.f), 255 };
		state.fogVertexColor = { static_cast<u8>(rendContext->fog_col_vert.red() * 255.f),
			static_cast<u8>(rendContext->fog_col_vert.green() * 255.f), static_cast<u8>(rendContext->fog_col_vert.blue() * 255.f), 255 };
		state.fogDensity = rendContext->fog_density.get() * config::ExtraDepthScale;
		state.fogTable = rendContext->fog_table;
		state.colorClamp = poly.tsp.ColorClamp
				&& (rendContext->fog_clamp_min.full != 0 || rendContext->fog_clamp_max.full != 0xffffffff);
		state.clampMin = { static_cast<u8>(rendContext->fog_clamp_min.red() * 255.f),
			static_cast<u8>(rendContext->fog_clamp_min.green() * 255.f),
			static_cast<u8>(rendContext->fog_clamp_min.blue() * 255.f),
			static_cast<u8>(rendContext->fog_clamp_min.alpha() * 255.f) };
		state.clampMax = { static_cast<u8>(rendContext->fog_clamp_max.red() * 255.f),
			static_cast<u8>(rendContext->fog_clamp_max.green() * 255.f),
			static_cast<u8>(rendContext->fog_clamp_max.blue() * 255.f),
			static_cast<u8>(rendContext->fog_clamp_max.alpha() * 255.f) };
		state.shadow = poly.pcw.Shadow;
		state.intensityShadow = rendContext->fpu_shad_scale.intensity_shadow;
		state.srcSelect = poly.tsp.SrcSelect;
		state.dstSelect = poly.tsp.DstSelect;
		if (punchThrough)
			state.srcSelect = state.dstSelect = false;
		state.bumpMap = poly.tcw.PixelFmt == PixelBumpMap;
		// Zero is illegal on hardware; keep it neutral for malformed streams.
		state.mipDAdjust = poly.tsp.MipMapD == 0 ? 1.f : poly.tsp.MipMapD / 4.f;
		state.trilinearFilter = poly.pcw.Texture && validTrilinear;
		state.trilinearPassB = poly.tsp.FilterMode == 3;
		state.shadeScale = rendContext->fpu_shad_scale.scale_factor;
		configureClip(poly.tileclip, state);
	}

	void configureArea1(const PolyParam& poly, PipelineState& state)
	{
		state.area1 = nullptr;
		state.srcBlend = convertBlend(poly.tsp1.SrcInstr);
		state.dstBlend = convertBlend(poly.tsp1.DstInstr);
		state.useAlpha = poly.tsp1.UseAlpha;
		state.ignoreTextureAlpha = poly.tsp1.IgnoreTexA || poly.tcw1.PixelFmt == Pixel565;
		state.shadingInstruction = poly.tsp1.ShadInstr;
		state.texture = {};
		if (poly.pcw.Texture)
			state.texture = currentTexture(poly.texture1);
		const bool hasMipChain = state.texture.mipLevelCount > 1;
		const bool validTrilinear = poly.tsp1.FilterMode > 1 && hasMipChain
				&& !state.alphaTest;
		state.bilinearFilter = poly.tsp1.FilterMode == 1 || validTrilinear;
		state.clampU = poly.tsp1.ClampU;
		state.clampV = poly.tsp1.ClampV;
		state.flipU = poly.tsp1.FlipU;
		state.flipV = poly.tsp1.FlipV;
		state.fpuPixelCenter = (rendContext->half_offset & 1) != 0;
		state.tspPixelCenter = (rendContext->half_offset & 2) != 0;
		state.texelSampleCenter = (rendContext->half_offset & 4) != 0;
		state.textureSuperSample = poly.tsp1.SupSample;
		state.dCalcCtrl = poly.isp.DCalcCtrl;
		state.fogMode = config::Fog ? poly.tsp1.FogCtrl : 2;
		state.colorClamp = poly.tsp1.ColorClamp
				&& (rendContext->fog_clamp_min.full != 0 || rendContext->fog_clamp_max.full != 0xffffffff);
		state.srcSelect = poly.tsp1.SrcSelect;
		state.dstSelect = poly.tsp1.DstSelect;
		if (state.alphaTest)
		{
			state.srcBlend = BlendFactor::One;
			state.dstBlend = BlendFactor::Zero;
			state.srcSelect = state.dstSelect = false;
		}
		state.bumpMap = poly.tcw1.PixelFmt == PixelBumpMap;
		state.mipDAdjust = poly.tsp1.MipMapD == 0 ? 1.f : poly.tsp1.MipMapD / 4.f;
		state.trilinearFilter = poly.pcw.Texture && validTrilinear;
		state.trilinearPassB = poly.tsp1.FilterMode == 3;
	}

	void configureClip(u32 tileclip, PipelineState& state)
	{
		Rect clip = matrices.getBaseScissor();
		Rect tileClip;
		const TileClipping tileMode = matrices.getTileClip(tileclip, tileClip);
		if (tileMode == TileClipping::Outside)
			clip = tileClip;
		state.scissorEnabled = true;
		state.scissorX0 = clip.origin.x;
		state.scissorY0 = clip.origin.y;
		state.scissorX1 = clip.origin.x + clip.size.x;
		state.scissorY1 = clip.origin.y + clip.size.y;
		// Region-array passes revisit the same primitive lists once per tile.
		// Restrict rasterization to those tiles as well as using the coverage
		// mask; otherwise every tile scans each triangle's full-screen bounds.
		if (regionClipEnabled)
		{
			state.scissorX0 = std::max(state.scissorX0, regionClipX0);
			state.scissorY0 = std::max(state.scissorY0, regionClipY0);
			state.scissorX1 = std::min(state.scissorX1, regionClipX1);
			state.scissorY1 = std::min(state.scissorY1, regionClipY1);
		}
		if (tileMode == TileClipping::Inside)
		{
			state.excludeClip = true;
			state.excludeX0 = tileClip.origin.x;
			state.excludeY0 = tileClip.origin.y;
			state.excludeX1 = tileClip.origin.x + tileClip.size.x;
			state.excludeY1 = tileClip.origin.y + tileClip.size.y;
		}
	}

	Vertex convertVertex(const ::Vertex& source) const
	{
		const glm::vec4 position = matrices.GetViewportMatrix() * glm::vec4(source.x, source.y, source.z, 1.f);
		return { position.x, position.y, source.z, convertColor(source.col), convertColor(source.spc), source.u, source.v,
			convertColor(source.col1), convertColor(source.spc1), source.u1, source.v1 };
	}

	bool culled(const PolyParam& poly, const Vertex& a, const Vertex& b, const Vertex& c) const
	{
		return SoftwareRasterizer::cullTriangle(poly.isp.CullMode, rendContext->fpu_cull_val, a, b, c);
	}

	bool culled(u32 cullMode, const Vertex& a, const Vertex& b, const Vertex& c) const
	{
		return SoftwareRasterizer::cullTriangle(cullMode, rendContext->fpu_cull_val, a, b, c);
	}

	void drawTriangle(const PolyParam& poly, u32 ia, u32 ib, u32 ic, const PipelineState& state)
	{
		if (ia >= rendContext->verts.size() || ib >= rendContext->verts.size() || ic >= rendContext->verts.size())
			return;
		const ::Vertex& sourceA = rendContext->verts[ia];
		const ::Vertex& sourceB = rendContext->verts[ib];
		const ::Vertex& sourceC = rendContext->verts[ic];
		// FPU_CULL_VAL is compared with the determinant in native PVR screen
		// coordinates. Host render-resolution scaling must not change whether
		// a primitive is culled.
		const Vertex nativeA { sourceA.x, sourceA.y, sourceA.z };
		const Vertex nativeB { sourceB.x, sourceB.y, sourceB.z };
		const Vertex nativeC { sourceC.x, sourceC.y, sourceC.z };
		if (culled(poly, nativeA, nativeB, nativeC))
			return;
		const Vertex a = convertVertex(sourceA);
		const Vertex b = convertVertex(sourceB);
		const Vertex c = convertVertex(sourceC);
		rasterizer->drawTriangle(a, b, c, state);
	}

	void drawStrip(const PolyParam& poly, u32 first, u32 count, const PipelineState& state)
	{
		if (first >= rendContext->idx.size())
			return;
		count = std::min<u32>(count, rendContext->idx.size() - first);
		for (u32 i = 2; i < count; ++i)
		{
			u32 a = rendContext->idx[first + i - 2];
			u32 b = rendContext->idx[first + i - 1];
			const u32 c = rendContext->idx[first + i];
			if (i & 1)
				std::swap(a, b);
			PipelineState triangleState = state;
			triangleState.sequence += i;
			drawTriangle(poly, a, b, c, triangleState);
			if (triangleState.deferBlend && triangleState.area1 != nullptr)
			{
				PipelineState area1State = *triangleState.area1;
				area1State.deferArea1 = true;
				area1State.useArea1VertexData = true;
				area1State.sequence = triangleState.sequence;
				drawTriangle(poly, a, b, c, area1State);
			}
		}
	}

	void drawList(const std::vector<PolyParam>& list, u32 first, u32 end, bool punchThrough, bool sorting,
			bool colorWrite, bool deferBlend = false, bool forceDepthWrite = false,
			bool recordVisibility = false, bool testVisibility = false)
	{
		end = std::min<u32>(end, list.size());
		for (u32 i = first; i < end; ++i)
		{
			const PolyParam& poly = list[i];
			// Do not discard DepthMode::Never here. Punch Through and autosorted
			// translucent lists ignore the encoded mode and force GreaterOrEqual;
			// ordinary polygons will be rejected naturally by depthTest.
			if (poly.count < 3)
				continue;
			PipelineState state;
			configureState(poly, state, punchThrough, sorting);
			if (&list == &rendContext->global_param_op && i == 0
					&& backgroundCoverageMask != nullptr)
				state.coverageMask = backgroundCoverageMask->data();
			if (forceDepthWrite)
				state.depthWrite = !poly.isp.ZWriteDis;
			state.colorWrite = colorWrite;
			state.deferBlend = deferBlend;
			state.recordVisibility = recordVisibility;
			state.testVisibility = testVisibility;
			const u32 listSequence = &list == &rendContext->global_param_pt ? 0x40000000u
					: &list == &rendContext->global_param_tr ? 0x80000000u : 0u;
			state.sequence = listSequence | (i << 17);
			PipelineState area1State;
			if (colorWrite && !state.intensityShadow
					&& poly.tsp1.full != static_cast<u32>(-1))
			{
				area1State = state;
				configureArea1(poly, area1State);
				state.area1 = &area1State;
			}
			if (punchThrough && state.area1 != nullptr)
			{
				// Holly2 first draws the accepted area-0 Punch Through pixel,
				// then redraws area 1. If area 1 fails its own alpha test, the
				// area-0 result remains visible rather than becoming a hole.
				state.area1 = nullptr;
				drawStrip(poly, poly.first, poly.count, state);
				area1State.useArea1VertexData = true;
				area1State.modifierArea1Only = true;
				drawStrip(poly, poly.first, poly.count, area1State);
			}
			else
				drawStrip(poly, poly.first, poly.count, state);
		}
	}

	void drawSorted(u32 first, u32 end)
	{
		end = std::min<u32>(end, rendContext->sortedTriangles.size());
		for (u32 i = first; i < end; ++i)
		{
			const SortedTriangle& triangle = rendContext->sortedTriangles[i];
			if (triangle.polyIndex >= rendContext->global_param_tr.size())
				continue;
			const PolyParam& poly = rendContext->global_param_tr[triangle.polyIndex];
			PipelineState state;
			configureState(poly, state, false, true);
			if (triangle.first >= rendContext->idx.size())
				continue;
			const u32 count = std::min<u32>(triangle.count,
					rendContext->idx.size() - triangle.first);
			for (u32 j = 0; j + 2 < count; j += 3)
				drawTriangle(poly, rendContext->idx[triangle.first + j], rendContext->idx[triangle.first + j + 1],
						rendContext->idx[triangle.first + j + 2], state);
		}
	}

	void drawModifierRange(u32 first, u32 end, ModifierMode mode, u32 cullMode,
			const PipelineState& state, bool deferred)
	{
		end = std::min<u32>(end, rendContext->modtrig.size());
		for (u32 i = first; i < end; ++i)
		{
			const ModTriangle& source = rendContext->modtrig[i];
			const Vertex native0 { source.x0, source.y0, source.z0 };
			const Vertex native1 { source.x1, source.y1, source.z1 };
			const Vertex native2 { source.x2, source.y2, source.z2 };
			if (culled(cullMode, native0, native1, native2))
				continue;
			const glm::mat4& matrix = matrices.GetViewportMatrix();
			const glm::vec4 p0 = matrix * glm::vec4(source.x0, source.y0, source.z0, 1.f);
			const glm::vec4 p1 = matrix * glm::vec4(source.x1, source.y1, source.z1, 1.f);
			const glm::vec4 p2 = matrix * glm::vec4(source.x2, source.y2, source.z2, 1.f);
			const Vertex v0 { p0.x, p0.y, source.z0 };
			const Vertex v1 { p1.x, p1.y, source.z1 };
			const Vertex v2 { p2.x, p2.y, source.z2 };
			rasterizer->drawModifierTriangle(v0, v1, v2, mode, state, deferred);
		}
	}

	void drawModifierVolumes(const std::vector<ModifierVolumeParam>& volumes, u32 first, u32 end,
			bool deferred = false)
	{
		end = std::min<u32>(end, volumes.size());
		int volumeParamBase = -1;
		auto modifierState = [&](const ModifierVolumeParam& volume) {
			PipelineState state;
			state.coverageMask = activeCoverageMask == nullptr ? nullptr : activeCoverageMask->data();
			configureClip(volume.tileclip, state);
			state.fpuPixelCenter = (rendContext->half_offset & 1) != 0;
			state.tspPixelCenter = (rendContext->half_offset & 2) != 0;
			state.texelSampleCenter = (rendContext->half_offset & 4) != 0;
			return state;
		};
		for (u32 i = first; i < end; ++i)
		{
			const ModifierVolumeParam& volume = volumes[i];
			if (volume.count == 0 || volume.isNaomi2())
				continue;
			if (volumeParamBase < 0)
				volumeParamBase = i;
			const PipelineState state = modifierState(volume);
			const u32 volumeMode = volume.isp.DepthMode;
			const ModifierMode accumulation = !volume.isp.VolumeLast && volumeMode > 0
					? ModifierMode::Or : ModifierMode::Xor;
			drawModifierRange(volume.first, volume.first + volume.count, accumulation,
					volume.isp.CullMode, state, deferred);
			if (volumeMode == 1 || volumeMode == 2)
			{
				const ModifierMode summary = volumeMode == 1
						? ModifierMode::Inclusion : ModifierMode::Exclusion;
				// A volume may span parameter records with different user clips
				// and culling modes. Replay each record with the state under which
				// the TA submitted it rather than applying the last record's state
				// to the entire volume.
				for (u32 j = volumeParamBase; j <= i; ++j)
				{
					const ModifierVolumeParam& member = volumes[j];
					if (member.count == 0 || member.isNaomi2())
						continue;
					const PipelineState memberState = modifierState(member);
					drawModifierRange(member.first, member.first + member.count, summary,
							member.isp.CullMode, memberState, deferred);
				}
				volumeParamBase = -1;
			}
		}
	}

	rend_context *rendContext = nullptr;
	TransformMatrix matrices;
	SoftwareTextureCache textureCache;
	// Screen and render-to-texture accumulation buffers are distinct PVR
	// destinations. Keeping separate CPU surfaces also preserves the previous
	// display frame when an intervening RTT render uses identical dimensions.
	std::unique_ptr<SoftwareRasterizer> screenRasterizer;
	std::unique_ptr<SoftwareRasterizer> framebufferRasterizer;
	std::unique_ptr<SoftwareRasterizer> rttRasterizer;
	SoftwareRasterizer *rasterizer = nullptr;
	SoftwareRasterizer *displayRasterizer = nullptr;
	// Keep the emulated frame store in pre-dither CORE color precision. A new
	// partial render may preserve untouched tiles; deriving presentation from
	// this raw copy prevents those tiles from accumulating dither repeatedly.
	std::vector<Color> rawFrameStore;
	PixelBuffer<u32> framebufferPixels;
	std::vector<Color> presentationBuffer;
	u32 presentationWidth = 0;
	u32 presentationHeight = 0;
	const std::vector<u8> *activeCoverageMask = nullptr;
	const std::vector<u8> *backgroundCoverageMask = nullptr;
	bool regionClipEnabled = false;
	s32 regionClipX0 = 0;
	s32 regionClipY0 = 0;
	s32 regionClipX1 = 0;
	s32 regionClipY1 = 0;
	bool frameReady = false;
	float frameAspectRatio = 4.f / 3.f;
#ifdef USE_OPENGL
	GLuint presentTexture = 0;
	std::unique_ptr<GlQuadDrawer> quadDrawer;
#endif

};

} // namespace
} // namespace rend::soft

Renderer *rend_software()
{
	return new rend::soft::SoftwareRenderer();
}
