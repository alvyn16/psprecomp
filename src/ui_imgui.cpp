/* psprecomp — Dear ImGui-backed overlay menu, FPS/performance HUD, and save-state UI. */

extern "C" {
#include "psprecomp/ui.h"
#include "psprecomp/state.h"
#include "psprecomp/hle.h"
#include "psprecomp/mem.h"
#include "psprecomp/render.h"
#include "ui_overlay.h"
}

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <SDL.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>

static SDL_Thread *g_thread;
static SDL_sem *g_ready;
static volatile int g_stop;
static int g_slot;
static char g_status[96] = "ready";
static int g_owns_sdl;

static psp_overlay_state *g_ov;
static ImGuiContext *g_ov_ctx;
static bool g_show_settings;
static bool g_show_controls;

static struct {
    Uint64 last_ms;
    uint64_t last_finishes, last_cmds, last_verts, last_pixels, last_ablocks;
    float ge_fps, mpix_s, kverts_s, kcmds_s, ablocks_s;
} g_perf;

static void state_path(char *out, size_t n) {
    const char *dir = getenv("PSPRECOMP_STATE_DIR");
    snprintf(out, n, "%s/psprecomp-state-%d.bin", dir ? dir : ".", g_slot);
}

static void do_save(void) {
    char path[512];
    state_path(path, sizeof path);
    snprintf(g_status, sizeof g_status, "%s",
             psp_state_save(path) == 0 ? "saved" : "save failed");
}

static void do_load(void) {
    char path[512];
    state_path(path, sizeof path);
    snprintf(g_status, sizeof g_status, "%s",
             psp_state_load(path) == 0 ? "loaded" : "load failed");
}

static void update_perf_metrics(void) {
    const Uint64 now = SDL_GetTicks64();
    if (!g_perf.last_ms) {
        g_perf.last_ms = now;
        g_perf.last_finishes = psp_ge_finish_count();
        g_perf.last_cmds = psp_ge_command_count();
        g_perf.last_verts = psp_ge_vertex_count();
        g_perf.last_pixels = psp_render_pixels();
        g_perf.last_ablocks = psp_audio_blocks();
        return;
    }
    const Uint64 elapsed = now - g_perf.last_ms;
    if (elapsed < 500) return;

    const float dt = (float)elapsed / 1000.0f;
    const uint64_t f = psp_ge_finish_count();
    const uint64_t c = psp_ge_command_count();
    const uint64_t v = psp_ge_vertex_count();
    const uint64_t p = psp_render_pixels();
    const uint64_t a = psp_audio_blocks();

    g_perf.ge_fps   = (float)(f - g_perf.last_finishes) / dt;
    g_perf.kcmds_s  = (float)(c - g_perf.last_cmds) / dt / 1000.0f;
    g_perf.kverts_s = (float)(v - g_perf.last_verts) / dt / 1000.0f;
    g_perf.mpix_s   = (float)(p - g_perf.last_pixels) / dt / 1000000.0f;
    g_perf.ablocks_s = (float)(a - g_perf.last_ablocks) / dt;

    g_perf.last_ms = now;
    g_perf.last_finishes = f;
    g_perf.last_cmds = c;
    g_perf.last_verts = v;
    g_perf.last_pixels = p;
    g_perf.last_ablocks = a;
}

static void draw_save_state_controls(void) {
    ImGui::SliderInt("Slot", &g_slot, 0, 9);
    if (ImGui::Button("Save (F5)", ImVec2(110, 0))) do_save();
    ImGui::SameLine();
    if (ImGui::Button("Load (F8)", ImVec2(110, 0))) do_load();
    ImGui::Text("Status: %s", g_status);
}

