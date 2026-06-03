#include <monkey_dust/platform/window.h>
#include <monkey_dust/platform/font_loader.h>
#include <monkey_dust/platform/input.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
#include <monkey_dust/render/light_system.h>
#include <monkey_dust/world/terrain_gen.h>
#ifdef MONKEY_DUST_EDITOR_HOT_RELOAD
#include <monkey_dust/hot/editor_module.h>
#endif
#include <SDL3/SDL.h>
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "imgui.h"
#include "editor_core.h"
#include "editor_ui.h"
#include "item_editor.h"
#include "faction_editor.h"
#include "settings_editor.h"
#include "editor_world_panel.h"
#include "editor_world_3d_sdlgpu.h"
#include "editor_3d_bridge.h"
#include "editor_hmap_2d.h"
#include "editor_char_preview_sdlgpu.h"
#include "character_editor.h"
#include "npc_archetype_editor.h"
#include "editor_map_view.h"
#include "editor_node_graph.h"
#include "editor_terrain_panel.h"
#include "editor_layout.h"
#include "bug_capture.h"
#include <cstdio>

// ── Hot-reload: called by /reload-shaders console command ─────────────────────
void EditorReloadAllShaderPipelines() {
#ifdef MONKEY_DUST_EDITOR_HOT_RELOAD
    EditorModule::Get().ReloadShaders();
#else
    CharPreviewSDLGPU::ReloadPipelines();
#endif
    fprintf(stdout, "[Editor] Shader pipelines reloaded\n");
}

// ── Bridge: PCG terrain upload (defined here — only TU that includes W3D header) ─
void EditorW3D_UploadTerrainHeightmap(const float* hmap, int W, int H,
                                       float world_size_m, int chunk_x, int chunk_z) {
#ifdef MONKEY_DUST_EDITOR_HOT_RELOAD
    EditorModule::Get().UploadTerrainHeightmap(hmap, W, H, world_size_m, chunk_x, chunk_z);
#else
    WorldEditor3D_SDLGPU::UploadTerrainHeightmap(hmap, W, H, world_size_m, chunk_x, chunk_z);
#endif
}

// ──────────────────────────────────────────────────────────────────────────────
// monkey_dust EDITOR v1.0 — SDL_GPU (Vulkan) backend.
// Tabs: Items | Factions | NPCs | World | 3D World | Characters | Settings
// ──────────────────────────────────────────────────────────────────────────────

static constexpr const char* CFG_PATH    = "data/editor_config.json";
static constexpr const char* LAYOUT_PATH = "data/editor_layout.json";

// Extra Y offset when running under RenderDoc (overlay occupies top ~50px).
// Set via env var MD_OVERLAY_TOP_OFFSET=50 in .cap file or mde.sh --rd.
float s_overlay_top = 0.f;

