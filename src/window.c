/* psprecomp -- a window to play in. See include/psprecomp/window.h. */

#include "psprecomp/window.h"
#include "psprecomp/hle.h"
#include "psprecomp/render.h"

#include <string.h>

#define W 480
#define H 272

/* PSP button bits (SceCtrlData.Buttons), shared by both backends. */
enum {
    B_SELECT = 0x0001, B_START = 0x0008,
    B_UP = 0x0010, B_RIGHT = 0x0020, B_DOWN = 0x0040, B_LEFT = 0x0080,
    B_L = 0x0100, B_R = 0x0200,
    B_TRIANGLE = 0x1000, B_CIRCLE = 0x2000, B_CROSS = 0x4000, B_SQUARE = 0x8000
};
/* Default button map, indexed by the slots in window.h. Both backends read it,
 * so a rebind applies to keyboard and controller alike. */
#define DEFAULT_BUTTON_MAP_INIT { \
    B_UP, B_RIGHT, B_DOWN, B_LEFT, \
    B_CROSS, B_CIRCLE, B_SQUARE, B_TRIANGLE, \
    B_L, B_R, B_START, B_SELECT \
}
static const uint32_t DEFAULT_MAP[PSP_WINDOW_BUTTON_COUNT] = DEFAULT_BUTTON_MAP_INIT;
static uint32_t g_button_map[PSP_WINDOW_BUTTON_COUNT] = DEFAULT_BUTTON_MAP_INIT;

void psp_window_set_button_map(const uint32_t map[PSP_WINDOW_BUTTON_COUNT]) {
    if (map) memcpy(g_button_map, map, sizeof g_button_map);
    else     memcpy(g_button_map, DEFAULT_MAP, sizeof g_button_map);
}


#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>

static volatile LONG g_closed;
static HANDLE g_thread;
static HWND g_hwnd;
static int g_scale;
static const char *g_title = "psprecomp";
static uint32_t g_px[W * H];

static uint32_t keyboard(void) {
    static const int K[] = {
        VK_UP, VK_RIGHT, VK_DOWN, VK_LEFT,
        'Z', 'X', 'A', 'S', 'Q', 'W', VK_RETURN, VK_BACK
    };
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
        if (GetAsyncKeyState(K[i]) & 0x8000) b |= g_button_map[i];
    return b;
}

/* The first XInput controller, if one is connected. */
static uint32_t pad(uint8_t *ax, uint8_t *ay) {
    static const WORD P[] = {
        XINPUT_GAMEPAD_DPAD_UP, XINPUT_GAMEPAD_DPAD_RIGHT,
        XINPUT_GAMEPAD_DPAD_DOWN, XINPUT_GAMEPAD_DPAD_LEFT,
        XINPUT_GAMEPAD_A, XINPUT_GAMEPAD_B, XINPUT_GAMEPAD_X, XINPUT_GAMEPAD_Y,
        XINPUT_GAMEPAD_LEFT_SHOULDER, XINPUT_GAMEPAD_RIGHT_SHOULDER,
        XINPUT_GAMEPAD_START, XINPUT_GAMEPAD_BACK
    };
    XINPUT_STATE s;
    if (XInputGetState(0, &s) != ERROR_SUCCESS) return 0;
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (s.Gamepad.wButtons & P[i]) b |= g_button_map[i];
    if (s.Gamepad.bLeftTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= g_button_map[PSP_WINDOW_L];
    if (s.Gamepad.bRightTrigger > XINPUT_GAMEPAD_TRIGGER_THRESHOLD) b |= g_button_map[PSP_WINDOW_R];
    /* The PSP stick is 0..255 with 128 at rest and y growing downwards. */
    const int lx = s.Gamepad.sThumbLX, ly = s.Gamepad.sThumbLY;
    if (lx * lx + ly * ly > XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE * XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE) {
        *ax = (uint8_t)((lx + 32768) >> 8);
        *ay = (uint8_t)((32767 - ly) >> 8);
    }
    return b;
}

static void paint(HWND h) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(h, &ps);
    RECT r;
    GetClientRect(h, &r);
    const int cw = r.right, ch = r.bottom;
    /* Keep the PSP's aspect, black bars around it. */
    int w = cw, hh = cw * H / W;
    if (hh > ch) { hh = ch; w = ch * W / H; }
    const int x0 = (cw - w) / 2, y0 = (ch - hh) / 2;
    PatBlt(dc, 0, 0, cw, y0, BLACKNESS);
    PatBlt(dc, 0, y0 + hh, cw, ch - y0 - hh, BLACKNESS);
    PatBlt(dc, 0, 0, x0, ch, BLACKNESS);
    PatBlt(dc, x0 + w, 0, cw - x0 - w, ch, BLACKNESS);

    if (psp_display_read(g_px) == 0) {
        for (int i = 0; i < W * H; i++) {      /* 0xAABBGGRR to the DIB's BGRA */
            const uint32_t c = g_px[i];
            g_px[i] = (c & 0xFF00FF00u) | ((c & 0xFFu) << 16) | ((c >> 16) & 0xFFu);
        }
        BITMAPINFO bi;
        ZeroMemory(&bi, sizeof bi);
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;            /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, COLORONCOLOR);   /* nearest: crisp pixels */
        StretchDIBits(dc, x0, y0, w, hh, 0, 0, W, H, g_px, &bi, DIB_RGB_COLORS, SRCCOPY);
    } else {
        PatBlt(dc, x0, y0, w, hh, BLACKNESS);
    }
    EndPaint(h, &ps);
}

