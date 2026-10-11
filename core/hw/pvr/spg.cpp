#include "spg.h"
#include "hw/holly/holly_intc.h"
#include "hw/holly/sb.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_core.h"
#include "hw/maple/maple_if.h"
#include "serialize.h"
#include "network/ggpo.h"
#include "hw/pvr/Renderer_if.h"
#include "stdclass.h"
#include <array>

#ifdef TEST_AUTOMATION
#include "input/gamepad_device.h"
#endif

//SPG emulation; Scanline/Raster beam registers & interrupts

static u32 clc_pvr_scanline;
static u32 pvr_numscanlines = 512;
static u32 prv_cur_scanline = -1;

#if !defined(NDEBUG) || defined(DEBUGFAST)
static u32 vblk_cnt;
static u64 last_fps;
#endif

// 27 mhz pixel clock
constexpr int PIXEL_CLOCK = 27 * 1000 * 1000;
static u32 Line_Cycles;
static u32 Frame_Cycles;
static u32 line_cycle_base;
static u32 line_cycle_remainder;
static u32 line_cycle_denominator = 1;
static u32 line_cycle_phase;
static u32 current_line_cycles;
static u32 hblankCompareCounter;
static bool hblankFiredThisLine;
static u64 spgLastUpdateCycle;
int render_end_schid = -1;
int vblank_schid = -1;

static std::array<double, 4> real_times;
static std::array<u64, 4> cpu_cycles;
static u32 cpu_time_idx;
bool SH4FastEnough;
u32 fskip;

static u32 lightgun_line = 0xffff;
static u32 lightgun_hpos;
static bool maple_int_pending;
static int getNextSpgInterrupt();

static u64 spgNow()
{
	return sh4_sched_now64() + sh4_sched_cpu_slice()
			- p_sh4rcb->cntx.cycle_counter;
}

#ifdef LIBRETRO
extern bool retro_refresh_av_info(double fps);
#endif

void CalculateSync()
{
	u32 pixel_clock = PIXEL_CLOCK / (FB_R_CTRL.vclk_div ? 1 : 2);

	// We need to calculate the pixel clock

	pvr_numscanlines = SPG_LOAD.vcount + 1;

	const u64 numerator = (u64)SH4_MAIN_CLOCK * (SPG_LOAD.hcount + 1);
	line_cycle_denominator = pixel_clock * (SPG_CONTROL.interlace ? 2 : 1);
	line_cycle_base = numerator / line_cycle_denominator;
	line_cycle_remainder = numerator % line_cycle_denominator;
	line_cycle_phase = line_cycle_remainder == 0
			? 0 : line_cycle_denominator - line_cycle_remainder;
	auto nextLineCycles = []() {
		line_cycle_phase += line_cycle_remainder;
		u32 cycles = line_cycle_base;
		if (line_cycle_phase >= line_cycle_denominator)
		{
			line_cycle_phase -= line_cycle_denominator;
			cycles++;
		}
		return cycles;
	};
	current_line_cycles = nextLineCycles();
	Line_Cycles = line_cycle_base;
	Frame_Cycles = (numerator * pvr_numscanlines + line_cycle_denominator - 1)
			/ line_cycle_denominator;
	prv_cur_scanline = 0;
	clc_pvr_scanline = 0;
	hblankCompareCounter = 0;
	hblankFiredThisLine = false;
	spgLastUpdateCycle = spgNow();

	sh4_sched_request(vblank_schid, getNextSpgInterrupt());

#ifdef LIBRETRO
	retro_refresh_av_info((double)SH4_MAIN_CLOCK * line_cycle_denominator
			/ ((double)numerator * pvr_numscanlines));
#endif
}

static u32 cyclesToScanlines(u32 count)
{
	verify(count > 0);
	u64 cycles = current_line_cycles - clc_pvr_scanline;
	u32 phase = line_cycle_phase;
	for (u32 i = 1; i < count; ++i)
	{
		phase += line_cycle_remainder;
		cycles += line_cycle_base;
		if (phase >= line_cycle_denominator)
		{
			phase -= line_cycle_denominator;
			cycles++;
		}
	}
	return cycles;
}

