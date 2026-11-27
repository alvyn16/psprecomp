/* psprecomp — a tiny save-state UI. See include/psprecomp/ui.h.
 *
 * It is deliberately immediate-mode and dependency-light: SDL2 draws two
 * buttons and a slot digit, and the same events that hit those buttons also
 * work as keyboard shortcuts (F5/F8, Left/Right). No GUI toolkit is linked.
 */

#include "psprecomp/ui.h"
#include "psprecomp/state.h"

#if !defined(PSPRECOMP_SDL2_WINDOW)
int psp_ui_start(void) { return -1; }
void psp_ui_stop(void) { }
#elif !defined(PSPRECOMP_IMGUI_UI)

#include <SDL.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static SDL_Thread *g_thread;
static SDL_sem *g_ready;
static volatile int g_stop;
static volatile int g_slot;
static volatile int g_status;          /* 0 idle, 1 saved, 2 loaded, -1 failed */
static int g_owns_sdl;

/* 3x5 glyphs, one bit per pixel, row-major. Only what the UI prints. */
static const unsigned char GLYPH[][5] = {
    [' '] = { 0,0,0,0,0 },
    ['S'] = { 7,4,7,1,7 }, ['A'] = { 7,5,7,5,5 },
    ['V'] = { 5,5,5,5,2 }, ['E'] = { 7,4,6,4,7 },
    ['L'] = { 4,4,4,4,7 }, ['O'] = { 7,5,5,5,7 },
    ['D'] = { 6,5,5,5,6 }, ['!'] = { 2,2,2,0,2 },
    ['0'] = { 7,5,5,5,7 }, ['1'] = { 2,6,2,2,7 },
    ['2'] = { 7,1,7,4,7 }, ['3'] = { 7,1,7,1,7 },
    ['4'] = { 5,5,7,1,1 }, ['5'] = { 7,4,7,1,7 },
    ['6'] = { 7,4,7,5,7 }, ['7'] = { 7,1,2,2,2 },
    ['8'] = { 7,5,7,5,7 }, ['9'] = { 7,5,7,1,7 }
};

static void text(SDL_Renderer *r, int x, int y, const char *s) {
    for (; *s; s++) {
        const unsigned char c = (unsigned char)*s;
        const unsigned char (*g)[5] = (c < sizeof GLYPH / sizeof GLYPH[0]) ? &GLYPH[c] : &GLYPH[' '];
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 3; col++)
                if ((*g)[row] & (1u << (2 - col))) {
                    SDL_Rect p = { x + col * 4, y + row * 4, 3, 3 };
                    SDL_RenderFillRect(r, &p);
                }
        x += 16;
    }
}

static void state_path(char *out, size_t n) {
    const char *dir = getenv("PSPRECOMP_STATE_DIR");
    snprintf(out, n, "%s/psprecomp-state-%d.bin", dir ? dir : ".", g_slot);
}

static void do_save(void) {
    char path[512];
    state_path(path, sizeof path);
    g_status = psp_state_save(path) == 0 ? 1 : -1;
}

static void do_load(void) {
    char path[512];
    state_path(path, sizeof path);
    g_status = psp_state_load(path) == 0 ? 2 : -1;
}

static int SDLCALL ui_main(void *arg) {
    (void)arg;
    SDL_Window *w = SDL_CreateWindow("psprecomp states", SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED, 320, 90, SDL_WINDOW_SHOWN);
    if (!w) { g_stop = 1; SDL_SemPost(g_ready); return 1; }
    SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
    if (!r) { SDL_DestroyWindow(w); g_stop = 1; SDL_SemPost(g_ready); return 1; }
    SDL_SemPost(g_ready);

    const SDL_Rect save_btn = { 10, 35, 90, 40 };
    const SDL_Rect load_btn = { 110, 35, 90, 40 };
    while (!g_stop) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) { g_stop = 1; break; }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.scancode == SDL_SCANCODE_F5) do_save();
                else if (e.key.keysym.scancode == SDL_SCANCODE_F8) do_load();
                else if (e.key.keysym.scancode == SDL_SCANCODE_LEFT && g_slot > 0) g_slot--;
                else if (e.key.keysym.scancode == SDL_SCANCODE_RIGHT && g_slot < 9) g_slot++;
            }
            if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT) {
                const SDL_Point p = { e.button.x, e.button.y };
                if (SDL_PointInRect(&p, &save_btn)) do_save();
                else if (SDL_PointInRect(&p, &load_btn)) do_load();
            }
        }
        if (g_stop) break;
        SDL_SetRenderDrawColor(r, 20, 20, 24, 255);
        SDL_RenderClear(r);
        SDL_SetRenderDrawColor(r, 40, 120, 70, 255); SDL_RenderFillRect(r, &save_btn);
        SDL_SetRenderDrawColor(r, 60, 80, 160, 255); SDL_RenderFillRect(r, &load_btn);
        SDL_SetRenderDrawColor(r, 255, 255, 255, 255);
        text(r, 36, 46, "SAVE");
        text(r, 136, 46, "LOAD");
        char slot[2] = { (char)('0' + g_slot), 0 };
        text(r, 230, 20, slot);
        if (g_status == 1) text(r, 230, 50, "S");
        else if (g_status == 2) text(r, 230, 50, "L");
        else if (g_status < 0) text(r, 230, 50, "!");
        SDL_RenderPresent(r);
        SDL_Delay(30);
    }
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    return 0;
}

int psp_ui_start(void) {
    if (g_thread) return -1;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) return -1;
        g_owns_sdl = 1;
    }
    g_stop = 0;
    g_status = 0;
    g_ready = SDL_CreateSemaphore(0);
    if (!g_ready) { if (g_owns_sdl) { SDL_Quit(); g_owns_sdl = 0; } return -1; }
    g_thread = SDL_CreateThread(ui_main, "psprecomp-ui", NULL);
    if (!g_thread) {
        SDL_DestroySemaphore(g_ready); g_ready = NULL;
        if (g_owns_sdl) { SDL_Quit(); g_owns_sdl = 0; }
        return -1;
    }
    SDL_SemWait(g_ready);
    SDL_DestroySemaphore(g_ready);
    g_ready = NULL;
    if (g_stop) { SDL_WaitThread(g_thread, NULL); g_thread = NULL;
                  if (g_owns_sdl) { SDL_Quit(); g_owns_sdl = 0; } return -1; }
    return 0;
}

void psp_ui_stop(void) {
    if (!g_thread) return;
    g_stop = 1;
    SDL_WaitThread(g_thread, NULL);
    g_thread = NULL;
    if (g_owns_sdl) { SDL_Quit(); g_owns_sdl = 0; }
}

#endif /* SDL2 and not ImGui */

#if defined(PSPRECOMP_SDL2_WINDOW) && defined(PSPRECOMP_IMGUI_UI)
/* The ImGui implementation lives in ui_imgui.cpp; no symbols here. */
#endif
