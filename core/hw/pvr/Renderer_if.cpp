#include "Renderer_if.h"
#include "spg.h"
#include "rend/texconv.h"
#include "rend/transform_matrix.h"
#include "cfg/option.h"
#include "emulator.h"
#include "serialize.h"
#include "hw/holly/holly_intc.h"
#include "hw/sh4/sh4_if.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_sched.h"
#include "profiler/fc_profiler.h"
#include "network/ggpo.h"
#include "pvr_mem.h"
#include <xxhash.h>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <cstring>
#include <locale>
#include <chrono>

#include <mutex>
#include <deque>

#ifdef LIBRETRO
void retro_rend_present();
void retro_resize_renderer(int w, int h, float aspectRatio);
#endif

u32 FrameCount=1;

Renderer* renderer;
static const char *rendererBackend = "uninitialized";

static cResetEvent renderEnd;
u32 fb_w_cur = 1;
static cResetEvent vramRollback;

// direct framebuffer write detection
static bool render_called = false;
u32 fb_watch_addr_start;
u32 fb_watch_addr_end;
bool fb_dirty;

static bool pend_rend;
static bool rendererEnabled = true;

static bool presented;
static u32 fbAddrHistory[2] { 1, 1 };

class PvrMessageQueue
{
	using lock_guard = std::lock_guard<std::mutex>;

public:
	enum MessageType { NoMessage = -1, Render, RenderFramebuffer, Present, Stop };
	struct Message
	{
		Message() = default;
		Message(MessageType type, FramebufferInfo config)
			: type(type), config(config) {}

		MessageType type = NoMessage;
		FramebufferInfo config;
	};

	void enqueue(MessageType type, FramebufferInfo config = FramebufferInfo())
	{
		Message msg { type, config };
		if (config::ThreadedRendering)
		{
			// FIXME need some synchronization to avoid blinking in densha de go
			// or use !threaded rendering for emufb?
			// or read framebuffer vram on emu thread
			bool dupe;
			do {
				dupe = false;
				{
					const lock_guard lock(mutex);
					for (const auto& m : queue)
						if (m.type == type) {
							dupe = true;
							break;
						}
					if (!dupe || type == Present) {
						// Bound the queue to keep the emu-thread producer and
						// the renderer-thread consumer from drifting apart.
						// Render/RenderFramebuffer/Stop are already deduplicated
						// above, but Present is intentionally allowed to repeat
						// and can stack up indefinitely if the consumer stalls
						// (notably under libretro frontends, which drive the
						// swap from their own video callback). Unbounded growth
						// here is the producer side of progressive audio/video
						// drift in long sessions: the SH4 keeps running ahead
						// while pending Presents pile up. Drop the oldest
						// pending Present to keep latency bounded.
						constexpr size_t MAX_QUEUE_DEPTH = 4;
						if (queue.size() >= MAX_QUEUE_DEPTH)
						{
							for (auto it = queue.begin(); it != queue.end(); ++it)
							{
								if (it->type == Present)
								{
									queue.erase(it);
									break;
								}
							}
						}
						queue.push_back(msg);
						dupe = false;
					}
				}
				if (dupe)
				{
					if (type == Stop)
						return;
					dequeueEvent.Wait();
				}
			} while (dupe);
			enqueueEvent.Set();
		}
		else
		{
			setDefaultRoundingMode();
			// drain the queue after switching to !threaded rendering
			while (!queue.empty())
				waitAndExecute();
			execute(msg);
			Sh4cntx.restoreHostRoundingMode();
		}
	}

	bool waitAndExecute(int timeoutMs = -1)
	{
		return execute(dequeue(timeoutMs));
	}

	void reset() {
		const lock_guard lock(mutex);
		queue.clear();
	}

	void cancelEnqueue()
	{
		const lock_guard lock(mutex);
		for (auto it = queue.begin(); it != queue.end(); )
		{
			if (it->type != Render)
				it = queue.erase(it);
			else
				++it;
		}
		dequeueEvent.Set();
	}
private:
	Message dequeue(int timeoutMs = -1)
	{
		FC_PROFILE_SCOPE;

		Message msg;
		while (true)
		{
			{
				const lock_guard lock(mutex);
				if (!queue.empty())
				{
					msg = queue.front();
					queue.pop_front();
				}
			}
			if (msg.type != NoMessage) {
				dequeueEvent.Set();
				break;
			}
			if (timeoutMs == -1)
				enqueueEvent.Wait();
			else if (!enqueueEvent.Wait(timeoutMs))
				break;
		}
		return msg;
	}

