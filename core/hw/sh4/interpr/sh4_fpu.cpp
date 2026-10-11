#include "types.h"
#include <cfenv>
#include <cmath>
#include <cstring>
#include <limits>

#include "sh4_opcodes.h"
#include "hw/sh4/sh4_core.h"
#include "hw/sh4/sh4_rom.h"
#include "hw/sh4/sh4_mem.h"
#include "hw/sh4/sh4_mmr.h"
#include "hw/sh4/sh4_interpreter.h"

static u32 GetN(u32 op) {
	return (op >> 8) & 0xf;
}
static u32 GetM(u32 op) {
	return (op >> 4) & 0xf;
}

static double getDRn(Sh4Context *ctx, u32 op) {
	return ctx->getDR((op >> 9) & 7);
}
static double getDRm(Sh4Context *ctx, u32 op) {
	return ctx->getDR((op >> 5) & 7);
}
static void setDRn(Sh4Context *ctx, u32 op, double d) {
	ctx->setDR((op >> 9) & 7, d);
}

#include "hw/sh4/modules/mmu.h"
#undef GetN
#undef GetM

static inline void checkFpuDataAccess(Sh4Context *ctx, u32 addr, u32 size, bool write)
{
	if (sh4DataAddressError(addr, size, write))
	{
		CCN_TEA = addr;
		CCN_PTEH.VPN = addr >> 10;
		throw SH4ThrownException(ctx->pc - 2,
				write ? Sh4Ex_AddressErrorWrite : Sh4Ex_AddressErrorRead);
	}
}

static inline u32 readFpu32(Sh4Context *ctx, u32 addr)
{
	checkFpuDataAccess(ctx, addr, 4, false);
	const u32 data = ReadMem32(addr);
	Sh4Interpreter::RecordUbcOperandAccessIfActive(addr, 4, false, data);
	return data;
}

static inline u64 readFpu64(Sh4Context *ctx, u32 addr)
{
	checkFpuDataAccess(ctx, addr, 8, false);
	const u64 data = ReadMem64(addr);
	Sh4Interpreter::RecordUbcOperandAccessIfActive(addr, 8, false, data);
	return data;
}

static inline void writeFpu32(Sh4Context *ctx, u32 addr, u32 data)
{
	checkFpuDataAccess(ctx, addr, 4, true);
	WriteMem32(addr, data);
	Sh4Interpreter::RecordUbcOperandAccessIfActive(addr, 4, true, data);
}

static inline void writeFpu64(Sh4Context *ctx, u32 addr, u64 data)
{
	checkFpuDataAccess(ctx, addr, 8, true);
	WriteMem64(addr, data);
	Sh4Interpreter::RecordUbcOperandAccessIfActive(addr, 8, true, data);
}

static inline bool sh4NaN32(u32 value)
{
	return (value & 0x7f800000) == 0x7f800000
			&& (value & 0x007fffff) != 0;
}

static inline bool sh4SignalingNaN32(u32 value)
{
	return sh4NaN32(value) && (value & 0x00400000) != 0;
}

static inline bool sh4NaN64(u32 high, u32 low)
{
	return (high & 0x7ff00000) == 0x7ff00000
			&& ((high & 0x000fffff) != 0 || low != 0);
}

static inline bool sh4SignalingNaN64(u32 high, u32 low)
{
	return sh4NaN64(high, low) && (high & 0x00080000) != 0;
}

static inline void fpuInvalid(Sh4Context *ctx)
{
	ctx->fpscr.cinvalid = 1;
	ctx->fpscr.finvalidop = 1;
	if (ctx->fpscr.einvalidop)
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
}

static inline void fpuDivideByZero(Sh4Context *ctx)
{
	ctx->fpscr.cdivbyzero = 1;
	ctx->fpscr.fdivbyzero = 1;
	if (ctx->fpscr.edivbyzero)
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
}

static inline void fpuInexactPossibility(Sh4Context *ctx, bool actual)
{
	if (actual)
	{
		ctx->fpscr.cinexact = 1;
		ctx->fpscr.finexact = 1;
	}
	if (ctx->fpscr.einexact)
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
}

static inline void fpuInexact(Sh4Context *ctx)
{
	fpuInexactPossibility(ctx, true);
}

static inline void fpuDenormalInput(Sh4Context *ctx)
{
	ctx->fpscr.cfpuerr = 1;
	throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
}

static inline void fpuArithmeticExceptions(Sh4Context *ctx, bool overflow,
		bool underflow, bool inexact)
{
	if (overflow)
	{
		ctx->fpscr.coverflow = 1;
		ctx->fpscr.foverflow = 1;
	}
	if (underflow)
	{
		ctx->fpscr.cunderflow = 1;
		ctx->fpscr.funderflow = 1;
	}
	if (inexact)
	{
		ctx->fpscr.cinexact = 1;
		ctx->fpscr.finexact = 1;
	}

	// Arithmetic operations have possible O/U/I outcomes for every finite operation.
	// The SH-4 traps on an enabled possibility even if its Cause bit is clear.
	if (ctx->fpscr.eoverflow || ctx->fpscr.eunderflow || ctx->fpscr.einexact)
		throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
}

enum class BasicFpuOp { Add, Subtract, Multiply };