static u32 horizontalCompareCycles(u32 lineCycles, u32 position, u32 total)
{
	// A compare becomes true on the first integral SH-4 cycle at or after the
	// programmed pixel boundary. Rounding down would expose the interrupt while
	// the projected horizontal counter is still on the preceding pixel.
	return ((u64)lineCycles * position + total - 1) / total;
}

static int getNextSpgInterrupt()
{
	const u32 horizontalTotal = SPG_LOAD.hcount + 1;
	if (!hblankFiredThisLine && SPG_HBLANK_INT.hblank_in_interrupt < horizontalTotal)
	{
		const u32 hblankCycles = horizontalCompareCycles(current_line_cycles,
				SPG_HBLANK_INT.hblank_in_interrupt, horizontalTotal);
		const bool enabledThisLine = SPG_HBLANK_INT.hblank_int_mode == 1
				|| SPG_HBLANK_INT.hblank_int_mode == 2
				|| (SPG_HBLANK_INT.hblank_int_mode == 0
						&& prv_cur_scanline == SPG_HBLANK_INT.line_comp_val);
		if (enabledThisLine && clc_pvr_scanline <= hblankCycles)
			return hblankCycles - clc_pvr_scanline;
	}
	if (SPG_HBLANK_INT.hblank_int_mode == 1
			|| SPG_HBLANK_INT.hblank_int_mode == 2)
		return cyclesToScanlines(1);

	u32 min_scanline = prv_cur_scanline + 1;
	u32 min_active = pvr_numscanlines;

	if (min_scanline <= SPG_VBLANK_INT.vblank_in_interrupt_line_number)
		min_active = std::min(min_active, SPG_VBLANK_INT.vblank_in_interrupt_line_number);

	if (min_scanline <= SPG_VBLANK_INT.vblank_out_interrupt_line_number)
		min_active = std::min(min_active, SPG_VBLANK_INT.vblank_out_interrupt_line_number);

	if (min_scanline <= SPG_VBLANK.vstart)
		min_active = std::min(min_active, SPG_VBLANK.vstart);

	if (min_scanline <= SPG_VBLANK.vbend)
		min_active = std::min(min_active, SPG_VBLANK.vbend);

	if (lightgun_line != 0xffff && min_scanline <= lightgun_line)
		min_active = std::min(min_active, lightgun_line);

	if (SPG_HBLANK_INT.hblank_int_mode == 0 && min_scanline <= SPG_HBLANK_INT.line_comp_val)
		min_active = std::min(min_active, SPG_HBLANK_INT.line_comp_val);

	min_active = std::max(min_active, min_scanline);

	return cyclesToScanlines(min_active - prv_cur_scanline);
}

static void raiseHblankInterrupt()
{
	switch (SPG_HBLANK_INT.hblank_int_mode)
	{
	case 0:
		if (prv_cur_scanline == SPG_HBLANK_INT.line_comp_val)
			asic_RaiseInterrupt(holly_HBLank);
		break;
	case 1:
		if (hblankCompareCounter == SPG_HBLANK_INT.line_comp_val)
		{
			asic_RaiseInterrupt(holly_HBLank);
			hblankCompareCounter = 0;
		}
		else
			hblankCompareCounter++;
		break;
	case 2:
		asic_RaiseInterrupt(holly_HBLank);
		break;
	default:
		INFO_LOG(PVR, "Invalid HBLANK INT mode");
		break;
	}
	hblankFiredThisLine = true;
}

static bool counterInRange(u32 value, u32 start, u32 end)
{
	return start <= end ? value >= start && value < end
			: value >= start || value < end;
}