	bool execute(Message msg)
	{
		switch (msg.type)
		{
		case Render:
			render();
			return true;
		case RenderFramebuffer:
			renderFramebuffer(msg.config);
			return true;
		case Present:
			present();
			return true;
		case Stop:
		case NoMessage:
		default:
			return false;
		}
	}

	void render()
	{
		FC_PROFILE_SCOPE;

		TA_context *taContext = DequeueRender();
		if (taContext == nullptr)
			return;
		const bool traceCadence = std::getenv("FLYCAST_PVR_TRACE_CADENCE") != nullptr;
		const auto renderStarted = traceCadence ? std::chrono::steady_clock::now()
				: std::chrono::steady_clock::time_point {};
		auto fingerprintStage = [&](const char *stage) {
			if (std::getenv("FLYCAST_PVR_INPUT_FINGERPRINT") != nullptr
					&& !config::ThreadedRendering)
			{
				std::fprintf(stderr,
						"PVR_STAGE frame=%u backend=%s stage=%s rtt=%u efb=%u write=%08x vram=%016llx\n",
						FrameCount, rendererBackend, stage, taContext->rend.isRTT ? 1u : 0u,
						config::EmulateFramebuffer ? 1u : 0u, taContext->rend.fb_W_SOF1,
						static_cast<unsigned long long>(XXH64(&vram[0], VRAM_SIZE, 0)));
				const char *prefix = std::getenv("FLYCAST_PVR_SNAPSHOT_PREFIX");
				const char *frame = std::getenv("FLYCAST_PVR_INPUT_PAGE_FRAME");
				if (prefix != nullptr && frame != nullptr)
				{
					char *end;
					const unsigned long requested = std::strtoul(frame, &end, 10);
					if (end != frame && *end == '\0' && requested == FrameCount)
					{
						auto save = [&](const char *extension, const void *data, size_t size) {
							const std::string path = std::string(prefix) + "." + stage + extension;
							FILE *file = std::fopen(path.c_str(), "wbx");
							if (file == nullptr)
								return false;
							const bool written = std::fwrite(data, 1, size, file) == size;
							return std::fclose(file) == 0 && written;
						};
						const bool saved = save(".vram", &vram[0], VRAM_SIZE);
						// Raw byte-addressed live register bank, including fog and palette.
						// This is not the render context's latched register snapshot.
						const bool registersSaved = save(".registers", pvr_regs, sizeof(pvr_regs));
						// Serialize named values, never C++ object storage or pointers.
						const auto& r = taContext->rend;
						std::ostringstream context;
						context.imbue(std::locale::classic());
						context << "{\"schema\":2,\"kind\":\"latched-pvr-state\"";
						auto field = [&](const char *name, auto value) {
							context << ",\"" << name << "\":" << value;
						};
						field("frame", FrameCount);
						field("config_TextureFiltering", static_cast<int>(config::TextureFiltering));
						field("config_EmulateFramebuffer", static_cast<bool>(config::EmulateFramebuffer));
						field("config_PerStripSorting", static_cast<bool>(config::PerStripSorting));
						field("config_ModifierVolumes", static_cast<bool>(config::ModifierVolumes));
						field("config_Fog", static_cast<bool>(config::Fog));
						field("config_LinearInterpolation", static_cast<bool>(config::LinearInterpolation));
						field("config_Rotate90", static_cast<bool>(config::Rotate90));
						const float extraDepthScale = config::ExtraDepthScale;
						u32 depthScaleBits;
						std::memcpy(&depthScaleBits, &extraDepthScale, sizeof(depthScaleBits));
						field("config_ExtraDepthScale_bits", depthScaleBits);
						field("isRTT", r.isRTT);
						field("clearFramebuffer", r.clearFramebuffer);
						field("globClipX", r.globClip.x);
						field("globClipY", r.globClip.y);
						field("fbClipX", r.fbClip.origin.x);
						field("fbClipY", r.fbClip.origin.y);
						field("fbClipWidth", r.fbClip.size.x);
						field("fbClipHeight", r.fbClip.size.y);
						field("framebufferWidth", r.framebufferWidth);
						field("framebufferHeight", r.framebufferHeight);
						u32 cullBits;
						std::memcpy(&cullBits, &r.fpu_cull_val, sizeof(cullBits));
						field("fpu_cull_val_bits", cullBits);
						field("fb_W_SOF1", r.fb_W_SOF1);
						field("fb_W_CTRL", r.fb_W_CTRL.full);
						field("fb_W_LINESTRIDE", r.fb_W_LINESTRIDE);
						field("fb_R_CTRL", r.fb_R_CTRL.full);
						field("fb_R_SIZE", r.fb_R_SIZE.full);
						field("fb_R_SOF1", r.fb_R_SOF1);
						field("fb_R_SOF2", r.fb_R_SOF2);
						field("spg_control", r.spg_control.full);
						field("spg_status", r.spg_status.full);
						field("vo_control", r.vo_control.full);
						field("scaler_ctl", r.scaler_ctl.full);
						field("y_coeff", r.y_coeff);
						field("fog_clamp_min", r.fog_clamp_min.full);
						field("fog_clamp_max", r.fog_clamp_max.full);
						field("fog_col_ram", r.fog_col_ram.full);
						field("fog_col_vert", r.fog_col_vert.full);
						field("fog_density", r.fog_density.full);
						field("pt_alpha_ref", r.pt_alpha_ref);
						field("half_offset", r.half_offset);
						field("fpu_shad_scale", r.fpu_shad_scale.full);
						field("region_base", r.region_base);
						field("fpu_param_cfg", r.fpu_param_cfg);
						field("isp_feed_cfg", r.isp_feed_cfg);
						field("vo_border_col", r.vo_border_col.full);
						field("region_tile_size", r.region_tile_size);
						context << ",\"fog_table\":[";
						for (size_t i = 0; i < std::size(r.fog_table); ++i)
							context << (i ? "," : "") << r.fog_table[i];
						context << "],\"region_array\":[";
						for (size_t i = 0; i < r.region_array.size(); ++i)
						{
							context << (i ? ",[" : "[");
							for (size_t j = 0; j < r.region_array[i].words.size(); ++j)
								context << (j ? "," : "") << r.region_array[i].words[j];
							context << "]";
						}
						context << "]}\n";
						const std::string contextData = context.str();
						const bool contextSaved = save(".context.json", contextData.data(), contextData.size());
						std::fprintf(stderr, "PVR_CONTEXT_SNAPSHOT stage=%s saved=%u bytes=%zu\n",
								stage, contextSaved ? 1u : 0u, contextData.size());
						std::fprintf(stderr, "PVR_SNAPSHOT stage=%s saved=%u registers_saved=%u registers_bytes=%zu registers_hash=%016llx pack=%u dither=%u scaler=%08x stride=%u\n",
								stage, saved ? 1u : 0u, registersSaved ? 1u : 0u,
								sizeof(pvr_regs), static_cast<unsigned long long>(XXH64(pvr_regs, sizeof(pvr_regs), 0)),
								taContext->rend.fb_W_CTRL.fb_packmode,
								taContext->rend.fb_W_CTRL.fb_dither, taContext->rend.scaler_ctl.full,
								taContext->rend.fb_W_LINESTRIDE * 8);
					}
				}
			}
		};

		// Diagnostic only: hash raw inputs before backend-specific processing.
		// Require single-threaded execution so live register/VRAM reads cannot
		// race emulation. These hashes do not cover retained renderer state.
		if (std::getenv("FLYCAST_PVR_INPUT_FINGERPRINT") != nullptr)
		{
			if (config::ThreadedRendering)
			{
				static bool warned = false;
				if (!warned)
					std::fprintf(stderr, "PVR_INPUT disabled: requires ThreadedRendering=no\n");
				warned = true;
			}
			else
			{
				const auto vramHash = XXH64(&vram[0], VRAM_SIZE, 0);
				const auto registerHash = XXH64(pvr_regs, sizeof(pvr_regs), 0);
				const char *pageFrame = std::getenv("FLYCAST_PVR_INPUT_PAGE_FRAME");
				if (pageFrame != nullptr)
				{
					char *end;
					const unsigned long requested = std::strtoul(pageFrame, &end, 10);
					if (end != pageFrame && *end == '\0' && requested == FrameCount)
						for (u32 offset = 0; offset < VRAM_SIZE; offset += 65536)
							std::fprintf(stderr, "PVR_PAGE frame=%u offset=%08x bytes=65536 hash=%016llx\n",
									FrameCount, offset,
									static_cast<unsigned long long>(XXH64(&vram[offset], 65536, 0)));
				}
				u32 contextIndex = 0;
				for (TA_context *context = taContext; context != nullptr;
						context = context->nextContext, ++contextIndex)
				{
					const size_t bytes = context->getTADataEnd() - context->getTADataBegin();
					std::fprintf(stderr,
							"PVR_INPUT frame=%u backend=%s context=%u address=%08x bytes=%zu ta=%016llx vram=%016llx regs=%016llx\n",
							FrameCount, rendererBackend, contextIndex, context->Address, bytes,
							static_cast<unsigned long long>(XXH64(context->getTADataBegin(), bytes, 0)),
							static_cast<unsigned long long>(vramHash),
							static_cast<unsigned long long>(registerHash));
					const char *prefix = std::getenv("FLYCAST_PVR_SNAPSHOT_PREFIX");
					if (prefix != nullptr && pageFrame != nullptr)
					{
						char *end;
						const unsigned long requested = std::strtoul(pageFrame, &end, 10);
						if (end != pageFrame && *end == '\0' && requested == FrameCount)
						{
							const std::string path = std::string(prefix) + ".input.context-"
									+ std::to_string(contextIndex) + ".ta";
							FILE *file = std::fopen(path.c_str(), "wbx");
							bool saved = false;
							if (file != nullptr)
							{
								saved = std::fwrite(context->getTADataBegin(), 1, bytes, file) == bytes;
								saved = std::fclose(file) == 0 && saved;
							}
							std::fprintf(stderr, "PVR_TA_SNAPSHOT frame=%u context=%u address=%08x bytes=%zu saved=%u\n",
									FrameCount, contextIndex, context->Address, bytes, saved ? 1u : 0u);
						}
					}
				}
			}
		}

		// tile clipping is used to calculate framebuffer size in RTT below
		setTileClipping(taContext->rend);
		int width, height;
		getScaledFramebufferSize(taContext->rend, width, height);
		taContext->rend.framebufferWidth = width;
		taContext->rend.framebufferHeight = height;
		bool renderToScreen = !taContext->rend.isRTT && !config::EmulateFramebuffer;
#ifdef LIBRETRO
		if (renderToScreen)
			retro_resize_renderer(taContext->rend.framebufferWidth, taContext->rend.framebufferHeight,
					getOutputFramebufferAspectRatio());
#endif
		{
			FC_PROFILE_SCOPE_NAMED("Renderer::Process");
			try {
				renderer->Process(taContext);
				fingerprintStage("processed");
			} catch (...) {
				renderEnd.Set();
				rend_allow_rollback();
				FinishRender(taContext);
				throw;
			}
		}

		if (renderToScreen)
			// If rendering to texture or in full framebuffer emulation, continue locking until the frame is rendered
			renderEnd.Set();
		rend_allow_rollback();
		{
			FC_PROFILE_SCOPE_NAMED("Renderer::Render");
			try {
				renderer->Render();
				fingerprintStage("rendered");
			} catch (...) {
				if (!renderToScreen)
					renderEnd.Set();
				FinishRender(taContext);
				throw;
			}
		}

		if (!renderToScreen)
			renderEnd.Set();
		else if (config::DelayFrameSwapping && fb_w_cur == FB_R_SOF1)
			present();

		if (traceCadence && !taContext->rend.isRTT)
		{
			// Count successful render submissions, not Present() calls, which
			// can repeat a frame. Async backends may still have GPU work queued.
			using Clock = std::chrono::steady_clock;
			static auto windowStart = Clock::time_point {};
			static u32 count = 0;
			static double renderMilliseconds = 0.;
			const auto now = Clock::now();
			if (windowStart == Clock::time_point {})
				windowStart = now;
			else
			{
				++count;
				renderMilliseconds += std::chrono::duration<double, std::milli>(now - renderStarted).count();
				if (count == 120)
				{
					const double seconds = std::chrono::duration<double>(now - windowStart).count();
					std::fprintf(stderr, "PVR_CADENCE backend=%s renders=%u seconds=%.6f renders_per_second=%.3f render_call_mean_ms=%.3f\n",
							rendererBackend, count, seconds, count / seconds, renderMilliseconds / count);
					windowStart = now;
					count = 0;
					renderMilliseconds = 0.;
				}
			}
		}
		//clear up & free data ..
		FinishRender(taContext);
	}

