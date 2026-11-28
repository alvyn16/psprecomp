/* psprecomp — render backend selection, and the two backends that need no
 * external dependencies. See docs/RENDERER.md.
 *
 * The software backend is not a fallback. It is the reference: every other
 * backend is correct exactly insofar as it agrees with this one on the same
 * display list, and `tests/test_raster.c` pins its behaviour using synthetic
 * lists with no game data and no GPU. A GPU backend that cannot be diffed
 * against something is a backend whose bugs are indistinguishable from the
 * game's.
 *
 * Per pixel, in order: clear mode short-circuits everything; otherwise vertex
 * colour, texture function, alpha test, depth test, blend, write. Stencil,
 * fog, dithering, colour test and logic ops are not implemented.
 */

#include "psprecomp/render.h"
#include "psprecomp/mem.h"

#include <string.h>

/* ---- shared target state ------------------------------------------------- */

static uint32_t g_fb_addr, g_fb_stride;
static int      g_fb_fmt = 3;
static uint64_t g_pixels;
static uint64_t g_bad_texels;

uint64_t psp_render_pixels(void) { return g_pixels; }
void     psp_render_reset_pixels(void) { g_pixels = 0; g_bad_texels = 0; }
void     psp_render_add_pixels(uint64_t n) { g_pixels += n; }
uint64_t psp_render_unsupported_texels(void) { return g_bad_texels; }

/* ---- colour helpers ------------------------------------------------------ */

#define R(c) ((c) & 0xFF)
#define G(c) (((c) >> 8) & 0xFF)
#define B(c) (((c) >> 16) & 0xFF)
#define A(c) ((c) >> 24)

static uint32_t rgba(int r, int g, int b, int a) {
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (a > 255) a = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    if (a < 0) a = 0;
    return (uint32_t)r | ((uint32_t)g << 8) | ((uint32_t)b << 16) | ((uint32_t)a << 24);
}

static int mul8(int a, int b) { return (a * b + 127) / 255; }

typedef struct {
    uint8_t       *fb_base;
    uint16_t      *z_base;
    const uint8_t *tex_base;
    uint32_t       bytes;
    int            have_z;
    uint32_t       tex_rowb;
    uint32_t       tex_blocks;
    uint32_t       dxt_bpr;
    int            sc_x0, sc_y0, sc_x1, sc_y1;
    uint32_t       pmask;
    uint32_t       amask;
    int            stest_eff;
    uint32_t       dxt_last_off;
    uint32_t       dxt_lut[4];
} render_ctx;

uint32_t psp_color_expand(uint32_t px, int fmt) {
    uint32_t r, g, b, a;
    switch (fmt) {
    case 0:   /* 565 */
        r = px & 0x1F; g = (px >> 5) & 0x3F; b = (px >> 11) & 0x1F;
        return rgba((int)((r << 3) | (r >> 2)), (int)((g << 2) | (g >> 4)),
                    (int)((b << 3) | (b >> 2)), 255);
    case 1:   /* 5551 */
        r = px & 0x1F; g = (px >> 5) & 0x1F; b = (px >> 10) & 0x1F;
        return rgba((int)((r << 3) | (r >> 2)), (int)((g << 3) | (g >> 2)),
                    (int)((b << 3) | (b >> 2)), (px & 0x8000) ? 255 : 0);
    case 2:   /* 4444 */
        r = px & 0xF; g = (px >> 4) & 0xF; b = (px >> 8) & 0xF; a = (px >> 12) & 0xF;
        return rgba((int)((r << 4) | r), (int)((g << 4) | g), (int)((b << 4) | b),
                    (int)((a << 4) | a));
    default:  /* 8888 */
        return px;
    }
}

/* The inverse, for writing 16-bit framebuffers. */
uint32_t psp_color_pack(uint32_t c, int fmt) {
    switch (fmt) {
    case 0:  return (R(c) >> 3) | ((G(c) >> 2) << 5) | ((B(c) >> 3) << 11);
    case 1:  return (R(c) >> 3) | ((G(c) >> 3) << 5) | ((B(c) >> 3) << 10) | ((A(c) >> 7) << 15);
    case 2:  return (R(c) >> 4) | ((G(c) >> 4) << 4) | ((B(c) >> 4) << 8) | ((A(c) >> 4) << 12);
    default: return c;
    }
}

/* ---- textures ------------------------------------------------------------ */

static const int TEX_BPP[11] = { 16, 16, 16, 32, 4, 8, 16, 32, 4, 8, 8 };

static uint8_t tex_r8(const uint8_t *base, uint32_t addr, uint32_t off) {
    return base ? base[off] : psp_read8(addr + off);
}
static uint16_t tex_r16(const uint8_t *base, uint32_t addr, uint32_t off) {
    if (base) { uint16_t v; memcpy(&v, base + off, 2); return v; }
    return psp_read16(addr + off);
}
static uint32_t tex_r32(const uint8_t *base, uint32_t addr, uint32_t off) {
    if (base) { uint32_t v; memcpy(&v, base + off, 4); return v; }
    return psp_read32(addr + off);
}

