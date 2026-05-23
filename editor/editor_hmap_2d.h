#pragma once
#ifdef MD_SDL_GPU
// editor_hmap_2d.h — 2D topographic heightmap editor panel.
// Displays md_master_hmap.r32 as a coloured top-down map; mouse = paint.
// Integration:
//   1. HmapEditor2D::UploadTexture(cmd)  — in RenderFrame, before ImGui draw
//   2. HmapEditor2D::DrawPanel()         — inside ImGui frame (any tab/window)

#include "imgui.h"
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/world/terrain_gen.h>
#include <SDL3/SDL_gpu.h>
#include <cstring>
#include <cmath>

namespace HmapEditor2D {

static constexpr const char* MASTER_PATH = "game/data/terrain/md_master_hmap.r32";

static SDL_GPUTexture*        s_tex   = nullptr;
static SDL_GPUTransferBuffer* s_tbuf  = nullptr;
static int   s_tw = 0, s_th = 0;
static uint8_t* s_rgba = nullptr;   // CPU RGBA8 representation
static bool  s_dirty   = false;
static bool  s_inited  = false;

// ── Topographic colour gradient ───────────────────────────────────────────────
static void s_height_to_rgba(float h, float hmax, uint8_t out[3]) {
    float t = (hmax > 0.f) ? (h / hmax) : 0.f;
    struct Stop { float t; uint8_t r,g,b; };
    static const Stop S[] = {
        {0.000f,  10, 26,  77},  // deep (sea-ish dark)
        {0.015f, 195,179, 128},  // coast / sand
        {0.080f,  89,158,  64},  // plains green
        {0.280f,  56,114,  35},  // hills dark green
        {0.500f, 140, 97,  52},  // mountain brown
        {0.720f, 120,110, 100},  // high grey
        {0.900f, 200,200, 200},  // near-peak grey
        {1.000f, 255,255, 255},  // snow white
    };
    constexpr int N = (int)(sizeof(S)/sizeof(S[0]));
    if (t <= S[0].t)     { out[0]=S[0].r; out[1]=S[0].g; out[2]=S[0].b; return; }
    if (t >= S[N-1].t)   { out[0]=S[N-1].r; out[1]=S[N-1].g; out[2]=S[N-1].b; return; }
    for (int i = 1; i < N; ++i) {
        if (t <= S[i].t) {
            float f = (t - S[i-1].t) / (S[i].t - S[i-1].t);
            out[0] = (uint8_t)(S[i-1].r + f*(S[i].r - S[i-1].r));
            out[1] = (uint8_t)(S[i-1].g + f*(S[i].g - S[i-1].g));
            out[2] = (uint8_t)(S[i-1].b + f*(S[i].b - S[i-1].b));
            return;
        }
    }
}

static void s_refresh_pixel(int col, int row) {
    if (!s_rgba || col < 0 || col >= s_tw || row < 0 || row >= s_th) return;
    float h = TerrainMaster_GetPixel(col, row);
    uint8_t rgb[3]; s_height_to_rgba(h, TerrainMaster_HMax(), rgb);
    uint8_t* p = &s_rgba[(row * s_tw + col) * 4];
    p[0]=rgb[0]; p[1]=rgb[1]; p[2]=rgb[2]; p[3]=255;
}

static void s_rebuild_all_rgba() {
    for (int r = 0; r < s_th; ++r)
        for (int c = 0; c < s_tw; ++c)
            s_refresh_pixel(c, r);
}

// ── Init (call once after TerrainMaster_Load) ─────────────────────────────────
static bool Init() {
    if (!TerrainMaster_Loaded()) return false;
    s_tw = TerrainMaster_Width();
    s_th = TerrainMaster_Height();

    s_rgba = new uint8_t[s_tw * s_th * 4]();
    s_rebuild_all_rgba();

    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();

    SDL_GPUTextureCreateInfo ci = {};
    ci.type        = SDL_GPU_TEXTURETYPE_2D;
    ci.format      = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.usage       = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width       = (uint32_t)s_tw;
    ci.height      = (uint32_t)s_th;
    ci.layer_count_or_depth = 1;
    ci.num_levels  = 1;
    s_tex = SDL_CreateGPUTexture(dev, &ci);

    SDL_GPUTransferBufferCreateInfo tci = {};
    tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tci.size  = (uint32_t)(s_tw * s_th * 4);
    s_tbuf = SDL_CreateGPUTransferBuffer(dev, &tci);

    s_dirty  = true;
    s_inited = (s_tex != nullptr && s_tbuf != nullptr);
    return s_inited;
}

// ── Upload dirty CPU buffer to GPU (call in RenderFrame, same cmd as ImGui) ───
static void UploadTexture(SDL_GPUCommandBuffer* cmd) {
    if (!s_dirty || !s_tex || !s_tbuf) return;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    void* mem = SDL_MapGPUTransferBuffer(dev, s_tbuf, false);
    if (!mem) return;
    memcpy(mem, s_rgba, (size_t)(s_tw * s_th * 4));
    SDL_UnmapGPUTransferBuffer(dev, s_tbuf);

    SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src = {};
    src.transfer_buffer = s_tbuf;
    src.offset          = 0;
    src.pixels_per_row  = (uint32_t)s_tw;
    src.rows_per_layer  = (uint32_t)s_th;
    SDL_GPUTextureRegion dst = {};
    dst.texture = s_tex;
    dst.w = (uint32_t)s_tw; dst.h = (uint32_t)s_th; dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    s_dirty = false;
}

// ── Brush ─────────────────────────────────────────────────────────────────────
enum class Mode { Raise=0, Lower, Smooth, Flatten };
static Mode  s_mode    = Mode::Raise;
static float s_radius  = 4.f;   // hmap pixels
static float s_str     = 10.f;  // metres/second

static void s_paint(float pc, float pr, float dt) {
    if (!TerrainMaster_Loaded() || !s_rgba) return;
    float R2  = s_radius * s_radius;
    float str = s_str * dt;
    int c0 = (int)(pc - s_radius) - 1, c1 = (int)(pc + s_radius) + 1;
    int r0 = (int)(pr - s_radius) - 1, r1 = (int)(pr + s_radius) + 1;
    if (c0 < 0) c0 = 0; if (c1 >= s_tw) c1 = s_tw - 1;
    if (r0 < 0) r0 = 0; if (r1 >= s_th) r1 = s_th - 1;
    float ch = TerrainMaster_GetPixel((int)pc, (int)pr);
    bool touched = false;
    for (int r = r0; r <= r1; ++r) {
        for (int c = c0; c <= c1; ++c) {
            float dx = c - pc, dz = r - pr;
            if (dx*dx + dz*dz > R2) continue;
            float t  = 1.f - sqrtf(dx*dx+dz*dz) / s_radius;
            float fo = t * t;
            float h  = TerrainMaster_GetPixel(c, r);
            float nh = h;
            switch (s_mode) {
                case Mode::Raise:   nh = h + str * fo; break;
                case Mode::Lower:   nh = h - str * fo; if (nh < 0.f) nh = 0.f; break;
                case Mode::Flatten: nh = h + (ch - h) * fo * (str * 0.05f < 1.f ? str * 0.05f : 1.f); break;
                case Mode::Smooth: {
                    float avg = 0.f;
                    for (int dc=-2;dc<=2;++dc) for(int dr=-2;dr<=2;++dr)
                        avg += TerrainMaster_GetPixel(c+dc, r+dr);
                    avg /= 25.f;
                    nh = h + (avg - h) * fo * (str * 0.03f < 1.f ? str * 0.03f : 1.f);
                    break;
                }
            }
            TerrainMaster_SetPixel(c, r, nh);
            s_refresh_pixel(c, r);
            touched = true;
        }
    }
    if (touched) s_dirty = true;
}

// ── Dock/Detach state ─────────────────────────────────────────────────────────
static bool    s_detached = false;
static ImVec2  s_win_pos  = {100.f, 60.f};
static ImVec2  s_win_size = {600.f, 620.f};

// ── Zoom/pan state ────────────────────────────────────────────────────────────
static float s_zoom = 4.f;   // 1 = full world; 4 = ~16 zones visible; 32 = max
static float s_vcx  = 0.5f; // view centre [0..1] in hmap X
static float s_vcy  = 0.5f; // view centre [0..1] in hmap Y

static void s_clamp_view() {
    float half = 0.5f / s_zoom;
    if (s_vcx - half < 0.f) s_vcx = half;
    if (s_vcx + half > 1.f) s_vcx = 1.f - half;
    if (s_vcy - half < 0.f) s_vcy = half;
    if (s_vcy + half > 1.f) s_vcy = 1.f - half;
}

// ── Content (toolbar + map image) — called both docked and floating ───────────
static void s_draw_content() {
    if (!s_inited) { ImGui::TextDisabled("Master hmap not loaded."); return; }

    // ── Toolbar ───────────────────────────────────────────────────────────────
    const char* modes[] = {"Raise","Lower","Smooth","Flatten"};
    int m = (int)s_mode;
    ImGui::SetNextItemWidth(90.f);
    if (ImGui::Combo("##md", &m, modes, 4)) s_mode = (Mode)m;
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.f);
    ImGui::SliderFloat("R##r", &s_radius, 1.f, 40.f, "%.0fpx");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.f);
    ImGui::SliderFloat("S##s", &s_str, 0.5f, 80.f, "%.0fm/s");
    ImGui::SameLine();
    if (ImGui::Button("Save (F5)")) TerrainMaster_Save(MASTER_PATH);
    // Detach / Dock button — right-aligned
    {
        const char* lbl = s_detached ? "Dock##hmap" : "Detach##hmap";
        float bw = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - bw);
        ImGui::PushStyleColor(ImGuiCol_Button,
            s_detached ? ImVec4(0.25f,0.45f,0.65f,1.f) : ImVec4(0.18f,0.18f,0.28f,1.f));
        if (ImGui::Button(lbl)) s_detached = !s_detached;
        ImGui::PopStyleColor();
    }

    // ── Map image (zoom/pan) ──────────────────────────────────────────────────
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float dw = avail.x, dh = avail.y;
    // Keep square aspect if map is square
    if (s_tw == s_th) {
        float side = dw < dh ? dw : dh;
        dw = dh = side;
    }

    ImVec2 origin = ImGui::GetCursorScreenPos();
    float ox = origin.x + (avail.x - dw) * 0.5f;
    float oy = origin.y + (avail.y - dh) * 0.5f;

    // UV range for the zoomed view
    float uv0x = s_vcx - 0.5f / s_zoom,  uv1x = s_vcx + 0.5f / s_zoom;
    float uv0y = s_vcy - 0.5f / s_zoom,  uv1y = s_vcy + 0.5f / s_zoom;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage((ImTextureID)s_tex, {ox, oy}, {ox+dw, oy+dh},
                 {uv0x, uv0y}, {uv1x, uv1y});

    ImGui::InvisibleButton("##hmapbtn", {avail.x, avail.y});
    bool hov = ImGui::IsItemHovered();

    if (hov && s_tex) {
        ImVec2 mouse = ImGui::GetMousePos();

        // Scroll wheel: zoom (keep world point under mouse fixed)
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.f) {
            float frac_x = (mouse.x - ox) / dw;
            float frac_y = (mouse.y - oy) / dh;
            float uv_mx = uv0x + frac_x / s_zoom;
            float uv_my = uv0y + frac_y / s_zoom;
            s_zoom *= (wheel > 0 ? 1.25f : 0.8f);
            if (s_zoom < 1.f) s_zoom = 1.f;
            if (s_zoom > 32.f) s_zoom = 32.f;
            // Re-centre so the hovered world point stays under cursor
            s_vcx = uv_mx - (frac_x - 0.5f) / s_zoom;
            s_vcy = uv_my - (frac_y - 0.5f) / s_zoom;
            s_clamp_view();
            uv0x = s_vcx - 0.5f/s_zoom; uv1x = s_vcx + 0.5f/s_zoom;
            uv0y = s_vcy - 0.5f/s_zoom; uv1y = s_vcy + 0.5f/s_zoom;
        }

        // RMB drag: pan
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.f)) {
            ImVec2 delta = ImGui::GetMouseDragDelta(ImGuiMouseButton_Right, 0.f);
            ImGui::ResetMouseDragDelta(ImGuiMouseButton_Right);
            s_vcx -= delta.x / (dw * s_zoom);
            s_vcy -= delta.y / (dh * s_zoom);
            s_clamp_view();
            uv0x = s_vcx - 0.5f/s_zoom; uv1x = s_vcx + 0.5f/s_zoom;
            uv0y = s_vcy - 0.5f/s_zoom; uv1y = s_vcy + 0.5f/s_zoom;
        }

        // LMB: paint — compute hmap pixel from zoomed view
        float px = (uv0x + (mouse.x - ox) / dw / s_zoom) * s_tw;
        float py = (uv0y + (mouse.y - oy) / dh / s_zoom) * s_th;
        if (px >= 0.f && px < (float)s_tw && py >= 0.f && py < (float)s_th) {
            // Brush circle in screen space
            float pix_per_hmap = dw * s_zoom / s_tw;  // screen pixels per hmap pixel
            float sr = s_radius * pix_per_hmap;
            float scr_x = ox + (px / s_tw - uv0x) * dw * s_zoom;
            float scr_y = oy + (py / s_th - uv0y) * dh * s_zoom;
            uint32_t cc = (s_mode == Mode::Lower)  ? IM_COL32(255, 80, 60,220)
                        : (s_mode == Mode::Smooth || s_mode == Mode::Flatten)
                                                    ? IM_COL32( 80,200,255,220)
                                                    : IM_COL32(255,220, 60,220);
            dl->AddCircle({scr_x, scr_y}, sr, cc, 32, 1.5f);

            float h  = TerrainMaster_GetPixel((int)px, (int)py);
            float wx = (px / s_tw) * 64.f * 500.f;
            float wz = (py / s_th) * 64.f * 500.f;
            ImGui::SetTooltip("Zone %d,%d  |  %.0fm  |  world %.0f,%.0f",
                              (int)(px/s_tw*64.f), (int)(py/s_th*64.f), h, wx, wz);

            if (ImGui::IsMouseDown(ImGuiMouseButton_Left))
                s_paint(px, py, ImGui::GetIO().DeltaTime);
        }
    }
    if (ImGui::IsKeyPressed(ImGuiKey_F5)) TerrainMaster_Save(MASTER_PATH);
}

// ── DrawPanel — call inside BeginTabItem/EndTabItem ───────────────────────────
static void DrawPanel() {
    if (!s_inited && TerrainMaster_Loaded()) Init();

    if (s_detached) {
        // Floating window
        ImGui::SetNextWindowPos (s_win_pos,  ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(s_win_size, ImGuiCond_Appearing);
        bool open = true;
        if (ImGui::Begin("Heightmap Editor##hmapfloat", &open)) {
            s_win_pos  = ImGui::GetWindowPos();
            s_win_size = ImGui::GetWindowSize();
            s_draw_content();
        }
        ImGui::End();
        if (!open) s_detached = false;
        ImGui::Dummy({0.f, 0.f});  // leave tab area empty
        return;
    }
    // Docked — draw inline inside tab
    s_draw_content();
}

} // namespace HmapEditor2D
#endif // MD_SDL_GPU