u32 spgReadStatus()
{
	SPG_STATUS_type status = SPG_STATUS;
	u64 cycleInLine = (u64)clc_pvr_scanline
			+ spgNow() - spgLastUpdateCycle;
	u32 scanline = prv_cur_scanline;
	u32 lineCycles = std::max(1u, current_line_cycles);
	u32 phase = line_cycle_phase;
	while (cycleInLine >= lineCycles)
	{
		cycleInLine -= lineCycles;
		scanline = (scanline + 1) % pvr_numscanlines;
		phase += line_cycle_remainder;
		lineCycles = line_cycle_base;
		if (phase >= line_cycle_denominator)
		{
			phase -= line_cycle_denominator;
			lineCycles++;
		}
		lineCycles = std::max(1u, lineCycles);
	}
	const u32 horizontalTotal = SPG_LOAD.hcount + 1;
	const u32 hcount = (u64)cycleInLine * horizontalTotal / lineCycles;
	const bool hblank = counterInRange(hcount, SPG_HBLANK.hstart, SPG_HBLANK.hbend);
	const bool vblank = counterInRange(scanline, SPG_VBLANK.vstart,
			SPG_VBLANK.vbend);
	status.scanline = scanline;
	status.hsync = hcount < SPG_WIDTH.hswidth;
	status.vsync = scanline < SPG_WIDTH.vswidth;
	status.blank = hblank || vblank;
	return status.full;
}

static void syncSpgRaster()
{
	const u64 now = spgNow();
	u64 elapsed = now - spgLastUpdateCycle;
	if (elapsed == 0)
		return;

	clc_pvr_scanline += elapsed;
	while (clc_pvr_scanline >= current_line_cycles)
	{
		clc_pvr_scanline -= current_line_cycles;
		line_cycle_phase += line_cycle_remainder;
		current_line_cycles = line_cycle_base;
		if (line_cycle_phase >= line_cycle_denominator)
		{
			line_cycle_phase -= line_cycle_denominator;
			current_line_cycles++;
		}
		current_line_cycles = std::max(1u, current_line_cycles);
		prv_cur_scanline = (prv_cur_scanline + 1) % pvr_numscanlines;
		hblankFiredThisLine = false;
	}
	SPG_STATUS.scanline = prv_cur_scanline;
	spgLastUpdateCycle = now;
}

void rescheduleSPG(bool hblankConfigChanged)
{
	// Register writes can occur many scanlines after the last scheduled event.
	// Anchor the new compare deadline to the beam position at the write cycle.
	syncSpgRaster();
	if (hblankConfigChanged && SPG_HBLANK_INT.hblank_int_mode == 1)
		hblankCompareCounter = 0;
	sh4_sched_request(vblank_schid, getNextSpgInterrupt());
}

