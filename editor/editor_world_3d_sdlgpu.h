#pragma once
#ifdef MD_SDL_GPU
// editor_world_3d_sdlgpu.h — 3D world viewport for standalone SDL_GPU editor.
// Uses the same TerrainRenderer as the game; renders to an SDL_GPU RTT
// and displays via ImGui::Image().
//
// Usage in main.cpp:
//   1. WorldEditor3D_SDLGPU::Init(...)   — once, after GpuDevice ready
//   2. Before ImGui render:
//      WorldEditor3D_SDLGPU::RenderFrame(cmd, dt)
//   3. Inside "3D World" tab:
//      WorldEditor3D_SDLGPU::DrawImGui(avail_w, avail_h)

#include "imgui.h"
#include <monkey_dust/render/terrain_renderer.h>
#include <monkey_dust/render/prop_renderer.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
#include <monkey_dust/render/light_system.h>
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/world/terrain_chunk.h>
#include <monkey_dust/world/chunk_def.h>
#include "world/biome_def.h"
#include "world/world_registry.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <atomic>
#include <thread>
#include <cstring>
#include <cstdio>
#include <cmath>

// SkyUBO — mirrors game's scene_render.h SkyUBO (same sky.vert/frag shaders)
struct alignas(16) EditorSkyUBO {
    float cam_right[4];
    float cam_up[4];
    float cam_fwd[4];
    float sun_dir[4];
    float horizon_col[4];
    float fov_tan;
    float aspect;
    float _pad[2];
};