static void basicFpu32(Sh4Context *ctx, u32 n, u32 m, BasicFpuOp op)
{
	const u32 lhsBits = ctx->fr_hex(n);
	const u32 rhsBits = ctx->fr_hex(m);
	float lhs = ctx->fr[n];
	float rhs = ctx->fr[m];

	bool invalid = sh4SignalingNaN32(lhsBits) || sh4SignalingNaN32(rhsBits);
	if (op == BasicFpuOp::Add)
		invalid |= std::isinf(lhs) && std::isinf(rhs)
				&& std::signbit(lhs) != std::signbit(rhs);
	else if (op == BasicFpuOp::Subtract)
		invalid |= std::isinf(lhs) && std::isinf(rhs)
				&& std::signbit(lhs) == std::signbit(rhs);

	if (invalid)
	{
		fpuInvalid(ctx);
		fpuArithmeticExceptions(ctx, false, false, false);
		ctx->fr_hex(n) = 0x7fbfffff;
		return;
	}
	if (sh4NaN32(lhsBits) || sh4NaN32(rhsBits))
	{
		fpuArithmeticExceptions(ctx, false, false, false);
		ctx->fr_hex(n) = 0x7fbfffff;
		return;
	}

	if (std::fpclassify(lhs) == FP_SUBNORMAL
			|| std::fpclassify(rhs) == FP_SUBNORMAL)
	{
		if (!ctx->fpscr.DN)
			fpuDenormalInput(ctx);
		if (std::fpclassify(lhs) == FP_SUBNORMAL)
			lhs = std::copysign(0.f, lhs);
		if (std::fpclassify(rhs) == FP_SUBNORMAL)
			rhs = std::copysign(0.f, rhs);
	}

	if (op == BasicFpuOp::Multiply
			&& ((lhs == 0.f && std::isinf(rhs))
				|| (rhs == 0.f && std::isinf(lhs))))
	{
		fpuInvalid(ctx);
		fpuArithmeticExceptions(ctx, false, false, false);
		ctx->fr_hex(n) = 0x7fbfffff;
		return;
	}

	double exact;
	float result;
	switch (op)
	{
	case BasicFpuOp::Add: exact = (double)lhs + (double)rhs; result = lhs + rhs; break;
	case BasicFpuOp::Subtract: exact = (double)lhs - (double)rhs; result = lhs - rhs; break;
	case BasicFpuOp::Multiply: exact = (double)lhs * (double)rhs; result = lhs * rhs; break;
	}
	if (ctx->fpscr.DN && std::fpclassify(result) == FP_SUBNORMAL)
		result = std::copysign(0.f, result);
	const bool overflow = std::isfinite(exact)
			&& std::fabs(exact) > (double)std::numeric_limits<float>::max();
	const bool inexact = std::isfinite(exact) && (double)result != exact;
	const bool underflow = exact != 0.0 && std::isfinite(exact)
			&& std::fabs(exact) < (double)std::numeric_limits<float>::min()
			&& inexact;
	fpuArithmeticExceptions(ctx, overflow, underflow, inexact);
	ctx->fr[n] = fixNaN(result);
}

static void basicFpu64(Sh4Context *ctx, u32 opCode, BasicFpuOp op)
{
	const u32 n = GetN(opCode);
	const u32 m = GetM(opCode);
	const u32 lhsHigh = ctx->fr_hex(n);
	const u32 lhsLow = ctx->fr_hex(n + 1);
	const u32 rhsHigh = ctx->fr_hex(m);
	const u32 rhsLow = ctx->fr_hex(m + 1);
	double lhs = getDRn(ctx, opCode);
	double rhs = getDRm(ctx, opCode);

	bool invalid = sh4SignalingNaN64(lhsHigh, lhsLow)
			|| sh4SignalingNaN64(rhsHigh, rhsLow);
	if (op == BasicFpuOp::Add)
		invalid |= std::isinf(lhs) && std::isinf(rhs)
				&& std::signbit(lhs) != std::signbit(rhs);
	else if (op == BasicFpuOp::Subtract)
		invalid |= std::isinf(lhs) && std::isinf(rhs)
				&& std::signbit(lhs) == std::signbit(rhs);

	if (invalid)
	{
		fpuInvalid(ctx);
		fpuArithmeticExceptions(ctx, false, false, false);
		ctx->fr_hex(n) = 0x7ff7ffff;
		ctx->fr_hex(n + 1) = 0xffffffff;
		return;
	}
	if (sh4NaN64(lhsHigh, lhsLow) || sh4NaN64(rhsHigh, rhsLow))
	{
		fpuArithmeticExceptions(ctx, false, false, false);
		ctx->fr_hex(n) = 0x7ff7ffff;
		ctx->fr_hex(n + 1) = 0xffffffff;
		return;
	}

	if (std::fpclassify(lhs) == FP_SUBNORMAL
			|| std::fpclassify(rhs) == FP_SUBNORMAL)
	{
		if (!ctx->fpscr.DN)
			fpuDenormalInput(ctx);
		if (std::fpclassify(lhs) == FP_SUBNORMAL)
			lhs = std::copysign(0.0, lhs);
		if (std::fpclassify(rhs) == FP_SUBNORMAL)
			rhs = std::copysign(0.0, rhs);
	}

	if (op == BasicFpuOp::Multiply
			&& ((lhs == 0.0 && std::isinf(rhs))
				|| (rhs == 0.0 && std::isinf(lhs))))
	{
		fpuInvalid(ctx);
		fpuArithmeticExceptions(ctx, false, false, false);
		ctx->fr_hex(n) = 0x7ff7ffff;
		ctx->fr_hex(n + 1) = 0xffffffff;
		return;
	}

	std::feclearexcept(FE_ALL_EXCEPT);
	volatile double hostLhs = lhs;
	volatile double hostRhs = rhs;
	double result;
	switch (op)
	{
	case BasicFpuOp::Add: result = hostLhs + hostRhs; break;
	case BasicFpuOp::Subtract: result = hostLhs - hostRhs; break;
	case BasicFpuOp::Multiply: result = hostLhs * hostRhs; break;
	}
	const int hostExceptions = std::fetestexcept(FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT);
	if (ctx->fpscr.DN && std::fpclassify(result) == FP_SUBNORMAL)
		result = std::copysign(0.0, result);
	fpuArithmeticExceptions(ctx, (hostExceptions & FE_OVERFLOW) != 0,
			(hostExceptions & FE_UNDERFLOW) != 0,
			(hostExceptions & FE_INEXACT) != 0);
	setDRn(ctx, opCode, fixNaN64(result));
}

