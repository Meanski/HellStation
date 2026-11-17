# HellStation Phase 1: i686 Linux with SDL2.
#
#   tools/port/linux-i386.sh cmake -S . -B build-linux -G Ninja \
#     -DREDIVIVUS_PORT=ON -DCMAKE_TOOLCHAIN_FILE=cmake/linux-i686-sdl.cmake
#
# A cross toolchain (i686-linux-gnu-*) with i386 libraries installed multiarch,
# as in tools/port/Dockerfile.linux-i386. Works natively on an i386 host too.

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR i686)

if(NOT HS_TOOL_PREFIX)
    set(HS_TOOL_PREFIX i686-linux-gnu-)
endif()
set(CMAKE_C_COMPILER   ${HS_TOOL_PREFIX}gcc)
set(CMAKE_CXX_COMPILER ${HS_TOOL_PREFIX}g++)
set(CMAKE_ASM_COMPILER ${HS_TOOL_PREFIX}gcc)
set(PKG_CONFIG_EXECUTABLE ${HS_TOOL_PREFIX}pkg-config CACHE FILEPATH "")

set(HS_PORT_BACKEND sdl CACHE STRING "Port backend")
