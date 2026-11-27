# This is a Linux-focused fork

This repository is **alvyn16/psprecomp**, a fork of
[sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp). The upstream
toolkit targets several hosts; this fork makes **Linux the primary host** and
changes the defaults so that a fresh Linux clone can build, test, play a game
and save state without extra flags.

Nothing here changes the project's legal boundary: MIT, no Sony code, no
firmware images and no bundled keys. Game dumps stay in game repos.

## What "Linux-focused" means in practice

| Setting | Upstream | This fork on Linux |
|---|---|---|
| `PSPRECOMP_SDL2_WINDOW` | off / opt-in | **on** when SDL2 is found; falls back to a windowless build with a warning |
| `PSPRECOMP_REAL_THREADS` | off / opt-in | **on**; a guest that waits on another thread needs it |
| Window | Win32/GDI/XInput or none | SDL2 window, nearest scaling, focus-gated keys, F11 fullscreen, VSync |
| Controllers | first pad only | hotplug, GUID/name logged on open, rebindable button map |
| Audio | mixed in guest memory | same mix forwarded to a 44.1 kHz SDL stereo device |
| Save states | none | `psp_state_save/load`, small SDL2 UI or optional Dear ImGui UI |
| Renderer | software/null | software/null plus an opt-in SDL2-accelerated `gpu` sprite backend |
| CI | Windows/Linux/macOS required | Ubuntu required; Windows best-effort (`continue-on-error`) |
| Install | none | `cmake --install`, pkg-config `psprecomp.pc` |

Every Linux default has an escape hatch. `-DPSPRECOMP_SDL2_WINDOW=OFF` gives a
dependency-free toolchain build; `-DPSPRECOMP_REAL_THREADS=OFF` restores the
inline guest model; `-DPSPRECOMP_IMGUI_UI=ON` opts into the Dear ImGui state UI
(C++ required). The Win32/GDI/XInput backend is still in `src/window.c` behind
`#ifdef _WIN32` and is compiled on Windows CI — it is retained, just no longer
the target the defaults are chosen for.

## Where to start

- [docs/LINUX.md](LINUX.md) — packages, build, controls, audio, rendering, states.
- [README.md](../README.md) — the toolkit itself, CLI and architecture.
- [docs/THREADING.md](THREADING.md) — the cooperative scheduler used by the Linux default.

## Tracking upstream

The fork is a series of stacked changes on top of upstream's work. When pulling
new upstream commits, keep the Linux defaults in the CMake options and the
Linux-first wording in docs; a silent merge that resets
`PSPRECOMP_REAL_THREADS` to OFF would make games stall at their first guest
semaphore again. Run the full matrix locally before pushing:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
ctest --test-dir build -C Release --output-on-failure
cmake -S . -B build-core -DCMAKE_BUILD_TYPE=Release -DPSPRECOMP_SDL2_WINDOW=OFF
cmake --build build-core -j"$(nproc)"
ctest --test-dir build-core -C Release
```