namespace WorldEditor3D_SDLGPU {

// ── State ──────────────────────────────────────────────────────────────────────
static constexpr int   EDITOR_TNKN = 64;  // 64×64 = full world (32×32 km)

static TerrainRenderer    s_terrain;
static PropRenderer       s_props;
static TerrainChunk       s_chunks[EDITOR_TNKN][EDITOR_TNKN];
static bool               s_loaded = false;
static GpuPipeline        s_sky_pipeline;

// Prop scatter state — rebuilt once on init
static constexpr int  PROPS_PER_CHUNK = 8;
static float          s_prop_pos[PropRenderer::MAX_PROPS * 3] = {};
static int            s_prop_count = 0;
static bool           s_props_built = false;

// Async loading state
static std::atomic<bool>  s_master_ready{false};
static std::atomic<int>   s_chunks_built{0};
static std::thread        s_loader_thread;
static constexpr int      s_zone_ox_saved = 0;
static constexpr int      s_zone_oz_saved = 0;
static bool               s_rebuild_pending = false;

// Per-zone amplitude from terrain_config.txt (indexed [gz][gx], default 40)
static float s_zone_amp[64][64];


// Build deterministic rock positions across the 7×7 near-zone viewport.
// Uses LCG seeded per-chunk so positions are stable across camera moves.
// Called once after atlas is ready; rebuilt when zone_ox/oz changes.
static void s_build_prop_positions() {
    s_prop_count = 0;
    const int max = PropRenderer::MAX_PROPS;
    for (int dz = 0; dz < EDITOR_TNKN && s_prop_count < max; ++dz) {
        for (int dx = 0; dx < EDITOR_TNKN && s_prop_count < max; ++dx) {
            int zx = s_zone_ox_saved + dx;
            int zz = s_zone_oz_saved + dz;
            // LCG seed from chunk coords — deterministic scatter
            unsigned int rng = (unsigned int)(zx * 73856093u ^ zz * 19349663u ^ 2654435761u);
            for (int p = 0; p < PROPS_PER_CHUNK && s_prop_count < max; ++p) {
                rng = rng * 1664525u + 1013904223u;
                float lx = (float)((rng >> 8) & 0xFFFF) / 65535.f * CHUNK_SIZE;
                rng = rng * 1664525u + 1013904223u;
                float lz = (float)((rng >> 8) & 0xFFFF) / 65535.f * CHUNK_SIZE;
                float wx = zx * CHUNK_SIZE + lx;
                float wz = zz * CHUNK_SIZE + lz;
                float wy = TerrainMaster_SampleWorld(wx, wz);
                float* p3 = &s_prop_pos[s_prop_count * 3];
                p3[0] = wx; p3[1] = wy; p3[2] = wz;
                ++s_prop_count;
            }
        }
    }
    s_props_built = true;
}

static void s_load_zone_amplitudes(const char* cfg_path) {
    for (int i = 0; i < 64; ++i)
        for (int j = 0; j < 64; ++j)
            s_zone_amp[i][j] = 40.f;
    FILE* f = fopen(cfg_path, "r");
    if (!f) return;
    char line[256];
    int gx = -1, gz = -1; float amp = 40.f;
    auto commit = [&]() {
        if (gx >= 0 && gx < 64 && gz >= 0 && gz < 64) s_zone_amp[gz][gx] = amp;
    };
    while (fgets(line, sizeof(line), f)) {
        int v; float fv;
        if (strncmp(line, "zone=", 5) == 0)   { commit(); gx = gz = -1; amp = 40.f; }
        else if (sscanf(line, " grid_x=%d",    &v)  == 1) gx  = v;
        else if (sscanf(line, " grid_z=%d",    &v)  == 1) gz  = v;
        else if (sscanf(line, " amplitude=%f", &fv) == 1) amp = fv;
    }
    commit();
    fclose(f);
    fprintf(stdout, "[W3D] zone amplitudes loaded from %s\n", cfg_path);
}


// RTT
static SDL_GPUTexture* s_color = nullptr;
static SDL_GPUTexture* s_depth = nullptr;
static int             s_rtt_w = 0, s_rtt_h = 0;
static int             s_last_w = 1280, s_last_h = 720;  // use prev frame size

// Camera (free-fly world-space)
static float s_cam_x    = 16000.f;
static float s_cam_z    = 14000.f;
static float s_cam_az   = 0.f;
static float s_cam_el   = 0.70f;
static float s_cam_dist = 22.f;
// Free-fly state
static float s_cx = 16000.f, s_cy = 1500.f, s_cz = 14000.f;
static float s_yaw = 0.f, s_pitch = 0.38f;
static float s_speed = 500.f;
static bool  s_rmb      = false;
static bool  s_focused  = false;

// ── Terrain brush ──────────────────────────────────────────────────────────────
enum class BrushMode { Raise=0, Lower, Smooth, Flatten };
static BrushMode s_brush_mode     = BrushMode::Raise;
static float     s_brush_radius   = 150.f;    // metres
static float     s_brush_strength = 8.f;      // m/s
static bool      s_brush_hit      = false;
static float     s_brush_wx = 0.f, s_brush_wy = 0.f, s_brush_wz = 0.f;
static float     s_last_vp[16]    = {};        // VP matrix from last RenderFrame
static float     s_last_eye[3]    = {};        // camera world pos from last RenderFrame
static bool      s_chunk_dirty[EDITOR_TNKN][EDITOR_TNKN] = {};
static constexpr const char* MASTER_PATH = "game/data/terrain/md_master_hmap.r32";

// ── Mat4 helpers (column-major) ────────────────────────────────────────────────
struct M4 { float m[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; };
static M4 m4_mul(const M4& a, const M4& b) {
    M4 c; memset(c.m, 0, 64);
    for (int i=0;i<4;i++) for (int j=0;j<4;j++) for (int k=0;k<4;k++)
        c.m[i*4+j] += a.m[k*4+j] * b.m[i*4+k];
    return c;
}
static M4 m4_persp(float fov, float asp, float n, float f) {
    M4 r; memset(r.m, 0, 64);
    float t = 1.f / tanf(fov * 0.5f);
    r.m[0]=t/asp; r.m[5]=t;
    r.m[10]=(f+n)/(n-f); r.m[11]=-1.f; r.m[14]=(2.f*f*n)/(n-f);
    return r;
}
static M4 m4_view(float ex, float ey, float ez, float yaw, float pitch) {
    float sy=sinf(yaw), cy=cosf(yaw), sp=sinf(pitch), cp=cosf(pitch);
    float fx=sy*cp, fy=-sp, fz=cy*cp;
    float rx=-fz/cp, ry=0.f, rz=fx/cp;
    float ux=ry*fz-rz*fy, uy=rz*fx-rx*fz, uz=rx*fy-ry*fx;
    M4 r;
    r.m[0]=rx; r.m[1]=ux; r.m[2]=-fx; r.m[3]=0;
    r.m[4]=ry; r.m[5]=uy; r.m[6]=-fy; r.m[7]=0;
    r.m[8]=rz; r.m[9]=uz; r.m[10]=-fz;r.m[11]=0;
    r.m[12]=-(rx*ex+ry*ey+rz*ez);
    r.m[13]=-(ux*ex+uy*ey+uz*ez);
    r.m[14]= (fx*ex+fy*ey+fz*ez);
    r.m[15]=1;
    return r;
}

// ── Ray → terrain intersection ─────────────────────────────────────────────────
// Returns true if the ray hits terrain within 30 km; fills out_w*.
static bool s_ray_terrain(float ox, float oy, float oz,
                           float rdx, float rdy, float rdz,
                           float& out_wx, float& out_wy, float& out_wz) {
    float len = sqrtf(rdx*rdx + rdy*rdy + rdz*rdz);
    if (len < 1e-6f) return false;
    rdx /= len; rdy /= len; rdz /= len;
    if (rdy >= 0.f) return false;  // ray goes upward — never hits ground

    auto sample_h = [](float wx, float wz) -> float {
        return TerrainMaster_SampleWorld(wx, wz);
    };

    float prev_t = 0.f, t = 0.f;
    while (t < 30000.f) {
        float wx = ox + rdx*t, wy = oy + rdy*t, wz = oz + rdz*t;
        if (wx < 0||wz < 0||wx > 64.f*CHUNK_SIZE||wz > 64.f*CHUNK_SIZE) break;
        float h = sample_h(wx, wz);
        if (wy < h) {
            // Binary refine
            float lo = prev_t, hi = t;
            for (int i = 0; i < 12; ++i) {
                float mid = (lo + hi) * 0.5f;
                float my = oy + rdy*mid, mh = sample_h(ox + rdx*mid, oz + rdz*mid);
                if (my < mh) hi = mid; else lo = mid;
            }
            float ft = (lo + hi) * 0.5f;
            out_wx = ox + rdx*ft;  out_wz = oz + rdz*ft;
            out_wy = sample_h(out_wx, out_wz);
            return true;
        }
        prev_t = t;
        float gap = wy - h;
        t += (gap < 5.f) ? 2.f : (gap < 100.f ? 10.f : fminf(gap * 0.4f, 200.f));
    }
    return false;
}

// ── Apply brush to master heightmap pixels ─────────────────────────────────────
static void s_apply_brush(float dt) {
    if (!TerrainMaster_Loaded()) return;
    int   mw  = TerrainMaster_Width();
    int   mhh = TerrainMaster_Height();
    float R2  = s_brush_radius * s_brush_radius;
    float str = s_brush_strength * dt;
    // world extent covered by master hmap = 64 zones × CHUNK_SIZE
    const float WEXT = (float)(64 * CHUNK_SIZE);
    float px_sz = WEXT / (float)mw;  // metres per pixel (≈125m at 256×256)

    int c0 = (int)((s_brush_wx - s_brush_radius) / WEXT * mw) - 1;
    int c1 = (int)((s_brush_wx + s_brush_radius) / WEXT * mw) + 1;
    int r0 = (int)((s_brush_wz - s_brush_radius) / WEXT * mhh) - 1;
    int r1 = (int)((s_brush_wz + s_brush_radius) / WEXT * mhh) + 1;
    if (c0 < 0) c0 = 0;  if (c1 >= mw)  c1 = mw  - 1;
    if (r0 < 0) r0 = 0;  if (r1 >= mhh) r1 = mhh - 1;

    bool any_touched = false;
    for (int row = r0; row <= r1; ++row) {
        for (int col = c0; col <= c1; ++col) {
            float wx = (col + 0.5f) * px_sz;
            float wz = (row + 0.5f) * px_sz;
            float dx = wx - s_brush_wx, dz = wz - s_brush_wz;
            float d2 = dx*dx + dz*dz;
            if (d2 > R2) continue;
            float t       = 1.f - sqrtf(d2) / s_brush_radius;
            float falloff = t * t;
            float h  = TerrainMaster_GetPixel(col, row);
            float nh = h;
            switch (s_brush_mode) {
                case BrushMode::Raise:   nh = h + str * falloff; break;
                case BrushMode::Lower:   nh = h - str * falloff; break;
                case BrushMode::Flatten: nh = h + (s_brush_wy - h) * falloff * fminf(str * 0.2f, 1.f); break;
                case BrushMode::Smooth: {
                    float avg = 0.f;
                    for (int dc = -2; dc <= 2; ++dc)
                        for (int dr = -2; dr <= 2; ++dr)
                            avg += TerrainMaster_GetPixel(col+dc, row+dr);
                    avg /= 25.f;
                    nh = h + (avg - h) * falloff * fminf(str * 0.05f, 1.f);
                    break;
                }
            }
            if (nh < 0.f) nh = 0.f;
            TerrainMaster_SetPixel(col, row, nh);
            any_touched = true;
        }
    }
    if (any_touched) {
        // Mark all near chunks dirty — master hmap change affects the whole area
        for (int dz = 0; dz < EDITOR_TNKN; ++dz)
            for (int dx = 0; dx < EDITOR_TNKN; ++dx) {
                float chunk_wx0 = (s_zone_ox_saved + dx) * CHUNK_SIZE;
                float chunk_wz0 = (s_zone_oz_saved + dz) * CHUNK_SIZE;
                if (s_brush_wx >= chunk_wx0 - s_brush_radius &&
                    s_brush_wx <= chunk_wx0 + CHUNK_SIZE + s_brush_radius &&
                    s_brush_wz >= chunk_wz0 - s_brush_radius &&
                    s_brush_wz <= chunk_wz0 + CHUNK_SIZE + s_brush_radius)
                    s_chunk_dirty[dz][dx] = true;
            }
    }
}

// ── UploadTerrainHeightmap — write PCG tile into master + mark chunks dirty ─────
// hmap: W×H float array (metres), chunk_x/z: zone-grid coords (same as TileData).
static void UploadTerrainHeightmap(const float* hmap, int W, int H,
                                    float world_size_m, int chunk_x, int chunk_z) {
    if (!hmap || W <= 0 || H <= 0) return;
    if (TerrainMaster_Loaded()) {
        int mw = TerrainMaster_Width();
        int mhh = TerrainMaster_Height();
        const float WEXT = (float)(64 * CHUNK_SIZE);
        float m_px = WEXT / (float)mw; // metres per master pixel
        float pcg_cell = world_size_m / (float)W;

        float wx0 = (float)chunk_x * world_size_m;
        float wz0 = (float)chunk_z * world_size_m;

        int c0 = (int)(wx0 / m_px);
        int c1 = (int)((wx0 + world_size_m) / m_px) + 1;
        int r0 = (int)(wz0 / m_px);
        int r1 = (int)((wz0 + world_size_m) / m_px) + 1;
        if (c0 < 0) c0 = 0; if (c1 > mw)  c1 = mw;
        if (r0 < 0) r0 = 0; if (r1 > mhh) r1 = mhh;

        for (int row = r0; row < r1; ++row) {
            for (int col = c0; col < c1; ++col) {
                float lx = col * m_px - wx0;
                float lz = row * m_px - wz0;
                float fx = lx / pcg_cell, fz = lz / pcg_cell;
                int ix = (int)fx, iz = (int)fz;
                if (ix < 0) ix = 0; if (ix >= W-1) ix = W-2;
                if (iz < 0) iz = 0; if (iz >= H-1) iz = H-2;
                float tx = fx - (float)ix, tz = fz - (float)iz;
                float h = hmap[iz*W+ix]*(1-tx)*(1-tz)
                        + hmap[iz*W+(ix+1)]*tx*(1-tz)
                        + hmap[(iz+1)*W+ix]*(1-tx)*tz
                        + hmap[(iz+1)*W+(ix+1)]*tx*tz;
                TerrainMaster_SetPixel(col, row, h);
            }
        }
    }
    // Mark near chunks that overlap this zone dirty
    for (int dz = 0; dz < EDITOR_TNKN; ++dz)
        for (int dx = 0; dx < EDITOR_TNKN; ++dx)
            if (s_zone_ox_saved + dx == chunk_x && s_zone_oz_saved + dz == chunk_z)
                s_chunk_dirty[dz][dx] = true;
}

// ── RTT management ─────────────────────────────────────────────────────────────
static void ensure_rtt(int w, int h) {
    if (w == s_rtt_w && h == s_rtt_h) return;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (s_color) SDL_ReleaseGPUTexture(dev, s_color);
    if (s_depth) SDL_ReleaseGPUTexture(dev, s_depth);
    s_rtt_w = w; s_rtt_h = h;

    SDL_GPUTextureCreateInfo ci = {};
    ci.type        = SDL_GPU_TEXTURETYPE_2D;
    ci.format      = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.usage       = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width       = (uint32_t)w; ci.height = (uint32_t)h;
    ci.layer_count_or_depth = 1; ci.num_levels = 1;
    s_color = SDL_CreateGPUTexture(dev, &ci);

    ci.format = SDL_GPU_TEXTUREFORMAT_D32_FLOAT;   // D32_FLOAT — D24_UNORM causes GPU hang on Intel Gen9
    ci.usage   = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    s_depth = SDL_CreateGPUTexture(dev, &ci);

    fprintf(stdout, "[W3D-SDLGPU] RTT %dx%d\n", w, h);
}

// ── Init — loads master hmap synchronously (257 KiB), background: renderer ──
static bool Init(const char* overlay_path, int /*zone_ox*/ = 28, int /*zone_oz*/ = 28) {
    s_cx = 16000.f;
    s_cy = 8000.f;   // start high to see whole world
    s_cz = 16000.f;
    s_yaw = 0.f; s_pitch = 0.38f;

    s_load_zone_amplitudes("game/data/terrain_config.txt");

    // Master hmap is 257 KiB — load synchronously before spinning the thread
    if (!TerrainMaster_Load(MASTER_PATH, 64.f * CHUNK_SIZE, 64.f * CHUNK_SIZE))
        fprintf(stderr, "[W3D-SDLGPU] master hmap load failed: %s\n", MASTER_PATH);

    // Sky pipeline — same shaders as game (sky.vert/frag, SkyUBO)
    {
        GpuPipeline::Desc sd;
        sd.vert_path          = "shaders/sky.vert";
        sd.frag_path          = "shaders/sky.frag";
        sd.vert_uniform_bufs  = 1;
        sd.frag_uniform_bufs  = 1;
        sd.has_depth_target   = true;
        sd.raster.depth_test  = false;
        sd.raster.depth_write = false;
        sd.raster.cull_back   = false;
        sd.layout.count       = 0;
        s_sky_pipeline.Create(sd);
    }

    const char* op = overlay_path;
    s_loader_thread = std::thread([op]() {
        if (!s_terrain.Init()) {
            fprintf(stderr, "[W3D-SDLGPU] TerrainRenderer init failed\n"); return;
        }
        s_props.Init("game/data/props/rocks/rock_01.glb"); // no-op if missing
        s_terrain.InitKenshiOverlay(op);
        TerrainRenderer::PomParams pom; pom.height_scale=0.04f; pom.layers_min=4; pom.layers_max=8;
        s_terrain.InitPOM("game/data/textures/terrain_pom_rock.png", pom);
        s_master_ready = true;
        s_build_prop_positions();
    });
    s_loader_thread.detach();
    return true;
}

// ── Internal: begin rebuild (s_zone_ox/oz must be set before calling) ─────────
static void s_begin_rebuild() {
    s_chunks_built.store(0);
    s_rebuild_pending = false;
    s_loaded = false;
    if (s_rmb) {
        SDL_SetWindowRelativeMouseMode(SDL_GetMouseFocus(), false);
        s_rmb = false;
    }
    float _dx, _dy;
    SDL_GetRelativeMouseState(&_dx, &_dy);  // drain accumulated delta
}

// R: rebuild all chunks (after sculpting)
static void rebuild_inplace() {
    s_begin_rebuild();
}

// ── Tick: build 8 chunks per call until all 64×64 are loaded ──────────────────
static void tick_chunk_build() {
    if (s_loaded) return;
    if (!s_master_ready) return;

    for (int b = 0; b < 8; ++b) {
    int idx = s_chunks_built.load();
    if (idx >= EDITOR_TNKN * EDITOR_TNKN) { s_loaded = true; return; }

    int cz = idx / EDITOR_TNKN, cx = idx % EDITOR_TNKN;
    ChunkCoord coord = { cx, cz };
    TerrainGenParams p;
    p.zone_origin_x = 0;
    p.zone_origin_z = 0;
    p.amplitude = TerrainMaster_Loaded() ? 0.f :
        ((coord.x >= 0 && coord.x < 64 && coord.z >= 0 && coord.z < 64)
            ? s_zone_amp[coord.z][coord.x] : 40.f);
    TerrainGen_Build(s_chunks[cz][cx], coord, p);
    TerrainGen_Upload(s_chunks[cz][cx]);
    s_chunks[cz][cx].center_x = (float)cx * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
    s_chunks[cz][cx].center_z = (float)cz * CHUNK_SIZE + CHUNK_SIZE * 0.5f;

    ++s_chunks_built;
    if (s_chunks_built >= EDITOR_TNKN * EDITOR_TNKN) {
        s_loaded = true;
        fprintf(stdout, "[W3D-SDLGPU] %dx%d chunks ready\n", EDITOR_TNKN, EDITOR_TNKN);
    }
    } // for b
}

// ── Camera input (original free-fly) ─────────────────────────────────────────
static void handle_input(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    if (io.MouseDown[1]) {
        s_rmb = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        s_yaw   -= io.MouseDelta.x * 0.003f;
        s_pitch += io.MouseDelta.y * 0.002f;
        if (s_pitch < -0.3f) s_pitch = -0.3f;
        if (s_pitch >  1.3f) s_pitch =  1.3f;
    } else {
        s_rmb = false;
    }
    float sp = s_cy * dt;
    float sy = sinf(s_yaw), cy2 = cosf(s_yaw);
    if (ImGui::IsKeyDown(ImGuiKey_W)||ImGui::IsKeyDown(ImGuiKey_UpArrow))   { s_cx+=sp*sy; s_cz+=sp*cy2; }
    if (ImGui::IsKeyDown(ImGuiKey_S)||ImGui::IsKeyDown(ImGuiKey_DownArrow)) { s_cx-=sp*sy; s_cz-=sp*cy2; }
    if (ImGui::IsKeyDown(ImGuiKey_A))  { s_cx+=sp*cy2; s_cz-=sp*sy; }
    if (ImGui::IsKeyDown(ImGuiKey_D))  { s_cx-=sp*cy2; s_cz+=sp*sy; }
    if (ImGui::IsKeyDown(ImGuiKey_Q)||ImGui::IsKeyDown(ImGuiKey_PageDown)) s_cy-=sp;
    if (ImGui::IsKeyDown(ImGuiKey_E)||ImGui::IsKeyDown(ImGuiKey_PageUp))   s_cy+=sp;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) rebuild_inplace();
    if (ImGui::IsKeyPressed(ImGuiKey_T)) { s_cx=16000.f; s_cy=8000.f; s_cz=16000.f; }
    if (io.MouseWheel != 0.f) {
        float step = s_cy * 0.05f * io.MouseWheel;
        s_cx += step*sy; s_cz += step*cy2;
        if (io.MouseWheel > 0) s_cy = fmaxf(s_cy * 0.92f, 10.f);
        else                   s_cy = fminf(s_cy * 1.08f, 150000.f);
    }
    static constexpr float ATLAS_MAX = 63.f * CHUNK_SIZE;
    if (s_cx < 0.f) s_cx = 0.f; if (s_cx > ATLAS_MAX) s_cx = ATLAS_MAX;
    if (s_cz < 0.f) s_cz = 0.f; if (s_cz > ATLAS_MAX) s_cz = ATLAS_MAX;
    if (s_cy < 5.f) s_cy = 5.f; if (s_cy > 150000.f) s_cy = 150000.f;
}

// ── Render terrain to RTT (call AFTER ImGui build, BEFORE ImGui present) ────────
// ensure_rtt() is called from DrawImGui (during ImGui build) so s_color is stable.
static void RenderFrame(SDL_GPUCommandBuffer* cmd, float dt) {
    tick_chunk_build(); tick_chunk_build();
    if (!s_master_ready.load() || !s_color) return;
    int w = s_rtt_w, h = s_rtt_h;  // use already-created RTT dimensions
    if (w < 8 || h < 8) return;

    float asp = (float)w / (float)h;
    M4 proj = m4_persp(0.80f, asp, 5.f, 350000.f);
    M4 view = m4_view(s_cx, s_cy, s_cz, s_yaw, s_pitch);
    M4 vp   = m4_mul(proj, view);
    memcpy(s_last_vp, vp.m, 64);
    // eye position for POM + brush ray
    float eye_x = s_cx, eye_y = s_cy, eye_z = s_cz;
    s_last_eye[0]=s_cx; s_last_eye[1]=s_cy; s_last_eye[2]=s_cz;

    // Rebuild dirty chunks (marked by s_apply_brush)
    if (s_loaded) {
        for (int cz = 0; cz < EDITOR_TNKN; ++cz) for (int cx = 0; cx < EDITOR_TNKN; ++cx) {
            if (!s_chunk_dirty[cz][cx]) continue;
            int chunk_zx = s_zone_ox_saved + cx, chunk_zz = s_zone_oz_saved + cz;
            ChunkCoord coord = { chunk_zx, chunk_zz };
            TerrainGenParams p; p.zone_origin_x = 0; p.zone_origin_z = 0;
            p.amplitude = TerrainMaster_Loaded() ? 0.f :
                ((chunk_zx>=0&&chunk_zx<64&&chunk_zz>=0&&chunk_zz<64)
                    ? s_zone_amp[chunk_zz][chunk_zx] : 40.f);
            TerrainGen_Build(s_chunks[cz][cx], coord, p);
            TerrainGen_Upload(s_chunks[cz][cx]);
            s_chunk_dirty[cz][cx] = false;
        }
    }

    // Sun direction + biome sky/fog from LightSystem + WorldRegistry
    const auto& ls  = LightSystem::Get();
    const char* cur_zone = WorldRegistry::Get().CurrentZone();
    const BiomeDef& biome = BiomeDef::ForZone(cur_zone ? cur_zone : "");

    // ── Terrain render pass ──────────────────────────────────────────────────
    SDL_GPUColorTargetInfo ct = {};
    ct.texture     = s_color;
    ct.load_op     = SDL_GPU_LOADOP_CLEAR;
    ct.store_op    = SDL_GPU_STOREOP_STORE;
    ct.clear_color = { biome.fog_r, biome.fog_g, biome.fog_b, 1.f };  // biome fog as clear

    SDL_GPUDepthStencilTargetInfo di = {};
    di.texture          = s_depth;
    di.clear_depth      = 1.f;
    di.load_op          = SDL_GPU_LOADOP_CLEAR;
    di.store_op         = SDL_GPU_STOREOP_STORE;
    di.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
    di.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &di);
    if (rp) {
        // ── Sky — same shader + SkyUBO as game ───────────────────────────────
        if (s_sky_pipeline.SDLPipeline()) {
            EditorSkyUBO sky{};
            // View matrix rows → camera basis vectors for sky ray generation
            const float* v = view.m;  // column-major
            sky.cam_right[0]=v[0]; sky.cam_right[1]=v[4]; sky.cam_right[2]=v[8];
            sky.cam_up[0]   =v[1]; sky.cam_up[1]   =v[5]; sky.cam_up[2]   =v[9];
            sky.cam_fwd[0]  =-v[2];sky.cam_fwd[1]  =-v[6];sky.cam_fwd[2]  =-v[10];
            sky.sun_dir[0]  = ls.sun_dir.x;
            sky.sun_dir[1]  = ls.sun_dir.y;
            sky.sun_dir[2]  = ls.sun_dir.z;
            sky.horizon_col[0] = biome.sky_horizon_r;
            sky.horizon_col[1] = biome.sky_horizon_g;
            sky.horizon_col[2] = biome.sky_horizon_b;
            sky.fov_tan = tanf(0.80f * 0.5f);
            sky.aspect  = asp;
            SDL_BindGPUGraphicsPipeline(rp, s_sky_pipeline.SDLPipeline());
            SDL_PushGPUVertexUniformData(cmd, 0, &sky, sizeof(sky));
            SDL_PushGPUFragmentUniformData(cmd, 0, &sky, sizeof(sky));
            SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
        }

        // High-res near terrain (7×7 chunks) with sun from LightSystem
        if (s_loaded && s_terrain.IsReady()) {
            static constexpr float W2UV = 1.f / (64.f * CHUNK_SIZE);
            static constexpr float WCX  = 32.f * CHUNK_SIZE;
            static constexpr float WCZ  = 32.f * CHUNK_SIZE;
            TerrainRenderer::SunParams sun;
            sun.dir[0] = ls.sun_dir.x; sun.dir[1] = ls.sun_dir.y; sun.dir[2] = ls.sun_dir.z;
            sun.strength   = 1.1f;
            sun.ambient[0] = biome.fog_r * 0.6f + 0.1f;
            sun.ambient[1] = biome.fog_g * 0.6f + 0.12f;
            sun.ambient[2] = biome.fog_b * 0.6f + 0.16f;
            // Distance-based LOD (uniform per frame = no T-junctions).
            // Pick LOD from camera altitude: high alt → coarser mesh.
            int world_lod = (s_cy > 4000.f) ? 3 : (s_cy > 1500.f) ? 2 : (s_cy > 500.f) ? 1 : 0;
            for (int cz = 0; cz < EDITOR_TNKN; ++cz)
                for (int cx = 0; cx < EDITOR_TNKN; ++cx)
                    s_terrain.DrawRawPOM(rp, cmd, s_chunks[cz][cx], vp.m,
                                        sun, eye_x, eye_y, eye_z, WCX, WCZ, W2UV,
                                        world_lod);
        }

        SDL_EndGPURenderPass(rp);
    }