static int spg_line_sched(int tag, int cycles, int jitter, void *arg)
{
	clc_pvr_scanline += cycles + jitter;
	spgLastUpdateCycle = spgNow();

	while (clc_pvr_scanline >= current_line_cycles)
	{
		const u32 horizontalTotal = SPG_LOAD.hcount + 1;
		const u32 hblankCycles = horizontalCompareCycles(current_line_cycles,
				SPG_HBLANK_INT.hblank_in_interrupt, horizontalTotal);
		if (!hblankFiredThisLine && SPG_HBLANK_INT.hblank_in_interrupt < horizontalTotal
				&& clc_pvr_scanline >= hblankCycles)
			raiseHblankInterrupt();
		clc_pvr_scanline -= current_line_cycles;
		line_cycle_phase += line_cycle_remainder;
		current_line_cycles = line_cycle_base;
		if (line_cycle_phase >= line_cycle_denominator)
		{
			line_cycle_phase -= line_cycle_denominator;
			current_line_cycles++;
		}
		prv_cur_scanline = (prv_cur_scanline + 1) % pvr_numscanlines;
		hblankFiredThisLine = false;
		
		if (SPG_VBLANK_INT.vblank_in_interrupt_line_number == prv_cur_scanline)
		{
			if (maple_int_pending)
			{
				maple_int_pending = false;
				SB_MDST = 0;
			}
			asic_RaiseInterrupt(holly_SCANINT1);
		}

		if (SPG_VBLANK_INT.vblank_out_interrupt_line_number == prv_cur_scanline)
		{
			maple_vblank();
			asic_RaiseInterrupt(holly_SCANINT2);
		}

		if (SPG_VBLANK.vstart == prv_cur_scanline)
			SPG_STATUS.vsync = 1;

		if (SPG_VBLANK.vbend == prv_cur_scanline)
			SPG_STATUS.vsync = 0;

		SPG_STATUS.scanline = prv_cur_scanline;
		
		// Vblank
		if (prv_cur_scanline == 0)
		{
			if (SPG_CONTROL.interlace)
				SPG_STATUS.fieldnum = ~SPG_STATUS.fieldnum;
			else
				SPG_STATUS.fieldnum = 0;

			rend_vblank();

			u64 now = getTimeMs();
			cpu_time_idx = (cpu_time_idx + 1) % cpu_cycles.size();
			if (cpu_cycles[cpu_time_idx] != 0)
			{
				u32 cycle_span = (u32)(sh4_sched_now64() - cpu_cycles[cpu_time_idx]);
				u64 time_span = now - real_times[cpu_time_idx];
				float cpu_speed = ((float)cycle_span / time_span) / (SH4_MAIN_CLOCK / 100000);
				SH4FastEnough = cpu_speed >= 85.f;
			}
			else {
				SH4FastEnough = false;
			}
			cpu_cycles[cpu_time_idx] = sh4_sched_now64();
			real_times[cpu_time_idx] = now;

#ifdef TEST_AUTOMATION
			replay_input();
#endif

#if !defined(NDEBUG) || defined(DEBUGFAST)
			vblk_cnt++;
			if (getTimeMs() - last_fps >= 2000)
			{
				static int Last_FC;
				double ts = ((double)getTimeMs() - last_fps) / 1000.0;
				double spd_fps = (FrameCount - Last_FC) / ts;
				double spd_vbs = vblk_cnt / ts;
				double spd_cpu = spd_vbs * Frame_Cycles;
				spd_cpu /= 1000000.0;	//mrhz kthx
				double fullvbs = (spd_vbs / spd_cpu) * 200.0;

				Last_FC=FrameCount;

				vblk_cnt=0;

				const char* mode=0;
				const char* res=0;

				res = SPG_CONTROL.interlace ? "480i" : "240p";

				if (SPG_CONTROL.isPAL())
					mode = "PAL";
				else if (SPG_CONTROL.isNTSC())
					mode = "NTSC";
				else
				{
					res = SPG_CONTROL.interlace ? "480i" : "480p";
					mode = "VGA";
				}

				double frames_done=spd_cpu/2;
				double mspdf=1/frames_done*1000;

				double full_rps = spd_fps + fskip / ts;

				INFO_LOG(COMMON, "SPG - %4.2f - %4.2f - V: %4.2f (%.2f, %s%s%4.2f) R: %4.2f+%4.2f",
					mspdf, spd_cpu * 100 / 200, spd_vbs,
					spd_vbs / full_rps, mode, res, fullvbs,
					spd_fps, fskip / ts);
				
				fskip = 0;
				last_fps = getTimeMs();
			}
#endif
		}
		if (lightgun_line != 0xffff && lightgun_line == prv_cur_scanline)
		{
			maple_int_pending = false;
			SPG_TRIGGER_POS = ((lightgun_line & 0x3FF) << 16) | (lightgun_hpos & 0x3FF);
			SB_MDST = 0;
			lightgun_line = 0xffff;
		}
	}
	const u32 horizontalTotal = SPG_LOAD.hcount + 1;
	const u32 hblankCycles = horizontalCompareCycles(current_line_cycles,
			SPG_HBLANK_INT.hblank_in_interrupt, horizontalTotal);
	if (!hblankFiredThisLine && SPG_HBLANK_INT.hblank_in_interrupt < horizontalTotal
			&& clc_pvr_scanline >= hblankCycles)
		raiseHblankInterrupt();

	// Raster state was advanced by the actual elapsed time (cycles + jitter),
	// so getNextSpgInterrupt() is already relative to "now". The scheduler
	// normally subtracts jitter from callback return values; add it here to
	// avoid compensating the same lateness twice.
	return getNextSpgInterrupt() + jitter;
}

