# HellStation port builds (-DREDIVIVUS_PORT=ON): the generated tree on a
# non-Windows target, over the Win32 shim in Redivivus/port/.
#
# CMakeLists.txt calls three functions from here at fixed points:
#   hs_port_sources(<list>)   which hand-written upstream files a port replaces
#   hs_port_flags(<target>)   the compile environment game code sees
#   hs_port_link(<target>)    shim, backend and system libraries
# Everything else about the port lives in this file.

set(HS_PORT_DIR "${D2_ROOT}/Redivivus/port")
if(NOT HS_PORT_BACKEND)
    set(HS_PORT_BACKEND sdl)
endif()

# mingw-w64's Win32 API headers: declarations only, a build dependency
# (Debian: mingw-w64-i686-dev). Searched after the target's own headers.
find_path(HS_MINGW_INCLUDE NAMES windows.h winsock2.h
    PATHS "$ENV{HS_MINGW_INCLUDE}" /usr/i686-w64-mingw32/include /usr/share/mingw-w64/include /opt/mingw-w64/include
    NO_DEFAULT_PATH NO_CMAKE_FIND_ROOT_PATH)
if(NOT HS_MINGW_INCLUDE)
    message(FATAL_ERROR "REDIVIVUS_PORT needs mingw-w64's headers (mingw-w64-i686-dev); set HS_MINGW_INCLUDE")
endif()

# Upstream's hand-written support that is Windows/PE specific. Each has a port
# counterpart under Redivivus/port/ (or is not needed there), named alongside.
set(HS_PORT_REPLACED
    "${D2_ROOT}/Redivivus/d2_link_support.s"   # port/link_data.cpp (generated)
    "${D2_ROOT}/Redivivus/d2_crash.cpp"        # port crash handler (signals)
    "${D2_ROOT}/Redivivus/d2_patch.cpp"        # port/patches.cpp (linker --wrap)
)

function(hs_port_sources var)
    set(srcs ${${var}})
    list(REMOVE_ITEM srcs ${HS_PORT_REPLACED})
    if(HS_PORT_BACKEND STREQUAL "ps2")
        # x87 inline asm; port/win32/crt_impl_portable.cpp carries the rest.
        list(REMOVE_ITEM srcs "${D2_ROOT}/Redivivus/d2_crt_impl.cpp")
    endif()
    set(${var} ${srcs} PARENT_SCOPE)
endfunction()

set(HS_GAME_DEFS -D_WIN32 -DHELLSTATION_PORT)
if(HS_PORT_BACKEND STREQUAL "ps2")
    # int32_t is int on every platform the tree was built for (d2_platform.h
    # overloads on int32_t vs LONG); the EE's GCC makes it long.
    list(APPEND HS_GAME_DEFS -U__INT32_TYPE__ -D__INT32_TYPE__=int
        -U__UINT32_TYPE__ "-D__UINT32_TYPE__=unsigned int"
        -U__INT_LEAST32_TYPE__ -D__INT_LEAST32_TYPE__=int
        -U__UINT_LEAST32_TYPE__ "-D__UINT_LEAST32_TYPE__=unsigned int")
endif()
# -fvisibility=hidden: the game's and the shim's Win32 names (socket, select,
# send, ...) must never be exported, or host libraries that call the POSIX
# function of the same name would bind to the stdcall Win32 one.
# -ffunction-sections/-fdata-sections + --gc-sections: d2_enums.h gives every TU
# its own copy of thousands of static const enum constants (16.6 MB of .rodata
# on the EE at -O0) and the tree carries code no port reaches; the linker drops
# whatever nothing references.
set(HS_GAME_FLAGS -fshort-wchar -fvisibility=hidden -ffunction-sections -fdata-sections "-I${HS_PORT_DIR}/include" "SHELL:-idirafter ${HS_MINGW_INCLUDE}")

