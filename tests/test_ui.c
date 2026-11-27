/* Save-state UI smoke test: the shortcut events drive real save/load calls.
 * Headless SDL2, synthetic values, no game data. */

#include "psprecomp/ui.h"
#include "psprecomp/state.h"
#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(PSPRECOMP_SDL2_WINDOW) && !defined(_WIN32)
#  include <SDL.h>
#  define SLOT_PATH "./psprecomp-state-0.bin"
#endif

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

#if defined(PSPRECOMP_SDL2_WINDOW) && !defined(_WIN32)
static void key(SDL_Scancode sc) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = SDL_KEYDOWN;
    e.key.keysym.scancode = sc;
    CHECK(SDL_PushEvent(&e) == 1, "key event queued");
}
#endif

int main(void) {
#if defined(PSPRECOMP_SDL2_WINDOW) && !defined(_WIN32)
    setenv("SDL_VIDEODRIVER", "dummy", 1);
    setenv("SDL_AUDIODRIVER", "dummy", 1);
    setenv("PSPRECOMP_STATE_DIR", ".", 1);
    if (psp_mem_init() != 0) { printf("memory init failed\n"); return 1; }
    psp_cpu_reset();
    psp_cpu.r[PSP_REG_V0] = 0x1234u;
    CHECK(psp_state_save(SLOT_PATH) == 0, "slot 0 seeded");

    CHECK(psp_ui_start() == 0, "UI opens headlessly");
    psp_cpu.r[PSP_REG_V0] = 0;
    key(SDL_SCANCODE_F8);                       /* load */
    SDL_Delay(150);
    CHECK(psp_cpu.r[PSP_REG_V0] == 0x1234u, "F8 loaded the state: 0x%08X",
          psp_cpu.r[PSP_REG_V0]);

    psp_cpu.r[PSP_REG_V0] = 0x5678u;
    key(SDL_SCANCODE_F5);                       /* save */
    SDL_Delay(150);
    CHECK(psp_state_load(SLOT_PATH) == 0, "F5 wrote a loadable state");
    CHECK(psp_cpu.r[PSP_REG_V0] == 0x5678u, "saved value survived: 0x%08X",
          psp_cpu.r[PSP_REG_V0]);

    key(SDL_SCANCODE_RIGHT);                    /* slot selector */
    SDL_Delay(50);
    psp_ui_stop();
    psp_ui_stop();                              /* idempotent */
    remove(SLOT_PATH);
    psp_mem_free();
#else
    CHECK(psp_ui_start() != 0, "no UI without SDL2");
    psp_ui_stop();
#endif
    if (failures) {
        printf("\n%d check(s) failed\n", failures);
        return 1;
    }
    printf("UI checks passed (headless SDL2, synthetic state)\n");
    return 0;
}