static LRESULT CALLBACK proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: {
        uint8_t ax = 128, ay = 128;
        uint32_t b = pad(&ax, &ay);
        if (GetForegroundWindow() == h) b |= keyboard();   /* keys only when focused */
        psp_ctrl_set(b, ax, ay);
        InvalidateRect(h, NULL, FALSE);
        return 0;
    }
    case WM_PAINT:      paint(h); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_DESTROY:    PostQuitMessage(0); return 0;
    default:            return DefWindowProcA(h, msg, wp, lp);
    }
}

static DWORD WINAPI window_main(LPVOID arg) {
    HANDLE ready = (HANDLE)arg;
    WNDCLASSA wc;
    ZeroMemory(&wc, sizeof wc);
    wc.lpfnWndProc = proc;
    wc.hInstance = GetModuleHandleA(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "psprecomp_window";
    RegisterClassA(&wc);

    /* Per-monitor pixels, not Windows' blurry bitmap stretch of a DPI-unaware
     * window. With no scale asked for, take the largest whole multiple that
     * fits most of the screen's height. */
    SetProcessDPIAware();
    if (g_scale <= 0) {
        RECT wa;
        SystemParametersInfoA(SPI_GETWORKAREA, 0, &wa, 0);
        g_scale = (int)((wa.bottom - wa.top) * 7 / 10 / H);
        if (g_scale < 1) g_scale = 1;
    }
    RECT r = { 0, 0, W * g_scale, H * g_scale };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);
    HWND h = CreateWindowA(wc.lpszClassName, g_title, WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                           CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                           NULL, NULL, wc.hInstance, NULL);
    if (!h) { InterlockedExchange(&g_closed, 1); SetEvent(ready); return 1; }
    g_hwnd = h;
    SetTimer(h, 1, 16, NULL);
    SetEvent(ready);

    MSG m;
    while (GetMessageA(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageA(&m);
    }
    InterlockedExchange(&g_closed, 1);
    return 0;
}

int psp_window_start(const char *title, int scale) {
    if (g_thread) return -1;
    InterlockedExchange(&g_closed, 0);
    if (title) g_title = title;
    g_scale = scale;
    HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    if (!ready) return -1;
    HANDLE t = CreateThread(NULL, 0, window_main, ready, 0, NULL);
    if (!t) { CloseHandle(ready); return -1; }
    g_thread = t;
    WaitForSingleObject(ready, INFINITE);       /* the window exists, or failed */
    CloseHandle(ready);
    if (g_closed) {
        WaitForSingleObject(t, INFINITE);
        CloseHandle(t);
        g_thread = NULL;
        return -1;
    }
    return 0;
}

void psp_window_stop(void) {
    if (!g_thread) return;
    InterlockedExchange(&g_closed, 1);
    if (g_hwnd) PostMessageA(g_hwnd, WM_CLOSE, 0, 0);
    WaitForSingleObject(g_thread, INFINITE);
    CloseHandle(g_thread);
    g_thread = NULL;
    g_hwnd = NULL;
}

int psp_window_closed(void) { return g_closed != 0; }

#elif defined(PSPRECOMP_SDL2_WINDOW)

#include <SDL.h>
#include <stdint.h>

#ifdef PSPRECOMP_IMGUI_UI
#include "ui_overlay.h"
#endif

static volatile int g_closed;
static int g_scale;
static int g_win_w = W * 2, g_win_h = H * 2;
static const char *g_title = "psprecomp";
static uint32_t g_px[W * H];
static uint32_t g_hires_px[W * 4 * H * 4];
static SDL_GameController *g_pad;
static SDL_Thread *g_thread;
static int g_focused;
static SDL_JoystickID g_pad_instance = -1;
#define AUDIO_RING_FRAMES 16384u
#define AUDIO_SOURCES     16u

static SDL_mutex *g_audio_lock;
static SDL_AudioDeviceID g_audio_dev;
static int32_t  g_audio_mix[AUDIO_RING_FRAMES * 2u];
static uint64_t g_audio_src_pos[AUDIO_SOURCES];
static uint64_t g_audio_tail, g_audio_max_head;
static volatile int g_audio_vol_256 = 256;

/* Mix each guest audio stream (hardware channels 0..7, Output2 stream 8, or
 * raw SAS fallback 9) by its own write cursor into a shared accumulation ring
 * so concurrent streams (e.g. FMV audio + FMOD) mix without fighting for one FIFO. */
static void audio_ch_sink(int source, const int16_t *stereo, uint32_t frames,
                          uint32_t rate, void *user) {
    (void)rate; (void)user;
    if (!g_audio_lock || !g_audio_dev || !frames) return;
    unsigned src = (unsigned)source < AUDIO_SOURCES ? (unsigned)source : 0u;
    SDL_LockMutex(g_audio_lock);
    if (g_audio_src_pos[src] < g_audio_tail) g_audio_src_pos[src] = g_audio_tail;
    uint32_t off = 0;
    if (frames > AUDIO_RING_FRAMES) {
        off = frames - (uint32_t)AUDIO_RING_FRAMES;
        frames = (uint32_t)AUDIO_RING_FRAMES;
    }
    while (g_audio_src_pos[src] + frames - g_audio_tail > AUDIO_RING_FRAMES) {
        size_t idx = (size_t)(g_audio_tail & (AUDIO_RING_FRAMES - 1u)) * 2u;
        g_audio_mix[idx] = g_audio_mix[idx + 1] = 0;
        g_audio_tail++;
    }
    for (uint32_t i = 0; i < frames; i++) {
        size_t idx = (size_t)((g_audio_src_pos[src] + i) & (AUDIO_RING_FRAMES - 1u)) * 2u;
        g_audio_mix[idx]     += stereo[(off + i) * 2u];
        g_audio_mix[idx + 1] += stereo[(off + i) * 2u + 1u];
    }
    g_audio_src_pos[src] += frames;
    if (g_audio_src_pos[src] > g_audio_max_head) g_audio_max_head = g_audio_src_pos[src];
    SDL_UnlockMutex(g_audio_lock);
}

static int16_t clamp_pcm(int32_t v) {
    if (v >  32767) return  32767;
    if (v < -32768) return -32768;
    return (int16_t)v;
}

static void audio_callback(void *user, Uint8 *stream, int len) {
    (void)user;
    int16_t *out = (int16_t *)stream;
    size_t want = (size_t)len / 4;            /* stereo int16 frames */
    if (!g_audio_lock) { memset(stream, 0, (size_t)len); return; }
    const int vol = g_audio_vol_256;
    SDL_LockMutex(g_audio_lock);
    for (size_t i = 0; i < want; i++) {
        if (g_audio_tail >= g_audio_max_head) { out[i * 2] = out[i * 2 + 1] = 0; continue; }
        size_t idx = (size_t)(g_audio_tail & (AUDIO_RING_FRAMES - 1u)) * 2u;
        if (vol == 0) {
            out[i * 2] = out[i * 2 + 1] = 0;
        } else if (vol == 256) {
            out[i * 2]     = clamp_pcm(g_audio_mix[idx]);
            out[i * 2 + 1] = clamp_pcm(g_audio_mix[idx + 1]);
        } else {
            out[i * 2]     = clamp_pcm((g_audio_mix[idx] * vol) >> 8);
            out[i * 2 + 1] = clamp_pcm((g_audio_mix[idx + 1] * vol) >> 8);
        }
        g_audio_mix[idx] = g_audio_mix[idx + 1] = 0;
        g_audio_tail++;
    }
    SDL_UnlockMutex(g_audio_lock);
}

static uint32_t keyboard(void) {
    static const SDL_Scancode K[] = {
        SDL_SCANCODE_UP, SDL_SCANCODE_RIGHT, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT,
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_A, SDL_SCANCODE_S,
        SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_RETURN, SDL_SCANCODE_BACKSPACE
    };
    /* Keys belong to the focused window: a game must not keep moving while
     * the player is typing into another application. The controller has no
     * such ambiguity and is always sampled. */
    if (!g_focused) return 0;
    const Uint8 *keys = SDL_GetKeyboardState(NULL);
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof K / sizeof K[0]; i++)
        if (keys[K[i]]) b |= g_button_map[i];
    return b;
}