static bool compareFpu32(Sh4Context *ctx, u32 n, u32 m, bool greater)
{
	const u32 lhsBits = ctx->fr_hex(n);
	const u32 rhsBits = ctx->fr_hex(m);
	float lhs = ctx->fr[n];
	float rhs = ctx->fr[m];
	const bool lhsNaN = sh4NaN32(lhsBits);
	const bool rhsNaN = sh4NaN32(rhsBits);

	// FCMP/EQ is an unordered comparison and only signals for sNaN.
	// FCMP/GT signals invalid for either kind of NaN.
	if ((greater && (lhsNaN || rhsNaN))
			|| sh4SignalingNaN32(lhsBits) || sh4SignalingNaN32(rhsBits))
	{
		fpuInvalid(ctx);
		return false;
	}
	if (lhsNaN || rhsNaN)
		return false;

	if (std::fpclassify(lhs) == FP_SUBNORMAL
			|| std::fpclassify(rhs) == FP_SUBNORMAL)
	{
		if (!ctx->fpscr.DN)
			fpuDenormalInput(ctx);
		if (std::fpclassify(lhs) == FP_SUBNORMAL)
			lhs = std::copysign(0.f, lhs);
		if (std::fpclassify(rhs) == FP_SUBNORMAL)
			rhs = std::copysign(0.f, rhs);
	}
	return greater ? lhs > rhs : lhs == rhs;
}

static bool compareFpu64(Sh4Context *ctx, u32 opCode, bool greater)
{
	const u32 n = GetN(opCode);
	const u32 m = GetM(opCode);
	const u32 lhsHigh = ctx->fr_hex(n);
	const u32 lhsLow = ctx->fr_hex(n + 1);
	const u32 rhsHigh = ctx->fr_hex(m);
	const u32 rhsLow = ctx->fr_hex(m + 1);
	double lhs = getDRn(ctx, opCode);
	double rhs = getDRm(ctx, opCode);
	const bool lhsNaN = sh4NaN64(lhsHigh, lhsLow);
	const bool rhsNaN = sh4NaN64(rhsHigh, rhsLow);

	if ((greater && (lhsNaN || rhsNaN))
			|| sh4SignalingNaN64(lhsHigh, lhsLow)
			|| sh4SignalingNaN64(rhsHigh, rhsLow))
	{
		fpuInvalid(ctx);
		return false;
	}
	if (lhsNaN || rhsNaN)
		return false;

	if (std::fpclassify(lhs) == FP_SUBNORMAL
			|| std::fpclassify(rhs) == FP_SUBNORMAL)
	{
		if (!ctx->fpscr.DN)
			fpuDenormalInput(ctx);
		if (std::fpclassify(lhs) == FP_SUBNORMAL)
			lhs = std::copysign(0.0, lhs);
		if (std::fpclassify(rhs) == FP_SUBNORMAL)
			rhs = std::copysign(0.0, rhs);
	}
	return greater ? lhs > rhs : lhs == rhs;
}

struct GraphicsFpuResult
{
	float value;
	bool invalid;
	bool overflow;
	bool underflow;
};

static GraphicsFpuResult graphicsInnerProduct(Sh4Context *ctx,
		const float *lhsInput, const float *rhsInput)
{
	float lhs[4];
	float rhs[4];
	bool signalingNaN = false;
	bool quietNaN = false;
	bool zeroTimesInfinity = false;
	bool positiveInfinity = false;
	bool negativeInfinity = false;

	for (int i = 0; i < 4; ++i)
	{
		u32 lhsBits;
		u32 rhsBits;
		std::memcpy(&lhsBits, &lhsInput[i], sizeof(lhsBits));
		std::memcpy(&rhsBits, &rhsInput[i], sizeof(rhsBits));
		lhs[i] = lhsInput[i];
		rhs[i] = rhsInput[i];
		signalingNaN |= sh4SignalingNaN32(lhsBits) || sh4SignalingNaN32(rhsBits);
		quietNaN |= sh4NaN32(lhsBits) || sh4NaN32(rhsBits);

		if (ctx->fpscr.DN)
		{
			if (std::fpclassify(lhs[i]) == FP_SUBNORMAL)
				lhs[i] = std::copysign(0.f, lhs[i]);
			if (std::fpclassify(rhs[i]) == FP_SUBNORMAL)
				rhs[i] = std::copysign(0.f, rhs[i]);
		}

		zeroTimesInfinity |= (lhs[i] == 0.f && std::isinf(rhs[i]))
				|| (rhs[i] == 0.f && std::isinf(lhs[i]));
		if ((std::isinf(lhs[i]) || std::isinf(rhs[i]))
				&& lhs[i] != 0.f && rhs[i] != 0.f)
		{
			if (std::signbit(lhs[i]) != std::signbit(rhs[i]))
				negativeInfinity = true;
			else
				positiveInfinity = true;
		}
	}

	const bool invalid = signalingNaN || zeroTimesInfinity
			|| (!quietNaN && positiveInfinity && negativeInfinity);
	if (invalid || quietNaN)
	{
		u32 canonical = 0x7fbfffff;
		float value;
		std::memcpy(&value, &canonical, sizeof(value));
		return { value, invalid, false, false };
	}

	double sum = 0.0;
	for (int i = 0; i < 4; ++i)
		sum += (double)lhs[i] * (double)rhs[i];
	float value = (float)sum;
	if (ctx->fpscr.DN && std::fpclassify(value) == FP_SUBNORMAL)
		value = std::copysign(0.f, value);
	const bool overflow = std::isfinite(sum)
			&& std::fabs(sum) > (double)std::numeric_limits<float>::max();
	const bool underflow = sum != 0.0 && std::isfinite(sum)
			&& std::fabs(sum) < (double)std::numeric_limits<float>::min()
			&& (double)value != sum;
	return { fixNaN(value), false, overflow, underflow };
}

template<typename T>
static u32 ftrcResult(Sh4Context *ctx, T value)
{
	const bool nan = std::isnan(value);
	const bool negativeInvalid = nan || value < -2147483648.0;
	const bool positiveInvalid = value >= 2147483648.0;
	if (negativeInvalid || positiveInvalid)
	{
		// FTRC invalid operation: Cause.V records this instruction and Flag.V
		// remains sticky. With Enable.V set, FPUL must not be modified.
		ctx->fpscr.cinvalid = 1;
		ctx->fpscr.finvalidop = 1;
		if (ctx->fpscr.einvalidop)
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
		return positiveInvalid ? 0x7fffffff : 0x80000000;
	}
	return (u32)(s32)value;
}