function(hs_port_flags target)
    # BEFORE: the prelude has to precede d2_platform.h's forced include.
    target_compile_options(${target} BEFORE INTERFACE
        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${HS_PORT_DIR}/prelude.h>")
    target_compile_options(${target} INTERFACE
        "$<$<COMPILE_LANGUAGE:CXX>:${HS_GAME_DEFS};${HS_GAME_FLAGS}>")
endfunction()

# Overrides: generated functions a port replaces with hand-written ones, because
# the generated body is wrong for a non-VC8 compiler or carries a Blizzard bug
# upstream patches at runtime on x86 (docs/port/overrides.md has the why and the
# retire condition for each). After the module library is built, the generated
# definition is weakened in the archive so the definition in
# Redivivus/port/overrides/ wins at link; file-statics the override needs are
# renamed and globalized. Format: module|symbol to weaken|old=new static, ...
set(HS_PORT_OVERRIDES
    "D2gfx|-|_ZN7D2DDraw8Renderer10DirectDraw23DirectDrawFunctionTableE=hs_dropped_ddraw,_ZN7D2Glide8Renderer5Glide18GlideFunctionTableE=hs_dropped_glide,_ZN10D2Direct3D8Renderer21Direct3DFunctionTableE=hs_dropped_d3d"
    "D2CMP|_ZN5D2CMP7Pallete20PALETTE_LoadFromFileEPci"
    "D2CMP|_ZN5D2CMP7Pallete22PALETTE_GetItemPaletteEii"
    "D2CMP|_ZN5D2CMP3SRC7GfxHash26GFXHASH_CalcDirectionIndexEP13D2GfxDataStrcPjP23D2CelFileCacheEntryStrc"
    "Storm|-|_ZN5Storm6Source4SMemL29SMemCalculateAllocationLayoutEjPiS2_S2_S2_=hs_Storm_SMemCalculateAllocationLayout,_ZL11DebugMemory=hs_Storm_DebugMemory,_ZL13ProtectMemory=hs_Storm_ProtectMemory"
    "D2CMP|_ZN5D2CMP3SRC8LRUCache15LRUCACHE_UnlinkEP18D2LRUCacheNodeStrcP14D2LRUCacheStrc|_ZN5D2CMP3SRC8LRUCacheL18LRUCACHE_PopOldestEP14D2LRUCacheStrc=hs_D2CMP_LRUCACHE_PopOldest"
    "Storm|-|_ZN5Storm6Source5SFileL21SFILE_SearchHashTableEP13D2ArchiveStrcscPc=hs_Storm_SFILE_SearchHashTable,_ZN5Storm6Source5SFileL10SFILE_HashEiPc=hs_Storm_SFILE_Hash"
    "Storm|-|_ZN5Storm6Source5SFileL26SFILE_HandleAsyncReadErrorEP18D2StormAsyncOpStrc=hs_Storm_HandleAsyncReadError,_ZL26lpCriticalSection_00779068=hs_Storm_StreamCs"
    "D2Sound|-|_ZL22gpWavFileCacheListHead=hs_D2Sound_gpWavFileCacheListHead,_ZL21gpaSoundEffectBuffers=hs_D2Sound_gpaSoundEffectBuffers"
    "D2Client|-|_ZL21pSpellsClassSkilltree=hs_D2Client_pSpellsClassSkilltree,_ZL18gpInvMercBodyImage=hs_D2Client_gpInvMercBodyImage,_ZL25WaypointWayGateBackGround=hs_D2Client_WaypointWayGateBackGround"
)

# Wraps: generated functions the port interposes on rather than replaces. Before
# the module's archive is built, the definition in one object file is renamed to
# hs_orig_<symbol>; every caller outside that object then reaches the wrapper in
# Redivivus/port/overrides/, which calls hs_orig_<symbol>.
# Format: module|source file (tree-relative)|symbol
set(HS_PORT_WRAPS
    "Storm|3rdparty/Storm/Source/SMem.cpp|_ZN5Storm6Source4SMem9SMemAllocEjPcjj"
    "Storm|3rdparty/Storm/Source/SMem.cpp|_ZN5Storm6Source4SMem8SMemFreeEPvPcim"
    "Storm|3rdparty/Storm/Source/SMem.cpp|_ZN5Storm6Source4SMem11SMemReAllocEPjjPcij"
    "D2Win|Source/D2Win/Src/D2WinArchive.cpp|_ZN5D2Win3Src12D2WinArchive13IMAGE_LoadDC6EPci"
    "D2Win|Source/D2Win/D2WinAnimImage.cpp|_ZN5D2Win14D2WinAnimImageEiiiiP3DC6iPFiP12D2ControlMsgEP19FORMS_AnimFrameStrcjPFvPvE"
    "D2CMP|Source/D2CMP/SRC/SpriteCache.cpp|_ZN5D2CMP3SRC11SpriteCache20GFX_InitCelDataCacheEP17D2PoolManagerStrcjjj"
    "D2CMP|Source/D2CMP/SRC/SpriteCache.cpp|_ZN5D2CMP3SRC11SpriteCache16GFX_InitLRUCacheEP17D2PoolManagerStrcj"
    "D2CMP|Source/D2CMP/SRC/Codec.cpp|_ZN5D2CMP3SRC5Codec25CODEC_DecompressAllFramesEPjP17D2PoolManagerStrciPhP20D2CelDecodeStateStrciiS2_Pc"
    "Fog|Source/Fog/Memory.cpp|_ZN3Fog6Memory5AllocEP17D2PoolManagerStrcjPci"
    "Fog|Source/Fog/Memory.cpp|_ZN3Fog6Memory17AllocClientMemoryEjPcii"
    "Fog|Source/Fog/Memory.cpp|_ZN3Fog6Memory17AllocServerMemoryEP17D2PoolManagerStrcjPcii"
    "Fog|Source/Fog/Memory.cpp|_ZN3Fog6Memory19ReAllocServerMemoryEP17D2PoolManagerStrcPvjPcii"
    "Fog|Source/Fog/Memory.cpp|_ZN3Fog6Memory16FreeClientMemoryEPvPcii"
    "Fog|Source/Fog/Memory.cpp|_ZN3Fog6Memory16FreeServerMemoryEP17D2PoolManagerStrcPvPcii"
    "Fog|Source/Fog/Memory.cpp|_ZN3Fog6Memory14FreeMemoryPoolEP17D2PoolManagerStrc"
    "D2Hell|Source/D2Hell/D2HellRoot.cpp|_ZN6D2Hell25D2FILE_AllocAndLoadD2FileEP17D2PoolManagerStrcPcPjS2_i"
    "D2Client|Source/D2Client/UI/Draw.cpp|_ZN8D2Client2UI4Draw7DRAW_UIEP14D2GameViewStrc"
    "D2Client|Source/D2Client/UI/Draw.cpp|_ZN8D2Client2UI4Draw12InitializeUIEv"
    "D2Client|Source/D2Client/UI/ui.cpp|_ZN8D2Client2UI2ui21UI_LoadAllPanelAssetsEv"
    "D2Client|Source/D2Client/Sound/SoundHdr.cpp|_ZN8D2Client5Sound8SoundHdr19SOUNDHDR_SetGlobal4Ej"
    "D2Client|Source/D2Client/Sound/SoundHdr.cpp|_ZN8D2Client5Sound8SoundHdr13SOUNDHDR_InitEj"
    "D2Sound|Source/D2Sound/Src/D2SoundFast.cpp|_ZN7D2Sound3Src11D2SoundFast17SOUND_LoadWavFileEPc"
    "D2Sound|Source/D2Sound/Src/D2SoundFast.cpp|_ZN7D2Sound3Src11D2SoundFast21SOUND_PlaySoundEffectEPh"
    # Not a definition: Codec.cpp's calls to memset (overrides/dcc_clear.cpp).
    "D2CMP|Source/D2CMP/SRC/Codec.cpp|memset"
)

function(hs_port_apply_wraps)
    foreach(entry IN LISTS HS_PORT_WRAPS)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 module)
        list(GET parts 1 source)
        list(GET parts 2 symbol)
        set(obj "${CMAKE_BINARY_DIR}/CMakeFiles/${module}.dir/${source}${CMAKE_CXX_OUTPUT_EXTENSION}")
        add_custom_command(TARGET ${module} PRE_LINK
            COMMAND "${CMAKE_OBJCOPY}" "--redefine-sym=${symbol}=hs_orig_${symbol}" "${obj}"
            COMMENT "port wrap: ${symbol}" VERBATIM)
        string(APPEND HS_OVERRIDE_TEXT_${module} "// wrap ${entry}\n")
        set(HS_OVERRIDE_TEXT_${module} "${HS_OVERRIDE_TEXT_${module}}" PARENT_SCOPE)
        set(HS_WRAP_MODULES ${HS_WRAP_MODULES} ${module} PARENT_SCOPE)
    endforeach()