/* The first game controller, opened once on the window thread. */
static uint32_t pad(uint8_t *ax, uint8_t *ay) {
    if (!g_pad) {
        for (int i = 0; i < SDL_NumJoysticks(); i++) {
            if (!SDL_IsGameController(i)) continue;
            g_pad = SDL_GameControllerOpen(i);
            if (g_pad) {
                SDL_Joystick *j = SDL_GameControllerGetJoystick(g_pad);
                g_pad_instance = j ? SDL_JoystickInstanceID(j) : -1;
                char guid[33] = { 0 };
                if (j) SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(j), guid, sizeof guid);
                fprintf(stderr, "psprecomp: controller '%s' guid %s\n",
                        SDL_GameControllerName(g_pad) ? SDL_GameControllerName(g_pad) : "?", guid);
                break;
            }
        }
    }
    if (!g_pad) return 0;
    static const SDL_GameControllerButton P[] = {
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_LEFT,
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_BACK
    };
    uint32_t b = 0;
    for (size_t i = 0; i < sizeof P / sizeof P[0]; i++)
        if (SDL_GameControllerGetButton(g_pad, P[i])) b |= g_button_map[i];
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 8192)
        b |= g_button_map[PSP_WINDOW_L];
    if (SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 8192)
        b |= g_button_map[PSP_WINDOW_R];

    const Sint16 lx = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTX);
    const Sint16 ly = SDL_GameControllerGetAxis(g_pad, SDL_CONTROLLER_AXIS_LEFTY);
    if ((int64_t)lx * lx + (int64_t)ly * ly > 8000LL * 8000LL) {
        *ax = (uint8_t)((lx + 32768) >> 8);
        *ay = (uint8_t)((ly + 32768) >> 8);   /* SDL's Y already grows downwards */
    }
    return b;
}

