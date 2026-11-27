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
static SDL_Texture *g_bg;
static SDL_Texture *g_src;
static int g_src_w, g_src_h;
static uint32_t *g_texels;
static size_t g_texcap;
static uint32_t g_fb, g_stride;
static int g_fmt;
static int g_dirty, g_dx0, g_dy0, g_dx1, g_dy1;

void gpu_shutdown(void);   /* forward for the failure path below */

int gpu_init(int width, int height) {
    (void)width; (void)height;
    g_win = SDL_CreateWindow("psprecomp gpu backend", SDL_WINDOWPOS_UNDEFINED,
                             SDL_WINDOWPOS_UNDEFINED, W, H, SDL_WINDOW_HIDDEN);
    if (!g_win) return -1;
    g_r = SDL_CreateRenderer(g_win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_TARGETTEXTURE);
    if (!g_r) { SDL_DestroyWindow(g_win); g_win = NULL; return -1; }
    g_target = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ABGR8888,
                                 SDL_TEXTUREACCESS_TARGET, W, H);
    g_bg = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ABGR8888,
                             SDL_TEXTUREACCESS_STREAMING, W, H);
    g_src_w = 64;
    g_src_h = 64;
    g_src = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ABGR8888,
                              SDL_TEXTUREACCESS_STREAMING, g_src_w, g_src_h);
    if (!g_target || !g_bg || !g_src) { gpu_shutdown(); return -1; }
    SDL_SetTextureScaleMode(g_src, SDL_ScaleModeNearest);
    SDL_SetTextureScaleMode(g_bg, SDL_ScaleModeNearest);
    SDL_SetTextureBlendMode(g_bg, SDL_BLENDMODE_NONE);
    SDL_SetRenderTarget(g_r, g_target);
    g_dirty = 0;
    g_dx0 = W; g_dy0 = H; g_dx1 = 0; g_dy1 = 0;
    return 0;
}

/* Pull the offscreen target back into the guest framebuffer, for the touched box only. */
static void gpu_flush(void) {
    if (!g_dirty || !g_r || !g_target || !g_fb || !g_stride ||
        g_dx1 <= g_dx0 || g_dy1 <= g_dy0) {
        g_dirty = 0;
        g_dx0 = W; g_dy0 = H; g_dx1 = 0; g_dy1 = 0;
        return;
    }
    const SDL_Rect r = { g_dx0, g_dy0, g_dx1 - g_dx0, g_dy1 - g_dy0 };
    uint32_t px[W * H];
    SDL_RenderSetClipRect(g_r, NULL);
    if (SDL_RenderReadPixels(g_r, &r, SDL_PIXELFORMAT_ABGR8888,
                             px, r.w * 4) != 0) return;
    const uint32_t bytes = g_fmt == 3 ? 4u : 2u;
    for (int ry = 0; ry < r.h; ry++) {
        const int y = r.y + ry;
        const uint32_t row_base = g_fb + (uint32_t)y * g_stride * bytes;
        if (!psp_mem_ptr(row_base + (uint32_t)r.x * bytes, (uint32_t)r.w * bytes)) continue;
        for (int rx = 0; rx < r.w; rx++) {
            const int x = r.x + rx;
            const uint32_t c = px[ry * r.w + rx];
            const uint32_t at = row_base + (uint32_t)x * bytes;
            if (bytes == 4) psp_write32(at, c);
            else            psp_write16(at, (uint16_t)psp_color_pack(c, g_fmt));
        }
    }
    g_dirty = 0;
    g_dx0 = W; g_dy0 = H; g_dx1 = 0; g_dy1 = 0;
}

static void gpu_target(uint32_t addr, uint32_t stride, int fmt) {
    uint32_t s = stride ? stride : 512;
    if (g_dirty && (addr != g_fb || s != g_stride || fmt != g_fmt)) gpu_flush();
    g_fb = addr;
    g_stride = s;
    g_fmt = fmt;
}