    (void)dt;
}

// ── ImGui draw (call inside "3D World" tab) ────────────────────────────────────
static void DrawImGui(float W, float H, float dt) {
    s_last_w = (int)W > 4 ? (int)W : s_last_w;
    s_last_h = (int)H > 4 ? (int)H : s_last_h;

    // (chunk building handled in RenderFrame, called every frame)

    if (!s_master_ready.load()) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({W, H});
        ImGui::GetWindowDrawList()->AddRectFilled(p, {p.x+W, p.y+H}, IM_COL32(15,15,20,255));
        const char* msg = "Initialising terrain renderer...";
        ImVec2 tc = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(
            {p.x + W*0.5f - tc.x*0.5f, p.y + H*0.5f - 20},
            IM_COL32(200,200,200,255), msg);
        return;
    }

    // Create/resize RTT HERE so s_color is stable when AddImage captures it.
    // RenderFrame (called after ImGui::Render) then renders into this texture.
    if ((int)W > 4 && (int)H > 4)
        ensure_rtt((int)W, (int)H);

    ImVec2 origin = ImGui::GetCursorScreenPos();

    // Invisible button for input capture
    ImGui::InvisibleButton("##w3dsgpu", {W, H},
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hov = ImGui::IsItemHovered();
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        s_focused = true;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hov)
        s_focused = false;
    if (hov || s_rmb || s_focused || ImGui::GetIO().MouseWheel != 0.f) handle_input(dt);

    // ── Mouse → terrain ray cast (brush targeting) ───────────────────────────
    bool edit_mode = s_loaded;
    if (hov && edit_mode && !s_rmb) {
        ImVec2 mouse = ImGui::GetMousePos();
        float ndc_x = ((mouse.x - origin.x) / W) * 2.f - 1.f;
        float ndc_y = 1.f - ((mouse.y - origin.y) / H) * 2.f;
        // Unproject mouse ray using VP matrix inverse for orbit camera
        float thf = tanf(45.f * 0.00872664f), asp_b = W / H;
        const float* v = s_last_vp;
        float rdx = v[0]*(ndc_x*thf*asp_b) + v[4]*(ndc_y*thf) - v[8];
        float rdy = v[1]*(ndc_x*thf*asp_b) + v[5]*(ndc_y*thf) - v[9];
        float rdz = v[2]*(ndc_x*thf*asp_b) + v[6]*(ndc_y*thf) - v[10];
        s_brush_hit = s_ray_terrain(s_last_eye[0], s_last_eye[1], s_last_eye[2], rdx, rdy, rdz,
                                     s_brush_wx, s_brush_wy, s_brush_wz);

        // LMB held → paint
        if (s_brush_hit && ImGui::GetIO().MouseDown[0])
            s_apply_brush(dt);
    } else {
        s_brush_hit = false;
    }

    // Display RTT
    if (s_color) {
        ImGui::GetWindowDrawList()->AddImage(
            (ImTextureID)s_color, origin, {origin.x+W, origin.y+H});
        if (hov)
            ImGui::GetWindowDrawList()->AddRect(origin, {origin.x+W, origin.y+H},
                IM_COL32(80,140,220,160), 1.f);
    } else {
        ImGui::GetWindowDrawList()->AddRectFilled(origin, {origin.x+W,origin.y+H},
            IM_COL32(20,20,28,255));
    }

    // ── Brush cursor circle (project 32 world points through VP) ─────────────
    if (s_brush_hit && edit_mode && !s_rmb) {
        auto project = [&](float px, float py, float pz) -> ImVec2 {
            float cp[4] = {};
            float wp[4] = {px, py, pz, 1.f};
            for (int i = 0; i < 4; ++i)
                for (int k = 0; k < 4; ++k)
                    cp[i] += s_last_vp[k*4+i] * wp[k];
            if (fabsf(cp[3]) < 1e-5f) return {-99999.f, -99999.f};
            float nx = cp[0]/cp[3], ny = cp[1]/cp[3];
            return { origin.x + (nx * 0.5f + 0.5f) * W,
                     origin.y + (1.f - ny * 0.5f - 0.5f) * H };
        };
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const int N = 32;
        ImVec2 prev = {};
        uint32_t col = (s_brush_mode == BrushMode::Lower) ? IM_COL32(255,80,60,220)
                     : (s_brush_mode == BrushMode::Smooth || s_brush_mode == BrushMode::Flatten)
                       ? IM_COL32(80,200,255,220) : IM_COL32(255,210,60,220);
        for (int i = 0; i <= N; ++i) {
            float a = (float)i / N * 6.28318f;
            ImVec2 sp = project(s_brush_wx + cosf(a)*s_brush_radius,
                                s_brush_wy,
                                s_brush_wz + sinf(a)*s_brush_radius);
            if (i > 0) dl->AddLine(prev, sp, col, 2.f);
            prev = sp;
        }
        dl->AddCircleFilled(project(s_brush_wx, s_brush_wy, s_brush_wz), 4.f, col);
    }

    // ── Brush settings overlay (top-right) ───────────────────────────────────
    if (edit_mode) {
        ImGui::SetNextWindowPos({origin.x + W - 210.f, origin.y + 5.f}, ImGuiCond_Always);
        ImGui::SetNextWindowSize({205.f, 145.f}, ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.78f);
        ImGuiWindowFlags pf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings |
            ImGuiWindowFlags_NoFocusOnAppearing;
        if (ImGui::Begin("##brush_panel", nullptr, pf)) {
            ImGui::TextUnformatted("Terrain Brush");
            ImGui::Separator();
            const char* modes[] = {"Raise","Lower","Smooth","Flatten"};
            int m = (int)s_brush_mode;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::Combo("##bmode", &m, modes, 4)) s_brush_mode = (BrushMode)m;
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##brad", &s_brush_radius, 30.f, 600.f, "R=%.0fm");
            ImGui::SetNextItemWidth(-1);
            ImGui::SliderFloat("##bstr", &s_brush_strength, 0.5f, 60.f, "S=%.1fm/s");
            ImGui::Separator();
            if (ImGui::Button("Save master hmap (F5)", {-1, 0}))
                TerrainMaster_Save(MASTER_PATH);
            ImGui::TextDisabled("LMB=paint  RMB=look");
        }
        ImGui::End();

        // F5 shortcut
        if (ImGui::IsKeyPressed(ImGuiKey_F5))
            TerrainMaster_Save(MASTER_PATH);
    }

    // HUD — top-left
    ImGui::SetCursorScreenPos({origin.x+8, origin.y+8});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,255,200,220));
    int cur_zx = (int)(s_cx / CHUNK_SIZE);
    int cur_zy = (int)(s_cz / CHUNK_SIZE);
    int built  = s_chunks_built.load();
    int total  = EDITOR_TNKN * EDITOR_TNKN;
    if (!s_loaded)
        ImGui::Text("Loading %d/%d chunks...", built, total);
    else
        ImGui::Text("Zone: %d,%d  Alt: %.0fm  LOD: %s",
            cur_zx, cur_zy, s_cy,
            s_cy > 4000.f ? "8x8" : s_cy > 1500.f ? "16x16" : s_cy > 500.f ? "32x32" : "64x64");
    ImGui::Text("RMB=look  WASD=move  Q/E=alt  Scroll=zoom  F5=save  T=centre");
    ImGui::PopStyleColor();

}

} // namespace WorldEditor3D_SDLGPU

#endif // MD_SDL_GPU