static void render(SDL_Renderer *r, SDL_Texture **tex, int *tex_w, int *tex_h,
                   int cur_filter, int aspect_mode) {
    int cw, ch;
    SDL_GetRendererOutputSize(r, &cw, &ch);
    int dw = cw, dh = ch;
    if (aspect_mode == 0) {
        dh = cw * H / W;
        if (dh > ch) { dh = ch; dw = ch * W / H; }
    } else if (aspect_mode == 1) {
        int k = cw / W;
        if (ch / H < k) k = ch / H;
        if (k >= 1) { dw = W * k; dh = H * k; }
        else { dh = cw * H / W; if (dh > ch) { dh = ch; dw = ch * W / H; } }
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    const SDL_Rect dst = { (cw - dw) / 2, (ch - dh) / 2, dw, dh };
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);

    int fw = W, fh = H;
    const uint32_t *src_px = NULL;
    static uint64_t last_front_gen = 0;
    const uint64_t gen = psp_render_front_gen();
    if (gen != 0 && gen == last_front_gen && *tex && *tex_w >= W) {
        /* Hi-res front buffer unchanged since last host present: skip 2-8 MB upload. */
        SDL_RenderCopy(r, *tex, NULL, &dst);
    } else {
        if (psp_render_copy_front(g_hires_px, (uint32_t)(W * 4 * H * 4), &fw, &fh) == 0 &&
            fw >= W && fh >= H && fw <= W * 4 && fh <= H * 4) {
            src_px = g_hires_px;
            last_front_gen = gen;
        } else if (psp_display_read(g_px) == 0) {
            fw = W; fh = H;
            src_px = g_px;
            last_front_gen = 0;
        }
        if (src_px) {
            if (*tex_w != fw || *tex_h != fh || !*tex) {
                SDL_Texture *nt = SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888,
                                                    SDL_TEXTUREACCESS_STREAMING, fw, fh);
                if (nt) {
                    SDL_SetTextureScaleMode(nt, cur_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
                    if (*tex) SDL_DestroyTexture(*tex);
                    *tex = nt;
                    *tex_w = fw;
                    *tex_h = fh;
                }
            }
            if (*tex && *tex_w == fw && *tex_h == fh) {
                /* The front buffer is 0xAABBGGRR (little-endian bytes RR,GG,BB,AA),
                 * which is SDL_PIXELFORMAT_ABGR8888 (SDL_PIXELFORMAT_RGBA32). */
                SDL_UpdateTexture(*tex, NULL, src_px, fw * 4);
                SDL_RenderCopy(r, *tex, NULL, &dst);
            }
        }
    }
#ifdef PSPRECOMP_IMGUI_UI
    psp_overlay_render(r);
#endif
    SDL_RenderPresent(r);
}