static int gpu_sync_bg(void) {
    if (g_dirty) return 0;
    if (!g_fb || !g_stride || !g_bg) return -1;
    uint32_t px[W * H];
    const uint32_t bytes = g_fmt == 3 ? 4u : 2u;
    for (int y = 0; y < H; y++) {
        const uint32_t row_base = g_fb + (uint32_t)y * g_stride * bytes;
        if (!psp_mem_ptr(row_base, (uint32_t)W * bytes)) {
            memset(&px[y * W], 0, W * sizeof px[0]);
            continue;
        }
        for (int x = 0; x < W; x++) {
            const uint32_t at = row_base + (uint32_t)x * bytes;
            px[y * W + x] = bytes == 4 ? psp_read32(at)
                                       : psp_color_expand(psp_read16(at), g_fmt);
        }
    }
    if (SDL_UpdateTexture(g_bg, NULL, px, W * 4) != 0) return -1;
    SDL_RenderSetClipRect(g_r, NULL);
    SDL_RenderCopy(g_r, g_bg, NULL, NULL);
    return 0;
}

static int gpu_ensure_src(int tw, int th) {
    if (g_src && tw <= g_src_w && th <= g_src_h) return 0;
    if (tw <= 0 || th <= 0 || tw > 2048 || th > 2048) return -1;
    int nw = g_src_w > 0 ? g_src_w : 64;
    int nh = g_src_h > 0 ? g_src_h : 64;
    while (nw < tw) nw *= 2;
    while (nh < th) nh *= 2;
    SDL_Texture *nt = SDL_CreateTexture(g_r, SDL_PIXELFORMAT_ABGR8888,
                                        SDL_TEXTUREACCESS_STREAMING, nw, nh);
    if (!nt) return -1;
    SDL_SetTextureScaleMode(nt, SDL_ScaleModeNearest);
    if (g_src) SDL_DestroyTexture(g_src);
    g_src = nt;
    g_src_w = nw;
    g_src_h = nh;
    return 0;
}

static int gpu_can_fast_sprites(const psp_vertex *v, int count, const psp_draw_state *st,
                                int tw, int th) {
    if (st->clear || st->ztest || st->atest) return 0;
    if (st->blend && (st->blend_op != 0 || st->blend_src != 2 || st->blend_dst != 3)) return 0;
    if (!st->tex_enable) return 1;
    if (st->tex_double || !st->tex_alpha || (st->tex_func != 0 && st->tex_func != 3)) return 0;
    if (tw <= 0 || th <= 0 || tw > 2048 || th > 2048) return 0;
    for (int i = 0; i + 1 < count; i += 2) {
        const psp_vertex *a = &v[i], *b = &v[i + 1];
        if (a->x > b->x || a->y > b->y) return 0;
        const int u0 = (int)a->u, v0 = (int)a->v;
        const int u1 = (int)b->u, v1 = (int)b->v;
        if ((float)u0 != a->u || (float)v0 != a->v ||
            (float)u1 != b->u || (float)v1 != b->v) return 0;
        if (u0 < 0 || v0 < 0 || u1 <= u0 || v1 <= v0 || u1 > tw || v1 > th) return 0;
    }
    return 1;
}

