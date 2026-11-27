/* Window lifecycle: start, stop, start again. Headless-safe and synthetic —
 * no game data and no real display needed. */

#include "psprecomp/window.h"
#include "psprecomp/mem.h"
#if defined(PSPRECOMP_SDL2_WINDOW)
#  include <SDL.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(cond, ...)                                        \
    do {                                                        \
        if (!(cond)) {                                          \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);         \
            printf(__VA_ARGS__);                                \
            printf("\n");                                       \
            failures++;                                         \
        }                                                       \
    } while (0)

int main(void) {
#ifdef _WIN32
    printf("window lifecycle test targets the SDL2 host; skipped on Windows\n");
    return 0;
#endif
#if !defined(PSPRECOMP_SDL2_WINDOW)
    /* Dependency-free core build: stop is a no-op and start reports no window. */
    psp_window_stop();
    CHECK(psp_window_start("none", 1) != 0, "no backend: start reports failure");
    psp_window_stop();
    printf("window lifecycle checks passed (no backend)\n");
    return failures;
#else
    /* SDL's dummy driver creates a real window object and thread without a
     * display, so the join path is exercised on a headless runner. */
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    psp_window_stop();                       /* safe before any start */

    /* A host remap must be copied and survive across start/stop/restart. */
    uint32_t map[PSP_WINDOW_BUTTON_COUNT] = {
        0x10, 0x20, 0x40, 0x80,             /* d-pad */
        0x2000, 0x4000, 0x8000, 0x1000,     /* Cross/Circle swapped with Square/Triangle */
        0x100, 0x200, 0x8, 0x1              /* L, R, Start, Select */
    };
    psp_window_set_button_map(map);
    CHECK(psp_window_start("lifecycle", 1) == 0, "first window opens");
    /* Drive the new event paths headlessly: focus changes, then the F11
     * fullscreen toggle. None of these may kill the window thread. */
    SDL_Event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = SDL_WINDOWEVENT;
    ev.window.event = SDL_WINDOWEVENT_FOCUS_LOST;
    CHECK(SDL_PushEvent(&ev) == 1, "focus-lost event queued");
    ev.window.event = SDL_WINDOWEVENT_FOCUS_GAINED;
    CHECK(SDL_PushEvent(&ev) == 1, "focus-gained event queued");
    ev.type = SDL_CONTROLLERDEVICEADDED;
    ev.cdevice.which = 0;
    CHECK(SDL_PushEvent(&ev) == 1, "controller-added event queued");
    ev.type = SDL_CONTROLLERDEVICEREMOVED;
    ev.cdevice.which = 0;
    CHECK(SDL_PushEvent(&ev) == 1, "controller-removed event queued");
    ev.type = SDL_KEYDOWN;
    ev.key.keysym.scancode = SDL_SCANCODE_F11;
    ev.key.repeat = 0;
    CHECK(SDL_PushEvent(&ev) == 1, "fullscreen toggle queued");
    SDL_Delay(50);
    CHECK(!psp_window_closed(), "the window survives all routed events");
    psp_window_stop();
    CHECK(psp_window_closed() != 0, "stop closes it and joins the thread");

    CHECK(psp_window_start("lifecycle-2", 1) == 0, "a second window can open after stop");
    psp_window_stop();
    CHECK(psp_window_closed() != 0, "second stop joins too");
    psp_window_stop();                       /* idempotent */
    psp_window_set_button_map(NULL);         /* restore defaults */

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("window lifecycle checks passed (headless SDL2)\n");
    return 0;
#endif
}
