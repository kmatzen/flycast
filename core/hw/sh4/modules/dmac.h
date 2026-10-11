#pragma once
#include "hw/hwreg.h"

void DMAC_Ch2St();
void DMAC_AbortCh2Dma();
void DMAC_StartPvrDma(u32 src, u32 dst, u32 length, bool pvrToSystem);
void DMAC_AbortPvrDma();
struct Sh4Context;
void DMAC_AddBusAccessCycles(Sh4Context *ctx, int accessCycles);
bool DMAC_IsBusTransferInProgress();
void DMAC_Nmi();

// DDT transfers require DDT and DME, with AE and NMIF clear. Priority and
// overrun-check selection do not determine whether the channel is enabled.
#define DMAOR_MASK	0x00008007
#define DMAOR_DDT_ENABLED	0x00008001

extern u32 DMAC[17];

class DMACRegisters : public RegisterBank<DMAC, 17>
{
	using super = RegisterBank<DMAC, 17>;

public:
	void init();
	void reset();
	void term();
	void serialize(Serializer& ser);
	void deserialize(Deserializer& deser);
};
extern DMACRegisters dmac;
