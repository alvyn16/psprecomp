/* psprecomp — Dear ImGui-backed save-state UI. See include/psprecomp/ui.h. */

extern "C" {
#include "psprecomp/ui.h"
#include "psprecomp/state.h"
}

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_sdlrenderer2.h>

#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

static SDL_Thread *g_thread;
static SDL_sem *g_ready;
static volatile int g_stop;
static int g_slot;
static char g_status[96] = "ready";
static int g_owns_sdl;

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

static int SDLCALL ui_main(void *arg) {
    (void)arg;
    SDL_Window *w = SDL_CreateWindow("psprecomp states", SDL_WINDOWPOS_UNDEFINED,
                                     SDL_WINDOWPOS_UNDEFINED, 360, 200, SDL_WINDOW_SHOWN);
    if (!w) { g_stop = 1; SDL_SemPost(g_ready); return 1; }
    SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
    if (!r) { SDL_DestroyWindow(w); g_stop = 1; SDL_SemPost(g_ready); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui_ImplSDL2_InitForSDLRenderer(w, r);
    ImGui_ImplSDLRenderer2_Init(r);
    SDL_SemPost(g_ready);

    while (!g_stop) {
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
        if (ImGui::Begin("Save States")) {
            ImGui::SliderInt("Slot", &g_slot, 0, 9);
            if (ImGui::Button("Save", ImVec2(110, 0))) do_save();
            ImGui::SameLine();
            if (ImGui::Button("Load", ImVec2(110, 0))) do_load();
            ImGui::Text("F5 save / F8 load, arrows change slot");
            ImGui::TextUnformatted(g_status);
        }
        ImGui::End();
        ImGui::Render();

        SDL_SetRenderDrawColor(r, 20, 20, 24, 255);
        SDL_RenderClear(r);
        ImGui_ImplSDLRenderer2_RenderDrawData(ImGui::GetDrawData(), r);
        SDL_RenderPresent(r);
        SDL_Delay(30);
    }

    ImGui_ImplSDLRenderer2_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    return 0;
}

extern "C" int psp_ui_start(void) {
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
