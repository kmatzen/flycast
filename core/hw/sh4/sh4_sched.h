#pragma once
#include "types.h"

#define SH4_TIMESLICE 448	// at 112 Bangai-O doesn't start. 224 is ok

/*
	tag, as passed on sh4_sched_register
	sch_cycles, the cycle duration that the callback requested (sh4_sched_request)
	jitter, the number of cycles that the callback was delayed, [0... 448]
*/
typedef int sh4_sched_callback(int tag, int sch_cycl, int jitter, void *arg);

/*
	Register a callback to the scheduler. The returned id
	is used for sh4_sched_request and sh4_sched_unregister calls
*/
int sh4_sched_register(int tag, sh4_sched_callback* ssc, void *arg = nullptr);

/***
 * Unregister a callback from the scheduler.
 */
void sh4_sched_unregister(int id);

/*
	current time, in SH4 cycles, referenced to boot.
	Does not wrap, 64 bits.
*/
u64 sh4_sched_now64();

/* Current CPU execution budget, shortened to the next scheduler deadline. */
int sh4_sched_cpu_slice();
void sh4_sched_set_cpu_slice(int cycles);

/*
	Schedule a callback to be called sh4 *cycles* after the
	invocation of this function. *Cycles* range is (0, 200M].
	
	Passing a value of -1 disables the callback.
	If called multiple times, only the last call is in effect
*/
void sh4_sched_request(int id, int cycles);

/*
	Returns true if the callback is scheduled to be called in the future.
 */
bool sh4_sched_is_scheduled(int id);

/* Cycles from the live CPU position to a callback, or -1 if disabled. */
int sh4_sched_cycles_until(int id);

/*
	Tick for *cycles*
*/
void sh4_sched_tick(int cycles);

void sh4_sched_ffts();
void sh4_sched_reset(bool hard);

void sh4_sched_serialize(Serializer& ser);
void sh4_sched_deserialize(Deserializer& deser);
void sh4_sched_serialize(Serializer& ser, int id);
void sh4_sched_deserialize(Deserializer& deser, int id);

constexpr u32 sh4CyclesForXfer(u32 sizeUnit, u32 unitPerSecond) {
	// Completion cannot be observed until the final fractional SH-4 cycle has
	// elapsed. This also keeps small transfers from collapsing to zero cycles.
	return ((u64)SH4_MAIN_CLOCK * sizeUnit + unitPerSecond - 1) / unitPerSecond;
}

template<u32 Frequency>
class FixedFrequencyClock
{
public:
	static_assert(Frequency > 0 && Frequency <= SH4_MAIN_CLOCK);
	static constexpr u32 BaseCycles = SH4_MAIN_CLOCK / Frequency;
	static constexpr u32 CycleRemainder = SH4_MAIN_CLOCK % Frequency;
	static constexpr u32 InitialPhase = CycleRemainder == 0
			? 0 : Frequency - CycleRemainder;

	int nextCycles()
	{
		phase_ += CycleRemainder;
		if (phase_ >= Frequency)
		{
			phase_ -= Frequency;
			return BaseCycles + 1;
		}
		return BaseCycles;
	}

	void reset() { phase_ = InitialPhase; }
	u32 phase() const { return phase_; }
	void setPhase(u32 phase) { phase_ = phase % Frequency; }

private:
	u32 phase_ = InitialPhase;
};
