/* Window lifecycle: start, stop, start again. Headless-safe and synthetic —
 * no game data and no real display needed. */

#include "psprecomp/window.h"
#include "psprecomp/mem.h"

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

    CHECK(psp_window_start("lifecycle", 1) == 0, "first window opens");
    psp_window_stop();
    CHECK(psp_window_closed() != 0, "stop closes it and joins the thread");

    CHECK(psp_window_start("lifecycle-2", 1) == 0, "a second window can open after stop");
    psp_window_stop();
    CHECK(psp_window_closed() != 0, "second stop joins too");
    psp_window_stop();                       /* idempotent */

    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("window lifecycle checks passed (headless SDL2)\n");
    return 0;
#endif
}
