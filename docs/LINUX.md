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

Cooperative host threads are the Linux default: a guest that waits on another
thread needs them, so the CPU baton is handed over at waits. The older inline
model is available with `-DPSPRECOMP_REAL_THREADS=OFF` and is what Windows keeps
as its default.

## Save states

`psp_state_save(path)` / `psp_state_load(path)` in
`<psprecomp/state.h>` snapshot the full CPU-visible register file (GPRs,
HI/LO, PC, FPU, VFPU and prefixes) plus scratchpad, VRAM and main RAM. The file
is versioned; a truncated or mismatched state is rejected rather than loaded
partially. Host-only call frames are cleared on load, so a state is for
rewinding a simple guest, not for snapshotting a live thread pool.

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
| F11 | — | Toggle fullscreen |

Keys are only forwarded while the window has input focus; the controller is
always sampled. Controllers are opened on hotplug and closed when removed, and
their name/GUID is logged on open. The SDL2 renderer requests VSync for natural
60 Hz pacing and falls back to a fixed 16 ms tick with a software renderer.

A host can rebind either input source with `psp_window_set_button_map()`: the
twelve slots are d-pad, the four face buttons, L/R, Start and Select, and the
values are `SceCtrlData` button bits. Pass `NULL` to restore the defaults. The
map is copied, so call it before `psp_window_start()`.

## Rendering and state UI

With SDL2 enabled, `psp_render_select("gpu")` chooses an accelerated renderer:
through-mode sprites are blitted through SDL's renderer into an offscreen target
and read back into the guest framebuffer, while triangles and textured
geometry keep using the software backend so the GE's depth/lighting rules stay
correct. Without SDL2 the name is rejected and software remains active.

`psp_ui_start()` / `psp_ui_stop()` in `<psprecomp/ui.h>` open a small SDL2
window with Save/Load buttons, a slot selector and F5/F8 shortcuts. It calls the
same `psp_state_*` API and is independent of the game window.

## Audio

The SAS mixer always writes its stereo grain into guest memory. When the SDL2
window is enabled it also registers `psp_audio_sink_set()` and opens a 44.1 kHz
stereo device, so the same frames are played on Linux. If no audio device is
available the window still opens and the mix remains in memory — audio failure
is never fatal. A different host can register its own sink without SDL2.

`psp_window_closed()` reports that the user closed the window. A game host should
call `psp_window_stop()` before tearing down its runtime: it signals the window
thread, joins it, and only then calls `SDL_Quit()`. Calling it twice is safe, and
`psp_window_start()` may be called again afterwards.

## What is not here

The window presents pixels, input and audio. Firmware is still implemented by
the runtime's HLE layer; there is no Dear ImGui frontend — the state UI is a
small SDL2 overlay. The
Windows backend remains in
`src/window.c` behind `#ifdef _WIN32` and is checked on a best-effort basis in
CI; it is no longer the primary target.