	void renderFramebuffer(const FramebufferInfo& config)
	{
		FC_PROFILE_SCOPE;

#ifdef LIBRETRO
		int w, h;
		getDCFramebufferReadSize(config, w, h);
		retro_resize_renderer(w, h, getDCFramebufferAspectRatio());
#endif
		renderer->RenderFramebuffer(config);
	}

	void present()
	{
		FC_PROFILE_SCOPE;

		if (renderer->Present())
		{
			presented = true;
			if (!config::ThreadedRendering && !ggpo::active())
				emu.getSh4Executor()->Stop();
#ifdef LIBRETRO
			retro_rend_present();
#endif
		}
	}

	std::mutex mutex;
	cResetEvent enqueueEvent;
	cResetEvent dequeueEvent;
	std::deque<Message> queue;
};

static PvrMessageQueue pvrQueue;

bool rend_single_frame(const bool& enabled)
{
	FC_PROFILE_SCOPE;

	const int timeout = SPG_CONTROL.isPAL() ? 23 : 20;
	presented = false;
	while (enabled && !presented)
		if (!pvrQueue.waitAndExecute(timeout))
			return false;
	return true;
}

class SwapIntervalDetector
{
public:
	SwapIntervalDetector() {
		EventManager::listen(Event::LoadState, eventHandler, this);
		reset();
	}
	~SwapIntervalDetector() {
		EventManager::unlisten(Event::LoadState, eventHandler, this);
	}