/* Byte offset of texel byte column `xb` in row `y`. A swizzled texture is
 * stored as 16-byte x 8-row blocks, left to right, then top to bottom; that is
 * what makes a row of blocks one contiguous 128-byte run per block. */
static uint32_t tex_offset(const render_ctx *ctx, const psp_draw_state *st, uint32_t xb, uint32_t y) {
    uint32_t rowb = ctx ? ctx->tex_rowb : (uint32_t)st->tex_bw * (uint32_t)TEX_BPP[st->tex_fmt] / 8;
    if (!st->tex_swizzle) return y * rowb + xb;
    uint32_t blocks = ctx ? ctx->tex_blocks : rowb / 16;
    return ((y >> 3) * blocks + (xb >> 4)) * 128 + (y & 7) * 16 + (xb & 15);
}

static uint32_t clut_entry(const psp_draw_state *st, uint32_t idx) {
    uint32_t e = ((idx >> st->clut_shift) & (uint32_t)st->clut_mask) |
                 ((uint32_t)st->clut_start << 4);
    if (!st->clut) return 0;
    if (st->clut_fmt == 3) {
        if (e >= 256) return 0;                 /* the CLUT cache is 1 KB */
        const uint8_t *p = st->clut + e * 4;
        return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    }
    if (e >= 512) return 0;
    const uint8_t *p = st->clut + e * 2;
    return psp_color_expand((uint32_t)p[0] | ((uint32_t)p[1] << 8), st->clut_fmt);
}

static int wrap(int c, int size, int clamp) {
    if (clamp) return c < 0 ? 0 : (c >= size ? size - 1 : c);
    return c & (size - 1);                      /* sizes are powers of two */
}

/* DXT1/3/5. The PSP lays a block out with its colour indices first -- one
 * byte per row, two bits per texel -- then the two 565 endpoint colours, the
 * reverse of the desktop order. DXT3 and DXT5 follow the colour half with
 * their alpha half. Blocks run left to right in rows of tex_bw / 4. */
static uint32_t dxt_texel(render_ctx *ctx, const psp_draw_state *st, uint32_t x, uint32_t y) {
    const uint32_t size = st->tex_fmt == PSP_TPSM_DXT1 ? 8u : 16u;
    const uint32_t bpr = ctx ? ctx->dxt_bpr : (uint32_t)st->tex_bw / 4u;
    const uint32_t blk_off = ((y >> 2) * bpr + (x >> 2)) * size;
    const uint8_t *base = ctx ? ctx->tex_base : NULL;
    const uint32_t i = (y & 3) * 4 + (x & 3);

    uint32_t *lut;
    uint32_t local_lut[4];
    if (ctx && ctx->dxt_last_off == blk_off) {
        lut = ctx->dxt_lut;
    } else {
        lut = ctx ? ctx->dxt_lut : local_lut;
        const uint32_t c0 = tex_r16(base, st->tex_addr, blk_off + 4);
        const uint32_t c1 = tex_r16(base, st->tex_addr, blk_off + 6);
        const uint32_t a = psp_color_expand(c0, 0), b = psp_color_expand(c1, 0);
        lut[0] = a;
        lut[1] = b;
        if (c0 > c1 || st->tex_fmt != PSP_TPSM_DXT1) {
            lut[2] = rgba((2 * R(a) + R(b)) / 3, (2 * G(a) + G(b)) / 3, (2 * B(a) + B(b)) / 3, 255);
            lut[3] = rgba((R(a) + 2 * R(b)) / 3, (G(a) + 2 * G(b)) / 3, (B(a) + 2 * B(b)) / 3, 255);
        } else {
            lut[2] = rgba((R(a) + R(b)) / 2, (G(a) + G(b)) / 2, (B(a) + B(b)) / 2, 255);
            lut[3] = 0;                         /* DXT1 three-colour mode: transparent */
        }
        if (ctx) ctx->dxt_last_off = blk_off;
    }
    uint32_t out = lut[(tex_r32(base, st->tex_addr, blk_off) >> (2 * i)) & 3];
    if (st->tex_fmt == PSP_TPSM_DXT3) {
        const uint32_t al = (tex_r16(base, st->tex_addr, blk_off + 8 + (y & 3) * 2) >> ((x & 3) * 4)) & 0xF;
        out = (out & 0x00FFFFFFu) | ((al * 17) << 24);
    } else if (st->tex_fmt == PSP_TPSM_DXT5) {
        const uint64_t bits = (uint64_t)tex_r16(base, st->tex_addr, blk_off + 12) |
                              ((uint64_t)tex_r32(base, st->tex_addr, blk_off + 8) << 16);
        const int a0 = tex_r8(base, st->tex_addr, blk_off + 14);
        const int a1 = tex_r8(base, st->tex_addr, blk_off + 15);
        const int k = (int)(bits >> (3 * i)) & 7;
        int al;
        if (k == 0) al = a0;
        else if (k == 1) al = a1;
        else if (a0 > a1) al = ((8 - k) * a0 + (k - 1) * a1) / 7;
        else if (k < 6) al = ((6 - k) * a0 + (k - 1) * a1) / 5;
        else al = k == 6 ? 0 : 255;
        out = (out & 0x00FFFFFFu) | ((uint32_t)al << 24);
    }
    return out;
}

