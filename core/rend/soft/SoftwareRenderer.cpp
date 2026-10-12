#include "SoftwareRenderer.h"

#include "HybridTileRenderer.h"
#include "SoftwareRasterizer.h"
#include "cfg/option.h"
#include "hw/pvr/Renderer_if.h"
#include "hw/pvr/ta.h"
#include "hw/pvr/pvr_mem.h"
#include "rend/TexCache.h"
#include "rend/transform_matrix.h"
#include "ui/imgui_driver.h"
#ifdef USE_OPENGL
#include "rend/gles/quad.h"
#ifdef TARGET_MAC
// Including the legacy macOS framework headers here collides with the PVR's
// own Rect type. Keep the small C ABI surface explicit instead.
struct __IOSurface;
using IOSurfaceRef = __IOSurface *;
using IOSurfaceID = uint32_t;
struct _CGLContextObject;
using CGLContextObj = _CGLContextObject *;
extern "C" IOSurfaceID IOSurfaceGetID(IOSurfaceRef);
extern "C" CGLContextObj CGLGetCurrentContext();
extern "C" int32_t CGLTexImageIOSurface2D(CGLContextObj, GLenum, GLenum,
		GLsizei, GLsizei, GLenum, GLenum, IOSurfaceRef, GLuint);
#endif
#endif

#include <glm/glm.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>

namespace rend::soft {
namespace {

bool writeVerificationPpm(const std::filesystem::path& path,
		const std::vector<Color>& pixels, u32 width, u32 height)
{
	if (pixels.size() != static_cast<size_t>(width) * height)
		return false;
	FILE *file = std::fopen(path.string().c_str(), "wb");
	if (file == nullptr)
		return false;
	std::fprintf(file, "P6\n%u %u\n255\n", width, height);
	for (const Color& color : pixels)
	{
		const u8 rgb[] { color.r, color.g, color.b };
		if (std::fwrite(rgb, sizeof(rgb), 1, file) != 1)
		{
			std::fclose(file);
			return false;
		}
	}
	return std::fclose(file) == 0;
}

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
#ifdef TARGET_MAC
		for (GLuint& texture : presentSurfaceTextures)
		{
			texture = glcache.GenTexture();
			glcache.BindTexture(GL_TEXTURE_RECTANGLE, texture);
			glcache.TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glcache.TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			glcache.TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
			glcache.TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
		}
#endif
		quadDrawer = std::make_unique<GlQuadDrawer>();
#endif
		return true;
	}

