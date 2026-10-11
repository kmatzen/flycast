#pragma once
#include "types.h"
#include "sh4_if.h"
#include <cstring>

#define sh4op(str) void  DYNACALL str (Sh4Context *ctx, u32 op)

typedef void (DYNACALL OpCallFP) (Sh4Context *ctx, u32 op);
extern OpCallFP* OpPtr[0x10000];

typedef void OpDissasmFP(char* out,const char* const FormatString,u32 pc,u16 opcode);

enum sh4_eu
{
	MT,
	EX,
	BR,
	LS,
	FE,
	CO,
};

enum OpcodeType
{
	//basic
	Normal       = 0,   // Heh , nothing special :P
	ReadsPC      = 1,   // PC must be set upon calling it
	WritesPC     = 2,   // It will write PC (branch)
	Delayslot    = 4,   // Has a delayslot opcode , valid only when WritesPC is set

	WritesSR     = 8,   // Writes to SR , and UpdateSR needs to be called
	WritesFPSCR  = 16,  // Writes to FPSCR , and UpdateSR needs to be called
	Invalid      = 128, // Invalid

	UsesFPU      = 2048, // Floating point op
	FWritesFPSCR = UsesFPU | WritesFPSCR,

	// Heh, not basic :P
	ReadWritePC  = ReadsPC|WritesPC,     // Read and writes pc :P
	WritesSRRWPC = WritesSR|ReadsPC|WritesPC,

	// Branches (not delay slot):
	Branch_dir   = ReadWritePC,          // Direct (eg , pc=r[xx]) -- this one is ReadWritePC b/c the delayslot may use pc ;)
	Branch_rel   = ReadWritePC,          // Relative (rg pc+=10);

	// Delay slot
	Branch_dir_d = Delayslot|Branch_dir, // Direct (eg , pc=r[xx])
	Branch_rel_d = Delayslot|Branch_rel, // Relative (rg pc+=10);
};

std::string disassemble_op(const char* tx1, u32 pc, u16 opcode);

typedef void ( RecOpCallFP) (u32 op);
struct sh4_opcodelistentry
{
	RecOpCallFP* rec_oph;
	OpCallFP* oph;
	u32 mask;
	u32 rez;
	u32 type;
	const char* diss;
	u8 IssueCycles;
	u8 LatencyCycles;
	sh4_eu unit;
	u8 ex_type;
	u64 decode;

	void Disassemble(char* strout, u32 pc, u16 op) const
	{
		const char *disOp = diss;
		if (!strcmp(disOp, "missing"))
		{
			static char tmp[6];
			snprintf(tmp, sizeof(tmp), "?%04X", op);
			disOp = tmp;
		}

		std::string text = disassemble_op(disOp, pc, op);
		strcpy(strout, text.c_str());
	}

	bool SetPC() const
	{
		return (type & WritesPC)!=0;
	}

	bool NeedPC() const
	{
		return (type & ReadsPC)!=0;
	}

	bool SetSR() const
	{
		return (type & WritesSR)!=0;
	}

	bool SetFPSCR() const
	{
		return (type & WritesFPSCR)!=0;
	}

	bool IsFloatingPoint() const
	{
		return (type & UsesFPU) != 0;
	}

	bool IsIllegalFpuMode(u16 opcode, bool doublePrecision, bool pairTransfer) const
	{
		if (!IsFloatingPoint())
			return false;

		// FPSCR.PR=1 and FPSCR.SZ=1 is a reserved combination for FPU
		// operation instructions. FMOV is a data-transfer instruction: it
		// ignores PR and continues to use SZ to select a 32- or 64-bit move.
		const u16 lowNibble = opcode & 0xf;
		const bool fmov = (opcode & 0xf000) == 0xf000
				&& lowNibble >= 6 && lowNibble <= 0xc;
		if (doublePrecision && pairTransfer
				&& (opcode & 0xf000) == 0xf000 && !fmov)
			return true;

		const u16 unary = opcode & 0xf0ff;
		if (!doublePrecision)
		{
			// FCNVSD and FCNVDS exist only in double-precision mode.
			return unary == 0xf0ad || unary == 0xf0bd;
		}

		// These graphics/single-precision operations are undefined in PR=1.
		if (unary == 0xf07d || unary == 0xf08d || unary == 0xf09d
				|| unary == 0xf0ed || unary == 0xf0fd
				|| (opcode & 0xf00f) == 0xf00e
				|| opcode == 0xfbfd || opcode == 0xf3fd)
			return true;

		// Double arithmetic and comparisons require even-numbered DR operands.
		if ((opcode & 0xf000) == 0xf000 && (opcode & 0xf) <= 5)
			return ((opcode >> 8) & 1) != 0 || ((opcode >> 4) & 1) != 0;

		// Unary double-precision instructions and conversions name one DR.
		if (unary == 0xf02d || unary == 0xf03d || unary == 0xf04d
				|| unary == 0xf05d || unary == 0xf06d
				|| unary == 0xf0ad || unary == 0xf0bd)
			return ((opcode >> 8) & 1) != 0;

		return false;
	}