static void iNimp(const char *str);

#define CHECK_FPU_32(v) v = fixNaN(v)

//fadd <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0000)
{
	if (ctx->fpscr.PR == 0)
		basicFpu32(ctx, GetN(op), GetM(op), BasicFpuOp::Add);
	else
		basicFpu64(ctx, op, BasicFpuOp::Add);
}

//fsub <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0001)
{
	if (ctx->fpscr.PR == 0)
		basicFpu32(ctx, GetN(op), GetM(op), BasicFpuOp::Subtract);
	else
		basicFpu64(ctx, op, BasicFpuOp::Subtract);
}
//fmul <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0010)
{
	if (ctx->fpscr.PR == 0)
		basicFpu32(ctx, GetN(op), GetM(op), BasicFpuOp::Multiply);
	else
		basicFpu64(ctx, op, BasicFpuOp::Multiply);
}
//fdiv <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0011)
{
	if (ctx->fpscr.PR == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);
		const u32 lhsBits = ctx->fr_hex(n);
		const u32 rhsBits = ctx->fr_hex(m);
		float lhs = ctx->fr[n];
		float rhs = ctx->fr[m];

		if (sh4SignalingNaN32(lhsBits) || sh4SignalingNaN32(rhsBits)
				|| (lhs == 0.f && rhs == 0.f)
				|| (std::isinf(lhs) && std::isinf(rhs)))
		{
			fpuInvalid(ctx);
			fpuArithmeticExceptions(ctx, false, false, false);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}
		if (sh4NaN32(lhsBits) || sh4NaN32(rhsBits))
		{
			fpuArithmeticExceptions(ctx, false, false, false);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}
		if (std::fpclassify(lhs) == FP_SUBNORMAL
				|| std::fpclassify(rhs) == FP_SUBNORMAL)
		{
			if (!ctx->fpscr.DN)
				fpuDenormalInput(ctx);
			if (std::fpclassify(lhs) == FP_SUBNORMAL)
				lhs = std::copysign(0.f, lhs);
			if (std::fpclassify(rhs) == FP_SUBNORMAL)
				rhs = std::copysign(0.f, rhs);
		}
		if (lhs == 0.f && rhs == 0.f)
		{
			fpuInvalid(ctx);
			fpuArithmeticExceptions(ctx, false, false, false);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}
		if (rhs == 0.f && std::isfinite(lhs) && lhs != 0.f)
			fpuDivideByZero(ctx);

		const double exact = (double)lhs / (double)rhs;
		float result = lhs / rhs;
		if (ctx->fpscr.DN && std::fpclassify(result) == FP_SUBNORMAL)
			result = std::copysign(0.f, result);
		const bool overflow = std::isfinite(exact)
				&& std::fabs(exact) > (double)std::numeric_limits<float>::max();
		const bool inexact = std::isfinite(exact) && (double)result != exact;
		const bool underflow = exact != 0.0 && std::isfinite(exact)
				&& std::fabs(exact) < (double)std::numeric_limits<float>::min()
				&& inexact;
		fpuArithmeticExceptions(ctx, overflow, underflow, inexact);
		ctx->fr[n] = fixNaN(result);
	}
	else
	{
		const u32 n = GetN(op);
		const u32 m = GetM(op);
		const u32 lhsHigh = ctx->fr_hex(n);
		const u32 lhsLow = ctx->fr_hex(n + 1);
		const u32 rhsHigh = ctx->fr_hex(m);
		const u32 rhsLow = ctx->fr_hex(m + 1);
		double lhs = getDRn(ctx, op);
		double rhs = getDRm(ctx, op);

		if (sh4SignalingNaN64(lhsHigh, lhsLow)
				|| sh4SignalingNaN64(rhsHigh, rhsLow)
				|| (lhs == 0.0 && rhs == 0.0)
				|| (std::isinf(lhs) && std::isinf(rhs)))
		{
			fpuInvalid(ctx);
			fpuArithmeticExceptions(ctx, false, false, false);
			ctx->fr_hex(n) = 0x7ff7ffff;
			ctx->fr_hex(n + 1) = 0xffffffff;
			return;
		}
		if (sh4NaN64(lhsHigh, lhsLow) || sh4NaN64(rhsHigh, rhsLow))
		{
			fpuArithmeticExceptions(ctx, false, false, false);
			ctx->fr_hex(n) = 0x7ff7ffff;
			ctx->fr_hex(n + 1) = 0xffffffff;
			return;
		}
		if (std::fpclassify(lhs) == FP_SUBNORMAL
				|| std::fpclassify(rhs) == FP_SUBNORMAL)
		{
			if (!ctx->fpscr.DN)
				fpuDenormalInput(ctx);
			if (std::fpclassify(lhs) == FP_SUBNORMAL)
				lhs = std::copysign(0.0, lhs);
			if (std::fpclassify(rhs) == FP_SUBNORMAL)
				rhs = std::copysign(0.0, rhs);
		}
		if (lhs == 0.0 && rhs == 0.0)
		{
			fpuInvalid(ctx);
			fpuArithmeticExceptions(ctx, false, false, false);
			ctx->fr_hex(n) = 0x7ff7ffff;
			ctx->fr_hex(n + 1) = 0xffffffff;
			return;
		}
		if (rhs == 0.0 && std::isfinite(lhs) && lhs != 0.0)
			fpuDivideByZero(ctx);

		std::feclearexcept(FE_ALL_EXCEPT);
		volatile double hostLhs = lhs;
		volatile double hostRhs = rhs;
		double result = fixNaN64(hostLhs / hostRhs);
		const int hostExceptions = std::fetestexcept(FE_OVERFLOW | FE_UNDERFLOW | FE_INEXACT);
		if (ctx->fpscr.DN && std::fpclassify(result) == FP_SUBNORMAL)
			result = std::copysign(0.0, result);
		fpuArithmeticExceptions(ctx, (hostExceptions & FE_OVERFLOW) != 0,
				(hostExceptions & FE_UNDERFLOW) != 0,
				(hostExceptions & FE_INEXACT) != 0);
		setDRn(ctx, op, result);
	}
}
//fcmp/eq <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0100)
{
	if (ctx->fpscr.PR == 0)
		ctx->sr.T = compareFpu32(ctx, GetN(op), GetM(op), false);
	else
		ctx->sr.T = compareFpu64(ctx, op, false);
}
//fcmp/gt <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_0101)
{
	if (ctx->fpscr.PR == 0)
		ctx->sr.T = compareFpu32(ctx, GetN(op), GetM(op), true);
	else
		ctx->sr.T = compareFpu64(ctx, op, true);
}
//All memory opcodes are here
//fmov.s @(R0,<REG_M>),<FREG_N>
sh4op(i1111_nnnn_mmmm_0110)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		ctx->fr_hex(n) = readFpu32(ctx, ctx->r[m] + ctx->r[0]);
	}
	else
	{
		u32 n = GetN(op)>>1;
		u32 m = GetM(op);
		if (((op >> 8) & 1) == 0)
			ctx->dr_hex(n) = readFpu64(ctx, ctx->r[m] + ctx->r[0]);
		else
			ctx->xd_hex(n) = readFpu64(ctx, ctx->r[m] + ctx->r[0]);
	}
}