int main(void) {
    const char* ov = getenv("MD_OVERLAY_TOP_OFFSET");
    if (ov) s_overlay_top = (float)atof(ov);

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
    SettingsEditor::Load(CFG_PATH);  // load saved sizes (path/font fields removed)
    float sc      = EditorUI::ui_scale;
    float ui_sz   = (float)SettingsEditor::g_cfg.ui_size   * sc; if (ui_sz   < 8.f) ui_sz   = 8.f;
    float mono_sz = (float)SettingsEditor::g_cfg.mono_size * sc; if (mono_sz < 8.f) mono_sz = 8.f;
    MdFonts::Load(ui_sz, mono_sz);  // embedded Arimo + UbuntuMono — no system dependency
    EditorUI::font_regular = MdFonts::regular;
    EditorUI::font_bold    = MdFonts::bold;
    EditorUI::font_mono    = MdFonts::mono;

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
    // Terrain atlas (editable heightmap) + light system for editor 3D view
    LightSystem::Get().Init();
    TerrainAtlas_Load("game/data/terrain/world_hmap.r32");
    TerrainAtlas_SmoothBoundaries();
#ifndef MONKEY_DUST_EDITOR_HOT_RELOAD
    WorldEditor3D_SDLGPU::Init(
        "game/data/textures/md_terrain.png",
        29, 25);  // 7×7 view centred near The Hub area
    CharacterEditor::LoadJSON("game/data/chars/player.chardef");
    CharacterEditor::LoadMorphNames("game/data/chars/morph_names.txt");
    MapViewPanel::Get().Init();
    EditorCore::Get().Init();
#endif

#ifdef MONKEY_DUST_EDITOR_HOT_RELOAD
    // Hot-reload path: all panel init/draw/render delegated to libeditor_panels.so.
    // EditorModule loads the .so, calls editor_panels_init() inside.
    {
        EditorModule::Config ecfg;
        ecfg.imgui_ctx   = ImGui::GetCurrentContext();
        ecfg.gpu         = gpu;
        ecfg.window      = _wnd::ptr();
        ecfg.overlay_top = s_overlay_top;
        ecfg.layout_path = LAYOUT_PATH;
        EditorModule::Get().Init("build/hot/libeditor_panels.so", ecfg);
    }
#endif

#ifndef MONKEY_DUST_EDITOR_HOT_RELOAD
    // Non-hot-reload: restore panel layout directly
    // Restore panel detach/position state from previous session
    {
        using P = EditorLayout::Panel;
        P pi = {ItemEditor::g_detached,      ItemEditor::g_win_pos,      ItemEditor::g_win_size};
        P pf = {FactionEditor::g_detached,   FactionEditor::g_win_pos,   FactionEditor::g_win_size};
        P pn = {NpcArchetypeEditor::g_detached, NpcArchetypeEditor::g_win_pos, NpcArchetypeEditor::g_win_size};
        P pc = {CharacterEditor::g_detached, CharacterEditor::g_win_pos, CharacterEditor::g_win_size};
        P ps = {SettingsEditor::g_detached,  SettingsEditor::g_win_pos,  SettingsEditor::g_win_size};
        if (EditorLayout::Load(LAYOUT_PATH, pi, pf, pn, pc, ps)) {
            ItemEditor::g_detached         = pi.detached; ItemEditor::g_win_pos         = pi.pos; ItemEditor::g_win_size         = pi.size;
            FactionEditor::g_detached      = pf.detached; FactionEditor::g_win_pos      = pf.pos; FactionEditor::g_win_size      = pf.size;
            NpcArchetypeEditor::g_detached = pn.detached; NpcArchetypeEditor::g_win_pos = pn.pos; NpcArchetypeEditor::g_win_size = pn.size;
            CharacterEditor::g_detached    = pc.detached; CharacterEditor::g_win_pos    = pc.pos; CharacterEditor::g_win_size    = pc.size;
            SettingsEditor::g_detached     = ps.detached; SettingsEditor::g_win_pos     = ps.pos; SettingsEditor::g_win_size     = ps.size;
        }
    }
#endif // !MONKEY_DUST_EDITOR_HOT_RELOAD

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

#ifdef MONKEY_DUST_EDITOR_HOT_RELOAD
        // F5: hot-reload editor panels
        if (input_key_pressed(SDL_SCANCODE_F5)) {
            EditorModule::Get().Reload();
            snprintf(status_msg, sizeof(status_msg), "[F5] Editor panels reloaded");
            status_timer = 3.f;
        }
        // Mtime watcher — auto-reload when libeditor_panels.so changes
        EditorModule::Get().Tick();
#endif

        // F9: dump editor state → tmp_md/bug_editor_TIMESTAMP.txt
        if (input_key_pressed(SDL_SCANCODE_F9)) {
            char path[256];
            FILE* f = BugCapture::Open("editor", path, sizeof(path));
            if (f) {
                fprintf(f, "[Editor]\n");
#ifdef MONKEY_DUST_EDITOR_HOT_RELOAD
                EditorModule::Get().DumpState(f);
#else
                fprintf(f, "  chars_detached=%d\n\n", CharacterEditor::g_detached ? 1 : 0);
#  ifdef MD_SDL_GPU
                CharPreviewSDLGPU::DumpState(f);
#  endif
#endif
                BugCapture::Close(f);
                snprintf(status_msg, sizeof(status_msg), "[F9] %s", path + 7);
                status_timer = 4.f;
            }
        }

        window_begin_frame();

        // ── SDL event pump — required for io.MouseWheel and quit detection ───
        // SDL_GetMouseState() covers position+buttons (realtime), but
        // SDL_EVENT_MOUSE_WHEEL is queue-only: without PollEvent, io.MouseWheel
        // is always 0 and scroll never reaches the terrain viewport.
        {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                ImGui_ImplSDL3_ProcessEvent(&ev);
                if (ev.type == SDL_EVENT_QUIT)
                    _sdl3_input::s_quit = true;
                if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                    int sc = (int)ev.key.scancode;
                    if (sc >= 0 && sc < SDL_SCANCODE_COUNT)
                        _sdl3_input::s_next[sc] = true;
                }
            }
            input_begin_frame();
        }

        // ── ImGui frame ───────────────────────────────────────────────────────
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        ImGuiIO& fio = ImGui::GetIO();

        // Toolbar draws the menu bar (~20px) + button bar (30px fixed)
        // f3_passthrough: pass-through mouse input when a fullscreen viewport tab is active.
        // Uses prev-frame flag (1-frame lag is imperceptible).
        static uint32_t s_active_flags = 0;  // viewport bitmask: bit0=3DWorld,1=CharPreview,2=Hmap,3=Map
        float toolbar_h = s_overlay_top + ImGui::GetFrameHeight() + 30.f;