	void Term() override
	{
		if (hybridAsyncActive)
		{
			std::vector<u32> ignored;
			hybrid::copyLatestPresentation(ignored);
		}
		hybridAsyncActive = false;
		hybridFrameQueued = false;
#ifdef USE_OPENGL
		quadDrawer.reset();
		if (presentTexture != 0)
			glcache.DeleteTextures(1, &presentTexture);
		presentTexture = 0;
#ifdef TARGET_MAC
		for (GLuint& texture : presentSurfaceTextures)
			if (texture != 0)
				glcache.DeleteTextures(1, &texture);
		presentSurfaceTextures = {};
		presentSurfaceIds = {};
		presentSurfaceReplacement = 0;
#endif
#endif
		textureCache.Clear();
		screenRasterizer.reset();
		framebufferRasterizer.reset();
		rttRasterizer.reset();
		displayRasterizer = nullptr;
		rasterizer = nullptr;
		rawFrameStore.clear();
		presentationBuffer.clear();
		hybridPresentation = {};
		hybridQueuedPresentation = {};
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
		const bool traceHybrid = std::getenv("FLYCAST_HYBRID_TRACE") != nullptr
				|| std::getenv("FLYCAST_HYBRID_TIMING") != nullptr;
		const auto renderWallStart = std::chrono::steady_clock::now();
		if (rendContext == nullptr)
			return false;
		hybridFrameQueued = false;
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
		const bool verifyHybrid = std::getenv("FLYCAST_HYBRID_VERIFY") != nullptr;
		std::vector<Color> hybridInitial;
		std::vector<u8> hybridVramInitial;
		if (verifyHybrid)
		{
			hybridInitial = frameStore;
			if ((rendContext->isRTT && rendContext->render_passes.size() > 1)
					|| config::EmulateFramebuffer)
				hybridVramInitial.assign(&vram[0], &vram[0] + VRAM_SIZE);
		}
		const bool hybridRendered = tryRenderHybrid(width, height, passRegions, frameStore);
		if (!hybridRendered)
		{
			if (!drainHybridAsync(frameStore))
			{
				activeCoverageMask = nullptr;
				return false;
			}
			hybridPresentation = {};
		}
		if (!hybridVramInitial.empty())
		{
			for (u32 offset = 0; offset < VRAM_SIZE; offset += PAGE_SIZE)
				VramLockedWriteOffset(offset);
			std::memcpy(&vram[0], hybridVramInitial.data(), VRAM_SIZE);
		}
		if (hybridRendered && !verifyHybrid)
		{
			if (hybridFrameQueued)
			{
				presentationWidth = width;
				presentationHeight = height;
				activeCoverageMask = nullptr;
				frameReady = true;
				frameAspectRatio = getOutputFramebufferAspectRatio();
				clearLastFrame = false;
				return true;
			}
			if (rendContext->isRTT || config::EmulateFramebuffer)
			{
				if (rendContext->render_passes.size() == 1)
				{
					std::vector<RegionInfo> committedRegions;
					for (const RegionInfo& region : passRegions[0])
						if (region.flushAccumulate)
							committedRegions.push_back(region);
					if (committedRegions.empty() && passRegions[0].empty())
						committedRegions.push_back({ 0, 0, static_cast<s32>(width),
								static_cast<s32>(height), false, true, false, false,
								true, true, true, true, true });
					if (rendContext->isRTT)
						writeFramebufferToVram(frameStore, committedRegions);
					else
						writeDisplayFramebufferToVram(frameStore, committedRegions);
				}
				if (rendContext->isRTT)
				{
					rasterizer = screenRasterizer.get();
					activeCoverageMask = nullptr;
					return false;
				}
			}
			rawFrameStore = frameStore;
			presentationBuffer = frameStore;
			presentationWidth = width;
			presentationHeight = height;
			activeCoverageMask = nullptr;
			frameReady = true;
			frameAspectRatio = getOutputFramebufferAspectRatio();
			clearLastFrame = false;
			if (traceHybrid)
			{
				const auto now = std::chrono::steady_clock::now();
				const double renderMilliseconds =
						std::chrono::duration<double, std::milli>(
								now - renderWallStart).count();
				static auto cadenceStart = now;
				static u32 cadenceFrames = 0;
				static double renderTotalMilliseconds = 0.;
				static double renderMaximumMilliseconds = 0.;
				++cadenceFrames;
				renderTotalMilliseconds += renderMilliseconds;
				renderMaximumMilliseconds = std::max(
						renderMaximumMilliseconds, renderMilliseconds);
				const double cadenceSeconds = std::chrono::duration<double>(
						now - cadenceStart).count();
				if (cadenceSeconds >= 2.)
				{
					std::fprintf(stderr, "HYBRID cadence frames=%u seconds=%.3f fps=%.3f render_avg=%.3f render_max=%.3f\n",
							cadenceFrames, cadenceSeconds,
							cadenceFrames / cadenceSeconds,
							renderTotalMilliseconds / cadenceFrames,
							renderMaximumMilliseconds);
					cadenceStart = now;
					cadenceFrames = 0;
					renderTotalMilliseconds = 0.;
					renderMaximumMilliseconds = 0.;
				}
			}
			return true;
		}
		std::vector<Color> hybridVerification;
		if (verifyHybrid && hybridRendered)
		{
			hybridVerification = std::move(frameStore);
			frameStore = std::move(hybridInitial);
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
		if (!hybridVerification.empty())
		{
			size_t mismatches = 0;
			for (size_t i = 0; i < frameStore.size(); ++i)
				mismatches += packedColor(frameStore[i]) != packedColor(hybridVerification[i]);
			std::fprintf(stderr, "HYBRID verify pixels=%zu mismatches=%zu\n",
					frameStore.size(), mismatches);
			if (const char *captureDirectory =
					std::getenv("FLYCAST_HYBRID_CAPTURE_DIR"))
			{
				static u64 verifiedFrame = 0;
				static bool captured = false;
				const u64 captureFrame = std::max<u64>(1, std::strtoull(
						std::getenv("FLYCAST_HYBRID_CAPTURE_FRAME") != nullptr
								? std::getenv("FLYCAST_HYBRID_CAPTURE_FRAME") : "1",
						nullptr, 10));
				if (!captured && ++verifiedFrame >= captureFrame)
				{
					std::error_code error;
					const std::filesystem::path directory(captureDirectory);
					std::filesystem::create_directories(directory, error);
					std::vector<Color> difference(frameStore.size());
					for (size_t i = 0; i < difference.size(); ++i)
						difference[i] = {
								static_cast<u8>(std::abs(static_cast<int>(frameStore[i].r)
										- hybridVerification[i].r)),
								static_cast<u8>(std::abs(static_cast<int>(frameStore[i].g)
										- hybridVerification[i].g)),
								static_cast<u8>(std::abs(static_cast<int>(frameStore[i].b)
										- hybridVerification[i].b)), 255 };
					captured = !error
							&& writeVerificationPpm(directory / "software-oracle.ppm",
									frameStore, width, height)
							&& writeVerificationPpm(directory / "metal-hybrid.ppm",
									hybridVerification, width, height)
							&& writeVerificationPpm(directory / "absolute-difference.ppm",
									difference, width, height);
					std::fprintf(stderr,
							"HYBRID capture frame=%llu directory=%s success=%u\n",
							static_cast<unsigned long long>(verifiedFrame),
							captureDirectory, captured);
				}
			}
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
			// A normal PVR render targets its internal frame store. It is not
			// written to Dreamcast VRAM unless framebuffer emulation is enabled,
			// so present the committed software frame directly.
			presentationBuffer = rawFrameStore;
			presentationWidth = rasterizer->width();
			presentationHeight = rasterizer->height();
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
		// The display is being reconstructed from Dreamcast framebuffer memory.
		// A raw hybrid accumulation surface predates framebuffer packing,
		// dithering, scaling, field selection, and read-format conversion and
		// must never override this result merely because its dimensions match.
		hybridPresentation = {};
		hybridQueuedPresentation = {};
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
		hybrid::PresentationSurface completed;
		if (hybrid::takeLatestPresentation(completed))
			hybridPresentation = completed;
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
		const bool traceHybrid = std::getenv("FLYCAST_HYBRID_TRACE") != nullptr
				|| std::getenv("FLYCAST_HYBRID_TIMING") != nullptr;
		const auto presentWallStart = std::chrono::steady_clock::now();
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
		const GLint filter = config::LinearInterpolation ? GL_LINEAR : GL_NEAREST;
		bool surfacePrepared = false;
		GLuint surfaceTexture = 0;
#ifdef TARGET_MAC
		if (hybridPresentation.handle != nullptr
				&& hybridPresentation.width == pixelWidth
				&& hybridPresentation.height == pixelHeight)
		{
			IOSurfaceRef surface = static_cast<IOSurfaceRef>(hybridPresentation.handle);
			const IOSurfaceID surfaceId = IOSurfaceGetID(surface);
			size_t slot = presentSurfaceIds.size();
			for (size_t i = 0; i < presentSurfaceIds.size(); ++i)
				if (presentSurfaceIds[i] == surfaceId)
				{
					slot = i;
					break;
				}
			bool needsBinding = slot == presentSurfaceIds.size();
			if (needsBinding)
			{
				slot = presentSurfaceReplacement++ % presentSurfaceIds.size();
				glcache.BindTexture(GL_TEXTURE_RECTANGLE, presentSurfaceTextures[slot]);
			}
			if (!needsBinding
					|| CGLTexImageIOSurface2D(CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE,
							GL_RGBA, pixelWidth, pixelHeight, GL_BGRA,
							GL_UNSIGNED_INT_8_8_8_8_REV, surface, 0) == 0)
			{
				presentSurfaceIds[slot] = surfaceId;
				surfaceTexture = presentSurfaceTextures[slot];
				glcache.BindTexture(GL_TEXTURE_RECTANGLE, surfaceTexture);
				glcache.TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MIN_FILTER, filter);
				glcache.TexParameteri(GL_TEXTURE_RECTANGLE, GL_TEXTURE_MAG_FILTER, filter);
				surfacePrepared = true;
			}
		}
#endif
		if (!surfacePrepared)
		{
			glcache.BindTexture(GL_TEXTURE_2D, presentTexture);
			glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pixelWidth, pixelHeight, 0,
					GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
			glcache.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
		}
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
#ifdef TARGET_MAC
		if (surfacePrepared)
			quadDrawer->drawRect(surfaceTexture, pixelWidth, pixelHeight,
					config::Rotate90, true);
		else
#endif
			quadDrawer->draw(presentTexture, config::Rotate90, true);
		glCheck();
#endif
		if (traceHybrid)
		{
			const auto now = std::chrono::steady_clock::now();
			const double milliseconds = std::chrono::duration<double, std::milli>(
					now - presentWallStart).count();
			static auto reportStart = now;
			static double totalMilliseconds = 0.;
			static double maximumMilliseconds = 0.;
			static u32 count = 0;
			totalMilliseconds += milliseconds;
			maximumMilliseconds = std::max(maximumMilliseconds, milliseconds);
			++count;
			const double seconds = std::chrono::duration<double>(
					now - reportStart).count();
			if (seconds >= 2.)
			{
				std::fprintf(stderr, "HYBRID presentation calls=%u seconds=%.3f avg=%.3f max=%.3f\n",
						count, seconds, totalMilliseconds / count,
						maximumMilliseconds);
				reportStart = now;
				totalMilliseconds = 0.;
				maximumMilliseconds = 0.;
				count = 0;
			}
		}
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

	static s32 hybridFixed(float value)
	{
		constexpr double scale = 16.;
		constexpr double limit = static_cast<double>(1 << 26);
		return static_cast<s32>(std::clamp(std::floor(value * scale + .5), -limit, limit));
	}

	static u32 packedColor(Color color)
	{
		u32 packed;
		std::memcpy(&packed, &color, sizeof(packed));
		return packed;
	}

	bool appendHybridTriangle(const PolyParam& poly, u32 ia, u32 ib, u32 ic,
			const PipelineState& state, std::vector<hybrid::Triangle>& commands,
			std::vector<u32>& textureAtlas,
			std::unordered_map<const Color *, std::array<u32, 11>>& textureOffsets,
			u32 listType, bool area1Data = false)
	{
		if (ia >= rendContext->verts.size() || ib >= rendContext->verts.size()
				|| ic >= rendContext->verts.size())
			return false;
		const ::Vertex& sourceA = rendContext->verts[ia];
		const ::Vertex& sourceB = rendContext->verts[ib];
		const ::Vertex& sourceC = rendContext->verts[ic];
		if (culled(poly, { sourceA.x, sourceA.y, sourceA.z },
				{ sourceB.x, sourceB.y, sourceB.z }, { sourceC.x, sourceC.y, sourceC.z }))
			return true;
		Vertex vertices[3] { convertVertex(sourceA), convertVertex(sourceB), convertVertex(sourceC) };
		// CullMode::None deliberately bypasses determinant validation, but the
		// software oracle still discards non-finite positions before fixed-point
		// conversion. Mirror that behavior here instead of converting NaN/Inf to
		// an implementation-defined integer while encoding the Metal command.
		for (const Vertex& vertex : vertices)
			if (!std::isfinite(vertex.x) || !std::isfinite(vertex.y)
					|| !std::isfinite(vertex.z))
				return true;
		const Vertex flatVertex = vertices[2];
		s32 x[3] { hybridFixed(vertices[0].x), hybridFixed(vertices[1].x), hybridFixed(vertices[2].x) };
		s32 y[3] { hybridFixed(vertices[0].y), hybridFixed(vertices[1].y), hybridFixed(vertices[2].y) };
		auto edge = [&](u32 a, u32 b, u32 c) {
			return (static_cast<s64>(x[b]) - x[a]) * (static_cast<s64>(y[c]) - y[a])
					- (static_cast<s64>(y[b]) - y[a]) * (static_cast<s64>(x[c]) - x[a]);
		};
		const s64 area = edge(0, 1, 2);
		if (area == 0)
			return true;
		if (area < 0)
		{
			std::swap(x[1], x[2]);
			std::swap(y[1], y[2]);
			std::swap(vertices[1], vertices[2]);
		}
		const s32 sampleOffset = state.fpuPixelCenter ? 8 : 0;
		s32 minX = std::max(0, static_cast<s32>(std::ceil(
				(std::min({ x[0], x[1], x[2] }) - sampleOffset) / 16.f)));
		s32 minY = std::max(0, static_cast<s32>(std::ceil(
				(std::min({ y[0], y[1], y[2] }) - sampleOffset) / 16.f)));
		s32 maxX = std::min(static_cast<s32>(rasterizer->width()) - 1,
				(std::max({ x[0], x[1], x[2] }) - sampleOffset) / 16);
		s32 maxY = std::min(static_cast<s32>(rasterizer->height()) - 1,
				(std::max({ y[0], y[1], y[2] }) - sampleOffset) / 16);
		if (state.scissorEnabled)
		{
			minX = std::max(minX, state.scissorX0);
			minY = std::max(minY, state.scissorY0);
			maxX = std::min(maxX, state.scissorX1 - 1);
			maxY = std::min(maxY, state.scissorY1 - 1);
		}
		if (minX > maxX || minY > maxY)
			return true;

		std::array<u32, 11> mipOffsets {};
		u32 textureWidth = 0;
		u32 textureHeight = 0;
		if (state.texture)
		{
			textureWidth = state.texture.width;
			textureHeight = state.texture.height;
			auto [entry, inserted] = textureOffsets.emplace(state.texture.pixels, mipOffsets);
			if (inserted)
			{
				for (u32 level = 0; level < state.texture.mipLevelCount; ++level)
				{
					const u32 levelWidth = std::max(1u, textureWidth >> level);
					const u32 levelHeight = std::max(1u, textureHeight >> level);
					const size_t pixelCount = static_cast<size_t>(levelWidth) * levelHeight;
					if (pixelCount > std::numeric_limits<u32>::max() - textureAtlas.size())
						return false;
					entry->second[level] = static_cast<u32>(textureAtlas.size());
					const size_t first = textureAtlas.size();
					textureAtlas.resize(first + pixelCount);
					std::memcpy(textureAtlas.data() + first, state.texture.mipLevels[level],
							pixelCount * sizeof(Color));
				}
			}
			mipOffsets = entry->second;
		}

		u32 encodedState = hybrid::makeState(static_cast<u32>(state.depthMode), state.depthWrite,
				static_cast<bool>(state.texture), state.bilinearFilter, state.useAlpha,
				state.shadingInstruction, static_cast<u32>(state.srcBlend),
				static_cast<u32>(state.dstBlend));
		if (state.clampU) encodedState |= hybrid::ClampU;
		if (state.clampV) encodedState |= hybrid::ClampV;
		if (state.flipU) encodedState |= hybrid::FlipU;
		if (state.flipV) encodedState |= hybrid::FlipV;
		if (!state.texelSampleCenter) encodedState &= ~hybrid::TexelSampleCenter;
		if (state.ignoreTextureAlpha) encodedState |= hybrid::IgnoreTextureAlpha;
		if (state.alphaTest) encodedState |= hybrid::AlphaTest;
		if (state.deferBlend) encodedState |= hybrid::Autosort;
		if (state.fogMode == 0) encodedState |= hybrid::LookupFog;
		if (state.trilinearFilter) encodedState |= hybrid::Trilinear;
		if (state.trilinearFilter && state.trilinearPassB) encodedState |= hybrid::TrilinearPassB;
		if (state.shadow && state.intensityShadow) encodedState |= hybrid::IntensityShadow;
		if (area1Data) encodedState |= hybrid::Area1Only;
		encodedState |= (listType & 3) << hybrid::ListTypeShift;
		if (state.textureSuperSample) encodedState |= hybrid::TextureSuperSample;
		hybrid::Triangle command {};
		for (u32 i = 0; i < 3; ++i)
		{
			command.x[i] = x[i];
			command.y[i] = y[i];
			command.z[i] = vertices[i].z;
			command.u[i] = area1Data ? vertices[i].u1 : vertices[i].u;
			command.v[i] = area1Data ? vertices[i].v1 : vertices[i].v;
			command.color[i] = packedColor(state.gouraud
					? (area1Data ? vertices[i].color1 : vertices[i].color)
					: (area1Data ? flatVertex.color1 : flatVertex.color));
			command.offsetColor[i] = packedColor(state.gouraud
					? (area1Data ? vertices[i].offsetColor1 : vertices[i].offsetColor)
					: (area1Data ? flatVertex.offsetColor1 : flatVertex.offsetColor));
		}
		command.textureOffset = mipOffsets[0];
		command.textureWidth = textureWidth;
		command.textureHeight = textureHeight;
		command.state = encodedState;
		command.alphaReference = state.alphaReference;
		command.minX = minX;
		command.minY = minY;
		command.maxX = maxX;
		command.maxY = maxY;
		std::copy(mipOffsets.begin(), mipOffsets.end(), command.mipOffsets);
		command.mipLevelCount = state.texture ? state.texture.mipLevelCount : 0;
		command.shadeScale = state.shadeScale;
		command.area1Index = std::numeric_limits<u32>::max();
		command.superSampleOffsetX = static_cast<s32>(std::lround(8.f * state.nativeToHostScaleX));
		command.superSampleOffsetY = static_cast<s32>(std::lround(8.f * state.nativeToHostScaleY));
		if (state.useOffsetColor) command.effects |= hybrid::UseOffsetColor;
		if (state.bumpMap) command.effects |= hybrid::BumpMap;
		if (state.fogMode == 1 && state.useOffsetColor && !state.bumpMap)
			command.effects |= hybrid::VertexFog;
		if (state.srcSelect) command.effects |= hybrid::SourceSelect;
		if (state.dstSelect) command.effects |= hybrid::DestinationSelect;
		if (state.colorClamp) command.effects |= hybrid::ColorClamp;
		if (state.dither) command.effects |= hybrid::Dither;
		if (state.fogMode == 3) command.effects |= hybrid::LookupFogAlpha;
		if (state.excludeClip) command.effects |= hybrid::ExcludeClip;
		command.clampMin = packedColor(state.clampMin);
		command.clampMax = packedColor(state.clampMax);
		command.ditherDivisors = packedColor({ state.ditherDivisorR,
			state.ditherDivisorG, state.ditherDivisorB, state.ditherDivisorA });
		command.fpuSampleOffset = state.fpuPixelCenter ? 8 : 0;
		command.tspSampleOffset = state.tspPixelCenter ? 8 : 0;
		command.excludeX0 = state.excludeX0;
		command.excludeY0 = state.excludeY0;
		command.excludeX1 = state.excludeX1;
		command.excludeY1 = state.excludeY1;
		if (command.mipLevelCount > 1)
		{
			auto vertexU = [&](u32 i) { return area1Data ? vertices[i].u1 : vertices[i].u; };
			auto vertexV = [&](u32 i) { return area1Data ? vertices[i].v1 : vertices[i].v; };
			const double inverseArea = 1. / static_cast<double>(std::abs(area));
			const float barycentricDx[3] {
				static_cast<float>(-(static_cast<s64>(y[2]) - y[1]) * 16 * inverseArea),
				static_cast<float>(-(static_cast<s64>(y[0]) - y[2]) * 16 * inverseArea),
				static_cast<float>(-(static_cast<s64>(y[1]) - y[0]) * 16 * inverseArea),
			};
			const float barycentricDy[3] {
				static_cast<float>((static_cast<s64>(x[2]) - x[1]) * 16 * inverseArea),
				static_cast<float>((static_cast<s64>(x[0]) - x[2]) * 16 * inverseArea),
				static_cast<float>((static_cast<s64>(x[1]) - x[0]) * 16 * inverseArea),
			};
			auto step = [&](bool u, const float *barycentric) {
				return (u ? vertexU(0) : vertexV(0)) * vertices[0].z * barycentric[0]
						+ (u ? vertexU(1) : vertexV(1)) * vertices[1].z * barycentric[1]
						+ (u ? vertexU(2) : vertexV(2)) * vertices[2].z * barycentric[2];
			};
			const float qDxHost = vertices[0].z * barycentricDx[0]
					+ vertices[1].z * barycentricDx[1] + vertices[2].z * barycentricDx[2];
			const float qDyHost = vertices[0].z * barycentricDy[0]
					+ vertices[1].z * barycentricDy[1] + vertices[2].z * barycentricDy[2];
			const float uqDxHost = step(true, barycentricDx);
			const float uqDyHost = step(true, barycentricDy);
			const float vqDxHost = step(false, barycentricDx);
			const float vqDyHost = step(false, barycentricDy);
			float referenceQ = vertices[0].z;
			float referenceUQ = vertexU(0) * vertices[0].z;
			float referenceVQ = vertexV(0) * vertices[0].z;
			if (!state.dCalcCtrl)
			{
				const s32 sampleXFixed = minX * 16 + command.tspSampleOffset;
				const s32 sampleYFixed = minY * 16 + command.tspSampleOffset;
				auto edgeAt = [&](u32 a, u32 b) {
					return (static_cast<s64>(x[b]) - x[a]) * (sampleYFixed - y[a])
							- (static_cast<s64>(y[b]) - y[a]) * (sampleXFixed - x[a]);
				};
				const float q0 = static_cast<float>(edgeAt(1, 2) * inverseArea);
				const float q1 = static_cast<float>(edgeAt(2, 0) * inverseArea);
				const float q2 = static_cast<float>(edgeAt(0, 1) * inverseArea);
				const float q = vertices[0].z * q0 + vertices[1].z * q1 + vertices[2].z * q2;
				const float uq = vertexU(0) * vertices[0].z * q0
						+ vertexU(1) * vertices[1].z * q1 + vertexU(2) * vertices[2].z * q2;
				const float vq = vertexV(0) * vertices[0].z * q0
						+ vertexV(1) * vertices[1].z * q1 + vertexV(2) * vertices[2].z * q2;
				const float sampleX = sampleXFixed / 16.f - state.nativeOriginX;
				const float sampleY = sampleYFixed / 16.f - state.nativeOriginY;
				referenceQ = q - qDxHost * sampleX - qDyHost * sampleY;
				referenceUQ = uq - uqDxHost * sampleX - uqDyHost * sampleY;
				referenceVQ = vq - vqDxHost * sampleX - vqDyHost * sampleY;
			}
			const float dqDx = qDxHost * state.nativeToHostScaleX;
			const float dqDy = qDyHost * state.nativeToHostScaleY;
			const float duqDx = uqDxHost * state.nativeToHostScaleX;
			const float duqDy = uqDyHost * state.nativeToHostScaleY;
			const float dvqDx = vqDxHost * state.nativeToHostScaleX;
			const float dvqDy = vqDyHost * state.nativeToHostScaleY;
			const float dxU = (duqDx * referenceQ - dqDx * referenceUQ) * textureWidth;
			const float dxV = (dvqDx * referenceQ - dqDx * referenceVQ) * textureHeight;
			const float dyU = (duqDy * referenceQ - dqDy * referenceUQ) * textureWidth;
			const float dyV = (dvqDy * referenceQ - dqDy * referenceVQ) * textureHeight;
			command.mipNumerator = std::sqrt(std::max(dxU * dxU + dxV * dxV,
					dyU * dyU + dyV * dyV)) * state.mipDAdjust;
		}
		commands.push_back(command);
		return true;
	}

	bool appendHybridList(const std::vector<PolyParam>& list, u32 first, u32 end,
			bool punchThrough, bool sorting, u32 listType,
			std::vector<hybrid::Triangle>& commands,
			std::vector<u32>& textureAtlas,
			std::unordered_map<const Color *, std::array<u32, 11>>& textureOffsets)
	{
		end = std::min<u32>(end, list.size());
		for (u32 i = std::min(first, end); i < end; ++i)
		{
			const PolyParam& poly = list[i];
			if (poly.count < 3)
				continue;
			PipelineState state;
			configureState(poly, state, punchThrough, sorting);
			state.deferBlend = sorting;
			const bool hasArea1 = state.shadow && !state.intensityShadow
					&& poly.tsp1.full != static_cast<u32>(-1);
			PipelineState area1State;
			if (hasArea1)
			{
				area1State = state;
				configureArea1(poly, area1State);
				area1State.deferBlend = sorting;
			}
			if (state.shadow && !state.intensityShadow && !hasArea1)
			{
				static u32 reports = 0;
				if (std::getenv("FLYCAST_HYBRID_TRACE") != nullptr && reports++ < 40)
					std::fprintf(stderr, "HYBRID polygon fpu=%u clip=%u offset=%u super=%u mip=%u tri=%u fog=%u clamp=%u shadow=%u src=%u dst=%u bump=%u dither=%u volume=%08x\n",
							state.fpuPixelCenter, state.excludeClip, state.useOffsetColor,
							state.textureSuperSample, state.texture.mipLevelCount,
							state.trilinearFilter, state.fogMode, state.colorClamp,
							state.shadow, state.srcSelect, state.dstSelect, state.bumpMap,
							state.dither, poly.tsp1.full);
				return false;
			}
			if (poly.first >= rendContext->idx.size())
				return false;
			const u32 count = std::min<u32>(poly.count, rendContext->idx.size() - poly.first);
			for (u32 vertex = 2; vertex < count; ++vertex)
			{
				u32 a = rendContext->idx[poly.first + vertex - 2];
				u32 b = rendContext->idx[poly.first + vertex - 1];
				const u32 c = rendContext->idx[poly.first + vertex];
				if (vertex & 1)
					std::swap(a, b);
				const size_t primaryIndex = commands.size();
				if (!appendHybridTriangle(poly, a, b, c, state, commands,
						textureAtlas, textureOffsets, listType))
					return false;
				if (hasArea1 && commands.size() != primaryIndex)
				{
					const size_t area1Index = commands.size();
					if (!appendHybridTriangle(poly, a, b, c, area1State, commands,
							textureAtlas, textureOffsets, listType, true))
						return false;
					if (commands.size() != area1Index + 1
							|| area1Index > std::numeric_limits<u32>::max())
						return false;
					if (punchThrough)
						commands[area1Index].effects |= hybrid::ModifierArea1Only;
					else
						commands[primaryIndex].area1Index = static_cast<u32>(area1Index);
				}
			}
		}
		return true;
	}

	bool tryRenderHybridPasses(u32 width, u32 height,
			const std::vector<std::vector<RegionInfo>>& passRegions,
			std::vector<Color>& frameStore, bool trace)
	{
		if (rendContext->render_passes.size() < 2
				|| passRegions.size() != rendContext->render_passes.size())
			return false;
		std::vector<u32> accumulation(frameStore.size());
		std::memcpy(accumulation.data(), frameStore.data(), frameStore.size() * sizeof(Color));
		std::vector<Color> committed = frameStore;
		std::vector<float> preservedDepth;
		std::vector<u32> preservedSecondary;
		RenderPass previous {};
		size_t totalTriangles = 0;
		size_t totalTextures = 0;
		double totalCpuTranslucentMilliseconds = 0.;
		hybrid::Timing totalTiming;
		for (size_t passIndex = 0; passIndex < rendContext->render_passes.size(); ++passIndex)
		{
			const RenderPass& pass = rendContext->render_passes[passIndex];
			const std::vector<RegionInfo>& regions = passRegions[passIndex];
			if (regions.empty())
				return false;
			const bool useOpaqueModifiers = config::ModifierVolumes
					&& pass.mvo_count != previous.mvo_count;
			const bool autosort = !config::PerStripSorting && pass.autosort;
			const bool hasTranslucent = pass.tr_count != previous.tr_count;
			const size_t pixelCount = static_cast<size_t>(width) * height;
			std::vector<u8> depthClearMask(pixelCount);
			std::vector<u8> opaqueMask(pixelCount);
			std::vector<u8> punchMask(pixelCount);
			std::vector<u8> translucentMask(pixelCount);
			std::vector<u8> opaqueModifierMask(pixelCount);
			bool regionSortDiffers = false;
			for (const RegionInfo& region : regions)
			{
				if (hasTranslucent && region.translucentPresent
						&& region.autosort != autosort)
					regionSortDiffers = true;
				const s32 x0 = std::clamp<s32>(region.x0, 0, width);
				const s32 y0 = std::clamp<s32>(region.y0, 0, height);
				const s32 x1 = std::clamp<s32>(region.x1, 0, width);
				const s32 y1 = std::clamp<s32>(region.y1, 0, height);
				for (s32 y = y0; y < y1; ++y)
				{
					const size_t first = static_cast<size_t>(y) * width + x0;
					const size_t last = static_cast<size_t>(y) * width + x1;
					if (region.zClear) std::fill(depthClearMask.begin() + first, depthClearMask.begin() + last, 1);
					if (region.opaquePresent) std::fill(opaqueMask.begin() + first, opaqueMask.begin() + last, 1);
					if (region.punchThroughPresent) std::fill(punchMask.begin() + first, punchMask.begin() + last, 1);
					if (region.translucentPresent) std::fill(translucentMask.begin() + first, translucentMask.begin() + last, 1);
					if (region.opaqueModifierPresent) std::fill(opaqueModifierMask.begin() + first, opaqueModifierMask.begin() + last, 1);
				}
			}
			const bool cpuTranslucent = hasTranslucent && (regionSortDiffers
					|| (config::ModifierVolumes
							&& (pass.mvo_count != previous.mvo_count
									|| pass.mvo_tr_count != previous.mvo_tr_count)));

			std::vector<hybrid::Triangle> commands;
			std::vector<u32> textureAtlas { 0xffffffffu };
			std::unordered_map<const Color *, std::array<u32, 11>> textureOffsets;
			if (!appendHybridList(rendContext->global_param_op, previous.op_count,
					pass.op_count, false, false, 0, commands, textureAtlas, textureOffsets)
					|| !appendHybridList(rendContext->global_param_pt, previous.pt_count,
							pass.pt_count, true, false, 1, commands, textureAtlas, textureOffsets)
					|| (!cpuTranslucent && !appendHybridList(rendContext->global_param_tr,
							previous.tr_count, pass.tr_count, false, autosort, 2,
							commands, textureAtlas, textureOffsets))
					|| commands.empty())
				return false;
			hybrid::Frame frame { width, height, &commands, &textureAtlas, &accumulation };
			const std::vector<float> initialDepth = preservedDepth;
			const std::vector<u32> initialSecondary = preservedSecondary;
			if (passIndex != 0)
			{
				frame.initialDepths = &initialDepth;
				frame.initialSecondary = &initialSecondary;
			}
			frame.depthClearMask = &depthClearMask;
			frame.opaqueMask = &opaqueMask;
			frame.punchThroughMask = &punchMask;
			frame.translucentMask = &translucentMask;
			frame.fogTable = rendContext->fog_table;
			frame.fogColor = packedColor({
				static_cast<u8>(rendContext->fog_col_ram.red() * 255.f),
				static_cast<u8>(rendContext->fog_col_ram.green() * 255.f),
				static_cast<u8>(rendContext->fog_col_ram.blue() * 255.f), 255 });
			frame.fogVertexColor = packedColor({
				static_cast<u8>(rendContext->fog_col_vert.red() * 255.f),
				static_cast<u8>(rendContext->fog_col_vert.green() * 255.f),
				static_cast<u8>(rendContext->fog_col_vert.blue() * 255.f), 255 });
			frame.fogDensity = rendContext->fog_density.get() * config::ExtraDepthScale;
			std::vector<u32> output;
			hybrid::Timing timing;
			if (!hybrid::render(frame, output, preservedDepth, &timing, &preservedSecondary)
					|| output.size() != accumulation.size())
				return false;
			if (useOpaqueModifiers)
			{
				SoftwareRasterizer modifierRasterizer(width, height);
				modifierRasterizer.restoreDepth(preservedDepth);
				SoftwareRasterizer *savedRasterizer = rasterizer;
				const std::vector<u8> *savedCoverageMask = activeCoverageMask;
				rasterizer = &modifierRasterizer;
				activeCoverageMask = &opaqueModifierMask;
				drawModifierVolumes(rendContext->global_param_mvo,
						previous.mvo_count, pass.mvo_count);
				activeCoverageMask = savedCoverageMask;
				rasterizer = savedRasterizer;
				std::vector<u8> modifierMask(pixelCount);
				const std::vector<u8>& stencil = modifierRasterizer.stencilBuffer();
				for (size_t i = 0; i < modifierMask.size(); ++i)
					modifierMask[i] = stencil[i] & 1;
				frame.modifierMask = &modifierMask;
				hybrid::Timing modifierTiming;
				if (!hybrid::render(frame, output, preservedDepth, &modifierTiming,
						&preservedSecondary) || output.size() != accumulation.size())
					return false;
				timing.encodeMilliseconds += modifierTiming.encodeMilliseconds;
				timing.gpuMilliseconds += modifierTiming.gpuMilliseconds;
				timing.gpuExecutionMilliseconds +=
					modifierTiming.gpuExecutionMilliseconds;
				timing.readbackMilliseconds += modifierTiming.readbackMilliseconds;
			}
			if (cpuTranslucent)
			{
				const auto cpuStart = std::chrono::steady_clock::now();
				std::vector<Color> colors(output.size());
				std::vector<Color> secondary(preservedSecondary.size());
				std::memcpy(colors.data(), output.data(), output.size() * sizeof(u32));
				std::memcpy(secondary.data(), preservedSecondary.data(),
						preservedSecondary.size() * sizeof(u32));
				SoftwareRasterizer translucentRasterizer(width, height);
				translucentRasterizer.restoreColor(colors);
				translucentRasterizer.restoreDepth(preservedDepth);
				translucentRasterizer.restoreSecondary(secondary);
				SoftwareRasterizer *savedRasterizer = rasterizer;
				const std::vector<u8> *savedCoverageMask = activeCoverageMask;
				rasterizer = &translucentRasterizer;
				drawTranslucentPass(passIndex, previous, pass, regions);
				activeCoverageMask = savedCoverageMask;
				rasterizer = savedRasterizer;
				std::memcpy(output.data(), translucentRasterizer.colorBuffer().data(),
						output.size() * sizeof(u32));
				preservedDepth = translucentRasterizer.depthBuffer();
				preservedSecondary.resize(pixelCount);
				std::memcpy(preservedSecondary.data(),
						translucentRasterizer.secondaryBuffer().data(),
						pixelCount * sizeof(u32));
				totalCpuTranslucentMilliseconds += std::chrono::duration<double, std::milli>(
						std::chrono::steady_clock::now() - cpuStart).count();
			}
			accumulation = std::move(output);
			std::vector<RegionInfo> committedRegions;
			for (const RegionInfo& region : regions)
				if (region.flushAccumulate)
				{
					committedRegions.push_back(region);
					const s32 x0 = std::clamp<s32>(region.x0, 0, width);
					const s32 y0 = std::clamp<s32>(region.y0, 0, height);
					const s32 x1 = std::clamp<s32>(region.x1, 0, width);
					const s32 y1 = std::clamp<s32>(region.y1, 0, height);
					for (s32 y = y0; y < y1; ++y)
					{
						const size_t first = static_cast<size_t>(y) * width + x0;
						std::memcpy(committed.data() + first, accumulation.data() + first,
								static_cast<size_t>(x1 - x0) * sizeof(u32));
					}
				}
			if (!committedRegions.empty())
			{
				if (rendContext->isRTT)
					writeFramebufferToVram(committed, committedRegions);
				else if (config::EmulateFramebuffer)
					writeDisplayFramebufferToVram(committed, committedRegions);
			}
			totalTriangles += commands.size();
			totalTextures += textureOffsets.size();
			totalTiming.encodeMilliseconds += timing.encodeMilliseconds;
			totalTiming.gpuMilliseconds += timing.gpuMilliseconds;
			totalTiming.gpuExecutionMilliseconds +=
				timing.gpuExecutionMilliseconds;
			totalTiming.readbackMilliseconds += timing.readbackMilliseconds;
			totalTiming.hardwareVisibilityTiles += timing.hardwareVisibilityTiles;
			totalTiming.totalTiles += timing.totalTiles;
			totalTiming.orderedAutosortFrames += timing.orderedAutosortFrames;
			totalTiming.orderedAutosortTiles += timing.orderedAutosortTiles;
			totalTiming.globalAutosortFrames += timing.globalAutosortFrames;
			totalTiming.simpleTranslucentFrames +=
				timing.simpleTranslucentFrames;
			totalTiming.orderedAutosortRejectReason |=
				timing.orderedAutosortRejectReason;
			totalTiming.opaqueTriangles += timing.opaqueTriangles;
			totalTiming.punchThroughTriangles += timing.punchThroughTriangles;
			totalTiming.translucentTriangles += timing.translucentTriangles;
			previous = pass;
		}
			frameStore = std::move(committed);
			if (trace)
				std::fprintf(stderr, "HYBRID multipass passes=%zu triangles=%zu textures=%zu encode=%.3f wait=%.3f gpu=%.3f readback=%.3f cpu_translucent=%.3f visibility_tiles=%u/%u ordered_autosort=%u/%zu tiles=%u/%u global=%u/%zu simple=%u/%zu reject=%u lists=%u/%u/%u\n",
						rendContext->render_passes.size(), totalTriangles, totalTextures,
						totalTiming.encodeMilliseconds, totalTiming.gpuMilliseconds,
						totalTiming.gpuExecutionMilliseconds,
						totalTiming.readbackMilliseconds, totalCpuTranslucentMilliseconds,
						totalTiming.hardwareVisibilityTiles, totalTiming.totalTiles,
						totalTiming.orderedAutosortFrames, rendContext->render_passes.size(),
						totalTiming.orderedAutosortTiles, totalTiming.totalTiles,
						totalTiming.globalAutosortFrames, rendContext->render_passes.size(),
						totalTiming.simpleTranslucentFrames,
						rendContext->render_passes.size(),
						totalTiming.orderedAutosortRejectReason,
					totalTiming.opaqueTriangles, totalTiming.punchThroughTriangles,
					totalTiming.translucentTriangles);
		return true;
	}

	bool tryRenderHybrid(u32 width, u32 height,
			const std::vector<std::vector<RegionInfo>>& passRegions,
			std::vector<Color>& frameStore)
	{
		const bool trace = std::getenv("FLYCAST_HYBRID_TRACE") != nullptr;
		auto reject = [trace](const char *reason) {
			static u32 reports = 0;
			if (trace && reports++ < 120)
				std::fprintf(stderr, "HYBRID reject %s\n", reason);
			return false;
		};
		if (std::getenv("FLYCAST_HYBRID_SOFT") == nullptr)
			return false;
		if (rendContext->render_passes.size() != passRegions.size()
				|| rendContext->render_passes.empty())
			return reject("invalid-passes");
		if (rendContext->render_passes.size() > 1)
		{
			return tryRenderHybridPasses(width, height, passRegions, frameStore, trace)
					|| reject("unsupported-multiple-passes");
		}
		const RenderPass& pass = rendContext->render_passes[0];
		const bool autosort = !config::PerStripSorting && pass.autosort;
		const bool hasTranslucent = pass.tr_count != 0;
		const bool useOpaqueModifiers = config::ModifierVolumes && pass.mvo_count != 0;
		bool regionSortDiffers = false;
		std::vector<u8> opaqueMask;
		std::vector<u8> punchMask;
		std::vector<u8> translucentMask;
		std::vector<u8> modifierCoverage;
		std::vector<u8> commitMask;
		if (!passRegions[0].empty())
		{
			const size_t pixelCount = static_cast<size_t>(width) * height;
			opaqueMask.resize(pixelCount);
			punchMask.resize(pixelCount);
			translucentMask.resize(pixelCount);
			modifierCoverage.resize(pixelCount);
			commitMask.resize(pixelCount);
			for (const RegionInfo& region : passRegions[0])
			{
				if (hasTranslucent && region.translucentPresent
						&& region.autosort != autosort)
					regionSortDiffers = true;
				const s32 x0 = std::clamp<s32>(region.x0, 0, width);
				const s32 y0 = std::clamp<s32>(region.y0, 0, height);
				const s32 x1 = std::clamp<s32>(region.x1, 0, width);
				const s32 y1 = std::clamp<s32>(region.y1, 0, height);
				for (s32 y = y0; y < y1; ++y)
				{
					const size_t first = static_cast<size_t>(y) * width + x0;
					const size_t last = static_cast<size_t>(y) * width + x1;
					if (region.opaquePresent) std::fill(opaqueMask.begin() + first, opaqueMask.begin() + last, 1);
					if (region.punchThroughPresent) std::fill(punchMask.begin() + first, punchMask.begin() + last, 1);
					if (region.translucentPresent) std::fill(translucentMask.begin() + first, translucentMask.begin() + last, 1);
					if (region.opaqueModifierPresent) std::fill(modifierCoverage.begin() + first, modifierCoverage.begin() + last, 1);
					if (region.flushAccumulate) std::fill(commitMask.begin() + first, commitMask.begin() + last, 1);
				}
			}
		}
		const bool cpuTranslucent = hasTranslucent && (regionSortDiffers
				|| (config::ModifierVolumes
						&& (pass.mvo_count != 0 || pass.mvo_tr_count != 0)));

		std::vector<hybrid::Triangle> commands;
		std::vector<u32> textureAtlas { 0xffffffffu };
		std::unordered_map<const Color *, std::array<u32, 11>> textureOffsets;
		if (!appendHybridList(rendContext->global_param_op, 0, pass.op_count, false, false, 0,
				commands, textureAtlas, textureOffsets)
				|| !appendHybridList(rendContext->global_param_pt, 0, pass.pt_count, true, false, 1,
						commands, textureAtlas, textureOffsets)
				|| (!cpuTranslucent && !appendHybridList(rendContext->global_param_tr,
						0, pass.tr_count, false, autosort, 2, commands, textureAtlas, textureOffsets))
				|| commands.empty())
			return reject("unsupported-polygon-state");
		// Direct IOSurface presentation is valid only when the Metal result is
		// already the final display image. Framebuffer emulation must pass through
		// VRAM write/read conversion, and CPU-completed translucency changes the
		// readback after Metal produced its surface.
		const bool usePresentationSurface = hybrid::canUseDirectPresentation(
				rendContext->isRTT, config::EmulateFramebuffer, cpuTranslucent)
				&& std::getenv("FLYCAST_HYBRID_DISABLE_IOSURFACE") == nullptr;
		const bool asyncEligible = std::getenv("FLYCAST_HYBRID_ASYNC") != nullptr
				&& std::getenv("FLYCAST_HYBRID_VERIFY") == nullptr
				&& !rendContext->isRTT && !config::EmulateFramebuffer
				&& usePresentationSurface && hybridQueuedPresentation.handle != nullptr
				&& !useOpaqueModifiers && !cpuTranslucent;
		if (trace)
		{
			static u32 asyncEligibilityReports = 0;
			if (asyncEligibilityReports++ < 20)
				std::fprintf(stderr, "HYBRID async eligible=%u surface=%u rtt=%u efb=%u modifier=%u cpu_translucent=%u\n",
						asyncEligible, hybridQueuedPresentation.handle != nullptr,
						rendContext->isRTT, static_cast<bool>(config::EmulateFramebuffer),
						useOpaqueModifiers, cpuTranslucent);
		}
		if (hybridAsyncActive && !asyncEligible && !drainHybridAsync(frameStore))
			return reject("async-drain-failure");
		std::vector<u32> initial(frameStore.size());
		std::memcpy(initial.data(), frameStore.data(),
				frameStore.size() * sizeof(Color));
		std::vector<u32> output;
		std::vector<float> depth;
		std::vector<u32> secondary;
		hybrid::Frame frame { width, height, &commands, &textureAtlas, &initial };
		if (!opaqueMask.empty()) frame.opaqueMask = &opaqueMask;
		if (!punchMask.empty()) frame.punchThroughMask = &punchMask;
		if (!translucentMask.empty()) frame.translucentMask = &translucentMask;
		if (!commitMask.empty()) frame.commitMask = &commitMask;
		frame.fogTable = rendContext->fog_table;
		frame.fogColor = packedColor({
			static_cast<u8>(rendContext->fog_col_ram.red() * 255.f),
			static_cast<u8>(rendContext->fog_col_ram.green() * 255.f),
			static_cast<u8>(rendContext->fog_col_ram.blue() * 255.f), 255 });
		frame.fogVertexColor = packedColor({
			static_cast<u8>(rendContext->fog_col_vert.red() * 255.f),
			static_cast<u8>(rendContext->fog_col_vert.green() * 255.f),
			static_cast<u8>(rendContext->fog_col_vert.blue() * 255.f), 255 });
		frame.fogDensity = rendContext->fog_density.get() * config::ExtraDepthScale;
		if (asyncEligible)
		{
			frame.initialPresentation = &hybridQueuedPresentation;
			hybrid::PresentationSurface queued;
			const auto asyncStart = std::chrono::steady_clock::now();
			if (hybrid::renderAsync(frame, queued))
			{
				hybridAsyncActive = true;
				hybridFrameQueued = true;
				hybridQueuedPresentation = queued;
				if (trace)
				{
					static auto reportStart = asyncStart;
					static u32 reportFrames = 0;
					static double totalSubmitMilliseconds = 0.;
					static double maximumSubmitMilliseconds = 0.;
					const double submitMilliseconds =
							std::chrono::duration<double, std::milli>(
									std::chrono::steady_clock::now() - asyncStart).count();
					++reportFrames;
					totalSubmitMilliseconds += submitMilliseconds;
					maximumSubmitMilliseconds = std::max(maximumSubmitMilliseconds,
							submitMilliseconds);
					const double reportSeconds = std::chrono::duration<double>(
							std::chrono::steady_clock::now() - reportStart).count();
					if (reportSeconds >= 2.)
					{
						std::fprintf(stderr,
								"HYBRID async frames=%u seconds=%.3f submit_avg=%.3f submit_max=%.3f\n",
								reportFrames, reportSeconds,
								totalSubmitMilliseconds / reportFrames,
								maximumSubmitMilliseconds);
						reportStart = std::chrono::steady_clock::now();
						reportFrames = 0;
						totalSubmitMilliseconds = 0.;
						maximumSubmitMilliseconds = 0.;
					}
				}
				return true;
			}
			if (trace)
				std::fprintf(stderr, "HYBRID async backend-reject\n");
			if (hybridAsyncActive && !drainHybridAsync(frameStore))
				return reject("async-backpressure-drain-failure");
			std::memcpy(initial.data(), frameStore.data(),
					frameStore.size() * sizeof(Color));
		}
		hybrid::Timing timing;
		hybrid::PresentationSurface presentation;
		if (!hybrid::render(frame, output, depth, &timing, &secondary,
					usePresentationSurface ? &presentation : nullptr)
				|| output.size() != frameStore.size())
			return reject("backend-failure");
		std::vector<u8> modifierMask;
		if (useOpaqueModifiers)
		{
			SoftwareRasterizer modifierRasterizer(width, height);
			modifierRasterizer.restoreDepth(depth);
			SoftwareRasterizer *savedRasterizer = rasterizer;
			const std::vector<u8> *savedCoverageMask = activeCoverageMask;
			rasterizer = &modifierRasterizer;
			if (!modifierCoverage.empty())
				activeCoverageMask = &modifierCoverage;
			drawModifierVolumes(rendContext->global_param_mvo, 0, pass.mvo_count);
			activeCoverageMask = savedCoverageMask;
			rasterizer = savedRasterizer;
			modifierMask.resize(static_cast<size_t>(width) * height);
			const std::vector<u8>& stencil = modifierRasterizer.stencilBuffer();
			for (size_t i = 0; i < modifierMask.size(); ++i)
				modifierMask[i] = stencil[i] & 1;
			frame.modifierMask = &modifierMask;
			hybrid::Timing modifierTiming;
			if (!hybrid::render(frame, output, depth, &modifierTiming, &secondary,
					usePresentationSurface ? &presentation : nullptr)
					|| output.size() != frameStore.size())
				return reject("modifier-backend-failure");
			timing.encodeMilliseconds += modifierTiming.encodeMilliseconds;
			timing.gpuMilliseconds += modifierTiming.gpuMilliseconds;
			timing.gpuExecutionMilliseconds +=
				modifierTiming.gpuExecutionMilliseconds;
			timing.readbackMilliseconds += modifierTiming.readbackMilliseconds;
		}
		double cpuTranslucentMilliseconds = 0.;
		if (cpuTranslucent)
		{
			const auto cpuStart = std::chrono::steady_clock::now();
			std::vector<Color> opaqueColors(output.size());
			std::vector<Color> opaqueSecondary(secondary.size());
			std::memcpy(opaqueColors.data(), output.data(), output.size() * sizeof(u32));
			std::memcpy(opaqueSecondary.data(), secondary.data(), secondary.size() * sizeof(u32));
			SoftwareRasterizer translucentRasterizer(width, height);
			translucentRasterizer.restoreColor(opaqueColors);
			translucentRasterizer.restoreDepth(depth);
			translucentRasterizer.restoreSecondary(opaqueSecondary);
			SoftwareRasterizer *savedRasterizer = rasterizer;
			rasterizer = &translucentRasterizer;
			drawTranslucentPass(0, RenderPass {}, pass, passRegions[0]);
			rasterizer = savedRasterizer;
			std::memcpy(output.data(), translucentRasterizer.colorBuffer().data(),
					output.size() * sizeof(u32));
			cpuTranslucentMilliseconds = std::chrono::duration<double, std::milli>(
					std::chrono::steady_clock::now() - cpuStart).count();
		}
		if (passRegions[0].empty())
			std::memcpy(frameStore.data(), output.data(), output.size() * sizeof(u32));
		else
			for (const RegionInfo& region : passRegions[0])
				if (region.flushAccumulate)
				{
					const s32 x0 = std::clamp<s32>(region.x0, 0, width);
					const s32 y0 = std::clamp<s32>(region.y0, 0, height);
					const s32 x1 = std::clamp<s32>(region.x1, 0, width);
					const s32 y1 = std::clamp<s32>(region.y1, 0, height);
					for (s32 y = y0; y < y1; ++y)
					{
						const size_t first = static_cast<size_t>(y) * width + x0;
						std::memcpy(frameStore.data() + first, output.data() + first,
								static_cast<size_t>(x1 - x0) * sizeof(u32));
					}
				}
		if (trace)
			std::fprintf(stderr, "HYBRID frame triangles=%zu textures=%zu encode=%.3f wait=%.3f gpu=%.3f readback=%.3f cpu_translucent=%.3f visibility_tiles=%u/%u ordered_autosort=%u tiles=%u/%u global=%u simple=%u reject=%u lists=%u/%u/%u\n",
					commands.size(), textureOffsets.size(), timing.encodeMilliseconds,
					timing.gpuMilliseconds, timing.gpuExecutionMilliseconds,
					timing.readbackMilliseconds,
					cpuTranslucentMilliseconds, timing.hardwareVisibilityTiles,
					timing.totalTiles, timing.orderedAutosortFrames,
					timing.orderedAutosortTiles, timing.totalTiles,
					timing.globalAutosortFrames,
					timing.simpleTranslucentFrames,
					timing.orderedAutosortRejectReason, timing.opaqueTriangles,
					timing.punchThroughTriangles, timing.translucentTriangles);
		hybridPresentation = presentation;
		hybridQueuedPresentation = presentation;
		return true;
	}

	bool drainHybridAsync(std::vector<Color>& frameStore)
	{
		if (!hybridAsyncActive)
			return true;
		std::vector<u32> drained;
		hybrid::PresentationSurface presentation;
		if (!hybrid::copyLatestPresentation(drained, &presentation)
				|| drained.size() != frameStore.size())
			return false;
		std::vector<Color> colors(drained.size());
		std::memcpy(colors.data(), drained.data(), drained.size() * sizeof(u32));
		frameStore = colors;
		rawFrameStore = colors;
		presentationBuffer = colors;
		if (rasterizer != nullptr)
			rasterizer->restoreColor(colors);
		hybridPresentation = presentation;
		hybridQueuedPresentation = presentation;
		hybridAsyncActive = false;
		return true;
	}

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
		if (scaledSize.y != static_cast<int>(scaleSourceHeight))
		{
			scaled = SoftwareRasterizer::scaleVerticalPvr(*scaleSource,
					scaleSourceWidth, scaleSourceHeight, scaledSize.y,
					rendContext->scaler_ctl.vscalefactor);
			scaleSource = &scaled;
			scaleSourceHeight = scaledSize.y;
			output = scaled.data();
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
		if (scaledSize.y != static_cast<int>(scaleSourceHeight))
		{
			scaled = SoftwareRasterizer::scaleVerticalPvr(*scaleSource,
					scaleSourceWidth, scaleSourceHeight, scaledSize.y,
					rendContext->scaler_ctl.vscalefactor);
			scaleSource = &scaled;
			scaleSourceHeight = scaledSize.y;
			output = scaled.data();
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
			ditheredOutput.assign(output,
					output + static_cast<size_t>(scaledSize.x) * scaledSize.y);
			SoftwareRasterizer::ditherFramebuffer(ditheredOutput, scaledSize.x, scaledSize.y,
					rendContext->fb_W_CTRL.fb_packmode);
			output = ditheredOutput.data();
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
		const bool forceNearest = config::TextureFiltering == 1;
		const bool validTrilinear = poly.tsp.FilterMode > 1 && hasMipChain
				&& !punchThrough && !forceNearest;
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
		state.mipDAdjust = SoftwareRasterizer::decodeMipDAdjust(poly.tsp.MipMapD);
		state.trilinearFilter = poly.pcw.Texture && validTrilinear;
		state.trilinearPassB = poly.tsp.FilterMode == 3;
		if (forceNearest)
			SoftwareRasterizer::forceNearestFiltering(state);
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
		const bool forceNearest = config::TextureFiltering == 1;
		const bool validTrilinear = poly.tsp1.FilterMode > 1 && hasMipChain
				&& !state.alphaTest && !forceNearest;
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
		state.mipDAdjust = SoftwareRasterizer::decodeMipDAdjust(poly.tsp1.MipMapD);
		state.trilinearFilter = poly.pcw.Texture && validTrilinear;
		state.trilinearPassB = poly.tsp1.FilterMode == 3;
		if (forceNearest)
			SoftwareRasterizer::forceNearestFiltering(state);
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
	hybrid::PresentationSurface hybridPresentation;
	hybrid::PresentationSurface hybridQueuedPresentation;
	bool hybridAsyncActive = false;
	bool hybridFrameQueued = false;
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
#ifdef TARGET_MAC
	std::array<GLuint, 3> presentSurfaceTextures {};
	std::array<IOSurfaceID, 3> presentSurfaceIds {};
	size_t presentSurfaceReplacement = 0;
#endif
	std::unique_ptr<GlQuadDrawer> quadDrawer;
#endif

};

} // namespace
} // namespace rend::soft

Renderer *rend_software()
{
	return new rend::soft::SoftwareRenderer();
}