static uint32_t tex_fetch_ctx(render_ctx *ctx, const psp_draw_state *st, int x, int y) {
    if (st->tex_fmt > PSP_TPSM_DXT5 || st->tex_w <= 0 || st->tex_h <= 0 || st->tex_bw <= 0) {
        g_bad_texels++;
        return 0xFFFF00FFu;                     /* nonsense: loud magenta */
    }
    x = wrap(x, st->tex_w, st->tex_clamp_u);
    y = wrap(y, st->tex_h, st->tex_clamp_v);
    if (st->tex_fmt >= PSP_TPSM_DXT1) return dxt_texel(ctx, st, (uint32_t)x, (uint32_t)y);

    const int bpp = TEX_BPP[st->tex_fmt];
    const uint32_t off = tex_offset(ctx, st, (uint32_t)x * (uint32_t)bpp / 8, (uint32_t)y);
    const uint8_t *base = ctx ? ctx->tex_base : NULL;
    switch (st->tex_fmt) {
    case PSP_TPSM_565: case PSP_TPSM_5551: case PSP_TPSM_4444:
        return psp_color_expand(tex_r16(base, st->tex_addr, off), st->tex_fmt);
    case PSP_TPSM_8888:
        return tex_r32(base, st->tex_addr, off);
    case PSP_TPSM_T4:
        return clut_entry(st, (tex_r8(base, st->tex_addr, off) >> ((x & 1) * 4)) & 0xF);
    case PSP_TPSM_T8:
        return clut_entry(st, tex_r8(base, st->tex_addr, off));
    case PSP_TPSM_T16:
        return clut_entry(st, tex_r16(base, st->tex_addr, off));
    default:  /* T32 */
        return clut_entry(st, tex_r32(base, st->tex_addr, off));
    }
}

uint32_t psp_tex_fetch(const psp_draw_state *st, int x, int y) {
    return tex_fetch_ctx(NULL, st, x, y);
}

/* The texture function combines the texel `t` with the fragment colour `c`. */
static uint32_t tex_combine(const psp_draw_state *st, uint32_t t, uint32_t c) {
    int r, g, b, a;
    const int ta = st->tex_alpha;
    switch (st->tex_func) {
    case 0:   /* modulate */
        r = mul8(R(t), R(c)); g = mul8(G(t), G(c)); b = mul8(B(t), B(c));
        a = ta ? mul8(A(t), A(c)) : (int)A(c);
        break;
    case 1:   /* decal */
        if (ta) {
            r = (R(t) * A(t) + R(c) * (255 - A(t)) + 127) / 255;
            g = (G(t) * A(t) + G(c) * (255 - A(t)) + 127) / 255;
            b = (B(t) * A(t) + B(c) * (255 - A(t)) + 127) / 255;
        } else { r = R(t); g = G(t); b = B(t); }
        a = (int)A(c);
        break;
    case 2: { /* blend: towards the environment colour by the texel */
        uint32_t e = st->tex_env;
        r = (R(c) * (255 - R(t)) + R(e) * R(t) + 127) / 255;
        g = (G(c) * (255 - G(t)) + G(e) * G(t) + 127) / 255;
        b = (B(c) * (255 - B(t)) + B(e) * B(t) + 127) / 255;
        a = ta ? mul8(A(t), A(c)) : (int)A(c);
        break;
    }
    case 3:   /* replace */
        r = R(t); g = G(t); b = B(t);
        a = ta ? (int)A(t) : (int)A(c);
        break;
    default:  /* 4: add */
        r = R(t) + R(c); g = G(t) + G(c); b = B(t) + B(c);
        a = ta ? mul8(A(t), A(c)) : (int)A(c);
        break;
    }
    if (st->tex_double) { r *= 2; g *= 2; b *= 2; }
    return rgba(r, g, b, a);
}

static int alpha_pass(const psp_draw_state *st, uint32_t c) {
    int a = (int)A(c) & st->atest_mask, ref = st->atest_ref & st->atest_mask;
    switch (st->atest_func) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return a == ref;
    case 3:  return a != ref;
    case 4:  return a < ref;
    case 5:  return a <= ref;
    case 6:  return a > ref;
    default: return a >= ref;
    }
}

/* Blend factor for one channel. Codes 0 and 1 name the *other* colour: as a
 * source factor they are the destination colour, as a destination factor the
 * source colour. Codes 2-9 are alpha-based for both, and 10 is SFIX/DFIX. */