static void gpu_draw(int prim, const psp_vertex *v, int count, const psp_draw_state *st) {
    const int tw = st->tex_w > 0 ? st->tex_w : 1;
    const int th = st->tex_h > 0 ? st->tex_h : 1;
    if (prim != PSP_PRIM_SPRITES || !g_r || !g_target || !g_fb || !g_stride || count < 2 ||
        !gpu_can_fast_sprites(v, count, st, tw, th) ||
        (st->tex_enable && gpu_ensure_src(tw, th) != 0)) {
        /* Keep the ordering: committed sprite pixels come before this primitive. */
        gpu_flush();
        psp_render_software.draw(prim, v, count, st);
        return;
    }

    int cx0 = st->sc_x0 < 0 ? 0 : st->sc_x0;
    int cy0 = st->sc_y0 < 0 ? 0 : st->sc_y0;
    int cx1 = st->sc_x1 + 1 > W ? W : st->sc_x1 + 1;
    int cy1 = st->sc_y1 + 1 > H ? H : st->sc_y1 + 1;
    if (cx1 <= cx0 || cy1 <= cy0) return;

    if (st->tex_enable) {
        const size_t need = (size_t)tw * (size_t)th;
        if (need > g_texcap) {
            uint32_t *nt = (uint32_t *)realloc(g_texels, need * 4);
            if (!nt) {
                gpu_flush();
                psp_render_software.draw(prim, v, count, st);
                return;
            }
            g_texels = nt;
            g_texcap = need;
        }
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++)
                g_texels[(size_t)y * (size_t)tw + (size_t)x] = psp_tex_fetch(st, x, y);
        const SDL_Rect upload = { 0, 0, tw, th };
        if (SDL_UpdateTexture(g_src, &upload, g_texels, tw * 4) != 0) {
            gpu_flush();
            psp_render_software.draw(prim, v, count, st);
            return;
        }
        SDL_SetTextureBlendMode(g_src, st->blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    } else {
        SDL_SetRenderDrawBlendMode(g_r, st->blend ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    }

    int synced = g_dirty;
    const SDL_Rect clip = { cx0, cy0, cx1 - cx0, cy1 - cy0 };
    for (int i = 0; i + 1 < count; i += 2) {
        const psp_vertex *a = &v[i], *b = &v[i + 1];
        const int x0 = a->x < b->x ? a->x : b->x;
        const int y0 = a->y < b->y ? a->y : b->y;
        const int x1 = a->x > b->x ? a->x : b->x;
        const int y1 = a->y > b->y ? a->y : b->y;
        const SDL_Rect dst = { x0, y0, x1 - x0, y1 - y0 };
        if (dst.w <= 0 || dst.h <= 0) continue;

        const int ix0 = x0 > cx0 ? x0 : cx0;
        const int iy0 = y0 > cy0 ? y0 : cy0;
        const int ix1 = x1 < cx1 ? x1 : cx1;
        const int iy1 = y1 < cy1 ? y1 : cy1;
        if (ix1 <= ix0 || iy1 <= iy0) continue;

        if (!synced) {
            if (gpu_sync_bg() != 0) {
                psp_render_software.draw(prim, v + i, count - i, st);
                return;
            }
            synced = 1;
            g_dirty = 1;
        }
        SDL_RenderSetClipRect(g_r, &clip);

        if (st->tex_enable) {
            const uint32_t tint = st->tex_func == 3 ? 0xFFFFFFFFu : b->rgba;
            SDL_SetTextureColorMod(g_src, (uint8_t)(tint & 0xFF),
                                   (uint8_t)((tint >> 8) & 0xFF),
                                   (uint8_t)((tint >> 16) & 0xFF));
            SDL_SetTextureAlphaMod(g_src, (uint8_t)((tint >> 24) & 0xFF));
            const SDL_Rect sr = { (int)a->u, (int)a->v,
                                  (int)(b->u - a->u), (int)(b->v - a->v) };
            SDL_RenderCopy(g_r, g_src, &sr, &dst);
        } else {
            const uint32_t col = b->rgba;
            SDL_SetRenderDrawColor(g_r, (uint8_t)(col & 0xFF),
                                   (uint8_t)((col >> 8) & 0xFF),
                                   (uint8_t)((col >> 16) & 0xFF),
                                   (uint8_t)((col >> 24) & 0xFF));
            SDL_RenderFillRect(g_r, &dst);
        }

        if (ix0 < g_dx0) g_dx0 = ix0;
        if (iy0 < g_dy0) g_dy0 = iy0;
        if (ix1 > g_dx1) g_dx1 = ix1;
        if (iy1 > g_dy1) g_dy1 = iy1;
        psp_render_add_pixels((uint64_t)(ix1 - ix0) * (uint64_t)(iy1 - iy0));
    }
    SDL_RenderSetClipRect(g_r, NULL);
}

static void gpu_finish(void) { gpu_flush(); }
static void gpu_present(void) { gpu_flush(); }

void gpu_shutdown(void) {
    gpu_flush();
    if (g_src) { SDL_DestroyTexture(g_src); g_src = NULL; }
    if (g_bg) { SDL_DestroyTexture(g_bg); g_bg = NULL; }
    if (g_target) { SDL_DestroyTexture(g_target); g_target = NULL; }
    if (g_r) { SDL_DestroyRenderer(g_r); g_r = NULL; }
    if (g_win) { SDL_DestroyWindow(g_win); g_win = NULL; }
    g_src_w = g_src_h = 0;
    free(g_texels); g_texels = NULL; g_texcap = 0;
}

const psp_render_backend psp_render_gpu = {
    "gpu", gpu_init, gpu_shutdown, gpu_target, gpu_draw, gpu_finish, gpu_present
};
