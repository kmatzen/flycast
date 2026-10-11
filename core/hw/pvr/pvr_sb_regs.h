#pragma once
#include "types.h"

class Serializer;
class Deserializer;

//Init/Term , global
void pvr_sb_Init();
void pvr_sb_Term();
//Reset -> Reset - Initialise
void pvr_sb_Reset(bool hard);
void pvr_sb_Serialize(Serializer& ser);
void pvr_sb_Deserialize(Deserializer& deser);

extern int sort_dma_schid;

// Deliver a rising edge on HOLLY's masked PVR-DMA trigger line.
void pvr_sb_interrupt();