static int factor(int code, int is_src, int ch, uint32_t s, uint32_t d, uint32_t fix) {
    int other = is_src ? (int)((d >> (ch * 8)) & 0xFF) : (int)((s >> (ch * 8)) & 0xFF);
    switch (code) {
    case 0:  return other;
    case 1:  return 255 - other;
    case 2:  return (int)A(s);
    case 3:  return 255 - (int)A(s);
    case 4:  return (int)A(d);
    case 5:  return 255 - (int)A(d);
    case 6:  return 2 * (int)A(s) > 255 ? 255 : 2 * (int)A(s);
    case 7:  return 255 - 2 * (int)A(s) < 0 ? 0 : 255 - 2 * (int)A(s);
    case 8:  return 2 * (int)A(d) > 255 ? 255 : 2 * (int)A(d);
    case 9:  return 255 - 2 * (int)A(d) < 0 ? 0 : 255 - 2 * (int)A(d);
    default: return (int)((fix >> (ch * 8)) & 0xFF);
    }
}

static uint32_t blend(const psp_draw_state *st, uint32_t s, uint32_t d) {
    int out[3];
    for (int ch = 0; ch < 3; ch++) {
        int sc = (int)((s >> (ch * 8)) & 0xFF), dc = (int)((d >> (ch * 8)) & 0xFF);
        int sv = mul8(sc, factor(st->blend_src, 1, ch, s, d, st->blend_sfix));
        int dv = mul8(dc, factor(st->blend_dst, 0, ch, s, d, st->blend_dfix));
        switch (st->blend_op) {
        case 0:  out[ch] = sv + dv; break;
        case 1:  out[ch] = sv - dv; break;
        case 2:  out[ch] = dv - sv; break;
        case 3:  out[ch] = sc < dc ? sc : dc; break;
        case 4:  out[ch] = sc > dc ? sc : dc; break;
        default: out[ch] = sc > dc ? sc - dc : dc - sc; break;
        }
    }
    /* ponytail: the framebuffer's alpha byte is stencil on hardware; the
     * source alpha is written instead until stencil exists. */
    return rgba(out[0], out[1], out[2], (int)A(s));
}

/* ---- software backend ---------------------------------------------------- */

static int sw_init(int w, int h) { (void)w; (void)h; return 0; }
static void sw_shutdown(void) { }

static void sw_target(uint32_t addr, uint32_t stride, int fmt) {
    g_fb_addr = addr;
    g_fb_stride = stride;
    g_fb_fmt = fmt;
}

static uint32_t fb_read(uint32_t at) {
    return g_fb_fmt == 3 ? psp_read32(at) : psp_color_expand(psp_read16(at), g_fb_fmt);
}

static int depth_pass(int func, int z, int d) {
    switch (func) {
    case 0:  return 0;
    case 1:  return 1;
    case 2:  return z == d;
    case 3:  return z != d;
    case 4:  return z < d;
    case 5:  return z <= d;
    case 6:  return z > d;
    default: return z >= d;
    }
}

static int stencil_op(int op, int dst, int ref) {
    switch (op) {
    case 0:  return dst;
    case 1:  return 0;
    case 2:  return ref & 0xFF;
    case 3:  return (~dst) & 0xFF;
    case 4:  return dst < 255 ? dst + 1 : 255;
    case 5:  return dst > 0 ? dst - 1 : 0;
    default: return dst;
    }
}

static void fb_write_fast(const render_ctx *ctx, int x, int y, uint32_t at, uint32_t c) {
    if (ctx->fb_base) {
        const size_t idx = (size_t)y * (size_t)g_fb_stride + (size_t)x;
        if (ctx->bytes == 4u) {
            memcpy(ctx->fb_base + idx * 4u, &c, 4);
        } else {
            uint16_t p = (uint16_t)psp_color_pack(c, g_fb_fmt);
            memcpy(ctx->fb_base + idx * 2u, &p, 2);
        }
    } else if (ctx->bytes == 4u) {
        psp_write32(at, c);
    } else {
        psp_write16(at, (uint16_t)psp_color_pack(c, g_fb_fmt));
    }
    g_pixels++;
}

static uint32_t fb_read_fast(const render_ctx *ctx, int x, int y, uint32_t at) {
    if (ctx->fb_base) {
        const size_t idx = (size_t)y * (size_t)g_fb_stride + (size_t)x;
        if (ctx->bytes == 4u) {
            uint32_t v;
            memcpy(&v, ctx->fb_base + idx * 4u, 4);
            return v;
        }
        uint16_t v;
        memcpy(&v, ctx->fb_base + idx * 2u, 2);
        return psp_color_expand(v, g_fb_fmt);
    }
    return fb_read(at);
}

static uint16_t z_read_fast(const render_ctx *ctx, const psp_draw_state *st, int x, int y, uint32_t zat) {
    return ctx->z_base ? ctx->z_base[(size_t)y * (size_t)st->zbw + (size_t)x] : psp_read16(zat);
}

static void z_write_fast(const render_ctx *ctx, const psp_draw_state *st, int x, int y, uint32_t zat, uint16_t z) {
    if (ctx->z_base) ctx->z_base[(size_t)y * (size_t)st->zbw + (size_t)x] = z;
    else             psp_write16(zat, z);
}

static void fb_write_stencil(const render_ctx *ctx, int x, int y, uint32_t at,
                             uint32_t fb, int new_a) {
    const uint32_t out = (fb & (0x00FFFFFFu | ctx->amask)) | (((uint32_t)new_a << 24) & ~ctx->amask);
    fb_write_fast(ctx, x, y, at, out);
}

