# Build inside the ps2dev image:  tools/port/ps2.sh make -C tools/port/ps2-cxx-probe
EE_BIN = ps2-cxx-probe.elf
EE_OBJS = probe.o atomics.o
EE_CXXFLAGS += -std=gnu++17
EE_LIBS = -lpthread
all: $(EE_BIN)
atomics.o: ../../../Redivivus/port/backend/ps2/atomics.cpp
	$(EE_CXX) $(EE_CXXFLAGS) $(EE_INCS) -c $< -o $@
clean:
	rm -f $(EE_BIN) $(EE_OBJS)
include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal_cpp
