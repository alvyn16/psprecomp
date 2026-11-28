/* Internal interface between window.c and ui_imgui.cpp when PSPRECOMP_IMGUI_UI=1. */
#ifndef PSPRECOMP_UI_OVERLAY_H
#define PSPRECOMP_UI_OVERLAY_H

#include <SDL.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int show_menu;          /* F1 / Esc toggles the menu bar and panels */
    int show_fps;           /* View -> Show FPS HUD */
    int show_stats;         /* Performance -> Show Detailed Stats */
    int hud_corner;         /* 0 Top-Left, 1 Top-Right, 2 Bottom-Left, 3 Bottom-Right */
    int vsync;              /* Renderer VSync */
    int linear_filter;      /* 0 Nearest, 1 Linear */
    int aspect_mode;        /* 0 Fit 16:9, 1 Integer Scale, 2 Stretch */
    int audio_mute;         /* Mute host SDL audio */
    int audio_vol_pct;      /* 0..150 % */
    int fullscreen_req;     /* Set by menu, cleared by window loop */
    int quit_req;           /* Set by menu to close window */
} psp_overlay_state;

void psp_overlay_attach(SDL_Window *win, SDL_Renderer *ren, psp_overlay_state *state);
void psp_overlay_detach(void);
void psp_overlay_handle_event(const SDL_Event *ev);
int  psp_overlay_want_keyboard(void);
void psp_overlay_render(SDL_Renderer *ren);
int  psp_overlay_is_attached(void);

#ifdef __cplusplus
}
#endif

#endif