	void render()
	{
		u64 now = sh4_sched_now64();
		if (lastRender != 0)
			renderInterval = now - lastRender;
		lastRender = now;
		renders++;
	}

	void vblank()
	{
		avgRenderInterval = 0.1f * renderInterval + 0.9f * avgRenderInterval;

		// Force transition to 60 FPS if the game swap interval is 1 for 3 consecutive frames.
		// Displaying a 60 FPS game at 30 FPS makes the game run in slo-mo and breaks audio.
		if (renders != 0)
		{
			renders = 0;
			rendersFullSpeed++;
			if (rendersFullSpeed >= 3)
			{
				// force 60/50 FPS now
				lastInterval = 1;
				currentInterval = 1;
				return;
			}
		}
		else {
			rendersFullSpeed = 0;
		}

		const float vblankPerRender = avgRenderInterval / (SPG_CONTROL.isPAL() ? 20_sh4ms : 16667_sh4us);
		int interval = std::round(vblankPerRender);
		float frac = std::abs(vblankPerRender - interval);

		if ((interval == 2 && frac <= .05f)
				|| (interval == 1 && frac <= .2f))
		{
			if (lastInterval == (int)interval)
			{
				if (++stable >= 10)
					currentInterval = std::min(lastInterval, 2);
				unstable = 0;
			}
			else
			{
				stable = 0;
				lastInterval = interval;
				unstable++;
			}
		}
		else {
			stable = 0;
			unstable++;
		}
		if (unstable >= 30 && vblankPerRender < 2.f)
			// Force swap interval to 1 if the frame rate is off over 30 frames
			// Helps with games that render slightly above 30 FPS (ECCO 33 FPS, Armada ~40 FPS)
			currentInterval = 1;
	}

