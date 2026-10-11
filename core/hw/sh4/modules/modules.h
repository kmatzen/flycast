#pragma once
#include "hw/hwreg.h"
#include <deque>

extern u32 UBC[9];
extern u32 BSC[19];
extern u32 CPG[5];
extern u32 RTC[16];
extern u32 INTC[5];
extern u32 TMU[12];
extern u32 SCI[8];
extern u32 SCIF[10];

class UBCRegisters : public RegisterBank<UBC, 9>
{
	using super = RegisterBank<UBC, 9>;

public:
	void init();
	void reset(bool hard);
	void serialize(Serializer& ser);
	void deserialize(Deserializer& deser);
};
extern UBCRegisters ubc;

// Tests the instruction-access portion of the two UBC channels. A true return
// requests an architectural user-break exception at the selected pre/post
// execution boundary and also updates BRCR.CMFA/CMFB.
bool UBC_InstructionBreakEnabled();
bool UBC_BreakEnabled();
bool UBC_SequenceArmed();
void UBC_RestoreSequenceArmed(bool armed);
bool UBC_InstructionBreakPending(u32 address, bool afterExecution);
bool UBC_InstructionBreak(u32 address, bool afterExecution);
bool UBC_OperandBreak(u32 address, u32 size, bool write, u64 data, bool hasData = true);

class BSCRegisters : public RegisterBank<BSC, 19>
{
	using super = RegisterBank<BSC, 19>;

public:
	void init();
	void reset(bool hard = true);
	void term();
	void serialize(Serializer& ser);
	void deserialize(Deserializer& deser);
	int sdramReadCycles(u32 addr);
	int sdramWriteCycles(u32 addr);
	int sdramDmaReadCycles(u32 addr);
	int sdramDmaWriteCycles(u32 addr);
	int takeRefreshBusCycles();
	void servicePendingRefresh();
	void endSdramDmaPipeline();
	void closeSdramRows();
};
extern BSCRegisters bsc;

class CPGRegisters : public RegisterBank<CPG, 5>
{
	using super = RegisterBank<CPG, 5>;

public:
	void init();
	void reset(bool hard);
	void term();
	void serialize(Serializer& ser);
	void deserialize(Deserializer& deser);
};
extern CPGRegisters cpg;

class RTCRegisters : public RegisterBank<RTC, 16>
{
	using super = RegisterBank<RTC, 16>;

public:
	void init();
	void reset(bool hard);
	void term();
	void serialize(Serializer& ser);
	void deserialize(Deserializer& deser);
};
extern RTCRegisters rtc;

class INTCRegisters : public RegisterBank<INTC, 5>
{
	using super = RegisterBank<INTC, 5>;

public:
	void init();
	void reset();
	void term();
};
extern INTCRegisters intc;

class TMURegisters : public RegisterBank<TMU, 12>
{
	using super = RegisterBank<TMU, 12>;

public:
	void init();
	void reset(bool hard);
	void term();
	void serialize(Serializer& ser);
	void deserialize(Deserializer& deser);
};
extern TMURegisters tmu;
void TMU_EnterModuleStandby();
void TMU_EnterStandby();

class SCIRegisters : public RegisterBank<SCI, 8>
{
	using super = RegisterBank<SCI, 8>;

public:
	void init();
	void reset();
	void term();
	void serialize(Serializer& ser);
	void deserialize(Deserializer& deser);
};
extern SCIRegisters sci;

class SCIFRegisters : public RegisterBank<SCIF, 10>
{
	using super = RegisterBank<SCIF, 10>;

public:
	void init();
	void reset(bool hard);
	void term();
};
extern SCIFRegisters scif;

constexpr u32 scifCyclesPerBit(u8 bitRate, u8 clockSelect)
{
	// Pphi is SH-4/4 and the SCIF divisor is 32 * (BRR + 1) * 4^CKS.
	return 4u * 32u * (bitRate + 1u) * (1u << (clockSelect * 2));
}

class SCIFSerialPort : public SerialPort
{
public:
	void setPipe(Pipe *pipe) override;
	Pipe *getPipe() const {
		return pipe;
	}
	void updateStatus() override {}
	void receiveBreak() override;
	void init();
	void term();
	void reset();
	void serialize(Serializer& ser);
	void deserialize(Deserializer& deser);
	void postDeserialize() {
		updateBaudRate(false);
	}

	u8 SCFRDR2_read();
	void SCFTDR2_write(u8 data);
	u16 readStatus();
	void writeStatus(u16 data);
	u16 SCFDR2_read();
	static u16 SCFCR2_read(u32 addr);
	void SCFCR2_write(u16 data);
	void SCSPTR2_write(u16 data);
	static void SCBRR2_write(u32 addr, u8 data);
	static void SCSMR2_write(u32 addr, u16 data);
	void SCSCR2_write(u16 data);

	static SCIFSerialPort& Instance();

private:
	enum StatusBit {
		DR = 0x01,
		RDF = 0x02,
		PER = 0x04,
		FER = 0x08,
		BRK = 0x10,
		TDFE = 0x20,
		TEND = 0x40,
		ER = 0x80,
	};

	void setStatusBit(StatusBit bit);
	bool isTDFE() const;
	bool isRDF() const;
	void updateBaudRate(bool reschedule = true);
	void startTransmission();
	void setBreak(bool on);
	void sendBreak();
	bool txDone();
	void rxSched();
	void rxTimeout();
	static int schedCallback(int tag, int cycles, int lag, void *arg);

	Pipe *pipe = nullptr;
	int schedId = -1;
	int brkSchedId = -1;
	int rxTimeoutSchedId = -1;
	int frameSize = 10; // default 8 data bits, 1 stop bit, no parity
	int cyclesPerBit = SH4_MAIN_CLOCK / 6103;
	u16 statusLastRead = 0;
	std::deque<u8> txFifo;
	std::deque<u8> rxFifo;
	bool transmitting = false;
	u8 txShiftData = 0;
	bool txShiftDataPending = false;
};

void setupPtyPipe();