static void draw_menu_bar(void) {
    if (!g_ov || !g_ov->show_menu) return;
    if (!ImGui::BeginMainMenuBar()) return;

    if (ImGui::BeginMenu("Game")) {
        if (ImGui::MenuItem("Save State", "F5")) do_save();
        if (ImGui::MenuItem("Load State", "F8")) do_load();
        ImGui::SliderInt("Slot", &g_slot, 0, 9);
        ImGui::TextDisabled("Status: %s", g_status);
        ImGui::Separator();
        if (ImGui::MenuItem("Quit")) g_ov->quit_req = 1;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        bool fps = g_ov->show_fps != 0;
        if (ImGui::MenuItem("Show FPS", NULL, &fps)) g_ov->show_fps = fps ? 1 : 0;
        bool stats = g_ov->show_stats != 0;
        if (ImGui::MenuItem("Show Detailed Stats", NULL, &stats)) g_ov->show_stats = stats ? 1 : 0;
        if (ImGui::BeginMenu("HUD Position")) {
            const char *corners[4] = { "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right" };
            for (int i = 0; i < 4; i++)
                if (ImGui::MenuItem(corners[i], NULL, g_ov->hud_corner == i)) g_ov->hud_corner = i;
            ImGui::EndMenu();
        }
        ImGui::Separator();
        bool lin = g_ov->linear_filter != 0;
        if (ImGui::MenuItem("Linear Filtering", NULL, &lin)) g_ov->linear_filter = lin ? 1 : 0;
        if (ImGui::BeginMenu("Aspect Mode")) {
            const char *modes[3] = { "Fit 16:9", "Integer Scale", "Stretch" };
            for (int i = 0; i < 3; i++)
                if (ImGui::MenuItem(modes[i], NULL, g_ov->aspect_mode == i)) g_ov->aspect_mode = i;
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Toggle Fullscreen", "F11")) g_ov->fullscreen_req = 1;
        ImGui::Separator();
        if (ImGui::MenuItem("Hide Menu Bar", "F1 / Esc")) g_ov->show_menu = 0;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Performance")) {
        bool vs = g_ov->vsync != 0;
        if (ImGui::MenuItem("VSync", NULL, &vs)) g_ov->vsync = vs ? 1 : 0;
        bool fps = g_ov->show_fps != 0;
        if (ImGui::MenuItem("Show FPS HUD", NULL, &fps)) g_ov->show_fps = fps ? 1 : 0;
        bool stats = g_ov->show_stats != 0;
        if (ImGui::MenuItem("Detailed Profiler HUD", NULL, &stats)) g_ov->show_stats = stats ? 1 : 0;
        ImGui::Separator();
        ImGui::MenuItem("Settings Panel...", NULL, &g_show_settings);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Audio")) {
        bool mute = g_ov->audio_mute != 0;
        if (ImGui::MenuItem("Mute", NULL, &mute)) g_ov->audio_mute = mute ? 1 : 0;
        ImGui::SetNextItemWidth(140.0f);
        ImGui::SliderInt("Volume %", &g_ov->audio_vol_pct, 0, 150);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Controls")) {
        ImGui::MenuItem("Show Controls...", NULL, &g_show_controls);
        ImGui::Separator();
        ImGui::TextDisabled("F1 / Esc: Toggle Menu");
        ImGui::TextDisabled("F5 / F8:  Save / Load");
        ImGui::TextDisabled("F11:      Fullscreen");
        ImGui::EndMenu();
    }

    char badge[64];
    snprintf(badge, sizeof badge, "GE: %.1f fps | Host: %.0f fps  [F1: Hide]",
             g_perf.ge_fps, ImGui::GetIO().Framerate);
    const float w = ImGui::CalcTextSize(badge).x + 16.0f;
    if (ImGui::GetWindowWidth() > w + 260.0f) {
        ImGui::SameLine(ImGui::GetWindowWidth() - w);
        if (ImGui::SmallButton(badge)) g_ov->show_fps = !g_ov->show_fps;
    }
    ImGui::EndMainMenuBar();
}

static void draw_fps_hud(void) {
    if (!g_ov || (!g_ov->show_fps && !g_ov->show_stats)) return;
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float pad = 10.0f;
    const float top_off = (g_ov->show_menu ? ImGui::GetFrameHeight() : 0.0f) + pad;
    const int c = g_ov->hud_corner;
    ImVec2 pos((c & 1) ? vp->WorkPos.x + vp->WorkSize.x - pad : vp->WorkPos.x + pad,
               (c & 2) ? vp->WorkPos.y + vp->WorkSize.y - pad : vp->WorkPos.y + top_off);
    ImVec2 pivot((c & 1) ? 1.0f : 0.0f, (c & 2) ? 1.0f : 0.0f);
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowBgAlpha(0.68f);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                   ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
    if (ImGui::Begin("##fps_hud", nullptr, flags)) {
        const float ms = g_perf.ge_fps > 0.01f ? (1000.0f / g_perf.ge_fps) : 0.0f;
        ImGui::Text("GE FPS:   %.1f (%.1f ms)", g_perf.ge_fps, ms);
        ImGui::Text("Host FPS: %.0f", ImGui::GetIO().Framerate);
        if (g_ov->show_stats) {
            ImGui::Separator();
            ImGui::Text("Raster:   %.1f MPix/s", g_perf.mpix_s);
            ImGui::Text("Verts:    %.0f k/s", g_perf.kverts_s);
            ImGui::Text("Cmds:     %.0f k/s", g_perf.kcmds_s);
            ImGui::Text("Audio:    %.0f blk/s", g_perf.ablocks_s);
            ImGui::Text("Bad Mem:  %" PRIu64, psp_mem_bad_access);
        }
    }
    ImGui::End();
}

static void draw_extra_windows(void) {
    if (!g_ov || !g_ov->show_menu) return;
    if (g_show_settings) {
        ImGui::SetNextWindowSize(ImVec2(360, 320), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Settings & Performance", &g_show_settings)) {
            if (ImGui::CollapsingHeader("HUD & Performance", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool fps = g_ov->show_fps != 0;
                if (ImGui::Checkbox("Show FPS HUD", &fps)) g_ov->show_fps = fps ? 1 : 0;
                bool stats = g_ov->show_stats != 0;
                if (ImGui::Checkbox("Show Detailed Stats", &stats)) g_ov->show_stats = stats ? 1 : 0;
                const char *corners[4] = { "Top-Left", "Top-Right", "Bottom-Left", "Bottom-Right" };
                ImGui::Combo("HUD Corner", &g_ov->hud_corner, corners, 4);
                bool vs = g_ov->vsync != 0;
                if (ImGui::Checkbox("VSync (Host Present)", &vs)) g_ov->vsync = vs ? 1 : 0;
            }
            if (ImGui::CollapsingHeader("Graphics", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool lin = g_ov->linear_filter != 0;
                if (ImGui::Checkbox("Linear Texture Filter", &lin)) g_ov->linear_filter = lin ? 1 : 0;
                const char *modes[3] = { "Fit 16:9", "Integer Scale", "Stretch" };
                ImGui::Combo("Aspect Mode", &g_ov->aspect_mode, modes, 3);
                if (ImGui::Button("Toggle Fullscreen (F11)")) g_ov->fullscreen_req = 1;
            }
            if (ImGui::CollapsingHeader("Audio", ImGuiTreeNodeFlags_DefaultOpen)) {
                bool mute = g_ov->audio_mute != 0;
                if (ImGui::Checkbox("Mute Audio", &mute)) g_ov->audio_mute = mute ? 1 : 0;
                ImGui::SliderInt("Master Volume %", &g_ov->audio_vol_pct, 0, 150);
            }
            if (ImGui::CollapsingHeader("Save States")) {
                draw_save_state_controls();
            }
        }
        ImGui::End();
    }
    if (g_show_controls) {
        ImGui::SetNextWindowSize(ImVec2(330, 270), ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Controls", &g_show_controls)) {
            ImGui::Text("D-Pad:         Arrow keys");
            ImGui::Text("Cross / X:     Z");
            ImGui::Text("Circle / O:    X");
            ImGui::Text("Square / []:   A");
            ImGui::Text("Triangle / ^:  S");
            ImGui::Text("L / R:         Q / W");
            ImGui::Text("Start / Sel:   Enter / Backspace");
            ImGui::Separator();
            ImGui::Text("Toggle Menu:   F1 or Esc");
            ImGui::Text("Save / Load:   F5 / F8");
            ImGui::Text("Fullscreen:    F11");
            ImGui::Separator();
            ImGui::TextDisabled("SDL GameControllers work automatically.");
        }
        ImGui::End();
    }
}

extern "C" void psp_overlay_attach(SDL_Window *win, SDL_Renderer *ren, psp_overlay_state *state) {
    g_ov = state;
    g_show_settings = false;
    g_show_controls = false;
    memset(&g_perf, 0, sizeof g_perf);
    IMGUI_CHECKVERSION();
    g_ov_ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_ov_ctx);
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 5.0f;
    st.FrameRounding = 3.0f;
    st.WindowBorderSize = 1.0f;
    ImGui_ImplSDL2_InitForSDLRenderer(win, ren);
    ImGui_ImplSDLRenderer2_Init(ren);
}

extern "C" void psp_overlay_detach(void) {
    if (!g_ov_ctx) return;
    ImGui::SetCurrentContext(g_ov_ctx);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(g_ov_ctx);
    g_ov_ctx = nullptr;
    g_ov = nullptr;
}

extern "C" void psp_overlay_handle_event(const SDL_Event *ev) {
    if (!g_ov_ctx || !ev) return;
    ImGui::SetCurrentContext(g_ov_ctx);
    ImGui_ImplSDL2_ProcessEvent(ev);
    if (ev->type == SDL_KEYDOWN && !ev->key.repeat) {
        const SDL_Scancode sc = ev->key.keysym.scancode;
        if (sc == SDL_SCANCODE_F1 || sc == SDL_SCANCODE_ESCAPE) {
            if (g_ov) g_ov->show_menu = !g_ov->show_menu;
        } else if (sc == SDL_SCANCODE_F5) {
            do_save();
        } else if (sc == SDL_SCANCODE_F8) {
            do_load();
        }
    }
}

extern "C" int psp_overlay_want_keyboard(void) {
    if (!g_ov_ctx || !g_ov || !g_ov->show_menu) return 0;
    ImGui::SetCurrentContext(g_ov_ctx);
    return ImGui::GetIO().WantCaptureKeyboard ? 1 : 0;
}

extern "C" void psp_overlay_render(SDL_Renderer *ren) {
    if (!g_ov_ctx || !g_ov) return;
    ImGui::SetCurrentContext(g_ov_ctx);
    update_perf_metrics();
    ImGui_ImplSDLRenderer2_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    draw_menu_bar();
    draw_fps_hud();
    draw_extra_windows();
    ImGui::Render();
    ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), ren);
}

