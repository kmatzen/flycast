#pragma once
#include "types.h"
#include "sh4_cycles.h"

class Sh4Interpreter : public Sh4Executor
{
public:
	void Run() override;
	void ResetCache() override  {}
	void Start() override;
	void Stop() override;
	void Step() override;
	void Reset(bool hard) override;
	void Init() override;
	void Term() override;
	bool IsCpuRunning() override;
	void ExecuteDelayslot();
	void ExecuteDelayslot(u32 delayedPr);
	void ExecuteDelayslot_RTE();
	void RecordUbcOperandAccess(u32 address, u32 size, bool write, u64 data,
			bool hasData = true);
	static void RecordUbcOperandAccessIfActive(u32 address, u32 size, bool write,
			u64 data, bool hasData = true)
	{
		if (Instance != nullptr)
			Instance->RecordUbcOperandAccess(address, size, write, data, hasData);
	}
	static u32 ExecuteUbcFallback(Sh4Context *ctx, u16 op, u32 pc);
	static void ExecuteFallbackHandler(Sh4Context *ctx, u16 op, OpCallFP *handler);
	Sh4Context *getContext() { return ctx; }

	static Sh4Interpreter *Instance;

protected:
	Sh4Context *ctx = nullptr;

private:
	void ExecuteOpcode(u16 op, bool chargeIssueCycles = true,
			bool checkPreExecutionBreak = false);
	u16 ReadNexOp(bool checkPreExecutionBreak = true);

	Sh4Cycles sh4cycles{CPU_RATIO};
	unsigned instructionDepth = 0;
	bool deferredUserBreak = false;
	bool deferredOperandBreak = false;
	bool dynarecUbcFallback = false;
	bool suppressUserBreak = false;
	// SH4 underclock factor when using the interpreter so that it's somewhat usable
#ifdef STRICT_MODE
	static constexpr int CPU_RATIO = 1;
#else
	static constexpr int CPU_RATIO = 8;
#endif
};