#ifdef MONKEY_DUST_EDITOR_HOT_RELOAD
        // Hot-reload path: ALL panel code in the .so
        {
            uint32_t new_flags = EditorModule::Get().BuildUI(dt, toolbar_h,
                                                              status_msg, &status_timer);
            uint32_t prev_flags = s_active_flags;
            s_active_flags = new_flags;
            ImGui::Render();
            SDL_GPUCommandBuffer* cmd = md::GpuDevice::Get().AcquireCommandBuffer();
            if (cmd) {
                EditorModule::Get().Render(cmd, dt, prev_flags);
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
            continue;  // skip non-hot-reload UI code below
        }
#endif

        // Non-hot-reload path ─────────────────────────────────────────────────
        static bool s_world3d_was_active  = false;
        static bool s_hmap_was_active     = false;
        static bool s_charpreview_active  = false;
        static bool s_mapview_active      = false;
        EditorCore::Get().f3_passthrough = s_world3d_was_active;
        s_world3d_was_active = false;
        s_hmap_was_active    = false;
        s_charpreview_active = false;
        s_mapview_active     = false;
        EditorCore::Get().Update(dt);

        ImGui::SetNextWindowPos({0, toolbar_h});
        ImGui::SetNextWindowSize({fio.DisplaySize.x, fio.DisplaySize.y - toolbar_h});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,{0,0});
        ImGui::Begin("##editor",nullptr,
            ImGuiWindowFlags_NoTitleBar|ImGuiWindowFlags_NoResize|
            ImGuiWindowFlags_NoMove|ImGuiWindowFlags_NoScrollbar|
            ImGuiWindowFlags_NoScrollWithMouse|
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
                s_mapview_active = true;
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
            if (ImGui::BeginTabItem("Terrain")) {
                ImVec2 avail = ImGui::GetContentRegionAvail();
                float graph_w = avail.x * 0.70f;
                ImGui::BeginChild("##terrain_ng", {graph_w, avail.y}, false,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
                EditorNodeGraphPanel::Get().DrawContent();
                ImGui::EndChild();
                ImGui::SameLine(0, 4);
                ImGui::BeginChild("##terrain_sculpt", {0, avail.y}, false);
                EditorTerrainPanel::Get().DrawContent(dt);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Heightmap")) {
                s_hmap_was_active = true;
                HmapEditor2D::DrawPanel();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("3D World")) {
                s_world3d_was_active = true;  // tell EditorCore next frame to pass through mouse
                ImVec2 avail = ImGui::GetContentRegionAvail();
                WorldEditor3D_SDLGPU::DrawImGui(avail.x, avail.y-2, dt);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("NPCs")) {
                ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                NpcArchetypeEditor::Draw();
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Characters")) {
                s_charpreview_active = true;
                ImGui::SetCursorPos({8,ImGui::GetCursorPosY()+4});
                CharacterEditor::Draw(false);  // tools editor uses EditorUI navy theme
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
            if (s_hmap_was_active)    HmapEditor2D::UploadTexture(cmd);
            WorldEditor3D_SDLGPU::RenderFrame(cmd, dt, s_world3d_was_active);
            if (s_charpreview_active) CharPreviewSDLGPU::RenderFrame(cmd);
            if (s_mapview_active)     MapViewPanel::Get().RenderFrame(cmd);

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

#ifdef MONKEY_DUST_EDITOR_HOT_RELOAD
    EditorModule::Get().Shutdown();  // calls editor_panels_shutdown() → saves layout
#else
    // Save panel layout before shutdown (non-hot-reload path)
    EditorLayout::Save(LAYOUT_PATH,
        {ItemEditor::g_detached,         ItemEditor::g_win_pos,         ItemEditor::g_win_size},
        {FactionEditor::g_detached,      FactionEditor::g_win_pos,      FactionEditor::g_win_size},
        {NpcArchetypeEditor::g_detached, NpcArchetypeEditor::g_win_pos, NpcArchetypeEditor::g_win_size},
        {CharacterEditor::g_detached,    CharacterEditor::g_win_pos,    CharacterEditor::g_win_size},
        {SettingsEditor::g_detached,     SettingsEditor::g_win_pos,     SettingsEditor::g_win_size});
    EditorCore::Get().Shutdown();
#endif
    ImGui_ImplSDLGPU3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    md::GpuDevice::Get().Shutdown();
    window_shutdown();
    return 0;
}