	bool ClearsFpuCause(u16 opcode) const
	{
		if (!IsFloatingPoint())
			return false;
		// FRCHG and FSCHG share the low-byte encoding used by FSCA/FTRV, but
		// they only change FPSCR mode bits and do not begin an FPU operation.
		if (opcode == 0xfbfd || opcode == 0xf3fd)
			return false;

		// Basic arithmetic and comparisons.
		if ((opcode & 0xf000) == 0xf000 && (opcode & 0xf) <= 5)
			return true;

		const u16 unary = opcode & 0xf0ff;
		return unary == 0xf02d || unary == 0xf03d
				|| unary == 0xf06d || unary == 0xf07d
				|| unary == 0xf0ad || unary == 0xf0bd
				|| unary == 0xf0ed || unary == 0xf0fd
				|| (opcode & 0xf00f) == 0xf00e;
	}

	bool IsIllegalDelaySlot(u16 opcode) const
	{
		// SH-4 forbids instructions which modify or address relative to PC in a
		// delay slot. SLEEP is also slot-illegal; its ReadWritePC classification
		// captures that restriction. LDTLB has no PC dependency but is separately
		// specified as slot-illegal.
		return NeedPC() || opcode == 0x0038;
	}

	bool IsPrivileged(u16 opcode) const
	{
		if (opcode == 0x0038 || opcode == 0x002b || opcode == 0x001b)
			return true; // LDTLB, RTE, SLEEP

		// SGR and DBR use the special-register STS/LDS encodings rather than
		// the ordinary control-register STC/LDC forms below. Unlike the other
		// special registers, both are privileged in every transfer form.
		switch (opcode & 0xf0ff)
		{
		case 0x003a: // STS SGR,Rn
		case 0x00fa: // STS DBR,Rn
		case 0x4032: // STS.L SGR,@-Rn
		case 0x40f2: // STS.L DBR,@-Rn
		case 0x4036: // LDS.L @Rn+,SGR
		case 0x40f6: // LDS.L @Rn+,DBR
		case 0x403a: // LDS Rn,SGR
		case 0x40fa: // LDS Rn,DBR
			return true;
		default:
			break;
		}

		const u16 form = opcode & 0xf00f;
		if (form != 0x0002 && form != 0x4003
				&& form != 0x400e && form != 0x4007)
			return false;

		// LDC/STC accesses are privileged except when the control register is
		// GBR (selector 1).
		return ((opcode >> 4) & 0xf) != 1;
	}
};

extern sh4_opcodelistentry* OpDesc[0x10000];

void DissasembleOpcode(u16 opcode,u32 pc,char* Dissasm);
enum DecParam
{
	// Constants
	PRM_PC_D8_x2,
	PRM_PC_D8_x4,
	PRM_ZERO,
	PRM_ONE,
	PRM_TWO,
	PRM_TWO_INV,
	PRM_ONE_F32,
	
	// imms
	PRM_SIMM8,
	PRM_UIMM8,

	// Direct registers
	PRM_R0,
	PRM_RN,
	PRM_RM,
	PRM_FRN,
	PRM_FRN_SZ, //single/double, selected bank
	PRM_FRM,
	PRM_FRM_SZ,
	PRM_FPN,    //float pair, 3 bits
	PRM_FVN,    //float quad, 2 bits
	PRM_FVM,    //float quad, 2 bits
	PRM_XMTRX,  //float matrix, 0 bits
	PRM_FRM_FR0,
	PRM_FPUL,
	PRM_SR_T,
	PRM_SR_STATUS,

	PRM_SREG,   //FPUL/FPSCR/MACH/MACL/PR/DBR/SGR
	PRM_CREG,   //SR/GBR/VBR/SSR/SPC/<RM_BANK>
	
	//reg/imm reg/reg
	PRM_RN_D4_x1,
	PRM_RN_D4_x2,
	PRM_RN_D4_x4,
	PRM_RN_R0,

	PRM_RM_R0,
	PRM_RM_D4_x1,
	PRM_RM_D4_x2,
	PRM_RM_D4_x4,

	PRM_GBR_D8_x1,
	PRM_GBR_D8_x2,
	PRM_GBR_D8_x4,
};

enum DecMode
{
	DM_ReadSRF,
	DM_BinaryOp,    //d=d op s
	DM_UnaryOp,     //d= op s
	DM_ReadM,       //d=readm(s);s+=e
	DM_WriteM,      //s-=e;writem(s,d);
	DM_fiprOp,
	DM_WriteTOp,    //T=d op s
	DM_DT,          //special case for dt
	DM_Shift,
	DM_Rot,
	DM_EXTOP,
	DM_MUL,
	DM_DIV0,
	DM_ADC,
	DM_NEGC,
};
