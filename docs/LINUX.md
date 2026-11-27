# Linux host

Linux is the primary target of this fork. The recompiler and runtime are plain
C11 and build with gcc or clang; the only optional dependency is SDL2, used for
the window that displays the front buffer and feeds `sceCtrl`.

## Build

```bash
sudo apt-get install -y build-essential cmake libsdl2-dev pkg-config
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build -C Release --output-on-failure
```

`PSPRECOMP_SDL2_WINDOW` defaults to **ON** on non-Windows hosts. If pkg-config
cannot find SDL2, CMake warns and falls back to a windowless build rather than
failing — the same behaviour the core had before the SDL2 backend existed. Pass
`-DPSPRECOMP_SDL2_WINDOW=OFF` explicitly for a dependency-free toolchain-only
build.

A game that waits on another guest thread needs cooperative host threads:

```bash
cmake -S . -B build-threads -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_REAL_THREADS=ON
cmake --build build-threads -j"$(nproc)"
ctest --test-dir build-threads -C Release
```

## Window controls

`psp_window_start(title, scale)` opens a window with whole-pixel nearest
sampling and letterboxing. `scale` 0 picks the largest whole multiple that fits;
a positive value fixes the integer scale. Input is polled on the window thread
at 60 Hz.

| Keyboard | Game controller | PSP |
|---|---|---|
| arrows | d-pad | d-pad |
| — | left stick | analog stick |
| Z / X / A / S | A / B / X / Y | Cross / Circle / Square / Triangle |
| Q / W | LB or LT / RB or RT | L / R |
| Enter | Start | Start |
| Backspace | Back | Select |

`psp_window_closed()` reports that the user closed the window.

## What is not here

The window presents pixels and input only. Firmware is still implemented by the
runtime's HLE layer; audio is mixed but not played to a Linux device, and there
is no ImGui frontend or save-state UI. The Windows backend remains in
`src/window.c` behind `#ifdef _WIN32` and is checked on a best-effort basis in
CI; it is no longer the primary target.