//fmov.s <FREG_M>,@(R0,<REG_N>)
sh4op(i1111_nnnn_mmmm_0111)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		writeFpu32(ctx, ctx->r[0] + ctx->r[n], ctx->fr_hex(m));
	}
	else
	{
		u32 n = GetN(op);
		u32 m = GetM(op)>>1;
		if (((op >> 4) & 0x1) == 0)
			writeFpu64(ctx, ctx->r[n] + ctx->r[0], ctx->dr_hex(m));
		else
			writeFpu64(ctx, ctx->r[n] + ctx->r[0], ctx->xd_hex(m));
	}
}


//fmov.s @<REG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_1000)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);
		ctx->fr_hex(n) = readFpu32(ctx, ctx->r[m]);
	}
	else
	{
		u32 n = GetN(op)>>1;
		u32 m = GetM(op);
		if (((op >> 8) & 1) == 0)
			ctx->dr_hex(n) = readFpu64(ctx, ctx->r[m]);
		else
			ctx->xd_hex(n) = readFpu64(ctx, ctx->r[m]);
	}
}


//fmov.s @<REG_M>+,<FREG_N>
sh4op(i1111_nnnn_mmmm_1001)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		ctx->fr_hex(n) = readFpu32(ctx, ctx->r[m]);
		ctx->r[m] += 4;
	}
	else
	{
		u32 n = GetN(op)>>1;
		u32 m = GetM(op);
		if (((op >> 8) & 1) == 0)
			ctx->dr_hex(n) = readFpu64(ctx, ctx->r[m]);
		else
			ctx->xd_hex(n) = readFpu64(ctx, ctx->r[m]);
		ctx->r[m] += 8;
	}
}


//fmov.s <FREG_M>,@<REG_N>
sh4op(i1111_nnnn_mmmm_1010)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);
		writeFpu32(ctx, ctx->r[n], ctx->fr_hex(m));
	}
	else
	{
		u32 n = GetN(op);
		u32 m = GetM(op)>>1;

		if (((op >> 4) & 0x1) == 0)
			writeFpu64(ctx, ctx->r[n], ctx->dr_hex(m));
		else
			writeFpu64(ctx, ctx->r[n], ctx->xd_hex(m));
	}
}

//fmov.s <FREG_M>,@-<REG_N>
sh4op(i1111_nnnn_mmmm_1011)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);

		u32 addr = ctx->r[n] - 4;

		writeFpu32(ctx, addr, ctx->fr_hex(m));

		ctx->r[n] = addr;
	}
	else
	{
		u32 n = GetN(op);
		u32 m = GetM(op)>>1;

		u32 addr = ctx->r[n] - 8;
		if (((op >> 4) & 0x1) == 0)
			writeFpu64(ctx, addr, ctx->dr_hex(m));
		else
			writeFpu64(ctx, addr, ctx->xd_hex(m));

		ctx->r[n] = addr;
	}
}

//end of memory opcodes

//fmov <FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_1100)
{
	if (ctx->fpscr.SZ == 0)
	{
		u32 n = GetN(op);
		u32 m = GetM(op);
		ctx->fr[n] = ctx->fr[m];
	}
	else
	{
		u32 n = GetN(op)>>1;
		u32 m = GetM(op)>>1;
		switch ((op >> 4) & 0x11)
		{
			case 0x00:
				//dr[n] = dr[m];
				ctx->dr_hex(n) = ctx->dr_hex(m);
				break;

			case 0x01:
				//dr[n] = xd[m];
				ctx->dr_hex(n) = ctx->xd_hex(m);
				break;

			case 0x10:
				//xd[n] = dr[m];
				ctx->xd_hex(n) = ctx->dr_hex(m);
				break;

			case 0x11:
				//xd[n] = xd[m];
				ctx->xd_hex(n) = ctx->xd_hex(m);
				break;
		}
	}
}


//fabs <FREG_N>
sh4op(i1111_nnnn_0101_1101)
{
	int n=GetN(op);

	if (ctx->fpscr.PR == 0)
		ctx->fr_hex(n) &= 0x7FFFFFFF;
	else
		ctx->fr_hex(n & 0xE) &= 0x7FFFFFFF;

}

