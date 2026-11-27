# psprecomp — a static-recompilation toolkit for PlayStation Portable games.
#
# This file builds two things:
#   1. the `psprecomp` runtime static library — what recompiled game code links
#      against (CPU state, memory map, semantic helpers)
#   2. `allegrexrecomp` — the tool that turns a PSP module into C
#
# A game repo consumes this repo as a git submodule and does:
#     add_subdirectory(psprecomp)
#     add_subdirectory(games/<game>)
# and links its host against the `psprecomp` target.
#
# The core has no external dependencies on purpose. SDL2 and friends only ever
# appear behind an opt-in option, so a fresh clone builds with nothing installed.

cmake_minimum_required(VERSION 3.16)
project(psprecomp C)

include(GNUInstallDirs)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W3)
    # /MP: the generated corpus is hundreds of translation units, and building
    # them one at a time is the slowest part of the loop. No /bigobj -- the
    # emitter splits its output, so no single unit is large enough to need it.
    add_compile_options(/MP)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# --- the runtime library ------------------------------------------------------

add_library(psprecomp STATIC
    src/cpu.c
    src/mem.c
    src/dispatch.c
    src/ctors.c
    src/render.c
    src/vfpu.c
    src/traps.c
    src/window.c
    src/state.c
    src/module.c
    src/hle/hle.c
    src/hle/sysmem.c
    src/hle/threadman.c
    src/hle/power.c
    src/hle/intrman.c
    src/hle/display.c
    src/hle/ge.c
    src/hle/sascore.c
    src/hle/iofilemgr.c
    src/hle/misc.c
)

target_include_directories(psprecomp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Runtime VFPU helpers use libm on Unix; propagate it to static-library users.
if(UNIX)
    target_link_libraries(psprecomp PUBLIC m)
endif()

# Linux is the primary host for this fork. The runtime core stays dependency
# free, but a normal non-Windows build gets a playable window by default.
# Windows keeps its Win32/GDI/XInput backend and needs no SDL2.
if(WIN32)
    option(PSPRECOMP_SDL2_WINDOW "Use SDL2 for the window backend" OFF)
else()
    option(PSPRECOMP_SDL2_WINDOW "Use SDL2 for the window backend" ON)
endif()
if(PSPRECOMP_SDL2_WINDOW AND NOT WIN32)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SDL2 QUIET IMPORTED_TARGET sdl2)
    endif()
    if(TARGET PkgConfig::SDL2)
        target_compile_definitions(psprecomp PUBLIC PSPRECOMP_SDL2_WINDOW=1)
        target_link_libraries(psprecomp PUBLIC PkgConfig::SDL2)
    else()
        # A fresh clone must still configure with nothing installed. The window
        # then reports that it cannot open, exactly as before this option.
        message(WARNING "SDL2 not found: building without a playable window. "
                        "Install libsdl2-dev and pkg-config, or pass "
                        "-DPSPRECOMP_SDL2_WINDOW=OFF to silence this warning.")
        set(PSPRECOMP_SDL2_WINDOW OFF)
    endif()
endif()

# On Linux a real game stalls at its first guest semaphore in the inline model,
# so cooperative host threads are the default there. Windows keeps the inline
# default for compatibility; either side can opt out explicitly.
if(WIN32)
    option(PSPRECOMP_REAL_THREADS "Enable cooperative host-thread scheduling" OFF)
else()
    option(PSPRECOMP_REAL_THREADS "Enable cooperative host-thread scheduling" ON)
endif()
target_compile_definitions(psprecomp PUBLIC
    PSPRECOMP_REAL_THREADS=$<BOOL:${PSPRECOMP_REAL_THREADS}>
)
if(WIN32)
    target_link_libraries(psprecomp PUBLIC xinput9_1_0)   # the window's controller input
endif()
if(PSPRECOMP_REAL_THREADS AND WIN32)
    target_link_libraries(psprecomp PUBLIC winmm)   # timeBeginPeriod
endif()
if(PSPRECOMP_REAL_THREADS AND NOT WIN32)
    find_package(Threads REQUIRED)
    target_link_libraries(psprecomp PUBLIC Threads::Threads)
endif()

# --- the recompiler -----------------------------------------------------------

add_subdirectory(tools/allegrexrecomp)

# --- the interpreter oracle ---------------------------------------------------
# A separate library: it reuses the toolkit decoder and the runtime state, but
# a game host does not need to link it unless it is diffing a bring-up problem.
add_library(psprecomp_interp STATIC src/interp.c)
target_include_directories(psprecomp_interp PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/tools/allegrexrecomp)
target_link_libraries(psprecomp_interp PUBLIC psprecomp allegrex_core)
# --- install ------------------------------------------------------------------
# A Linux host consumes psprecomp through pkg-config: a static runtime, the
# recompiler library/CLI and the headers, all under one prefix.
set(PSPRECOMP_VERSION "0.6.0")
if(TARGET PkgConfig::SDL2)
    set(PSPRECOMP_PC_REQUIRES "sdl2")
else()
    set(PSPRECOMP_PC_REQUIRES "")
endif()
configure_file(cmake/psprecomp.pc.in ${CMAKE_CURRENT_BINARY_DIR}/psprecomp.pc @ONLY)

install(TARGETS psprecomp allegrex_core allegrexrecomp
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
if(TARGET psprecomp_interp)
    install(TARGETS psprecomp_interp ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})
endif()
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/psprecomp
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.h")
install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/tools/allegrexrecomp/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/psprecomp
    FILES_MATCHING PATTERN "*.h")
install(FILES ${CMAKE_CURRENT_BINARY_DIR}/psprecomp.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

# --- tests (only when psprecomp is the top-level project, not a submodule) -----

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    enable_testing()
    add_subdirectory(tests)
endif()