/* One fragment through the pipeline (coordinates already clamped to screen + scissor). */
static void fragment_fast(render_ctx *ctx, const psp_draw_state *st, int x, int y,
                          uint32_t c, float u, float v, float zf) {
    const uint32_t at = ctx->fb_base ? 0u : g_fb_addr + (uint32_t)(y * (int)g_fb_stride + x) * ctx->bytes;
    const int z = zf < 0.0f ? 0 : (zf > 65535.0f ? 65535 : (int)(zf + 0.5f));
    const uint32_t zat = (!ctx->have_z || ctx->z_base) ? 0u : st->zaddr + (uint32_t)(y * st->zbw + x) * 2u;

    if (st->clear) {
        if (st->clear_depth && ctx->have_z) z_write_fast(ctx, st, x, y, zat, (uint16_t)z);
        if (!st->clear_color && !st->clear_alpha) return;
        if (!st->clear_color || !st->clear_alpha) {
            uint32_t d = fb_read_fast(ctx, x, y, at);
            c = st->clear_color ? (c & 0x00FFFFFFu) | (d & 0xFF000000u)
                                : (d & 0x00FFFFFFu) | (c & 0xFF000000u);
        }
        fb_write_fast(ctx, x, y, at, c);
        return;
    }

    uint32_t fb = 0;
    int have_fb = 0;
    if (!st->atest) {
        /* When alpha test is disabled, stencil and Z tests cannot depend on the
         * texture sample, so reject occluded pixels before fetching/decoding texels. */
        if (ctx->stest_eff) {
            fb = fb_read_fast(ctx, x, y, at);
            have_fb = 1;
            const int dst_a = (int)A(fb);
            if (!depth_pass(st->sfunc, st->sref & st->smask, dst_a & st->smask)) {
                if (st->sfail != 0)
                    fb_write_stencil(ctx, x, y, at, fb, stencil_op(st->sfail, dst_a, st->sref));
                return;
            }
            if (st->ztest && ctx->have_z) {
                if (!depth_pass(st->zfunc, z, z_read_fast(ctx, st, x, y, zat))) {
                    if (st->szfail != 0)
                        fb_write_stencil(ctx, x, y, at, fb, stencil_op(st->szfail, dst_a, st->sref));
                    return;
                }
                if (st->zwrite) z_write_fast(ctx, st, x, y, zat, (uint16_t)z);
            }
            if (ctx->pmask == 0xFFFFFFFFu && st->szpass == 0) return;
        } else if (st->ztest && ctx->have_z) {
            if (!depth_pass(st->zfunc, z, z_read_fast(ctx, st, x, y, zat))) return;
            if (st->zwrite) z_write_fast(ctx, st, x, y, zat, (uint16_t)z);
        }
        if (st->tex_enable && ctx->pmask != 0xFFFFFFFFu) {
            int tx = (int)u, ty = (int)v;
            if (u < 0 && (float)tx != u) tx--;
            if (v < 0 && (float)ty != v) ty--;
            c = tex_combine(st, tex_fetch_ctx(ctx, st, tx, ty), c);
        }
    } else {
        if (st->tex_enable) {
            int tx = (int)u, ty = (int)v;
            if (u < 0 && (float)tx != u) tx--;
            if (v < 0 && (float)ty != v) ty--;
            c = tex_combine(st, tex_fetch_ctx(ctx, st, tx, ty), c);
        }
        if (!alpha_pass(st, c)) return;
        if (ctx->stest_eff) {
            fb = fb_read_fast(ctx, x, y, at);
            have_fb = 1;
            const int dst_a = (int)A(fb);
            if (!depth_pass(st->sfunc, st->sref & st->smask, dst_a & st->smask)) {
                if (st->sfail != 0)
                    fb_write_stencil(ctx, x, y, at, fb, stencil_op(st->sfail, dst_a, st->sref));
                return;
            }
            if (st->ztest && ctx->have_z) {
                if (!depth_pass(st->zfunc, z, z_read_fast(ctx, st, x, y, zat))) {
                    if (st->szfail != 0)
                        fb_write_stencil(ctx, x, y, at, fb, stencil_op(st->szfail, dst_a, st->sref));
                    return;
                }
                if (st->zwrite) z_write_fast(ctx, st, x, y, zat, (uint16_t)z);
            }
        } else if (st->ztest && ctx->have_z) {
            if (!depth_pass(st->zfunc, z, z_read_fast(ctx, st, x, y, zat))) return;
            if (st->zwrite) z_write_fast(ctx, st, x, y, zat, (uint16_t)z);
        }
    }

    if (st->blend) {
        if (!have_fb) { fb = fb_read_fast(ctx, x, y, at); have_fb = 1; }
        c = blend(st, c, fb);
    }
    if (ctx->stest_eff) {
        const int dst_a = (int)A(fb);
        c = (c & 0x00FFFFFFu) | ((uint32_t)stencil_op(st->szpass, dst_a, st->sref) << 24);
    }
    if (ctx->pmask) {
        if (ctx->fb_base && ctx->bytes == 4u && ctx->pmask == 0xFF000000u && !have_fb) {
            uint8_t *p = ctx->fb_base + ((size_t)y * (size_t)g_fb_stride + (size_t)x) * 4u;
            p[0] = (uint8_t)c;
            p[1] = (uint8_t)(c >> 8);
            p[2] = (uint8_t)(c >> 16);
            g_pixels++;
            return;
        }
        if (!have_fb) fb = fb_read_fast(ctx, x, y, at);
        c = (fb & ctx->pmask) | (c & ~ctx->pmask);
    }
    fb_write_fast(ctx, x, y, at, c);
}