//FSCA FPUL, DRn//F0FD//1111_nnn0_1111_1101
sh4op(i1111_nnn0_1111_1101)
{
	int n=GetN(op) & 0xE;


	//cosine(x) = sine(pi/2 + x).
	if (ctx->fpscr.PR==0)
	{
		u32 pi_index = ctx->fpul & 0xFFFF;
		// FSCA is approximate and signals inexact for every input. An enabled
		// exception is taken before either destination register changes.
		fpuInexact(ctx);

	#ifdef NATIVE_FSCA
			float rads = pi_index / (65536.0f / 2) * float(M_PI);

			ctx->fr[n + 0] = sinf(rads);
			ctx->fr[n + 1] = cosf(rads);

			CHECK_FPU_32(ctx->fr[n]);
			CHECK_FPU_32(ctx->fr[n + 1]);
	#else
			ctx->fr[n + 0] = sin_table[pi_index].u[0];
			ctx->fr[n + 1] = sin_table[pi_index].u[1];
	#endif

	}
	else
		iNimp("FSCA : Double precision mode");
}

//FSRRA //1111_nnnn_0111_1101
sh4op(i1111_nnnn_0111_1101)
{
	const u32 n = GetN(op);
	if (ctx->fpscr.PR == 0)
	{
		const u32 bits = ctx->fr_hex(n);
		float value = ctx->fr[n];
		if (sh4SignalingNaN32(bits))
		{
			fpuInvalid(ctx);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}
		if (sh4NaN32(bits))
		{
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}

		if (std::fpclassify(value) == FP_SUBNORMAL)
		{
			if (ctx->fpscr.DN)
				value = std::copysign(0.f, value);
			else if (std::signbit(value))
			{
				fpuInvalid(ctx);
				ctx->fr_hex(n) = 0x7fbfffff;
				return;
			}
			else
				fpuDenormalInput(ctx);
		}

		if (value == 0.f)
		{
			fpuDivideByZero(ctx);
			ctx->fr[n] = std::copysign(std::numeric_limits<float>::infinity(), value);
			return;
		}
		if (std::signbit(value))
		{
			fpuInvalid(ctx);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}
		if (std::isinf(value))
		{
			ctx->fr[n] = 0.f;
			return;
		}

		// FSRRA is approximate and always signals inexact for a positive
		// normalized operand, irrespective of the host calculation's exactness.
		fpuInexact(ctx);
		ctx->fr[n] = fixNaN(1.f / sqrtf(value));
	}
	else
		iNimp("FSRRA : Double precision mode");
}

//fcnvds <DR_N>,FPUL
sh4op(i1111_nnnn_1011_1101)
{
	if (ctx->fpscr.PR == 1)
	{
		const u32 n = GetN(op);
		const u32 high = ctx->fr_hex(n);
		const u32 low = ctx->fr_hex(n + 1);
		double value = getDRn(ctx, op);

		if (sh4SignalingNaN64(high, low))
		{
			fpuInvalid(ctx);
			if (ctx->fpscr.eoverflow || ctx->fpscr.eunderflow || ctx->fpscr.einexact)
				throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
			ctx->fpul = 0x7fbfffff;
			return;
		}
		if (sh4NaN64(high, low))
		{
			if (ctx->fpscr.eoverflow || ctx->fpscr.eunderflow || ctx->fpscr.einexact)
				throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
			ctx->fpul = 0x7fbfffff;
			return;
		}
		if (std::fpclassify(value) == FP_SUBNORMAL)
		{
			if (!ctx->fpscr.DN)
				fpuDenormalInput(ctx);
			value = std::copysign(0.0, value);
		}

		const float result = (float)value;
		const bool finite = std::isfinite(value);
		const bool overflow = finite
				&& std::fabs(value) > (double)std::numeric_limits<float>::max();
		const bool inexact = finite && (double)result != value;
		const bool underflow = finite && value != 0.0
				&& std::fabs(value) < (double)std::numeric_limits<float>::min()
				&& inexact;

		if (overflow)
		{
			ctx->fpscr.coverflow = 1;
			ctx->fpscr.foverflow = 1;
		}
		if (underflow)
		{
			ctx->fpscr.cunderflow = 1;
			ctx->fpscr.funderflow = 1;
		}
		if (inexact)
		{
			ctx->fpscr.cinexact = 1;
			ctx->fpscr.finexact = 1;
		}

		// FCNVDS has possible O/U/I outcomes for every numeric input. The SH-4
		// takes an enabled exception even when the corresponding Cause bit is 0.
		if (ctx->fpscr.eoverflow || ctx->fpscr.eunderflow || ctx->fpscr.einexact)
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);

		std::memcpy(&ctx->fpul, &result, sizeof(result));
	}
	else
	{
		iNimp("FCNVDS: Single precision mode");
	}
}


//fcnvsd FPUL,<DR_N>
sh4op(i1111_nnnn_1010_1101)
{
	if (ctx->fpscr.PR == 1)
	{
		const u32 bits = ctx->fpul;
		float value;
		std::memcpy(&value, &bits, sizeof(value));
		const u32 n = GetN(op);

		if (sh4SignalingNaN32(bits))
		{
			fpuInvalid(ctx);
			ctx->fr_hex(n) = 0x7ff7ffff;
			ctx->fr_hex(n + 1) = 0xffffffff;
			return;
		}
		if (sh4NaN32(bits))
		{
			ctx->fr_hex(n) = 0x7ff7ffff;
			ctx->fr_hex(n + 1) = 0xffffffff;
			return;
		}
		if (std::fpclassify(value) == FP_SUBNORMAL)
		{
			if (!ctx->fpscr.DN)
				fpuDenormalInput(ctx);
			value = std::copysign(0.f, value);
		}
		setDRn(ctx, op, (double)value);
	}
	else
	{
		iNimp("FCNVSD: Single precision mode");
	}
}

