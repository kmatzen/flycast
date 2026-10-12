TARGET = pvr-cadence-probe.elf
OBJS = probe.o

all: $(TARGET)

include $(KOS_BASE)/Makefile.rules

$(TARGET): $(OBJS)
	kos-cc -o $@ $(OBJS)
