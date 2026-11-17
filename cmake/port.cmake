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
    PATHS /usr/i686-w64-mingw32/include /usr/share/mingw-w64/include
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
    set(${var} ${srcs} PARENT_SCOPE)
endfunction()

set(HS_GAME_DEFS -D_WIN32 -DHELLSTATION_PORT)
# -fvisibility=hidden: the game's and the shim's Win32 names (socket, select,
# send, ...) must never be exported, or host libraries that call the POSIX
# function of the same name would bind to the stdcall Win32 one.
set(HS_GAME_FLAGS -fshort-wchar -fvisibility=hidden "-I${HS_PORT_DIR}/include" "SHELL:-idirafter ${HS_MINGW_INCLUDE}")

function(hs_port_flags target)
    # BEFORE: the prelude has to precede d2_platform.h's forced include.
    target_compile_options(${target} BEFORE INTERFACE
        "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include ${HS_PORT_DIR}/prelude.h>")
    target_compile_options(${target} INTERFACE
        "$<$<COMPILE_LANGUAGE:CXX>:${HS_GAME_DEFS};${HS_GAME_FLAGS}>")
endfunction()

function(hs_port_link target)
    # The Win32 layer: compiled in the game's environment so every signature
    # matches the SDK declaration the game was compiled against.
    file(GLOB_RECURSE HS_WIN32_SOURCES CONFIGURE_DEPENDS "${HS_PORT_DIR}/win32/*.cpp")
    file(GLOB HS_PORT_TOP_SOURCES CONFIGURE_DEPENDS "${HS_PORT_DIR}/*.cpp")
    list(APPEND HS_WIN32_SOURCES ${HS_PORT_TOP_SOURCES})
    add_library(hs_win32 STATIC ${HS_WIN32_SOURCES})
    target_link_libraries(hs_win32 PRIVATE d2_flags)

    # The backend: plain target code (no Windows headers) behind port/platform.h.
    file(GLOB_RECURSE HS_BACKEND_SOURCES CONFIGURE_DEPENDS "${HS_PORT_DIR}/backend/${HS_PORT_BACKEND}/*.cpp")
    add_library(hs_backend STATIC ${HS_BACKEND_SOURCES})
    target_include_directories(hs_backend PRIVATE "${D2_ROOT}")
    target_compile_features(hs_backend PRIVATE cxx_std_17)
    target_compile_options(hs_backend PRIVATE -g -Wall -Wextra)

    if(HS_PORT_BACKEND STREQUAL "sdl")
        find_package(PkgConfig REQUIRED)
        pkg_check_modules(SDL2 REQUIRED IMPORTED_TARGET sdl2)
        find_package(Threads REQUIRED)
        target_link_libraries(hs_backend PUBLIC PkgConfig::SDL2 Threads::Threads ${CMAKE_DL_LIBS})
    endif()

    target_link_libraries(${target} PRIVATE hs_win32 hs_backend)
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
    # The link-name trap: refuse a binary that binds a game call to an unreviewed host symbol.
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND "${D2_ROOT}/tools/port/check-imports.sh" "$<TARGET_FILE:${target}>"
        WORKING_DIRECTORY "${D2_ROOT}" VERBATIM)
    # Fixed addresses, like the PS2's ELF: crash reports map straight to file:line.
    set_target_properties(${target} PROPERTIES POSITION_INDEPENDENT_CODE OFF)
    target_link_options(${target} PRIVATE -no-pie)
endfunction()