static int64_t edge64(const psp_vertex *p, const psp_vertex *q, int x, int y) {
    return (int64_t)(q->x - p->x) * (int64_t)(y - p->y) -
           (int64_t)(q->y - p->y) * (int64_t)(x - p->x);
}

static void render_ctx_init(render_ctx *ctx, const psp_draw_state *st) {
    memset(ctx, 0, sizeof *ctx);
    ctx->dxt_last_off = 0xFFFFFFFFu;
    ctx->bytes = g_fb_fmt == 3 ? 4u : 2u;
    ctx->have_z = st->zaddr && st->zbw > 0;
    ctx->sc_x0 = st->sc_x0 > 0 ? st->sc_x0 : 0;
    ctx->sc_y0 = st->sc_y0 > 0 ? st->sc_y0 : 0;
    ctx->sc_x1 = st->sc_x1 < 479 ? st->sc_x1 : 479;
    ctx->sc_y1 = st->sc_y1 < 271 ? st->sc_y1 : 271;
    ctx->amask = (st->mask_a & 0xFFu) << 24;
    ctx->pmask = ctx->amask | (st->mask_rgb & 0x00FFFFFFu);
    ctx->stest_eff = st->stest;
    if (st->stest && st->sfunc == 1 && st->szfail == 0 && st->szpass == 0) {
        /* Stencil ALWAYS passes and KEEP preserves destination alpha on every outcome,
         * so skip pre-Z framebuffer reads and simply mask alpha writes. */
        ctx->stest_eff = 0;
        ctx->amask = 0xFF000000u;
        ctx->pmask |= 0xFF000000u;
    }
    if (ctx->sc_x0 > ctx->sc_x1 || ctx->sc_y0 > ctx->sc_y1) return;

    const int watch = psp_mem_watching();
    if (!watch && g_fb_addr && g_fb_stride) {
        const uint32_t fb_need = ((uint32_t)ctx->sc_y1 * g_fb_stride + (uint32_t)ctx->sc_x1 + 1u) * ctx->bytes;
        ctx->fb_base = (uint8_t *)psp_mem_ptr(g_fb_addr, fb_need);
    }
    if (!watch && ctx->have_z) {
        const uint32_t z_need = ((uint32_t)ctx->sc_y1 * (uint32_t)st->zbw + (uint32_t)ctx->sc_x1 + 1u) * 2u;
        ctx->z_base = (uint16_t *)psp_mem_ptr(st->zaddr, z_need);
    }
    if (st->tex_enable && st->tex_fmt <= PSP_TPSM_DXT5 && st->tex_w > 0 && st->tex_h > 0 && st->tex_bw > 0) {
        if (st->tex_fmt >= PSP_TPSM_DXT1) {
            ctx->dxt_bpr = (uint32_t)st->tex_bw / 4u;
            if (!watch) {
                const uint32_t bsz = st->tex_fmt == PSP_TPSM_DXT1 ? 8u : 16u;
                const uint32_t max_blk = ((uint32_t)(st->tex_h - 1) >> 2) * ctx->dxt_bpr + ((uint32_t)(st->tex_w - 1) >> 2);
                ctx->tex_base = (const uint8_t *)psp_mem_ptr(st->tex_addr, (max_blk + 1u) * bsz);
            }
        } else {
            const uint32_t bpp = (uint32_t)TEX_BPP[st->tex_fmt];
            ctx->tex_rowb = (uint32_t)st->tex_bw * bpp / 8u;
            ctx->tex_blocks = ctx->tex_rowb / 16u;
            if (!watch && (!st->tex_swizzle || ctx->tex_blocks > 0)) {
                const uint32_t max_xb = ((uint32_t)(st->tex_w - 1) * bpp) / 8u;
                const uint32_t tbytes = bpp >= 8u ? bpp / 8u : 1u;
                const uint32_t need = st->tex_swizzle
                    ? ((((uint32_t)(st->tex_h - 1) >> 3) * ctx->tex_blocks + (max_xb >> 4) + 1u) * 128u)
                    : ((uint32_t)(st->tex_h - 1) * ctx->tex_rowb + max_xb + tbytes);
                ctx->tex_base = (const uint8_t *)psp_mem_ptr(st->tex_addr, need);
            }
        }
    }
}

/* Barycentric fill with incremental integer edge functions, so a shared edge
 * belongs to exactly one triangle: adjacent geometry neither double-draws nor
 * leaves seams. Colour is interpolated when Gouraud is on; flat uses `c`. */