	int swapInterval() const {
		return currentInterval;
	}

	void reset()
	{
		lastInterval = 1;
		stable = 0;
		unstable = 0;
		currentInterval = 1;

		lastRender = 0;
		renderInterval = 0;
		avgRenderInterval = 0.f;
		renders = 0;
		rendersFullSpeed = 0;
	}

private:
	static void eventHandler(Event event, void *arg) {
		SwapIntervalDetector *self = (SwapIntervalDetector *)arg;
		self->lastRender = 0;
		self->lastInterval = 1;
	}

	int lastInterval;
	int stable;
	int unstable;
	int currentInterval;

	u64 lastRender;
	u64 renderInterval;
	float avgRenderInterval;
	int renders;
	int rendersFullSpeed;
};
static SwapIntervalDetector swapIntervalDetector;


Renderer* rend_GLES2();
Renderer* rend_GL4();
Renderer* rend_norend();
Renderer* rend_Vulkan();
Renderer* rend_OITVulkan();
Renderer* rend_DirectX9();
Renderer* rend_DirectX11();
Renderer* rend_OITDirectX11();
Renderer* rend_software();

static void rend_create_renderer()
{
#ifdef NO_REND
	renderer	 = rend_norend();
	rendererBackend = "none";
#else
	switch (config::RendererType)
	{
	case RenderType::Software:
		renderer = rend_software();
		rendererBackend = "software";
		break;
#ifdef USE_OPENGL
	case RenderType::OpenGL:
		renderer = rend_GLES2();
		rendererBackend = "opengl";
		break;
#if !defined(GLES2) && !defined(__APPLE__)
	case RenderType::OpenGL_OIT:
		renderer = rend_GL4();
		rendererBackend = "opengl-oit";
		break;
#endif
#endif
#ifdef USE_VULKAN
	case RenderType::Vulkan:
		renderer = rend_Vulkan();
		rendererBackend = "vulkan";
		break;
	case RenderType::Vulkan_OIT:
		renderer = rend_OITVulkan();
		rendererBackend = "vulkan-oit";
		break;
#endif
#ifdef USE_DX9
	case RenderType::DirectX9:
		renderer = rend_DirectX9();
		rendererBackend = "direct3d9";
		break;
#endif
#ifdef USE_DX11
	case RenderType::DirectX11:
		renderer = rend_DirectX11();
		rendererBackend = "direct3d11";
		break;
	case RenderType::DirectX11_OIT:
		renderer = rend_OITDirectX11();
		rendererBackend = "direct3d11-oit";
		break;
#endif
	default:
#ifdef USE_OPENGL
		renderer = rend_GLES2();
		rendererBackend = "opengl-fallback";
#else
		renderer = rend_norend();
		rendererBackend = "none";
#endif
		break;
	}
#endif
}