endfunction()

function(hs_port_apply_overrides)
    hs_port_apply_wraps()
    list(APPEND HS_OVERRIDE_MODULES ${HS_WRAP_MODULES})
    if(NOT CMAKE_OBJCOPY)
        message(FATAL_ERROR "port overrides need objcopy")
    endif()
    foreach(entry IN LISTS HS_PORT_OVERRIDES)
        string(REPLACE "|" ";" parts "${entry}")
        list(GET parts 0 module)
        list(GET parts 1 symbol)
        list(LENGTH parts n)
        if(symbol STREQUAL "-")  # nothing to weaken: only globalize the statics below
            set(cmds)
        else()
            set(cmds COMMAND "${CMAKE_OBJCOPY}" "--weaken-symbol=${symbol}" "$<TARGET_FILE:${module}>")
        endif()
        if(n GREATER 2)
            list(GET parts 2 statics)
            if(statics)
                string(REPLACE "," ";" statics "${statics}")
                foreach(pair IN LISTS statics)
                    string(REPLACE "=" ";" names "${pair}")
                    list(GET names 0 from)
                    list(GET names 1 to)
                    list(APPEND cmds COMMAND "${CMAKE_OBJCOPY}" "--redefine-sym=${from}=${to}" "$<TARGET_FILE:${module}>"
                                     COMMAND "${CMAKE_OBJCOPY}" "--globalize-symbol=${to}" "$<TARGET_FILE:${module}>")
                endforeach()
            endif()
        endif()
        if(cmds)
            add_custom_command(TARGET ${module} POST_BUILD ${cmds} COMMENT "port override: ${entry}" VERBATIM)
        endif()
        list(APPEND HS_OVERRIDE_MODULES ${module})
        string(APPEND HS_OVERRIDE_TEXT_${module} "// ${entry}\n")
    endforeach()
    # A stamp source per module whose content is its override entries: changing
    # the table rebuilds that archive, which re-runs the POST_BUILD steps above.
    list(REMOVE_DUPLICATES HS_OVERRIDE_MODULES)
    foreach(module IN LISTS HS_OVERRIDE_MODULES)
        set(stamp "${CMAKE_BINARY_DIR}/hs_overrides_${module}.cpp")
        file(WRITE "${stamp}.in" "// Generated by cmake/port.cmake: port overrides applied to ${module}.\n${HS_OVERRIDE_TEXT_${module}}")
        configure_file("${stamp}.in" "${stamp}" COPYONLY)
        target_sources(${module} PRIVATE "${stamp}")
    endforeach()
