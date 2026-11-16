# Build inside the ps2dev image:  tools/port/ps2.sh make -C tools/port/ps2-smoke
EE_BIN = ps2-smoke.elf
EE_OBJS = main.o
EE_CFLAGS += -O2 -Wall -Wextra -I$(GSKIT)/include
EE_LDFLAGS += -L$(GSKIT)/lib
EE_LIBS = -lgskit -ldmakit

GSKIT ?= $(PS2DEV)/gsKit

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_OBJS)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