bool rend_init_renderer()
{
	rendererEnabled = true;
	if (renderer == nullptr)
		rend_create_renderer();
	bool success = renderer != nullptr && renderer->Init();
	if (!success) {
		delete renderer;
		renderer = rend_norend();
		rendererBackend = "none-init-failed";
		renderer->Init();
	}
	return success;
}

void rend_term_renderer()
{
	// Drain and stop the queue first so that any in-flight Render/Present
	// messages cannot be dispatched against a renderer that is about to be
	// destroyed. This is called from many libretro entry points (context
	// reset/destroy, deinit, content load, renderer switch) where the emu
	// thread may still be holding queued work; without cancelling first
	// we can race the consumer thread into a null renderer dereference and
	// also leave the producer side wedged, which manifests as audio/video
	// drift after a renderer switch.
	rend_cancel_emu_wait();

	if (renderer != nullptr)
	{
		renderer->Term();
		delete renderer;
		renderer = nullptr;
	}
}

void rend_reset()
{
	FinishRender(DequeueRender());
	render_called = false;
	pend_rend = false;
	FrameCount = 1;
	fb_w_cur = 1;
	pvrQueue.reset();
	rendererEnabled = true;
	fbAddrHistory[0] = 1;
	fbAddrHistory[1] = 1;
	swapIntervalDetector.reset();
}

