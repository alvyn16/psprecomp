/* psprecomp — SDL2-accelerated render backend.
 *
 * Sprites are the bulk of through-mode 2D work, so they are blitted through an
 * SDL renderer (GPU-backed where SDL can get one) into an offscreen target and
 * read back into the guest framebuffer. Everything else keeps using the
 * software backend, which owns the GE's depth/lighting/texture semantics; a
 * GPU triangle path would need to reproduce all of those before it could be
 * trusted. Ordering is preserved by flushing pending sprite work into guest
 * memory before a software primitive is drawn.
 */

#include "psprecomp/render.h"
#include "psprecomp/mem.h"

#include <SDL.h>

#include <stdlib.h>
#include <string.h>

#define W 480
#define H 272

static SDL_Window *g_win;
static SDL_Renderer *g_r;
static SDL_Texture *g_target;
static SDL_Texture *g_src;
static uint32_t *g_texels;
static size_t g_texcap;
static uint32_t g_fb, g_stride;
static int g_fmt;
static int g_dirty;

void gpu_shutdown(void);   /* forward for the failure path below */

int gpu_init(int width, int height) {
    (void)width; (void)height;
    g_win = SDL_CreateWindow("psprecomp gpu backend", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, W, H, SDL_WINDOW_HIDDEN);
    if (!g_win) return -1;
    g_r = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!g_r) { SDL_DestroyWindow(g_win); g_win = NULL; return -1; }
    g_target = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_RGBA8888,
                                 SDL_TEXTUREACCESS_TARGET, W, H);
    g_src = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_RGBA8888,
                              SDL_TEXTUREACCESS_STREAMING, 16, 16);
    if (!g_target || !g_src) { gpu_shutdown(); return -1; }
    SDL_SetRenderTarget(g_r, g_target);
    SDL_SetTextureBlendMode(g_src, SDL_BLENDMODE_BLEND);
    g_dirty = 0;
    return 0;
}

static void gpu_target(uint32_t addr, uint32_t stride, int fmt) {
    g_fb = addr;
    g_stride = stride ? stride : 512;
    g_fmt = fmt;
}

/* Pull the offscreen target back into the guest framebuffer. */
static void gpu_flush(void) {
    if (!g_dirty || !g_r || !g_target || !g_fb) return;
    uint32_t px[W * H];
    if (SDL_RenderReadPixels(g_r, NULL, SDL_PIXELFORMAT_RGBA8888,
                             px, W * 4) != 0) return;
    const int bytes = g_fmt == 3 ? 4 : 2;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            const uint32_t c = px[y * W + x];
            const uint32_t at = g_fb + (uint32_t)(y * (int)g_stride + x) * (uint32_t)bytes;
            if (bytes == 4) psp_write32(at, c);
            else            psp_write16(at, (uint16_t)psp_color_pack(c, g_fmt));
        }
    }
    g_dirty = 0;
}

static void gpu_draw(int prim, const psp_vertex *v, int count, const psp_draw_state *st) {
    if (prim != PSP_PRIM_SPRITES || !g_r || !g_target || !g_fb) {
        /* Keep the ordering: committed sprite pixels come before this primitive. */
        gpu_flush();
        psp_render_software.draw(prim, v, count, st);
        return;
    }

    const uint32_t tw = st->tex_w ? (uint32_t)st->tex_w : 1;
    const uint32_t th = st->tex_h ? (uint32_t)st->tex_h : 1;
    const size_t need = (size_t)tw * th;
    if (need > g_texcap) {
        free(g_texels);
        g_texels = (uint32_t *)malloc(need * 4);
        if (!g_texels) { g_texcap = 0; return; }
        g_texcap = need;
    }
    for (uint32_t y = 0; y < th; y++)
        for (uint32_t x = 0; x < tw; x++)
            g_texels[y * tw + x] = st->tex_enable ? psp_tex_fetch(st, (int)x, (int)y)
                                                  : 0xFFFFFFFFu;
    const SDL_Rect sr = { 0, 0, (int)tw, (int)th };
    if (SDL_UpdateTexture(g_src, &sr, g_texels, (int)(tw * 4)) != 0) return;

    const uint32_t tint = v[1].rgba;
    SDL_SetTextureColorMod(g_src, (uint8_t)(tint & 0xFF),
                           (uint8_t)((tint >> 8) & 0xFF), (uint8_t)((tint >> 16) & 0xFF));
    SDL_SetTextureAlphaMod(g_src, (uint8_t)((tint >> 24) & 0xFF));

    for (int i = 0; i + 1 < count; i += 2) {
        const psp_vertex *a = &v[i], *b = &v[i + 1];
        const SDL_Rect dst = { a->x < b->x ? a->x : b->x,
                               a->y < b->y ? a->y : b->y,
                               (a->x < b->x ? b->x : a->x) - (a->x < b->x ? a->x : b->x),
                               (a->y < b->y ? b->y : a->y) - (a->y < b->y ? a->y : b->y) };
        if (dst.w <= 0 || dst.h <= 0) continue;
        SDL_RenderCopy(g_r, g_src, &sr, &dst);
        psp_render_add_pixels((uint64_t)dst.w * dst.h);
    }
    g_dirty = 1;
}

static void gpu_finish(void) { gpu_flush(); }
static void gpu_present(void) { gpu_flush(); }

void gpu_shutdown(void) {
    gpu_flush();
    if (g_src) { SDL_DestroyTexture(g_src); g_src = NULL; }
    if (g_target) { SDL_DestroyTexture(g_target); g_target = NULL; }
    if (g_r) { SDL_DestroyRenderer(g_r); g_r = NULL; }
    if (g_win) { SDL_DestroyWindow(g_win); g_win = NULL; }
    free(g_texels); g_texels = NULL; g_texcap = 0;
}

const psp_render_backend psp_render_gpu = {
    "gpu", gpu_init, gpu_shutdown, gpu_target, gpu_draw, gpu_finish, gpu_present
};