//fipr <FV_M>,<FV_N>
sh4op(i1111_nnmm_1110_1101)
{
	int n = GetN(op) & 0xC;
	int m = (GetN(op) & 0x3) << 2;
	if (ctx->fpscr.PR == 0)
	{
		const GraphicsFpuResult result = graphicsInnerProduct(ctx,
				&ctx->fr[n], &ctx->fr[m]);
		// FIPR is approximate: I is always generated. V/O/U/I enables are
		// conservative possibility traps and suppress the destination write.
		ctx->fpscr.cinexact = 1;
		ctx->fpscr.finexact = 1;
		if (result.invalid)
		{
			ctx->fpscr.cinvalid = 1;
			ctx->fpscr.finvalidop = 1;
		}
		if (result.overflow)
		{
			ctx->fpscr.coverflow = 1;
			ctx->fpscr.foverflow = 1;
		}
		if (result.underflow)
		{
			ctx->fpscr.cunderflow = 1;
			ctx->fpscr.funderflow = 1;
		}
		if (ctx->fpscr.einvalidop || ctx->fpscr.eoverflow
				|| ctx->fpscr.eunderflow || ctx->fpscr.einexact)
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
		ctx->fr[n + 3] = result.value;
	}
	else {
		iNimp("FIPR with FPSCR.PR=1");
	}
}

//fldi0 <FREG_N>
sh4op(i1111_nnnn_1000_1101)
{
	if (ctx->fpscr.PR!=0)
		return;

	u32 n = GetN(op);

	ctx->fr[n] = 0.0f;

}

//fldi1 <FREG_N>
sh4op(i1111_nnnn_1001_1101)
{
	if (ctx->fpscr.PR!=0)
		return;

	u32 n = GetN(op);

	ctx->fr[n] = 1.0f;
}

//flds <FREG_N>,FPUL
sh4op(i1111_nnnn_0001_1101)
{
	u32 n = GetN(op);

	ctx->fpul = ctx->fr_hex(n);
}

//fsts FPUL,<FREG_N>
sh4op(i1111_nnnn_0000_1101)
{
	u32 n = GetN(op);
	ctx->fr_hex(n) = ctx->fpul;
}

//float FPUL,<FREG_N>
sh4op(i1111_nnnn_0010_1101)
{
	if (ctx->fpscr.PR == 0)
	{
		const u32 n = GetN(op);
		const s32 input = (s32)ctx->fpul;
		const float result = (float)input;
		if ((double)result != (double)input)
		{
			ctx->fpscr.cinexact = 1;
			ctx->fpscr.finexact = 1;
		}
		// FLOAT to single precision has a possible inexact result for every
		// input, so Enable.I traps even when Cause.I remains clear.
		if (ctx->fpscr.einexact)
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
		ctx->fr[n] = result;
	}
	else
	{
		setDRn(ctx, op, (double)(s32)ctx->fpul);
	}
}


//fneg <FREG_N>
sh4op(i1111_nnnn_0100_1101)
{
	u32 n = GetN(op);
	ctx->fr_hex(n) ^= 0x80000000;
}


//frchg
sh4op(i1111_1011_1111_1101)
{
 	ctx->fpscr.FR = 1 - ctx->fpscr.FR;

	Sh4Context::UpdateFPSCR(ctx);
}

//fschg
sh4op(i1111_0011_1111_1101)
{
	ctx->fpscr.SZ = 1 - ctx->fpscr.SZ;
}

//fsqrt <FREG_N>
sh4op(i1111_nnnn_0110_1101)
{
	if (ctx->fpscr.PR == 0)
	{
		const u32 n = GetN(op);
		const u32 bits = ctx->fr_hex(n);
		float value = ctx->fr[n];
		if (sh4SignalingNaN32(bits))
		{
			fpuInvalid(ctx);
			fpuInexactPossibility(ctx, false);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}
		if (sh4NaN32(bits))
		{
			fpuInexactPossibility(ctx, false);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}
		if (std::fpclassify(value) == FP_SUBNORMAL)
		{
			if (ctx->fpscr.DN)
				value = std::copysign(0.f, value);
			else if (std::signbit(value))
			{
				fpuInvalid(ctx);
				fpuInexactPossibility(ctx, false);
				ctx->fr_hex(n) = 0x7fbfffff;
				return;
			}
			else
				fpuDenormalInput(ctx);
		}
		if (std::signbit(value) && value != 0.f)
		{
			fpuInvalid(ctx);
			fpuInexactPossibility(ctx, false);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}
		const float result = sqrtf(value);
		const bool inexact = std::isfinite(value)
				&& (double)result * (double)result != (double)value;
		fpuInexactPossibility(ctx, inexact);
		ctx->fr[n] = result;
	}
	else
	{
		const u32 n = GetN(op);
		const u32 high = ctx->fr_hex(n);
		const u32 low = ctx->fr_hex(n + 1);
		double value = getDRn(ctx, op);
		if (sh4SignalingNaN64(high, low))
		{
			fpuInvalid(ctx);
			fpuInexactPossibility(ctx, false);
			ctx->fr_hex(n) = 0x7ff7ffff;
			ctx->fr_hex(n + 1) = 0xffffffff;
			return;
		}
		if (sh4NaN64(high, low))
		{
			fpuInexactPossibility(ctx, false);
			ctx->fr_hex(n) = 0x7ff7ffff;
			ctx->fr_hex(n + 1) = 0xffffffff;
			return;
		}
		if (std::fpclassify(value) == FP_SUBNORMAL)
		{
			if (ctx->fpscr.DN)
				value = std::copysign(0.0, value);
			else if (std::signbit(value))
			{
				fpuInvalid(ctx);
				fpuInexactPossibility(ctx, false);
				ctx->fr_hex(n) = 0x7ff7ffff;
				ctx->fr_hex(n + 1) = 0xffffffff;
				return;
			}
			else
				fpuDenormalInput(ctx);
		}
		if (std::signbit(value) && value != 0.0)
		{
			fpuInvalid(ctx);
			fpuInexactPossibility(ctx, false);
			ctx->fr_hex(n) = 0x7ff7ffff;
			ctx->fr_hex(n + 1) = 0xffffffff;
			return;
		}
		const double result = sqrt(value);
		const bool inexact = std::isfinite(value)
				&& (long double)result * (long double)result != (long double)value;
		fpuInexactPossibility(ctx, inexact);
		setDRn(ctx, op, result);
	}
}