void rend_start_render()
{
	render_called = true;
	pend_rend = false;

	TA_context *ctx = nullptr;
	u32 addresses[MAX_PASSES];
	int count = getTAContextAddresses(addresses);
	if (count > 0)
	{
		ctx = tactx_Pop(addresses[0]);
		if (ctx != nullptr)
		{
			TA_context *linkedCtx = ctx;
			for (int i = 1; i < count; i++)
			{
				linkedCtx->nextContext = tactx_Pop(addresses[i]);
				if (linkedCtx->nextContext != nullptr)
					linkedCtx = linkedCtx->nextContext;
				else
					INFO_LOG(PVR, "rend_start_render: Context%d @ %x not found", i, addresses[i]);
			}
		}
		else
			INFO_LOG(PVR, "rend_start_render: Context0 @ %x not found", addresses[0]);
	}
	else
		INFO_LOG(PVR, "rend_start_render: No context not found");

	scheduleRenderDone(ctx);

	if (ctx == nullptr)
		return;

	FillBGP(ctx);

	ctx->rend.isRTT = (FB_W_SOF1 & 0x1000000) != 0;
	ctx->rend.fb_W_SOF1 = FB_W_SOF1;
	ctx->rend.fb_W_CTRL.full = FB_W_CTRL.full;
	ctx->rend.fb_R_CTRL.full = FB_R_CTRL.full;
	ctx->rend.fb_R_SIZE.full = FB_R_SIZE.full;
	ctx->rend.fb_R_SOF1 = FB_R_SOF1;
	ctx->rend.fb_R_SOF2 = FB_R_SOF2;
	ctx->rend.spg_control.full = SPG_CONTROL.full;
	ctx->rend.spg_status.full = SPG_STATUS.full;
	ctx->rend.vo_control.full = VO_CONTROL.full;

	ctx->rend.globClip.x = (TA_GLOB_TILE_CLIP.tile_x_num + 1) * 32;
	ctx->rend.globClip.y = (TA_GLOB_TILE_CLIP.tile_y_num + 1) * 32;
	ctx->rend.scaler_ctl = SCALER_CTL;
	ctx->rend.y_coeff = Y_COEFF;
	ctx->rend.fbClip.origin.x = FB_X_CLIP.min;
	ctx->rend.fbClip.origin.y = FB_Y_CLIP.min;
	ctx->rend.fbClip.size.x = FB_X_CLIP.max - FB_X_CLIP.min + 1;
	ctx->rend.fbClip.size.y = FB_Y_CLIP.max - FB_Y_CLIP.min + 1;
	ctx->rend.fb_W_LINESTRIDE = FB_W_LINESTRIDE.stride;

	ctx->rend.fog_clamp_min = FOG_CLAMP_MIN;
	ctx->rend.fog_clamp_max = FOG_CLAMP_MAX;
	ctx->rend.fog_col_ram = FOG_COL_RAM;
	ctx->rend.fog_col_vert = FOG_COL_VERT;
	ctx->rend.fog_density = FOG_DENSITY;
	std::copy_n(FOG_TABLE, std::size(ctx->rend.fog_table), ctx->rend.fog_table);
	ctx->rend.pt_alpha_ref = PT_ALPHA_REF;
	ctx->rend.half_offset = HALF_OFFSET;
	ctx->rend.fpu_cull_val = FPU_CULL_VAL;
	ctx->rend.fpu_shad_scale = FPU_SHAD_SCALE;
	ctx->rend.region_base = REGION_BASE;
	ctx->rend.fpu_param_cfg = FPU_PARAM_CFG;
	ctx->rend.isp_feed_cfg = ISP_FEED_CFG;
	ctx->rend.vo_border_col = VO_BORDER_COL;
	snapshotRegionArray(ctx->rend);

	if (!ctx->rend.isRTT)
	{
		if (FB_W_SOF1 != fbAddrHistory[0] && FB_W_SOF1 != fbAddrHistory[1])
		{
			ctx->rend.clearFramebuffer = true;
			fbAddrHistory[0] = fbAddrHistory[1];
			fbAddrHistory[1] = FB_W_SOF1;
		}
		else {
			ctx->rend.clearFramebuffer = false;
		}
		ggpo::endOfFrame();
		swapIntervalDetector.render();
		if (!config::EmulateFramebuffer)
			ctx->rend.swapInterval = swapIntervalDetector.swapInterval();
		else
			ctx->rend.swapInterval = 1;
	}

	if (QueueRender(ctx))
	{
		palette_update();
		pend_rend = true;
		pvrQueue.enqueue(PvrMessageQueue::Render);
		if (!config::DelayFrameSwapping && !ctx->rend.isRTT && !config::EmulateFramebuffer)
			pvrQueue.enqueue(PvrMessageQueue::Present);
	}
}