extern "C" int psp_overlay_is_attached(void) {
    return g_ov_ctx != nullptr ? 1 : 0;
}

static int SDLCALL ui_main(void *arg) {
    (void)arg;
    SDL_Window *w = SDL_CreateWindow("psprecomp states", SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED, 360, 200, SDL_WINDOW_SHOWN);
    if (!w) { g_stop = 1; SDL_SemPost(g_ready); return 1; }
    SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
    if (!r) { SDL_DestroyWindow(w); g_stop = 1; SDL_SemPost(g_ready); return 1; }

    IMGUI_CHECKVERSION();
    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGui_ImplSDL2_InitForSDLRenderer(w, r);
    ImGui_ImplSDLRenderer2_Init(r);
    SDL_SemPost(g_ready);

    while (!g_stop) {
        ImGui::SetCurrentContext(ctx);
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL2_ProcessEvent(&e);
            if (e.type == SDL_QUIT) { g_stop = 1; break; }
            if (e.type == SDL_KEYDOWN) {
                if (e.key.keysym.scancode == SDL_SCANCODE_F5) do_save();
                else if (e.key.keysym.scancode == SDL_SCANCODE_F8) do_load();
                else if (e.key.keysym.scancode == SDL_SCANCODE_LEFT && g_slot > 0) g_slot--;
                else if (e.key.keysym.scancode == SDL_SCANCODE_RIGHT && g_slot < 9) g_slot++;
            }
        }
        if (g_stop) break;

        ImGui_ImplSDLRenderer2_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_Once);
        ImGui::SetNextWindowSize(ImVec2(340, 180), ImGuiCond_Once);
        if (ImGui::Begin("Save States")) draw_save_state_controls();
        ImGui::End();
        ImGui::Render();

        SDL_SetRenderDrawColor(r, 20, 20, 24, 255);
        SDL_RenderClear(r);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), r);
        SDL_RenderPresent(r);
        SDL_Delay(30);
    }

    ImGui::SetCurrentContext(ctx);
    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext(ctx);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    return 0;
}