static void sw_tri(render_ctx *ctx, const psp_vertex *a, const psp_vertex *b,
                   const psp_vertex *c, const psp_draw_state *st) {
    if (!g_fb_addr || !g_fb_stride || a->behind || b->behind || c->behind) return;

    int minx = a->x < b->x ? (a->x < c->x ? a->x : c->x) : (b->x < c->x ? b->x : c->x);
    int maxx = a->x > b->x ? (a->x > c->x ? a->x : c->x) : (b->x > c->x ? b->x : c->x);
    int miny = a->y < b->y ? (a->y < c->y ? a->y : c->y) : (b->y < c->y ? b->y : c->y);
    int maxy = a->y > b->y ? (a->y > c->y ? a->y : c->y) : (b->y > c->y ? b->y : c->y);

    if (minx < ctx->sc_x0) minx = ctx->sc_x0;
    if (miny < ctx->sc_y0) miny = ctx->sc_y0;
    if (maxx > ctx->sc_x1) maxx = ctx->sc_x1;
    if (maxy > ctx->sc_y1) maxy = ctx->sc_y1;
    if (minx > maxx || miny > maxy) return;

    int64_t area = edge64(a, b, c->x, c->y);
    if (area == 0) return;
    if (st->cull && (st->cull_ccw ? area > 0 : area < 0)) return;

    int64_t dx0 = a->y - b->y, dy0 = b->x - a->x;
    int64_t dx1 = b->y - c->y, dy1 = c->x - b->x;
    int64_t dx2 = c->y - a->y, dy2 = a->x - c->x;
    float h0 = 0.5f * (float)((b->x - a->x) - (b->y - a->y));
    float h1 = 0.5f * (float)((c->x - b->x) - (c->y - b->y));
    float h2 = 0.5f * (float)((a->x - c->x) - (a->y - c->y));
    int64_t w0_row = edge64(a, b, minx, miny);
    int64_t w1_row = edge64(b, c, minx, miny);
    int64_t w2_row = edge64(c, a, minx, miny);
    if (area < 0) {
        area = -area;
        dx0 = -dx0; dy0 = -dy0; h0 = -h0; w0_row = -w0_row;
        dx1 = -dx1; dy1 = -dy1; h1 = -h1; w1_row = -w1_row;
        dx2 = -dx2; dy2 = -dy2; h2 = -h2; w2_row = -w2_row;
    }
    const float inv = 1.0f / (float)area;
    const int gouraud = st->gouraud;
    const uint32_t flat_col = c->rgba;
    const int ar = (int)R(a->rgba), ag = (int)G(a->rgba), ab = (int)B(a->rgba), aa = (int)A(a->rgba);
    const int br = (int)R(b->rgba), bg = (int)G(b->rgba), bb = (int)B(b->rgba), ba = (int)A(b->rgba);
    const int cr = (int)R(flat_col), cg = (int)G(flat_col), cb = (int)B(flat_col), ca = (int)A(flat_col);

    for (int y = miny; y <= maxy; y++, w0_row += dy0, w1_row += dy1, w2_row += dy2) {
        int64_t w0 = w0_row, w1 = w1_row, w2 = w2_row;
        for (int x = minx; x <= maxx; x++, w0 += dx0, w1 += dx1, w2 += dx2) {
            if ((w0 | w1 | w2) < 0) continue;
            const float la = ((float)w1 + h1) * inv;
            const float lb = ((float)w2 + h2) * inv;
            const float lc = ((float)w0 + h0) * inv;
            uint32_t col = flat_col;
            if (gouraud) {
                col = rgba((int)(ar * la + br * lb + cr * lc + 0.5f),
                           (int)(ag * la + bg * lb + cg * lc + 0.5f),
                           (int)(ab * la + bb * lb + cb * lc + 0.5f),
                           (int)(aa * la + ba * lb + ca * lc + 0.5f));
            }
            fragment_fast(ctx, st, x, y, col,
                          a->u * la + b->u * lb + c->u * lc,
                          a->v * la + b->v * lb + c->v * lc,
                          a->z * la + b->z * lb + c->z * lc);
        }
    }
}

/* A sprite is the PSP's 2D quad: two vertices giving opposite corners of an
 * axis-aligned rectangle, coloured by the second. The far edge is exclusive
 * so adjacent sprites tile without overlapping. */
