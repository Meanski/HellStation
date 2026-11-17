# HellStation Phase 2: PlayStation 2 Emotion Engine.
#
#   tools/port/ps2.sh cmake -S . -B build-ps2 -G Ninja \
#     -DREDIVIVUS_PORT=ON -DCMAKE_TOOLCHAIN_FILE=cmake/ps2-ee.cmake
#
# Builds on ps2dev's own toolchain file, minus its -O2: the decompiled tree is
# built unoptimised, as on every other target (see CMakeLists.txt).
include("$ENV{PS2DEV}/share/ps2dev.cmake")
string(REPLACE "-O2" "" CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
string(REPLACE "-O2" "" CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT}")
set(CMAKE_ASM_COMPILER mips64r5900el-ps2-elf-gcc)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${EE_LDFLAGS}")
set(HS_PORT_BACKEND ps2 CACHE STRING "Port backend")
# CMake defines the WHOLE_ARCHIVE link feature per platform; "Generic" has none.
set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE "LINKER:--whole-archive" "<LINK_ITEM>" "LINKER:--no-whole-archive")
set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)
