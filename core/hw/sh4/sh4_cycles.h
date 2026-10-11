/*
	Copyright 2023 flyinghead

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
#pragma once
#include "types.h"
#include "sh4_opcode_list.h"
#include "sh4_if.h"
#include "sh4_sched.h"

class Sh4Cycles
{
public:
	Sh4Cycles(int cpuRatio = 1) : cpuRatio(cpuRatio) {}

	void init(Sh4Context *ctx) {
		this->ctx = ctx;
	}

	void executeCycles(u16 op)
	{
		ctx->cycle_counter -= countCycles(op);
	}

	void addCycles(int cycles) const
	{
		ctx->cycle_counter -= cycles;
	}

	void addReadAccessCycles(u32 addr, u32 size) const;
	void addWriteAccessCycles(u32 addr, u32 size) const;

	int countCycles(u16 op);

	void reset()
	{
		lastUnit = CO;
		lastOpcode = 0;
	}

	u64 now() {
		return sh4_sched_now64() + sh4_sched_cpu_slice() - ctx->cycle_counter;
	}

	int readAccessCycles(u32 addr, u32 size) const;
	int writeAccessCycles(u32 addr, u32 size) const;
	int dmaReadAccessCycles(u32 addr, u32 size) const;
	int dmaWriteAccessCycles(u32 addr, u32 size) const;

private:
	// Returns the number of external cycles (100 MHz) needed for a sized read at the given address
	static int readExternalAccessCycles(u32 addr, u32 size);
	// Returns the number of external cycles (100 MHz) needed for a sized write at the given address
	static int writeExternalAccessCycles(u32 addr, u32 size);

	sh4_eu lastUnit = CO;
	u16 lastOpcode = 0;
	const int cpuRatio;
	Sh4Context *ctx = nullptr;
};