extern "C" int psp_ui_start(void) {
    if (psp_overlay_is_attached()) return 0;
    if (g_thread) return -1;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) {
        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS) != 0) return -1;
        g_owns_sdl = 1;
    }
    g_stop = 0;
    g_ready = SDL_CreateSemaphore(0);
    if (!g_ready) { if (g_owns_sdl) { SDL_Quit(); g_owns_sdl = 0; } return -1; }
    g_thread = SDL_CreateThread(ui_main, "psprecomp-imgui", NULL);
    if (!g_thread) {
        SDL_DestroySemaphore(g_ready); g_ready = NULL;
        if (g_owns_sdl) { SDL_Quit(); g_owns_sdl = 0; }
        return -1;
    }
    SDL_SemWait(g_ready);
    SDL_DestroySemaphore(g_ready);
    g_ready = NULL;
    if (g_stop) {
        SDL_WaitThread(g_thread, NULL); g_thread = NULL;
        if (g_owns_sdl) { SDL_Quit(); g_owns_sdl = 0; }
        return -1;
    }
    return 0;
}

extern "C" void psp_ui_stop(void) {
    if (!g_thread) return;
    g_stop = 1;
    SDL_WaitThread(g_thread, NULL);
    g_thread = NULL;
    if (g_owns_sdl) { SDL_Quit(); g_owns_sdl = 0; }
}