endfunction()

function(hs_port_link target)
    hs_port_apply_overrides()
    if(HS_PORT_BACKEND STREQUAL "ps2")
        # The DCC sprite decoder is most of a busy frame once the sprite cache is
        # full (PORTING_LOG, 2026-11-19), and its bit reader is a call per field
        # under the EE's -fno-inline. Nothing patches a function in this file,
        # so it gets inlining and -O2 even in an -Os build.
        set_property(SOURCE "${D2_ROOT}/Source/D2CMP/SRC/Codec.cpp" APPEND PROPERTY
            COMPILE_OPTIONS -O2 -finline -finline-small-functions)
    endif()
    # The Win32 layer: compiled in the game's environment so every signature
    # matches the SDK declaration the game was compiled against.
    file(GLOB_RECURSE HS_WIN32_SOURCES CONFIGURE_DEPENDS "${HS_PORT_DIR}/win32/*.cpp")
    # Overrides go into the executable as objects: in an archive they'd never be
    # pulled, because the weakened generated definition already satisfies callers.
    file(GLOB HS_OVERRIDE_SOURCES CONFIGURE_DEPENDS "${HS_PORT_DIR}/overrides/*.cpp")
    add_library(hs_overrides OBJECT ${HS_OVERRIDE_SOURCES})
    target_link_libraries(hs_overrides PRIVATE d2_flags)
    target_sources(${target} PRIVATE $<TARGET_OBJECTS:hs_overrides>)
    file(GLOB HS_PORT_TOP_SOURCES CONFIGURE_DEPENDS "${HS_PORT_DIR}/*.cpp")
    list(APPEND HS_WIN32_SOURCES ${HS_PORT_TOP_SOURCES})
    add_library(hs_win32 STATIC ${HS_WIN32_SOURCES})
    target_link_libraries(hs_win32 PRIVATE d2_flags)

    # The backend: plain target code (no Windows headers) behind port/platform.h.
    file(GLOB_RECURSE HS_BACKEND_SOURCES CONFIGURE_DEPENDS
        "${HS_PORT_DIR}/backend/${HS_PORT_BACKEND}/*.cpp" "${HS_PORT_DIR}/backend/posix/*.cpp"
        "${HS_PORT_DIR}/input/*.cpp")
    add_library(hs_backend STATIC ${HS_BACKEND_SOURCES})
    target_include_directories(hs_backend PRIVATE "${D2_ROOT}")
    target_compile_features(hs_backend PRIVATE cxx_std_17)
    target_compile_options(hs_backend PRIVATE -g -Wall -Wextra)

    if(HS_PORT_BACKEND STREQUAL "ps2")
        target_include_directories(hs_backend PRIVATE "$ENV{PS2DEV}/gsKit/include")
        target_link_directories(${target} PRIVATE "$ENV{PS2DEV}/gsKit/lib")
        target_link_libraries(hs_backend PUBLIC gskit dmakit pad patches mc pthread)
        # The SPU2 side of audio (backend/ps2/iop/): an IOP module built with
        # ps2sdk's IRX rules and embedded in the ELF, loaded from memory.
        set(HS_IRX_DIR "${HS_PORT_DIR}/backend/ps2/iop")
        add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/hsaudio_irx.c"
            COMMAND make -s -C "${HS_IRX_DIR}" "IOP_OBJS_DIR=${CMAKE_BINARY_DIR}/iop/"
                    "IOP_BIN=${CMAKE_BINARY_DIR}/hsaudio.irx"
            COMMAND bin2c "${CMAKE_BINARY_DIR}/hsaudio.irx" "${CMAKE_BINARY_DIR}/hsaudio_irx.c" hsaudio_irx
            DEPENDS "${HS_IRX_DIR}/hsaudio.c" "${HS_IRX_DIR}/disc.c" "${HS_IRX_DIR}/hsaudio_rpc.h" "${HS_IRX_DIR}/imports.lst"
                    "${HS_IRX_DIR}/irx_imports.h" "${HS_IRX_DIR}/Makefile"
            COMMENT "IOP module: hsaudio.irx" VERBATIM)
        target_sources(hs_backend PRIVATE "${CMAKE_BINARY_DIR}/hsaudio_irx.c")
        option(HS_PS2_EXTENDED_RAM "PS2 bring-up: let the heap use PCSX2's 128 MB Extended RAM (off: stop at a retail 32 MB)" OFF)
        if(HS_PS2_EXTENDED_RAM)
            target_compile_definitions(hs_backend PRIVATE HS_PS2_EXTENDED_RAM)
        endif()
        # Memory work: cap the C heap (KiB) to find what a scene really needs,
        # fragmentation included, without leaving Extended RAM. 0 = no cap.
        set(HS_PS2_HEAP_LIMIT_KB 0 CACHE STRING "PS2: cap the C heap at this many KiB (0 = off)")
        if(HS_PS2_HEAP_LIMIT_KB)
            target_compile_definitions(hs_backend PRIVATE HS_PS2_HEAP_LIMIT_KB=${HS_PS2_HEAP_LIMIT_KB})
        endif()
        # Debugging: a timer-sampling profiler and memcpy/memset caller counts
        # (backend/ps2/profiler.cpp), logged every 250 frames.
        option(HS_PS2_PROFILE "PS2 debugging: sample the EE at vertical blank, count memcpy callers" OFF)
        if(HS_PS2_PROFILE)
            target_compile_definitions(hs_backend PRIVATE HS_PROFILE)
            target_link_options(${target} PRIVATE -Wl,--wrap=memcpy -Wl,--wrap=memset)
        endif()
        # backend/ps2/memory.cpp replaces libcglue's _sbrk; pull it before libc asks.
        target_link_options(${target} PRIVATE -Wl,-u,_sbrk)
    endif()
    if(HS_PORT_BACKEND STREQUAL "sdl")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
        find_package(Threads REQUIRED)
        target_link_libraries(hs_backend PUBLIC PkgConfig::SDL2 Threads::Threads ${CMAKE_DL_LIBS})
    endif()

    target_link_libraries(${target} PRIVATE hs_win32 hs_backend)
    target_link_options(${target} PRIVATE -Wl,--gc-sections)
    if(REDIVIVUS_PORT_SANITIZE)
        foreach(t ${target} hs_win32 hs_backend)
            target_compile_options(${t} PRIVATE -fsanitize=alignment,undefined -fno-sanitize-recover=alignment)
        endforeach()
        target_link_options(${target} PRIVATE -fsanitize=alignment,undefined)
    endif()
    # Every Win32 gap the link reports at once, not one at a time.
    target_link_options(${target} PRIVATE -Wl,--unresolved-symbols=report-all)
    # Nothing in the executable is for shared libraries to bind to.
    target_compile_options(hs_backend PRIVATE -fvisibility=hidden)
    if(HS_PORT_BACKEND STREQUAL "sdl")
        # The link-name trap: refuse a binary that binds a game call to an unreviewed host symbol.
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND "${D2_ROOT}/tools/port/check-imports.sh" "$<TARGET_FILE:${target}>"
            WORKING_DIRECTORY "${D2_ROOT}" VERBATIM)
        # Fixed addresses, like the PS2's ELF: crash reports map straight to file:line.
        set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
        target_link_options(${target} PRIVATE -no-pie)
    else()
        set_target_properties(${target} PROPERTIES SUFFIX ".elf")
        # The EE link-name trap: fail if the port defines a symbol the toolchain's
        # libraries also define (Redivivus/port/include/hs_ps2_renames.h).
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND sh -c "c=$(tools/port/ps2-collisions.sh '${CMAKE_BINARY_DIR}'); if [ -n \"$c\" ]; then echo \"symbol collisions with ps2sdk (add to hs_ps2_renames.h): $c\" >&2; exit 1; fi"
            WORKING_DIRECTORY "${D2_ROOT}" VERBATIM)
    endif()
endfunction()
