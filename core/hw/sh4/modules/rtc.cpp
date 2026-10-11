#include "hw/hwreg.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_sched.h"
#include "hw/sh4/sh4_interrupts.h"
#include "serialize.h"
#include "modules.h"

RTCRegisters rtc;

static constexpr int RTC_TICK_CYCLES = SH4_MAIN_CLOCK / 256;
static int rtc_sched = -1;
static u8 rtc_subtick;
static u16 rtc_period_phase;

static u8 fromBcd(u8 value) { return (value >> 4) * 10 + (value & 0xf); }
static u8 toBcd(u8 value) { return (value / 10) << 4 | value % 10; }

static bool isLeapYear(unsigned year)
{
	return year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);
}

static unsigned daysInMonth(unsigned year, unsigned month)
{
	static constexpr u8 days[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
	if (month < 1 || month > 12)
		return 31;
	return days[month - 1] + (month == 2 && isLeapYear(year));
}

static void updateInterrupts()
{
	InterruptPend(sh4_RTC_ATI, (RTC_RCR1 & 0x09) == 0x09);
	InterruptPend(sh4_RTC_PRI, (RTC_RCR2 & 0x70) != 0 && (RTC_RCR2 & 0x80) != 0);
	InterruptPend(sh4_RTC_CUI, (RTC_RCR1 & 0x90) == 0x90);
}

static bool alarmFieldMatches(u8 alarm, u8 counter)
{
	return (alarm & 0x80) == 0 || (alarm & 0x7f) == counter;
}

static void checkAlarm()
{
	const bool anyFieldEnabled = ((RTC_RSECAR | RTC_RMINAR | RTC_RHRAR
			| RTC_RWKAR | RTC_RDAYAR | RTC_RMONAR) & 0x80) != 0;
	if (anyFieldEnabled && alarmFieldMatches(RTC_RSECAR, RTC_RSECCNT)
			&& alarmFieldMatches(RTC_RMINAR, RTC_RMINCNT)
			&& alarmFieldMatches(RTC_RHRAR, RTC_RHRCNT)
			&& alarmFieldMatches(RTC_RWKAR, RTC_RWKCNT)
			&& alarmFieldMatches(RTC_RDAYAR, RTC_RDAYCNT)
			&& alarmFieldMatches(RTC_RMONAR, RTC_RMONCNT))
		RTC_RCR1 |= 1;
}

static void incrementMinute()
{
	unsigned minute = fromBcd(RTC_RMINCNT) + 1;
	if (minute < 60) {
		RTC_RMINCNT = toBcd(minute);
		return;
	}
	RTC_RMINCNT = 0;
	unsigned hour = fromBcd(RTC_RHRCNT) + 1;
	if (hour < 24) {
		RTC_RHRCNT = toBcd(hour);
		return;
	}
	RTC_RHRCNT = 0;
	RTC_RWKCNT = (RTC_RWKCNT + 1) % 7;
	unsigned year = fromBcd(RTC_RYRCNT >> 8) * 100 + fromBcd(RTC_RYRCNT);
	unsigned month = fromBcd(RTC_RMONCNT);
	unsigned day = fromBcd(RTC_RDAYCNT) + 1;
	if (day <= daysInMonth(year, month)) {
		RTC_RDAYCNT = toBcd(day);
		return;
	}
	RTC_RDAYCNT = 1;
	if (++month <= 12) {
		RTC_RMONCNT = toBcd(month);
		return;
	}
	RTC_RMONCNT = 1;
	year = (year + 1) % 10000;
	RTC_RYRCNT = toBcd(year / 100) << 8 | toBcd(year % 100);
}

static void incrementSecond()
{
	unsigned second = fromBcd(RTC_RSECCNT) + 1;
	if (second < 60)
		RTC_RSECCNT = toBcd(second);
	else {
		RTC_RSECCNT = 0;
		incrementMinute();
	}
	RTC_RCR1 |= 0x80;
	checkAlarm();
	updateInterrupts();
}

static void advanceRtcTick()
{
	static constexpr u16 periodicTicks[8] = { 0, 1, 4, 16, 64, 128, 256, 512 };
	rtc_period_phase = (rtc_period_phase + 1) & 0x1ff;
	const unsigned pes = (RTC_RCR2 >> 4) & 7;
	if (pes != 0) {
		if (rtc_period_phase % periodicTicks[pes] == 0)
			RTC_RCR2 |= 0x80;
	}

	// R64CNT is a seven-stage divider fed at 128 Hz; its bit 6 rising edge
	// clocks the calendar once per second.
	if (++rtc_subtick == 2) {
		rtc_subtick = 0;
		const u8 old = RTC_R64CNT;
		RTC_R64CNT = (old + 1) & 0x7f;
		if ((old & 0x40) == 0 && (RTC_R64CNT & 0x40) != 0 && (RTC_RCR2 & 1) != 0)
			incrementSecond();
	}
	updateInterrupts();
}

static int rtcTick(int, int, int jitter, void *)
{
	// Scheduler callbacks normally arrive on the requested edge, but an
	// indivisible instruction may retire after it. Consume every oscillator
	// edge crossed by that instruction and preserve the fractional phase.
	const int elapsedTicks = 1 + jitter / RTC_TICK_CYCLES;
	for (int i = 0; i < elapsedTicks; ++i)
		advanceRtcTick();
	return elapsedTicks * RTC_TICK_CYCLES;
}

static void writeRCR1(u32, u8 data)
{
	const u8 old = RTC_RCR1;
	RTC_RCR1 = (data & 0x98) | (old & data & 1);
	updateInterrupts();
}

static void writeRCR2(u32, u8 data)
{
	const bool wasEnabled = (RTC_RCR2 & 8) != 0;
	RTC_RCR2 = (data & 0x79) | (data & 0x80);
	if ((data & 2) != 0) {
		RTC_R64CNT = 0;
		rtc_subtick = 0;
		rtc_period_phase = 0;
	}
	if ((data & 4) != 0) {
		if (fromBcd(RTC_RSECCNT) >= 30)
			incrementMinute();
		RTC_RSECCNT = 0;
		RTC_R64CNT = 0;
		rtc_subtick = 0;
	}
	const bool enabled = (RTC_RCR2 & 8) != 0;
	if (enabled != wasEnabled)
		sh4_sched_request(rtc_sched, enabled ? RTC_TICK_CYCLES : -1);
	updateInterrupts();
}

void RTCRegisters::init()
{
	super::init();
	rtc_sched = sh4_sched_register(0, rtcTick);

	// NAOMI reads from at least RTC_R64CNT

	//RTC R64CNT 0xFFC80000 0x1FC80000 8 Held Held Held Held Pclk
	setReadOnly<RTC_R64CNT_addr, u8>();

	//RTC RSECCNT H'FFC8 0004 H'1FC8 0004 8 Held Held Held Held Pclk
	setRW<RTC_RSECCNT_addr, u8, 0x7f>();

	//RTC RMINCNT H'FFC8 0008 H'1FC8 0008 8 Held Held Held Held Pclk
	setRW<RTC_RMINCNT_addr, u8, 0x7f>();

	//RTC RHRCNT H'FFC8 000C H'1FC8 000C 8 Held Held Held Held Pclk
	setRW<RTC_RHRCNT_addr, u8, 0x3f>();

	//RTC RWKCNT H'FFC8 0010 H'1FC8 0010 8 Held Held Held Held Pclk
	setRW<RTC_RWKCNT_addr, u8, 0x07>();

	//RTC RDAYCNT H'FFC8 0014 H'1FC8 0014 8 Held Held Held Held Pclk
	setRW<RTC_RDAYCNT_addr, u8, 0x3f>();

	//RTC RMONCNT H'FFC8 0018 H'1FC8 0018 8 Held Held Held Held Pclk
	setRW<RTC_RMONCNT_addr, u8, 0x1f>();

	//RTC RYRCNT H'FFC8 001C H'1FC8 001C 16 Held Held Held Held Pclk
	setRW<RTC_RYRCNT_addr, u16>();
	
	//RTC RSECAR H'FFC8 0020 H'1FC8 0020 8 Held *2 Held Held Held Pclk
	setRW<RTC_RSECAR_addr, u8>();
	
	//RTC RMINAR H'FFC8 0024 H'1FC8 0024 8 Held *2 Held Held Held Pclk
	setRW<RTC_RMINAR_addr, u8>();

	//RTC RHRAR H'FFC8 0028 H'1FC8 0028 8 Held *2 Held Held Held Pclk
	setRW<RTC_RHRAR_addr, u8, 0xbf>();

	//RTC RWKAR H'FFC8 002C H'1FC8 002C 8 Held *2 Held Held Held Pclk
	setRW<RTC_RWKAR_addr, u8, 0x87>();

	//RTC RDAYAR H'FFC8 0030 H'1FC8 0030 8 Held *2 Held Held Held Pclk
	setRW<RTC_RDAYAR_addr, u8, 0xbf>();

	//RTC RMONAR H'FFC8 0034 H'1FC8 0034 8 Held *2 Held Held Held Pclk
	setRW<RTC_RMONAR_addr, u8, 0x9f>();

	//RTC RCR1 H'FFC8 0038 H'1FC8 0038 8 H'00*2 H'00*2 Held Held Pclk
	setWriteHandler<RTC_RCR1_addr, u8>(writeRCR1);

	//RTC RCR2 H'FFC8 003C H'1FC8 003C 8 H'09*2 H'00*2 Held Held Pclk
	setWriteHandler<RTC_RCR2_addr, u8>(writeRCR2);

	reset(true);
}

void RTCRegisters::reset(bool hard)
{
	/*
	RTC R64CNT H'FFC8 0000 H'1FC8 0000 8 Held Held Held Held Pclk
	RTC RSECCNT H'FFC8 0004 H'1FC8 0004 8 Held Held Held Held Pclk
	RTC RMINCNT H'FFC8 0008 H'1FC8 0008 8 Held Held Held Held Pclk
	RTC RHRCNT H'FFC8 000C H'1FC8 000C 8 Held Held Held Held Pclk
	RTC RWKCNT H'FFC8 0010 H'1FC8 0010 8 Held Held Held Held Pclk
	RTC RDAYCNT H'FFC8 0014 H'1FC8 0014 8 Held Held Held Held Pclk
	RTC RMONCNT H'FFC8 0018 H'1FC8 0018 8 Held Held Held Held Pclk
	RTC RYRCNT H'FFC8 001C H'1FC8 001C 16 Held Held Held Held Pclk
	RTC RSECAR H'FFC8 0020 H'1FC8 0020 8 Held *2 Held Held Held Pclk
	RTC RMINAR H'FFC8 0024 H'1FC8 0024 8 Held *2 Held Held Held Pclk
	RTC RHRAR H'FFC8 0028 H'1FC8 0028 8 Held *2 Held Held Held Pclk
	RTC RWKAR H'FFC8 002C H'1FC8 002C 8 Held *2 Held Held Held Pclk
	RTC RDAYAR H'FFC8 0030 H'1FC8 0030 8 Held *2 Held Held Held Pclk
	RTC RMONAR H'FFC8 0034 H'1FC8 0034 8 Held *2 Held Held Held Pclk
	RTC RCR1 H'FFC8 0038 H'1FC8 0038 8 H'00*2 H'00*2 Held Held Pclk
	RTC RCR2 H'FFC8 003C H'1FC8 003C 8 H'09*2 H'00*2 Held Held Pclk
	*/
	if (hard) {
		super::reset();
		RTC_RCR2 = 9;
		rtc_subtick = 0;
		rtc_period_phase = 0;
		sh4_sched_request(rtc_sched, RTC_TICK_CYCLES);
	}
	else {
		// Calendar, alarm, and divider registers are held across manual reset;
		// only the two control registers take their manual-reset values.
		RTC_RCR1 = 0;
		RTC_RCR2 = 0;
		sh4_sched_request(rtc_sched, -1);
	}
	updateInterrupts();
}

void RTCRegisters::term()
{
	super::term();
	sh4_sched_unregister(rtc_sched);
	rtc_sched = -1;
}

void RTCRegisters::serialize(Serializer& ser)
{
	ser << rtc_subtick;
	ser << rtc_period_phase;
	sh4_sched_serialize(ser, rtc_sched);
}

void RTCRegisters::deserialize(Deserializer& deser)
{
	if (deser.version() >= Deserializer::V70) {
		deser >> rtc_subtick;
		deser >> rtc_period_phase;
		sh4_sched_deserialize(deser, rtc_sched);
	}
	else {
		rtc_subtick = 0;
		rtc_period_phase = 0;
		sh4_sched_request(rtc_sched, (RTC_RCR2 & 8) != 0 ? RTC_TICK_CYCLES : -1);
	}
	updateInterrupts();
}
