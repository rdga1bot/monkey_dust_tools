#include <monkey_dust/platform/window.h>
#include <monkey_dust/platform/input.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
#include <SDL3/SDL.h>
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "imgui.h"
#include "editor_ui.h"
#include "item_editor.h"
#include "faction_editor.h"
#include "settings_editor.h"
#include "editor_world_panel.h"
#include "editor_world_3d_sdlgpu.h"
#include "editor_char_preview_sdlgpu.h"
#include "character_editor.h"
#include "npc_archetype_editor.h"
#include "editor_map_view.h"
#include <cstdio>

// ──────────────────────────────────────────────────────────────────────────────
// monkey_dust EDITOR v1.0 — SDL_GPU (Vulkan) backend.
// Tabs: Items | Factions | NPCs | World | 3D World | Characters | Settings
// ──────────────────────────────────────────────────────────────────────────────

static constexpr const char* CFG_PATH = "data/editor_config.json";

int main(void) {
    // ── Window ────────────────────────────────────────────────────────────────
    window_init(0, 0, "monkey_dust EDITOR v1.0");
    input_init();
    {
        SDL_DisplayID disp = SDL_GetDisplayForWindow(_wnd::ptr());
        SDL_Rect b = {};
        int mw = window_get_width(), mh = window_get_height();
        if (SDL_GetDisplayUsableBounds(disp, &b) && b.w > 0) { mw=b.w; mh=b.h; }
        int wh = (mh*85)/100, ww = (wh*16)/9;
        if (ww > (mw*90)/100) { ww=(mw*90)/100; wh=(ww*9)/16; }
        if (wh < 480) { wh=480; ww=854; }
        SDL_SetWindowSize(_wnd::ptr(), ww, wh);
        SDL_SetWindowPosition(_wnd::ptr(), SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        _wnd::width()=ww; _wnd::height()=wh;
        EditorUI::ui_scale = (float)wh/720.f;
    }

    // ── SDL_GPU ───────────────────────────────────────────────────────────────
    if (!md::GpuDevice::Get().Init(_wnd::ptr())) {
        fprintf(stderr, "[Editor] SDL_GPU init failed\n"); return 1;
    }
    SDL_GPUDevice* gpu = md::GpuDevice::Get().SDLDevice();
    SDL_GPUTextureFormat sc_fmt = SDL_GetGPUSwapchainTextureFormat(gpu, _wnd::ptr());

    // ── ImGui ─────────────────────────────────────────────────────────────────
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.Fonts->Clear();
    static const ImWchar ranges[] = { 0x0020,0x00FF, 0x0400,0x04FF, 0 };

    SettingsEditor::Load(CFG_PATH);
    auto& cfg = SettingsEditor::g_cfg;
    float sc = EditorUI::ui_scale;
    auto lf = [&](const char* p, float s) -> ImFont* {
        float sz = s*sc; if (sz<8.f) sz=8.f;
        ImFont* f = io.Fonts->AddFontFromFileTTF(p, sz, nullptr, ranges);
        return f ? f : io.Fonts->AddFontDefault();
    };
    EditorUI::font_regular = lf(cfg.label.path,  (float)cfg.label.size);
    EditorUI::font_bold    = lf(cfg.header.path, (float)cfg.header.size);
    EditorUI::font_mono    = lf(cfg.mono.path,   (float)cfg.mono.size);

    ImGui_ImplSDL3_InitForSDLGPU(_wnd::ptr());
    ImGui_ImplSDLGPU3_InitInfo info = {};
    info.Device = gpu; info.ColorTargetFormat = sc_fmt;
    ImGui_ImplSDLGPU3_Init(&info);
    EditorUI::SetupTheme();

    // ── Data ──────────────────────────────────────────────────────────────────
    ItemEditor::Load("data/items/items.json");
    FactionEditor::Load("data/factions/factions.json");
    NpcArchetypeEditor::Load("game/data/defs/npc_archetypes.json");
    WorldPanel::Init();
    WorldEditor3D_SDLGPU::Init(
        "game/data/terrain/world_hmap.r32",
        "game/data/textures/md_terrain.png",
        29, 25);  // zone offset: 7×7 view centred on The Hub (32,28)
    CharacterEditor::LoadJSON("game/data/chars/player.chardef");
    CharacterEditor::LoadMorphNames("game/data/chars/morph_names.txt");
    MapViewPanel::Get().Init();

    SDL_FlushEvent(SDL_EVENT_QUIT);

    // ── Main loop ─────────────────────────────────────────────────────────────
    char  status_msg[64] = {};
    float status_timer   = 0.f;
    uint64_t last_ticks  = SDL_GetTicks();

    while (!input_should_quit()) {
        // Frame cap — editor targets 60 fps; iGPU shares cooling with CPU
        {
            static Uint64 s_prev_ns = 0;
            static constexpr Uint64 TARGET_NS = 1000000000ULL / 60;
            Uint64 now_ns = SDL_GetTicksNS();
            if (s_prev_ns && now_ns - s_prev_ns < TARGET_NS)
                SDL_DelayNS(TARGET_NS - (now_ns - s_prev_ns));
            s_prev_ns = SDL_GetTicksNS();
        }
        uint64_t now = SDL_GetTicks();
        float dt = (float)(now-last_ticks)/1000.f;
        last_ticks = now;
        if (status_timer > 0.f) status_timer -= dt;
        window_begin_frame();

        // ── ImGui frame ───────────────────────────────────────────────────────
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& fio = ImGui::GetIO();
        float menu_h = 0.f;
        if (ImGui::BeginMainMenuBar()) {
            menu_h = ImGui::GetWindowSize().y;
            if (ImGui::BeginMenu("File")) {
                if (ImGui::BeginMenu("Map")) {
                    static char s_open_buf[256]="third_party/flare-game/mods/empyrean_campaign/maps/goblin_camp.txt";
                    static char s_save_buf[256]="";
                    ImGui::SetNextItemWidth(320); ImGui::InputText("##mopen", s_open_buf, sizeof(s_open_buf));
                    ImGui::SameLine();
                    if (ImGui::Button("Open")) {
                        if (MapViewPanel::Get().LoadMap(s_open_buf)) {
                            snprintf(status_msg, sizeof(status_msg), "Map loaded");
                            status_timer = 3.f;
                        }
                    }
                    ImGui::Separator();
                    ImGui::SetNextItemWidth(320); ImGui::InputText("##msave", s_save_buf, sizeof(s_save_buf));
                    ImGui::SameLine();
                    if (ImGui::Button("Save As")) {
                        if (MapViewPanel::Get().SaveTo(s_save_buf)) {
                            snprintf(status_msg, sizeof(status_msg), "Map saved");
                            status_timer = 3.f;
                        }
                    }
                    if (ImGui::MenuItem("Save", "Ctrl+S", false, MapViewPanel::Get().IsLoaded())) {
                        if (MapViewPanel::Get().SaveCurrent()) {
                            snprintf(status_msg, sizeof(status_msg), "Map saved");
                            status_timer = 3.f;
                        }
                    }
                    ImGui::EndMenu();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Quit","Alt+F4")) {
                    SDL_Event qe{}; qe.type=SDL_EVENT_QUIT; SDL_PushEvent(&qe);
                }
                ImGui::EndMenu();
            }
            if (status_timer > 0.f && status_msg[0]) {
                float alpha=(status_timer>1.f)?1.f:status_timer;
                float mw=ImGui::CalcTextSize(status_msg).x+16.f;
                ImGui::SetCursorPosX(ImGui::GetWindowWidth()-mw);
                ImGui::TextColored({0.4f,0.9f,0.5f,alpha},"%s",status_msg);
            }
            ImGui::EndMainMenuBar();
        }

        ImGui::SetNextWindowPos({0,menu_h});
        ImGui::SetNextWindowSize({fio.DisplaySize.x, fio.DisplaySize.y-menu_h});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
        ImGui::Begin("##editor",nullptr,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|
            ImGuiWindowFlags_NoBringToFrontOnFocus);
        ImGui::PopStyleVar();
        ImGui::SetCursorPos({0,0});
        ImGui::Separator();
        ImGui::SetCursorPosX(4);

        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Items")) {
                ImGui::SetCursorPos({8,ImGui::GetCursorPosY()+4});
                if (ItemEditor::Draw("data/items/items.json")) {
                    snprintf(status_msg,sizeof(status_msg),"Items saved!");
                    status_timer=3.f;
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Factions")) {
                ImGui::SetCursorPos({8,ImGui::GetCursorPosY()+4});
                if (FactionEditor::Draw("data/factions/factions.json")) {
                    snprintf(status_msg,sizeof(status_msg),"Factions saved!");
                    status_timer=3.f;
                }
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Map")) {
                ImGui::SetCursorPos({8,ImGui::GetCursorPosY()+4});
                ImGuiIO& mio = ImGui::GetIO();
                if (mio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false))
                    MapViewPanel::Get().Undo();
                if (mio.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false))
                    MapViewPanel::Get().Redo();
                MapViewPanel::Get().Draw(dt);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("World")) {
                ImGui::SetCursorPos({8,ImGui::GetCursorPosY()+4});
                WorldPanel::Draw(dt);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("3D World")) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                WorldEditor3D_SDLGPU::DrawImGui(avail.x, avail.y-2, dt);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("NPCs")) {
                NpcArchetypeEditor::Draw();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Characters")) {
                ImGui::SetCursorPos({8,ImGui::GetCursorPosY()+4});
                CharacterEditor::Draw();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Settings")) {
                ImGui::SetCursorPos({12,ImGui::GetCursorPosY()+6});
                SettingsEditor::Draw(CFG_PATH, status_msg, &status_timer);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::End();
        ImGui::Render();

        // ── SDL_GPU: render terrain RTT + ImGui to swapchain ─────────────────
        SDL_GPUCommandBuffer* cmd = md::GpuDevice::Get().AcquireCommandBuffer();
        if (cmd) {
            // 1. Render 3D terrain + character preview + tile map to off-screen RTTs
            WorldEditor3D_SDLGPU::RenderFrame(cmd, dt);
            CharPreviewSDLGPU::RenderFrame(cmd);
            MapViewPanel::Get().RenderFrame(cmd);

            // 2. Acquire swapchain + clear + ImGui
            uint32_t sw=0, sh=0;
            SDL_GPUTexture* sc = md::GpuDevice::Get().AcquireSwapchainTexture(cmd, &sw, &sh);
            if (sc) {
                SDL_GPUColorTargetInfo ct={};
                ct.texture=sc; ct.load_op=SDL_GPU_LOADOP_CLEAR;
                ct.store_op=SDL_GPU_STOREOP_STORE;
                ct.clear_color={0.10f,0.10f,0.13f,1.f};
                SDL_GPURenderPass* rp=SDL_BeginGPURenderPass(cmd,&ct,1,nullptr);
                if (rp) SDL_EndGPURenderPass(rp);

                ImDrawData* dd=ImGui::GetDrawData();
                if (dd && dd->CmdListsCount>0) {
                    ImGui_ImplSDLGPU3_PrepareDrawData(dd,cmd);
                    SDL_GPUColorTargetInfo ict={};
                    ict.texture=sc; ict.load_op=SDL_GPU_LOADOP_LOAD;
                    ict.store_op=SDL_GPU_STOREOP_STORE;
                    SDL_GPURenderPass* irp=SDL_BeginGPURenderPass(cmd,&ict,1,nullptr);
                    if (irp) { ImGui_ImplSDLGPU3_RenderDrawData(dd,cmd,irp); SDL_EndGPURenderPass(irp); }
                }
            }
            SDL_SubmitGPUCommandBuffer(cmd);
        }
        window_end_frame();
    }

    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    md::GpuDevice::Get().Shutdown();
    window_shutdown();
    return 0;
}