static void sw_sprite(render_ctx *ctx, const psp_vertex *a, const psp_vertex *b,
                      const psp_draw_state *st) {
    if (!g_fb_addr || !g_fb_stride) return;
    int x0 = a->x < b->x ? a->x : b->x, x1 = a->x > b->x ? a->x : b->x;
    int y0 = a->y < b->y ? a->y : b->y, y1 = a->y > b->y ? a->y : b->y;
    if (x0 == x1 || y0 == y1) return;

    int cx0 = x0 > ctx->sc_x0 ? x0 : ctx->sc_x0;
    int cy0 = y0 > ctx->sc_y0 ? y0 : ctx->sc_y0;
    int cx1 = x1 < ctx->sc_x1 + 1 ? x1 : ctx->sc_x1 + 1;
    int cy1 = y1 < ctx->sc_y1 + 1 ? y1 : ctx->sc_y1 + 1;
    if (cx0 >= cx1 || cy0 >= cy1) return;

    if (st->clear && ctx->fb_base && (!ctx->have_z || ctx->z_base || !st->clear_depth)) {
        const float zf = b->z;
        const uint16_t z = (uint16_t)(zf < 0.0f ? 0 : (zf > 65535.0f ? 65535 : (int)(zf + 0.5f)));
        const uint32_t clear_c = b->rgba;
        const uint16_t clear_16 = ctx->bytes == 2u ? (uint16_t)psp_color_pack(clear_c, g_fb_fmt) : 0u;
        const int span = cx1 - cx0;
        for (int y = cy0; y < cy1; y++) {
            if (st->clear_depth && ctx->have_z) {
                uint16_t *zr = ctx->z_base + (size_t)y * (size_t)st->zbw + (size_t)cx0;
                for (int x = 0; x < span; x++) zr[x] = z;
            }
            if (st->clear_color || st->clear_alpha) {
                const size_t row_idx = (size_t)y * (size_t)g_fb_stride + (size_t)cx0;
                if (st->clear_color && st->clear_alpha) {
                    if (ctx->bytes == 4u) {
                        uint32_t *fr = (uint32_t *)(ctx->fb_base + row_idx * 4u);
                        for (int x = 0; x < span; x++) fr[x] = clear_c;
                    } else {
                        uint16_t *fr = (uint16_t *)(ctx->fb_base + row_idx * 2u);
                        for (int x = 0; x < span; x++) fr[x] = clear_16;
                    }
                    g_pixels += (uint64_t)span;
                } else {
                    for (int x = cx0; x < cx1; x++)
                        fragment_fast(ctx, st, x, y, clear_c, 0, 0, b->z);
                }
            }
        }
        return;
    }

    const float du = (b->u - a->u) / (float)(b->x - a->x);
    const float dv = (b->v - a->v) / (float)(b->y - a->y);
    for (int y = cy0; y < cy1; y++) {
        const float tv = a->v + ((float)(y - a->y) + 0.5f) * dv;
        for (int x = cx0; x < cx1; x++)
            fragment_fast(ctx, st, x, y, b->rgba, a->u + ((float)(x - a->x) + 0.5f) * du, tv, b->z);
    }
}

static void sw_draw(int prim, const psp_vertex *v, int count, const psp_draw_state *st) {
    render_ctx ctx;
    render_ctx_init(&ctx, st);
    switch (prim) {
    case PSP_PRIM_SPRITES:
        for (int i = 0; i + 1 < count; i += 2) sw_sprite(&ctx, &v[i], &v[i + 1], st);
        break;
    case PSP_PRIM_TRIANGLES:
        for (int i = 0; i + 2 < count; i += 3) sw_tri(&ctx, &v[i], &v[i + 1], &v[i + 2], st);
        break;
    case PSP_PRIM_TRIANGLE_STRIP:
        for (int i = 0; i + 2 < count; i++)
            sw_tri(&ctx, (i & 1) ? &v[i + 1] : &v[i], (i & 1) ? &v[i] : &v[i + 1], &v[i + 2], st);
        break;
    case PSP_PRIM_TRIANGLE_FAN:
        for (int i = 1; i + 1 < count; i++) sw_tri(&ctx, &v[0], &v[i], &v[i + 1], st);
        break;
    default:
        break;                       /* points, lines: not yet */
    }
}

static void sw_noop(void) { }

const psp_render_backend psp_render_software = {
    "software", sw_init, sw_shutdown, sw_target, sw_draw, sw_noop, sw_noop
};

/* ---- null backend -------------------------------------------------------- */

/* Draws nothing, and that is the point. During bring-up the question is
 * usually "did the game ask to draw" rather than "does it look right", and the
 * answer is clearer without pixels in the way. */

static int null_init(int w, int h) { (void)w; (void)h; return 0; }
static void null_target(uint32_t a, uint32_t s, int f) { (void)a; (void)s; (void)f; }
static void null_draw(int p, const psp_vertex *v, int n, const psp_draw_state *st) {
    (void)p; (void)v; (void)n; (void)st;
}
static void null_noop(void) { }

const psp_render_backend psp_render_null = {
    "null", null_init, null_noop, null_target, null_draw, null_noop, null_noop
};

/* ---- selection ----------------------------------------------------------- */

static const psp_render_backend *g_backend = &psp_render_software;

const psp_render_backend *psp_render_current(void) { return g_backend; }

int psp_render_select(const char *name) {
    static const psp_render_backend *const all[] = {
        &psp_render_software, &psp_render_null
#ifdef PSPRECOMP_SDL2_WINDOW
        , &psp_render_gpu
#endif
    };
    if (!name) return -1;
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) {
        if (strcmp(all[i]->name, name) == 0) { g_backend = all[i]; return 0; }
    }
    return -1;                       /* unknown: keep the current backend */
}
