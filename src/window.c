/* psprecomp -- a window to play in. See include/psprecomp/window.h. */

#include "psprecomp/window.h"
#include "psprecomp/hle.h"

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
static const uint32_t DEFAULT_MAP[PSP_WINDOW_BUTTON_COUNT] = {
    B_UP, B_RIGHT, B_DOWN, B_LEFT,
    B_CROSS, B_CIRCLE, B_SQUARE, B_TRIANGLE,
    B_L, B_R, B_START, B_SELECT
};
static uint32_t g_button_map[PSP_WINDOW_BUTTON_COUNT];

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
    psp_window_set_button_map(NULL);           /* host overrides come before start */
    if (title) g_title = title;
    g_scale = scale;
    HANDLE ready = CreateEventA(NULL, TRUE, FALSE, NULL);
    HANDLE t = CreateThread(NULL, 0, window_main, ready, 0, NULL);
    if (!t) { CloseHandle(ready); return -1; }
    g_thread = t;
    WaitForSingleObject(ready, INFINITE);       /* the window exists, or failed */
    CloseHandle(ready);
    return g_closed ? -1 : 0;
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

static volatile int g_closed;
static int g_scale;
static const char *g_title = "psprecomp";
static uint32_t g_px[W * H];
static SDL_GameController *g_pad;
static SDL_Thread *g_thread;
static int g_focused;
static SDL_JoystickID g_pad_instance = -1;
static SDL_mutex *g_audio_lock;
static SDL_AudioDeviceID g_audio_dev;
static int16_t g_audio_ring[8192 * 2];
static size_t g_audio_head, g_audio_tail;   /* indices in frames */

/* The SAS mixer calls this with the same stereo grain it writes to guest
 * memory. Buffer it for SDL's audio thread; on overrun the oldest frames are
 * dropped rather than blocking the guest. */
static void audio_sink(const int16_t *stereo, uint32_t frames, uint32_t rate, void *user) {
    (void)rate; (void)user;
    if (!g_audio_lock || !g_audio_dev) return;
    SDL_LockMutex(g_audio_lock);
    for (uint32_t i = 0; i < frames; i++) {
        if ((g_audio_head + 1) % 8192 == g_audio_tail) g_audio_tail = (g_audio_tail + 1) % 8192;
        g_audio_ring[g_audio_head * 2]     = stereo[i * 2];
        g_audio_ring[g_audio_head * 2 + 1] = stereo[i * 2 + 1];
        g_audio_head = (g_audio_head + 1) % 8192;
    }
    SDL_UnlockMutex(g_audio_lock);
}

static void audio_callback(void *user, Uint8 *stream, int len) {
    (void)user;
    int16_t *out = (int16_t *)stream;
    size_t want = (size_t)len / 4;            /* stereo int16 frames */
    if (!g_audio_lock) { memset(stream, 0, (size_t)len); return; }
    SDL_LockMutex(g_audio_lock);
    for (size_t i = 0; i < want; i++) {
        if (g_audio_tail == g_audio_head) { out[i * 2] = out[i * 2 + 1] = 0; continue; }
        out[i * 2]     = g_audio_ring[g_audio_tail * 2];
        out[i * 2 + 1] = g_audio_ring[g_audio_tail * 2 + 1];
        g_audio_tail = (g_audio_tail + 1) % 8192;
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

static void render(SDL_Renderer *r, SDL_Texture *tex) {
    int cw, ch;
    SDL_GetRendererOutputSize(r, &cw, &ch);
    int scale = g_scale;
    if (scale > 0 && (W * scale > cw || H * scale > ch)) scale = 0;
    if (scale <= 0) {
        scale = cw / W;
        if (ch / H < scale) scale = ch / H;
        if (scale < 1) scale = 1;
    }
    const SDL_Rect dst = { (cw - W * scale) / 2, (ch - H * scale) / 2, W * scale, H * scale };
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    if (psp_display_read(g_px) == 0) {
        /* The front buffer is 0xAABBGGRR; RGBA8888 names memory bytes, which
         * on a little-endian host are exactly RR,GG,BB,AA. */
        SDL_UpdateTexture(tex, NULL, g_px, W * 4);
        SDL_RenderCopy(r, tex, NULL, &dst);
    }
    SDL_RenderPresent(r);
}

static int SDLCALL window_main(void *arg) {
    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");
    SDL_Window *win = SDL_CreateWindow(g_title, SDL_WINDOWPOS_CENTERED,
                                       SDL_WINDOWPOS_CENTERED, W * g_scale, H * g_scale,
                                       SDL_WINDOW_SHOWN);
    if (!win) { g_closed = 1; return 1; }
    g_focused = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) != 0;
    /* VSync gives natural 60 Hz pacing; the software fallback keeps the old
     * fixed-tick delay because SDL cannot vsync a software renderer. */
    SDL_Renderer *r = SDL_CreateRenderer(win, -1,
                                         SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!r) r = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    SDL_RendererInfo rinfo;
    const int vsync = r && SDL_GetRendererInfo(r, &rinfo) == 0 &&
                      (rinfo.flags & SDL_RENDERER_PRESENTVSYNC);
    SDL_Texture *tex = r ? SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA8888,
                                             SDL_TEXTUREACCESS_STREAMING, W, H) : NULL;
    if (tex) SDL_SetTextureScaleMode(tex, SDL_ScaleModeNearest);
    if (!tex) g_closed = 1;
    SDL_SemPost((SDL_sem *)arg);              /* the window exists, or failed */

    Uint32 next = SDL_GetTicks();
    while (!g_closed && r && tex) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
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
        if (g_closed) break;
        uint8_t ax = 128, ay = 128;
        psp_ctrl_set(keyboard() | pad(&ax, &ay), ax, ay);
        render(r, tex);
        if (vsync) {
            next = SDL_GetTicks();            /* SDL_RenderPresent paced us */
        } else {
            next += 16;
            const Uint32 now = SDL_GetTicks();
            if ((int)(next - now) > 0) SDL_Delay(next - now);
            else next = now;
        }
    }
    SDL_GameControllerClose(g_pad); g_pad = NULL;
    SDL_DestroyTexture(tex);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(win);
    g_closed = 1;
    return 0;
}

static void audio_shutdown(void) {
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
    psp_window_set_button_map(NULL);           /* host overrides come before start */
    if (title) g_title = title;
    g_closed = 0;
    g_scale = scale;
    if (g_scale <= 0) {
        SDL_DisplayMode m;
        g_scale = (SDL_GetCurrentDisplayMode(0, &m) == 0 && m.h > H)
                      ? m.h * 7 / 10 / H : 1;
        if (g_scale < 1) g_scale = 1;
    }
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMECONTROLLER | SDL_INIT_EVENTS |
                 SDL_INIT_AUDIO) != 0) return -1;

    /* SAS keeps mixing into guest memory regardless; a host audio device just
     * makes the same frames audible. No device is not a fatal error. */
    g_audio_head = g_audio_tail = 0;
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
            psp_audio_sink_set(audio_sink, NULL);
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
    return g_closed ? -1 : 0;
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
