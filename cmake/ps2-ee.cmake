# HellStation Phase 2: PlayStation 2 Emotion Engine.
#
#   tools/port/ps2.sh cmake -S . -B build-ps2 -G Ninja \
#     -DREDIVIVUS_PORT=ON -DCMAKE_TOOLCHAIN_FILE=cmake/ps2-ee.cmake
#
# Builds on ps2dev's own toolchain file. Unlike the other targets this one is
# optimised: unoptimised, the town ran at 10 FPS on the EE; this set runs at the
# game's 25 FPS cap (PORTING_LOG.md, 2026-11-17). Constraints:
#  - no inlining and no IPA cloning: overrides weaken and patch functions by
#    symbol (cmake/port.cmake), and other modules call template members that
#    plain -O2 only emits inline;
#  - no strict aliasing, wrapping signed overflow, kept null checks: the
#    decompiled code may rely on all three.
include("$ENV{PS2DEV}/share/ps2dev.cmake")
# Code size against speed: -DHS_EE_OPT_LEVEL=-Os frees about 550 KiB at about
# 2 FPS. Function and data sections (on unless -DHS_EE_FUNCTION_SECTIONS=OFF)
# let --gc-sections drop what nothing refers to: the overrides depend on it to
# lose the tables and renderers they replace (renderers_dropped.cpp, d2cmp.cpp).
if(NOT DEFINED HS_EE_OPT_LEVEL)
  set(HS_EE_OPT_LEVEL "-O2")
endif()
if(NOT DEFINED HS_EE_FUNCTION_SECTIONS)
  set(HS_EE_FUNCTION_SECTIONS ON)
endif()
set(HS_EE_SECTION_FLAGS "")
if(HS_EE_FUNCTION_SECTIONS)
  set(HS_EE_SECTION_FLAGS "-ffunction-sections -fdata-sections")
endif()
set(HS_EE_OPT_FLAGS "${HS_EE_OPT_LEVEL} ${HS_EE_SECTION_FLAGS} -g -fno-inline -fno-ipa-cp -fno-ipa-sra -fno-ipa-icf -fno-ipa-modref -fno-ipa-pure-const -fno-strict-aliasing -fwrapv -fno-delete-null-pointer-checks")
string(REPLACE "-O2" "${HS_EE_OPT_FLAGS}" CMAKE_C_FLAGS_INIT "${CMAKE_C_FLAGS_INIT}")
string(REPLACE "-O2" "${HS_EE_OPT_FLAGS}" CMAKE_CXX_FLAGS_INIT "${CMAKE_CXX_FLAGS_INIT}")
set(CMAKE_ASM_COMPILER mips64r5900el-ps2-elf-gcc)
set(CMAKE_EXE_LINKER_FLAGS_INIT "${EE_LDFLAGS}")
set(HS_PORT_BACKEND ps2 CACHE STRING "Port backend")
# CMake defines the WHOLE_ARCHIVE link feature per platform; "Generic" has none.
set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE "LINKER:--whole-archive" "<LINK_ITEM>" "LINKER:--no-whole-archive")
set(CMAKE_LINK_LIBRARY_USING_WHOLE_ARCHIVE_SUPPORTED TRUE)
# Every EE object goes through tools/port/ee-lwr-fixup.py (PCSX2 recompiler LWR
# bug; see the script). A launcher, so the stock and Linux builds are untouched.
# -DHS_EE_LWR_FIXUP=OFF reproduces the bug (comparison footage, emulator testing).
if(NOT DEFINED HS_EE_LWR_FIXUP OR HS_EE_LWR_FIXUP)
  set(CMAKE_C_COMPILER_LAUNCHER "${CMAKE_CURRENT_LIST_DIR}/../tools/port/ee-cc-launcher.sh")
  set(CMAKE_CXX_COMPILER_LAUNCHER "${CMAKE_CURRENT_LIST_DIR}/../tools/port/ee-cc-launcher.sh")
endif()