//ftrc <FREG_N>, FPUL
sh4op(i1111_nnnn_0011_1101)
{
	if (ctx->fpscr.PR == 0)
	{
		u32 n = GetN(op);
		ctx->fpul = ftrcResult(ctx, ctx->fr[n]);
	}
	else
	{
		ctx->fpul = ftrcResult(ctx, getDRn(ctx, op));
	}
}


//fmac <FREG_0>,<FREG_M>,<FREG_N>
sh4op(i1111_nnnn_mmmm_1110)
{
	if (ctx->fpscr.PR==0)
	{
		const u32 n = GetN(op);
		const u32 m = GetM(op);
		const u32 multiplicandBits = ctx->fr_hex(0);
		const u32 multiplierBits = ctx->fr_hex(m);
		const u32 addendBits = ctx->fr_hex(n);
		float multiplicand = ctx->fr[0];
		float multiplier = ctx->fr[m];
		float addend = ctx->fr[n];

		if (sh4SignalingNaN32(multiplicandBits)
				|| sh4SignalingNaN32(multiplierBits)
				|| sh4SignalingNaN32(addendBits))
		{
			fpuInvalid(ctx);
			fpuArithmeticExceptions(ctx, false, false, false);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}

		if (ctx->fpscr.DN)
		{
			if (std::fpclassify(multiplicand) == FP_SUBNORMAL)
				multiplicand = std::copysign(0.f, multiplicand);
			if (std::fpclassify(multiplier) == FP_SUBNORMAL)
				multiplier = std::copysign(0.f, multiplier);
			if (std::fpclassify(addend) == FP_SUBNORMAL)
				addend = std::copysign(0.f, addend);
		}

		// FMAC checks zero-times-infinity before quiet-NaN propagation.
		if ((multiplicand == 0.f && std::isinf(multiplier))
				|| (multiplier == 0.f && std::isinf(multiplicand)))
		{
			fpuInvalid(ctx);
			fpuArithmeticExceptions(ctx, false, false, false);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}
		if (sh4NaN32(multiplicandBits) || sh4NaN32(multiplierBits)
				|| sh4NaN32(addendBits))
		{
			fpuArithmeticExceptions(ctx, false, false, false);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}

		if ((std::isinf(multiplicand) || std::isinf(multiplier))
				&& std::isinf(addend)
				&& (std::signbit(multiplicand) != std::signbit(multiplier))
					!= std::signbit(addend))
		{
			fpuInvalid(ctx);
			fpuArithmeticExceptions(ctx, false, false, false);
			ctx->fr_hex(n) = 0x7fbfffff;
			return;
		}

		if (!ctx->fpscr.DN
				&& (std::fpclassify(multiplicand) == FP_SUBNORMAL
					|| std::fpclassify(multiplier) == FP_SUBNORMAL
					|| std::fpclassify(addend) == FP_SUBNORMAL))
			fpuDenormalInput(ctx);

		const double exact = std::fma((double)multiplicand,
				(double)multiplier, (double)addend);
		float result = std::fma(multiplicand, multiplier, addend);
		if (ctx->fpscr.DN && std::fpclassify(result) == FP_SUBNORMAL)
			result = std::copysign(0.f, result);
		const bool overflow = std::isfinite(exact)
				&& std::fabs(exact) > (double)std::numeric_limits<float>::max();
		const bool inexact = std::isfinite(exact) && (double)result != exact;
		const bool underflow = exact != 0.0 && std::isfinite(exact)
				&& std::fabs(exact) < (double)std::numeric_limits<float>::min()
				&& inexact;
		fpuArithmeticExceptions(ctx, overflow, underflow, inexact);
		ctx->fr[n] = fixNaN(result);
	}
	else
	{
		iNimp("fmac <DREG_0>,<DREG_M>,<DREG_N>");
	}
}


//ftrv xmtrx,<FV_N>
sh4op(i1111_nn01_1111_1101)
{
	/*
	XF[0] XF[4] XF[8] XF[12]    FR[n]      FR[n]
	XF[1] XF[5] XF[9] XF[13]  *	FR[n+1] -> FR[n+1]
	XF[2] XF[6] XF[10] XF[14]   FR[n+2]    FR[n+2]
	XF[3] XF[7] XF[11] XF[15]   FR[n+3]    FR[n+3]
	*/

	u32 n = GetN(op) & 0xC;

	if (ctx->fpscr.PR==0)
	{
		float matrixRows[4][4];
		for (int row = 0; row < 4; ++row)
			for (int column = 0; column < 4; ++column)
				matrixRows[row][column] = ctx->xf[row + column * 4];
		GraphicsFpuResult results[4];
		bool invalid = false;
		bool overflow = false;
		bool underflow = false;
		for (int row = 0; row < 4; ++row)
		{
			results[row] = graphicsInnerProduct(ctx, matrixRows[row], &ctx->fr[n]);
			invalid |= results[row].invalid;
			overflow |= results[row].overflow;
			underflow |= results[row].underflow;
		}

		// FTRV has the same approximate/conservative exception contract as
		// FIPR, applied atomically to all four destination lanes.
		ctx->fpscr.cinexact = 1;
		ctx->fpscr.finexact = 1;
		if (invalid)
		{
			ctx->fpscr.cinvalid = 1;
			ctx->fpscr.finvalidop = 1;
		}
		if (overflow)
		{
			ctx->fpscr.coverflow = 1;
			ctx->fpscr.foverflow = 1;
		}
		if (underflow)
		{
			ctx->fpscr.cunderflow = 1;
			ctx->fpscr.funderflow = 1;
		}
		if (ctx->fpscr.einvalidop || ctx->fpscr.eoverflow
				|| ctx->fpscr.eunderflow || ctx->fpscr.einexact)
			throw SH4ThrownException(ctx->pc - 2, Sh4Ex_FpuError);
		for (int row = 0; row < 4; ++row)
			ctx->fr[n + row] = results[row].value;
	}
	else {
		iNimp("FTRV with FPSCR.PR=1");
	}
}

static void iNimp(const char *str)
{
	WARN_LOG(INTERPRETER, "Unimplemented SH4 FPU op: %s", str);
}