void read_lightgun_position(int x, int y)
{
	static u32 flip;
	maple_int_pending = true;
	if (y < 0 || y >= 480 || x < 0 || x >= 640)
	{
		// Off screen
		lightgun_line = 0xffff;
	}
	else
	{
		lightgun_line = y / (SPG_CONTROL.interlace ? 2 : 1) + SPG_VBLANK_INT.vblank_out_interrupt_line_number;
		// For some reason returning the same position twice makes it register off screen.
		// Atomiswave Clay Challenge wants more wiggle than others for shots to register.
		lightgun_line ^= (flip >> 1) & 1;
		lightgun_hpos = (x + 286) ^ (flip & 1);
		flip++;
	}
	// The gun signal can arrive after the previously scheduled VBlank event.
	// Re-anchor the beam and install its target line as an active deadline.
	rescheduleSPG();
}

bool spg_Init()
{
	render_end_schid = sh4_sched_register(0, &rend_end_render);
	vblank_schid = sh4_sched_register(0, &spg_line_sched);

	return true;
}

void spg_Term()
{
	sh4_sched_unregister(render_end_schid);
	render_end_schid = -1;
	sh4_sched_unregister(vblank_schid);
	vblank_schid = -1;
}

void spg_Reset(bool hard)
{
	if (render_end_schid != -1)
		sh4_sched_request(render_end_schid, -1);
	maple_int_pending = false;
	lightgun_line = 0xffff;
	lightgun_hpos = 0;
	CalculateSync();

	SH4FastEnough = false;
	cpu_time_idx = 0;
	cpu_cycles.fill(0);
	real_times.fill(0.0);
}

void scheduleRenderDone(TA_context *cntx)
{
	int cycles = 4096;
	if (cntx != nullptr)
	{
		if (settings.platform.isNaomi2()) {
			cycles = 1500000;
		}
		else
		{
			int size = 0;
			for (TA_context *c = cntx; c != nullptr; c = c->nextContext)
				size += c->tad.thd_data - c->tad.thd_root;
			cycles = std::min(450000 + size * 100, 1500000);
		}
	}
	sh4_sched_request(render_end_schid, cycles);
}

void cancelRenderDone()
{
	if (render_end_schid != -1)
		sh4_sched_request(render_end_schid, -1);
}

void spg_Serialize(Serializer& ser)
{
	ser << clc_pvr_scanline;
	ser << maple_int_pending;
	ser << pvr_numscanlines;
	ser << prv_cur_scanline;
	ser << Line_Cycles;
	ser << Frame_Cycles;
	ser << lightgun_line;
	ser << lightgun_hpos;
	ser << line_cycle_base;
	ser << line_cycle_remainder;
	ser << line_cycle_denominator;
	ser << line_cycle_phase;
	ser << current_line_cycles;
	ser << hblankCompareCounter;
	ser << hblankFiredThisLine;
	ser << spgLastUpdateCycle;
}
void spg_Deserialize(Deserializer& deser)
{
	if (deser.version() < Deserializer::V30)
		deser.skip<u32>(); // in_vblank
	deser >> clc_pvr_scanline;
	deser >> maple_int_pending;
	deser >> pvr_numscanlines;
	deser >> prv_cur_scanline;
	deser >> Line_Cycles;
	deser >> Frame_Cycles;
	deser >> lightgun_line;
	deser >> lightgun_hpos;
	if (deser.version() >= Deserializer::V63)
	{
		deser >> line_cycle_base;
		deser >> line_cycle_remainder;
		deser >> line_cycle_denominator;
		deser >> line_cycle_phase;
		deser >> current_line_cycles;
	}
	else
	{
		line_cycle_base = Line_Cycles;
		line_cycle_remainder = 0;
		line_cycle_denominator = 1;
		line_cycle_phase = 0;
		current_line_cycles = Line_Cycles;
	}
	if (deser.version() >= Deserializer::V96)
		deser >> hblankCompareCounter;
	else
		hblankCompareCounter = 0;
	if (deser.version() >= Deserializer::V97)
		deser >> hblankFiredThisLine;
	else
		hblankFiredThisLine = false;
	if (deser.version() >= Deserializer::V98)
		deser >> spgLastUpdateCycle;
	else
		spgLastUpdateCycle = spgNow();
}
