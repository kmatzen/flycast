/*
	Copyright 2020 flyinghead

	This file is part of Flycast.

    Flycast is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    Flycast is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with Flycast.  If not, see <https://www.gnu.org/licenses/>.
*/

#include "mainui.h"
#include "hw/pvr/Renderer_if.h"
#include "gui.h"
#include "oslib/oslib.h"
#include "wsi/context.h"
#include "cfg/option.h"
#include "emulator.h"
#include "imgui_driver.h"
#include "profiler/fc_profiler.h"
#include "oslib/i18n.h"
#include "rend/soft/SoftwareRenderer.h"

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstdlib>

static bool mainui_enabled;
u32 MainFrameCount;
static bool forceReinit;

bool mainui_rend_frame()
{
	FC_PROFILE_SCOPE;

	os_DoEvents();
	os_UpdateInputState();

	if (gui_is_open())
	{
		try {
			gui_display_ui();
		} catch (const FlycastException& e) {
			// Assume this is a graphics API issue
			forceReinit = true;
			return false;
		}
#ifndef TARGET_IPHONE
		std::this_thread::sleep_for(std::chrono::milliseconds(16));
#endif
	}
	else
	{
		try {
			if (!emu.render())
				return false;
			if (config::ProfilerEnabled && config::ProfilerDrawToGUI)
				gui_display_profiler();
		} catch (const RendererException& e) {
			gui_error(i18n::Ts("Renderer error:") + "\n" + e.what() + "\n\n"
					+ i18n::Ts("The game has been paused but it is recommended to restart Flycast"));
			rend_term_renderer();
			if (!rend_init_renderer())
				ERROR_LOG(RENDERER, "Renderer re-initialization failed");
			gui_open_settings();
			if (std::getenv("FLYCAST_PVR_TEST_RENDER_ERROR") != nullptr)
				std::fprintf(stderr, "PVR_LIFECYCLE renderer_error_recovered running=%u decoupled=%u\n",
						emu.running(), renderer != nullptr && renderer->RendersOnEmulationThread());
			return false;
		} catch (const FlycastException& e) {
			gui_stop_game(e.what());
			return false;
		}
	}
	MainFrameCount++;

	return true;
}

void mainui_init()
{
	if (!rend_init_renderer()) {
		ERROR_LOG(RENDERER, "Renderer initialization failed");
		gui_error(i18n::T("Renderer initialization failed.\nPlease select a different graphics API"));
	}
}

void mainui_term()
{
	rend_term_renderer();
}

void mainui_loop(bool forceStart)
{
	ThreadName _("Flycast-rend");
	if (forceStart)
		mainui_enabled = true;
	mainui_init();
	RenderType currentRenderer = config::RendererType;
	// Explicit lifecycle regression hook, restricted below to the experimental
	// decoupled renderer. Never schedules a reinit without a valid opt-in value.
	u32 testReinitFrame = 0;
	const bool testPausedReinit = std::getenv("FLYCAST_PVR_TEST_PAUSED_REINIT") != nullptr;
	if (const char *value = std::getenv("FLYCAST_PVR_TEST_REINIT_FRAME"))
	{
		char *end = nullptr;
		const unsigned long parsed = std::strtoul(value, &end, 10);
		if (end != value && *end == '\0' && parsed > 0 && parsed <= 0xffffffffUL)
			testReinitFrame = static_cast<u32>(parsed);
	}

	while (mainui_enabled)
	{
		fc_profiler::startThread("main");

		if (mainui_rend_frame() && imguiDriver != nullptr)
		{
			try {
				imguiDriver->present();
			} catch (const FlycastException& e) {
				forceReinit = true;
			}
		}
		if (imguiDriver == nullptr)
			forceReinit = true;
		bool testingReinit = testReinitFrame != 0 && MainFrameCount >= testReinitFrame
				&& renderer != nullptr && renderer->RendersOnEmulationThread() && emu.running();
		std::vector<u8> testRetainedImage;
		int testRetainedWidth = 0, testRetainedHeight = 0;
		if (testingReinit)
		{
			std::fprintf(stderr, "PVR_LIFECYCLE reinit_begin frame=%u running=1\n", MainFrameCount);
			if (testPausedReinit)
			{
				emu.stopAndWait();
				renderer->GetLastFrame(testRetainedImage, testRetainedWidth, testRetainedHeight);
			}
			testReinitFrame = 0;
			forceReinit = true;
		}

		if (config::RendererType != currentRenderer || forceReinit)
		{
			const bool resumeAfterReinit = renderer != nullptr
					&& renderer->RendersOnEmulationThread() && emu.running();
			const auto retainedPresentation = rend::soft::retainPresentation(renderer);
			mainui_term();
			int prevApi = isOpenGL(currentRenderer) || isSoftware(currentRenderer) ? 0 : isVulkan(currentRenderer) ? 1 : currentRenderer == RenderType::DirectX9 ? 2 : 3;
			int newApi = isOpenGL(config::RendererType) || isSoftware(config::RendererType) ? 0 : isVulkan(config::RendererType) ? 1 : config::RendererType == RenderType::DirectX9 ? 2 : 3;
			if (newApi != prevApi || forceReinit)
			{
				try {
					switchRenderApi();
				} catch (const FlycastException& e) {
					ERROR_LOG(RENDERER, "switchRenderApi failed: %s", e.what());
					if (prevApi == newApi)
						// fatal
						throw;
					// try to go back to the previous API
					config::RendererType = currentRenderer;
					try {
						switchRenderApi();
					} catch (const FlycastException& e) {
						ERROR_LOG(RENDERER, "Falling back to previous renderer also failed: %s", e.what());
						// fatal
						throw;
					}
				}
			}
			mainui_init();
			rend::soft::restorePresentation(renderer, retainedPresentation);
			if (resumeAfterReinit)
				emu.start();
			if (testingReinit && testPausedReinit)
			{
				std::vector<u8> restoredImage;
				const bool captured = renderer != nullptr && renderer->GetLastFrame(
						restoredImage, testRetainedWidth, testRetainedHeight);
				const bool drawn = renderer != nullptr && imguiDriver != nullptr && renderer->RenderLastFrame();
				if (drawn)
				{
					imguiDriver->setFrameRendered();
					imguiDriver->present();
				}
				std::fprintf(stderr, "PVR_LIFECYCLE paused_reinit running=%u retained=%u drawn=%u\n",
						emu.running(), captured && !testRetainedImage.empty() && restoredImage == testRetainedImage, drawn);
				emu.start(); // Explicit test resume, not the normal recreation path.
			}
			forceReinit = false;
			currentRenderer = config::RendererType;
			if (testingReinit)
				std::fprintf(stderr, "PVR_LIFECYCLE reinit_complete running=%u decoupled=%u\n",
						emu.running(), renderer != nullptr && renderer->RendersOnEmulationThread());
		}

		fc_profiler::endThread(config::ProfilerFrameWarningTime);
	}

	mainui_term();
}

void mainui_start()
{
	mainui_enabled = true;
}

void mainui_stop()
{
	mainui_enabled = false;
}

void mainui_reinit()
{
	forceReinit = true;
}