static int SDLCALL window_main(void *arg) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_Window *win = SDL_CreateWindow(g_title, SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, g_win_w, g_win_h,
                                       SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
    if (!win) { g_closed = 1; SDL_SemPost((SDL_sem *)arg); return 1; }
    g_focused = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
    /* VSync gives natural 60 Hz pacing; the software fallback keeps the old
     * fixed-tick delay because SDL cannot vsync a software renderer. */
    SDL_Renderer *r = SDL_CreateRenderer(win, -1,
                                         SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r) r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_RendererInfo rinfo;
    int vsync = r && SDL_GetRendererInfo(r, &rinfo) == 0 &&
                (rinfo.flags & SDL_RENDERER_PRESENTVSYNC);
    int tex_w = W, tex_h = H;
    SDL_Texture *tex = r ? SDL_CreateTexture(r, SDL_PIXELFORMAT_ABGR8888,
                                             SDL_TEXTUREACCESS_STREAMING, tex_w, tex_h) : NULL;
    if (tex) SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    if (!tex) g_closed = 1;
#ifdef PSPRECOMP_IMGUI_UI
    psp_overlay_state ov = { 1, 0, 0, 1, vsync ? 1 : 0, 0, 0, 0, 100, 0, 0 };
    int cur_vsync = ov.vsync, cur_filter = ov.linear_filter;
    if (r && tex) psp_overlay_attach(win, r, &ov);
#endif
    SDL_SemPost((SDL_sem *)arg);              /* the window exists, or failed */

    Uint32 next = SDL_GetTicks();
    uint64_t presented_gen = UINT64_MAX;
    Uint32 last_present_ms = next - 16u;
    while (!g_closed && r && tex) {
        SDL_Event e;
        int had_event = 0;
        while (SDL_PollEvent(&e)) {
            had_event = 1;
#ifdef PSPRECOMP_IMGUI_UI
            psp_overlay_handle_event(&e);
#endif
            if (e.type == SDL_QUIT ||
                (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_CLOSE)) {
                g_closed = 1;
            } else if (e.type == SDL_WINDOWEVENT) {
                if (e.window.event == SDL_WINDOWEVENT_FOCUS_GAINED) g_focused = 1;
                else if (e.window.event == SDL_WINDOWEVENT_FOCUS_LOST) g_focused = 0;
            } else if (e.type == SDL_CONTROLLERDEVICEADDED) {
                /* A pad plugged in after boot becomes controller 0 on the next
                 * poll; opening it here removes the wait for a button press. */
                if (!g_pad) {
                    g_pad = SDL_GameControllerOpen(e.cdevice.which);
                    SDL_Joystick *j = g_pad ? SDL_GameControllerGetJoystick(g_pad) : NULL;
                    g_pad_instance = j ? SDL_JoystickInstanceID(j) : -1;
                }
            } else if (e.type == SDL_CONTROLLERDEVICEREMOVED) {
                if (g_pad && e.cdevice.which == g_pad_instance) {
                    SDL_GameControllerClose(g_pad);
                    g_pad = NULL;
                    g_pad_instance = -1;
                }
            } else if (e.type == SDL_KEYDOWN && !e.key.repeat &&
                       e.key.keysym.scancode == SDL_SCANCODE_F11) {
                const Uint32 flags = SDL_GetWindowFlags(win);
                SDL_SetWindowFullscreen(win,
                    (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
            }
        }
#ifdef PSPRECOMP_IMGUI_UI
        if (ov.quit_req) g_closed = 1;
        if (ov.fullscreen_req) {
            ov.fullscreen_req = 0;
            const Uint32 flags = SDL_GetWindowFlags(win);
            SDL_SetWindowFullscreen(win,
                (flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
        }
        if (ov.vsync != cur_vsync) {
            cur_vsync = ov.vsync;
            if (SDL_RenderSetVSync(r, cur_vsync ? 1 : 0) == 0) vsync = cur_vsync;
            else vsync = 0;
        }
        if (ov.linear_filter != cur_filter) {
            cur_filter = ov.linear_filter;
            SDL_SetTextureScaleMode(tex, cur_filter ? SDL_ScaleModeLinear : SDL_ScaleModeNearest);
        }
        g_audio_vol_256 = ov.audio_mute ? 0 : (ov.audio_vol_pct * 256 / 100);
#endif
        if (g_closed) break;
        /* Sync focus from SDL window flags directly so concurrent UI event
         * polling cannot leave g_focused stale when clicking into the game window. */
        g_focused = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
        uint8_t ax = 128, ay = 128;
#ifdef PSPRECOMP_IMGUI_UI
        uint32_t kbd = psp_overlay_want_keyboard() ? 0u : keyboard();
        psp_ctrl_set(kbd | pad(&ax, &ay), ax, ay);
#else
        psp_ctrl_set(keyboard() | pad(&ax, &ay), ax, ay);
#endif
        const uint64_t fgen = psp_render_front_gen();
        const Uint32 now_ms = SDL_GetTicks();
        if (had_event || fgen != presented_gen || (Uint32)(now_ms - last_present_ms) >= 16u) {
#ifdef PSPRECOMP_IMGUI_UI
            render(r, &tex, &tex_w, &tex_h, cur_filter, ov.aspect_mode);
#else
            render(r, &tex, &tex_w, &tex_h, 0, 0);
#endif
            presented_gen = fgen;
            last_present_ms = SDL_GetTicks();
            if (vsync) {
                next = last_present_ms;            /* SDL_RenderPresent paced us */
            } else {
                next += 16;
                if ((int)(next - last_present_ms) > 0) SDL_Delay(next - last_present_ms);
                else next = last_present_ms;
            }
        } else {
            SDL_Delay(2);
        }
    }
#ifdef PSPRECOMP_IMGUI_UI
    psp_overlay_detach();
#endif
    if (g_pad) { SDL_GameControllerClose(g_pad); g_pad = NULL; }
    g_pad_instance = -1;
    if (tex) SDL_DestroyTexture(tex);
    if (r) SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    g_closed = 1;
    return 0;
}

static void audio_shutdown(void) {
    psp_audio_channel_sink_set(NULL, NULL);
    psp_audio_sink_set(NULL, NULL);
    if (g_audio_dev) {
        SDL_PauseAudioDevice(g_audio_dev, 1);
        SDL_CloseAudioDevice(g_audio_dev);
        g_audio_dev = 0;
    }
    if (g_audio_lock) { SDL_DestroyMutex(g_audio_lock); g_audio_lock = NULL; }
}

int psp_window_start(const char *title, int scale) {
    if (g_thread) return -1;                   /* call psp_window_stop() first */
    if (title) g_title = title;
    g_closed = 0;
    g_scale = scale;
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS |
                 SDL_INIT_AUDIO) != 0) return -1;
    if (g_scale > 0) {
        g_win_w = W * g_scale;
        g_win_h = H * g_scale;
    } else {
        g_win_w = W * 3;
        g_win_h = H * 3;
        SDL_DisplayMode m;
        if (SDL_GetCurrentDisplayMode(0, &m) == 0 && m.w > W && m.h > H) {
            int dw = m.w * 85 / 100;
            int dh = dw * H / W;
            if (dh > m.h * 85 / 100) {
                dh = m.h * 85 / 100;
                dw = dh * W / H;
            }
            if (dw >= W && dh >= H) { g_win_w = dw; g_win_h = dh; }
        }
    }

    /* SAS keeps mixing into guest memory regardless; a host audio device just
     * makes the same frames audible. No device is not a fatal error. */
    memset(g_audio_mix, 0, sizeof g_audio_mix);
    memset(g_audio_src_pos, 0, sizeof g_audio_src_pos);
    g_audio_tail = g_audio_max_head = 0;
    g_audio_vol_256 = 256;
    g_audio_lock = SDL_CreateMutex();
    if (g_audio_lock) {
        SDL_AudioSpec want;
        memset(&want, 0, sizeof want);
        want.freq = 44100;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        want.callback = audio_callback;
        g_audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
        if (g_audio_dev) {
            psp_audio_channel_sink_set(audio_ch_sink, NULL);
            SDL_PauseAudioDevice(g_audio_dev, 0);
        } else {
            fprintf(stderr, "psprecomp: no audio device; sound stays mixed in memory\n");
        }
    }

    SDL_sem *ready = SDL_CreateSemaphore(0);
    if (!ready) { audio_shutdown(); SDL_Quit(); return -1; }
    SDL_Thread *t = SDL_CreateThread(window_main, "psprecomp-window", ready);
    if (!t) { SDL_DestroySemaphore(ready); audio_shutdown(); SDL_Quit(); return -1; }
    g_thread = t;
    SDL_SemWait(ready);                       /* window created, or failed */
    SDL_DestroySemaphore(ready);
    if (g_closed) {
        SDL_WaitThread(t, NULL);
        g_thread = NULL;
        audio_shutdown();
        SDL_Quit();
        return -1;
    }
    return 0;
}

void psp_window_stop(void) {
    if (!g_thread) return;
    g_closed = 1;                              /* the loop notices within a frame */
    SDL_WaitThread(g_thread, NULL);
    g_thread = NULL;
    audio_shutdown();                          /* no callbacks into a torn-down ring */
    SDL_Quit();                                /* only now: the thread is gone */
}

int psp_window_closed(void) { return g_closed != 0; }

#else

int psp_window_start(const char *title, int scale) { (void)title; (void)scale; return -1; }
int psp_window_closed(void) { return 0; }
void psp_window_stop(void) { }

#endif