int rend_end_render(int tag, int cycles, int jitter, void *arg)
{
	if (settings.platform.isNaomi2())
	{
		asic_RaiseInterruptBothCLX(holly_RENDER_DONE);
		asic_RaiseInterruptBothCLX(holly_RENDER_DONE_isp);
		asic_RaiseInterruptBothCLX(holly_RENDER_DONE_vd);
	}
	else
	{
		asic_RaiseInterrupt(holly_RENDER_DONE);
		asic_RaiseInterrupt(holly_RENDER_DONE_isp);
		asic_RaiseInterrupt(holly_RENDER_DONE_vd);
	}
	if (pend_rend && config::ThreadedRendering)
		renderEnd.Wait();

	return 0;
}

void rend_vblank()
{
	if (config::EmulateFramebuffer
			|| (!render_called && fb_dirty && FB_R_CTRL.fb_enable))
	{
		if (rend_is_enabled())
		{
			FramebufferInfo fbInfo;
			fbInfo.update();
			pvrQueue.enqueue(PvrMessageQueue::RenderFramebuffer, fbInfo);
			pvrQueue.enqueue(PvrMessageQueue::Present);
			if (!config::EmulateFramebuffer)
				DEBUG_LOG(PVR, "Direct framebuffer write detected");
		}
		fb_dirty = false;
	}
	render_called = false;
	check_framebuffer_write();
	emu.vblank();
	swapIntervalDetector.vblank();
}

void check_framebuffer_write()
{
	u32 fb_size = (FB_R_SIZE.fb_y_size + 1) * (FB_R_SIZE.fb_x_size + FB_R_SIZE.fb_modulus) * 4;
	fb_watch_addr_start = (SPG_CONTROL.interlace ? FB_R_SOF2 : FB_R_SOF1) & VRAM_MASK;
	fb_watch_addr_end = fb_watch_addr_start + fb_size;
}

void rend_cancel_emu_wait()
{
	if (config::ThreadedRendering)
	{
		FinishRender(NULL);
		renderEnd.Set();
		rend_allow_rollback();
		pvrQueue.cancelEnqueue();
		// Needed for android where this function may be called
		// from a thread different from the UI one
		pvrQueue.enqueue(PvrMessageQueue::Stop);
	}
}

void rend_set_fb_write_addr(u32 fb_w_sof1)
{
	if (fb_w_sof1 & 0x1000000)
		// render to texture
		return;
	fb_w_cur = fb_w_sof1;
}

void rend_swap_frame(u32 fb_r_sof)
{
	if (!config::EmulateFramebuffer && fb_r_sof == fb_w_cur && rend_is_enabled())
		pvrQueue.enqueue(PvrMessageQueue::Present);
}

void rend_disable_rollback()
{
	vramRollback.Reset();
}

void rend_allow_rollback()
{
	vramRollback.Set();
}

void rend_start_rollback()
{
	if (config::ThreadedRendering)
		vramRollback.Wait();
}

void rend_enable_renderer(bool enabled) {
	rendererEnabled = enabled;
}

bool rend_is_enabled() {
	return rendererEnabled;
}

void rend_serialize(Serializer& ser)
{
	ser << fb_w_cur;
	ser << render_called;
	ser << fb_dirty;
	ser << fb_watch_addr_start;
	ser << fb_watch_addr_end;
}
void rend_deserialize(Deserializer& deser)
{
	deser >> fb_w_cur;
	if (deser.version() >= Deserializer::V20)
	{
		deser >> render_called;
		deser >> fb_dirty;
		deser >> fb_watch_addr_start;
		deser >> fb_watch_addr_end;
	}
	pend_rend = false;
	fbAddrHistory[0] = 1;
	fbAddrHistory[1] = 1;
}
