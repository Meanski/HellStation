# hsaudio.irx, built by cmake/port.cmake through tools/port/ps2.sh (the ps2dev image).
IOP_BIN = hsaudio.irx
IOP_OBJS = hsaudio.o disc.o l2.o imports.o
IOP_OBJS_DIR ?= obj/
IOP_CFLAGS += -I.

all: $(IOP_BIN)

clean:
	rm -rf $(IOP_OBJS_DIR) $(IOP_BIN)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.iopglobal
