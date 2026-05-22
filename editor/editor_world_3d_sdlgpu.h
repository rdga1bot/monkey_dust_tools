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
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/world/terrain_chunk.h>
#include <monkey_dust/world/chunk_def.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include <atomic>
#include <thread>
#include <cstring>
#include <cstdio>
#include <cmath>

namespace WorldEditor3D_SDLGPU {

// ── State ──────────────────────────────────────────────────────────────────────
static constexpr int   EDITOR_TNKN = 7;   // 7×7 near chunks = 3500×3500m (full res)
static constexpr float WORLD_HALF  = (EDITOR_TNKN * CHUNK_SIZE) * 0.5f;

static TerrainRenderer    s_terrain;
static PropRenderer       s_props;
static TerrainChunk       s_chunks[EDITOR_TNKN][EDITOR_TNKN];
static bool               s_loaded = false;

// Async loading state
static std::atomic<bool>  s_atlas_ready{false};   // atlas + terrain renderer ready
static std::atomic<int>   s_chunks_built{0};       // near chunks built so far (main thread)
static std::thread        s_loader_thread;
static int                s_zone_ox_saved = 28, s_zone_oz_saved = 28;
static bool               s_rebuild_pending = false;

// Per-zone amplitude from terrain_config.txt (indexed [gz][gx], default 40)
static float s_zone_amp[64][64];

// ── World overview — full 64×64 map at 8×8 quads/chunk (built once) ───────────
static constexpr int OV_GRID    = 8;
static constexpr int OV_VERTS   = (OV_GRID+1)*(OV_GRID+1);   // 81 per chunk
static constexpr int OV_IDX     = OV_GRID*OV_GRID*6;          // 384 per chunk
static constexpr int OV_ZONES   = 64;
static constexpr int OV_TOTAL_V = OV_ZONES*OV_ZONES*OV_VERTS; // 331,776

struct OvVtx { float x,y,z, nx,ny,nz, height_t; };  // 28 bytes

static GpuStaticBuffer     s_ov_vbo;
static GpuStaticBuffer     s_ov_ibo;
static GpuPipeline         s_ov_pipeline;
static std::atomic<bool>   s_ov_data_ready{false};
static bool                s_ov_gpu_ready  = false;

// Staging in BSS (~9.3 MB + 768 B; OS lazy-maps pages)
static OvVtx    s_ov_stage[OV_TOTAL_V];
static uint16_t s_ov_ibo_data[OV_IDX];

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

// ── Overview: CPU mesh build (background thread, after atlas loaded) ───────────
static void s_build_overview_cpu() {
    // Shared IBO: indices 0..80, reused per chunk via vertex_offset in draw call
    {
        int ii = 0;
        for (int r = 0; r < OV_GRID; ++r)
            for (int c = 0; c < OV_GRID; ++c) {
                int v = r*(OV_GRID+1)+c;
                s_ov_ibo_data[ii++]=(uint16_t)v;
                s_ov_ibo_data[ii++]=(uint16_t)(v+OV_GRID+1);
                s_ov_ibo_data[ii++]=(uint16_t)(v+1);
                s_ov_ibo_data[ii++]=(uint16_t)(v+1);
                s_ov_ibo_data[ii++]=(uint16_t)(v+OV_GRID+1);
                s_ov_ibo_data[ii++]=(uint16_t)(v+OV_GRID+2);
            }
    }

    const float step     = CHUNK_SIZE / (float)OV_GRID;  // 62.5 m/quad
    const float H_RANGE  = 300.f;   // actual Kenshi world max (confirmed by cross-ref)

    // Atlas stores raw world-space Y metres — no amplitude multiply needed.
    auto get_h = [](int zx, int zz, int col, int row) -> float {
        zx  = zx < 0 ? 0 : (zx > 63 ? 63 : zx);
        zz  = zz < 0 ? 0 : (zz > 63 ? 63 : zz);
        col = col < 0 ? 0 : (col > OV_GRID ? OV_GRID : col);
        row = row < 0 ? 0 : (row > OV_GRID ? OV_GRID : row);
        return TerrainAtlas_GetHeight(zx, zz, col*8, row*8);
    };

    for (int zz = 0; zz < OV_ZONES; ++zz) {
        for (int zx = 0; zx < OV_ZONES; ++zx) {
            int   base = (zz*OV_ZONES + zx)*OV_VERTS;
            float ox   = (float)zx * CHUNK_SIZE;
            float oz   = (float)zz * CHUNK_SIZE;
            for (int r = 0; r <= OV_GRID; ++r) {
                for (int c = 0; c <= OV_GRID; ++c) {
                    float wy   = get_h(zx, zz, c, r);
                    float wx   = ox + c*step;
                    float wz   = oz + r*step;
                    float dhdx = (get_h(zx,zz,c+1,r) - get_h(zx,zz,c-1,r)) / (2.f*step);
                    float dhdz = (get_h(zx,zz,c,r+1) - get_h(zx,zz,c,r-1)) / (2.f*step);
                    float len  = sqrtf(dhdx*dhdx + 1.f + dhdz*dhdz);
                    OvVtx& v   = s_ov_stage[base + r*(OV_GRID+1)+c];
                    v.x=wx; v.y=wy; v.z=wz;
                    v.nx=-dhdx/len; v.ny=1.f/len; v.nz=-dhdz/len;
                    v.height_t = wy/H_RANGE;
                }
            }
        }
    }
    fprintf(stdout, "[W3D] overview CPU mesh built (%d verts)\n", OV_TOTAL_V);
}

// ── Overview: GPU upload + pipeline (main thread) ─────────────────────────────
static void s_init_overview_gpu() {
    // 0x8892 = GL_ARRAY_BUFFER, 0x8893 = GL_ELEMENT_ARRAY_BUFFER (avoid GL header)
    s_ov_vbo.Init(0x8892u, s_ov_stage,    sizeof(s_ov_stage));
    s_ov_ibo.Init(0x8893u, s_ov_ibo_data, sizeof(s_ov_ibo_data));

    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/terrain_overview.vert";
    pd.frag_path = "shaders/terrain_overview.frag";
    pd.layout.count  = 3;
    pd.layout.stride = sizeof(OvVtx);
    pd.layout.attribs[0] = { 0,  0, GpuAttribFmt::F3 };  // pos
    pd.layout.attribs[1] = { 1, 12, GpuAttribFmt::F3 };  // norm
    pd.layout.attribs[2] = { 2, 24, GpuAttribFmt::F1 };  // height_t
    pd.vert_uniform_bufs = 1;
    pd.frag_samplers     = 0;
    pd.frag_uniform_bufs = 0;
    pd.color_format      = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    pd.has_depth_target  = true;
    s_ov_pipeline.Create(pd);
    s_ov_gpu_ready = true;
    fprintf(stdout, "[W3D] overview GPU pipeline ready\n");
}

// ── Overview: draw world — skip_near=true leaves the EDITOR_TNKN×EDITOR_TNKN hole
//              for high-res near terrain to fill (low-altitude mode).
static void s_draw_overview(SDL_GPURenderPass* rp, SDL_GPUCommandBuffer* cmd,
                             const float* vp_mat, bool skip_near) {
    if (!s_ov_gpu_ready || !s_ov_pipeline.SDLPipeline()) return;
    SDL_BindGPUGraphicsPipeline(rp, s_ov_pipeline.SDLPipeline());
    SDL_PushGPUVertexUniformData(cmd, 0, vp_mat, 64);
    SDL_GPUBufferBinding vb = { s_ov_vbo.SDLBuffer(), 0 };
    SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
    SDL_GPUBufferBinding ib = { s_ov_ibo.SDLBuffer(), 0 };
    SDL_BindGPUIndexBuffer(rp, &ib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
    for (int zz = 0; zz < OV_ZONES; ++zz)
        for (int zx = 0; zx < OV_ZONES; ++zx) {
            if (skip_near &&
                zx >= s_zone_ox_saved && zx < s_zone_ox_saved + EDITOR_TNKN &&
                zz >= s_zone_oz_saved && zz < s_zone_oz_saved + EDITOR_TNKN)
                continue;
            SDL_DrawGPUIndexedPrimitives(rp, OV_IDX, 1, 0,
                                         (zz*OV_ZONES + zx)*OV_VERTS, 0);
        }
}

// RTT
static SDL_GPUTexture* s_color = nullptr;
static SDL_GPUTexture* s_depth = nullptr;
static int             s_rtt_w = 0, s_rtt_h = 0;
static int             s_last_w = 1280, s_last_h = 720;  // use prev frame size

// Camera (world-space meters, Y up)
static float s_cx = 16000.f, s_cy = 3500.f, s_cz = 11000.f; // overview: see full Kenshi world
static float s_yaw = 0.f, s_pitch = 0.42f;  // ~24° down
static float s_speed = 800.f;
static bool  s_rmb     = false;
static bool  s_focused = false;  // viewport has keyboard focus (set on click, cleared on outside click)

// ── Terrain brush ──────────────────────────────────────────────────────────────
enum class BrushMode { Raise=0, Lower, Smooth, Flatten };
static BrushMode s_brush_mode     = BrushMode::Raise;
static float     s_brush_radius   = 150.f;    // metres
static float     s_brush_strength = 8.f;      // m/s
static bool      s_brush_hit      = false;
static float     s_brush_wx = 0.f, s_brush_wy = 0.f, s_brush_wz = 0.f;
static float     s_last_vp[16]    = {};        // VP matrix from last RenderFrame
static bool      s_chunk_dirty[EDITOR_TNKN][EDITOR_TNKN] = {};
static constexpr const char* EDITS_PATH = "game/data/terrain/world_hmap.r32";

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
        int zx = (int)(wx / CHUNK_SIZE), zy = (int)(wz / CHUNK_SIZE);
        if (zx < 0||zx > 63||zy < 0||zy > 63) return 0.f;
        float col_f = (wx - zx * CHUNK_SIZE) / CHUNK_SIZE * 64.f;
        float row_f = (wz - zy * CHUNK_SIZE) / CHUNK_SIZE * 64.f;
        return TerrainAtlas_GetHeight(zx, zy, (int)col_f, (int)row_f);
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

// ── Apply brush to terrain heights ─────────────────────────────────────────────
static void s_apply_brush(float dt) {
    if (!TerrainAtlas_Loaded()) return;
    float R2 = s_brush_radius * s_brush_radius;
    float str = s_brush_strength * dt;

    int zx0 = (int)((s_brush_wx - s_brush_radius) / CHUNK_SIZE);
    int zx1 = (int)((s_brush_wx + s_brush_radius) / CHUNK_SIZE);
    int zy0 = (int)((s_brush_wz - s_brush_radius) / CHUNK_SIZE);
    int zy1 = (int)((s_brush_wz + s_brush_radius) / CHUNK_SIZE);
    if (zx0 < 0) zx0 = 0;  if (zx1 > 63) zx1 = 63;
    if (zy0 < 0) zy0 = 0;  if (zy1 > 63) zy1 = 63;

    float cell = CHUNK_SIZE / 64.f;
    for (int zy = zy0; zy <= zy1; ++zy) {
        for (int zx = zx0; zx <= zx1; ++zx) {
            bool touched = false;
            for (int row = 0; row <= 64; ++row) {
                for (int col = 0; col <= 64; ++col) {
                    float wx = zx * CHUNK_SIZE + col * cell;
                    float wz = zy * CHUNK_SIZE + row * cell;
                    float dx = wx - s_brush_wx, dz = wz - s_brush_wz;
                    float d2 = dx*dx + dz*dz;
                    if (d2 > R2) continue;
                    float t = 1.f - sqrtf(d2) / s_brush_radius;
                    float falloff = t * t;  // smooth at edges
                    float h = TerrainAtlas_GetHeight(zx, zy, col, row);
                    float nh = h;
                    switch (s_brush_mode) {
                        case BrushMode::Raise:   nh = h + str * falloff; break;
                        case BrushMode::Lower:   nh = h - str * falloff; break;
                        case BrushMode::Flatten: nh = h + (s_brush_wy - h) * falloff * fminf(str * 0.2f, 1.f); break;
                        case BrushMode::Smooth: {
                            float avg = 0.f;
                            for (int dc = -2; dc <= 2; ++dc)
                                for (int dr = -2; dr <= 2; ++dr)
                                    avg += TerrainAtlas_GetHeight(zx, zy, col+dc, row+dr);
                            avg /= 25.f;
                            nh = h + (avg - h) * falloff * fminf(str * 0.05f, 1.f);
                            break;
                        }
                    }
                    TerrainAtlas_SetHeight(zx, zy, col, row, nh);
                    touched = true;
                }
            }
            if (touched) {
                // Mark matching near-terrain chunk as dirty for rebuild next frame
                for (int dz = 0; dz < EDITOR_TNKN; ++dz)
                    for (int dx = 0; dx < EDITOR_TNKN; ++dx)
                        if (s_zone_ox_saved + dx == zx && s_zone_oz_saved + dz == zy)
                            s_chunk_dirty[dz][dx] = true;
            }
        }
    }
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

    ci.format = SDL_GPU_TEXTUREFORMAT_D24_UNORM;
    ci.usage   = SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    s_depth = SDL_CreateGPUTexture(dev, &ci);

    fprintf(stdout, "[W3D-SDLGPU] RTT %dx%d\n", w, h);
}

// ── Init — launches async background loader, returns immediately ───────────────
static bool Init(const char* hmap_path, const char* overlay_path,
                 int zone_ox = 28, int zone_oz = 28) {
    s_zone_ox_saved = zone_ox;
    s_zone_oz_saved = zone_oz;
    // Camera above the 7×7 zone grid centre in Kenshi atlas coords
    s_cx = (zone_ox + EDITOR_TNKN * 0.5f) * CHUNK_SIZE;
    s_cy = 1200.f;
    s_cz = (zone_oz + EDITOR_TNKN * 0.5f) * CHUNK_SIZE;

    s_load_zone_amplitudes("game/data/terrain_config.txt");

    // Phase 1 (background): load atlas + init TerrainRenderer (slow I/O)
    // Phase 2 (main thread, per-frame): TerrainGen_Build + Upload (needs GPU)
    const char* hp = hmap_path;
    const char* op = overlay_path;
    s_loader_thread = std::thread([hp, op]() {
        if (!TerrainAtlas_Load(hp)) {
            fprintf(stderr, "[W3D-SDLGPU] atlas load failed\n"); return;
        }
        TerrainAtlas_LoadEdits(EDITS_PATH);  // optional — silently ignored if absent
        TerrainAtlas_SmoothBoundaries();     // same fix as game: eliminate zone-seam NdotL artifacts
        if (!s_terrain.Init()) {
            fprintf(stderr, "[W3D-SDLGPU] TerrainRenderer init failed\n"); return;
        }
        s_props.Init("game/data/props/rocks/rock_01.glb"); // no-op if missing
        s_terrain.InitKenshiOverlay(op);
        s_terrain.InitPOM("game/data/terrain/pom_detail.png");
        s_atlas_ready = true;
        s_build_overview_cpu();
        s_ov_data_ready = true;
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

// R: rebuild same zone area (no grid shift) — use after sculpting to refresh heights
static void rebuild_inplace() {
    s_begin_rebuild();
}

// T: travel here — recenter grid on camera zone, then rebuild
static void travel_to_camera() {
    int zx = (int)(s_cx / CHUNK_SIZE);
    int zy = (int)(s_cz / CHUNK_SIZE);
    zx = zx < EDITOR_TNKN/2 ? EDITOR_TNKN/2 : (zx > 63-EDITOR_TNKN/2 ? 63-EDITOR_TNKN/2 : zx);
    zy = zy < EDITOR_TNKN/2 ? EDITOR_TNKN/2 : (zy > 63-EDITOR_TNKN/2 ? 63-EDITOR_TNKN/2 : zy);
    s_zone_ox_saved = zx - EDITOR_TNKN / 2;
    s_zone_oz_saved = zy - EDITOR_TNKN / 2;
    s_begin_rebuild();
}

// ── Tick: build one chunk per call; auto-reload when camera leaves grid ────────
static void tick_chunk_build() {
    if (s_ov_data_ready.load() && !s_ov_gpu_ready) s_init_overview_gpu();
    if (s_atlas_ready && s_loaded) return;
    if (!s_atlas_ready || s_loaded) return;
    int idx = s_chunks_built.load();
    if (idx >= EDITOR_TNKN * EDITOR_TNKN) { s_loaded = true; return; }

    int cz = idx / EDITOR_TNKN, cx = idx % EDITOR_TNKN;
    ChunkCoord coord = { s_zone_ox_saved + cx, s_zone_oz_saved + cz };
    TerrainGenParams p;
    p.zone_origin_x = 0;  // coord already has absolute atlas coords; 0+coord.x = zx
    p.zone_origin_z = 0;
    int zx = coord.x, zz = coord.z;
    p.amplitude = (zx >= 0 && zx < 64 && zz >= 0 && zz < 64) ? s_zone_amp[zz][zx] : 40.f;
    TerrainGen_Build(s_chunks[cz][cx], coord, p);
    TerrainGen_Upload(s_chunks[cz][cx]);
    s_chunks[cz][cx].center_x = (float)(s_zone_ox_saved + cx) * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
    s_chunks[cz][cx].center_z = (float)(s_zone_oz_saved + cz) * CHUNK_SIZE + CHUNK_SIZE * 0.5f;

    ++s_chunks_built;
    if (s_chunks_built >= EDITOR_TNKN * EDITOR_TNKN) {
        s_loaded = true;
        fprintf(stdout, "[W3D-SDLGPU] %dx%d chunks ready\n", EDITOR_TNKN, EDITOR_TNKN);
    }
}

// ── Camera input (called inside DrawImGui while item is hovered) ───────────────
static void handle_input(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    // RMB look — relative mouse mode hides cursor and gives unlimited delta
    if (io.MouseClicked[1] && !s_rmb) {
        s_rmb = true;
        SDL_SetWindowRelativeMouseMode(SDL_GetMouseFocus(), true);
        float _dx, _dy;
        SDL_GetRelativeMouseState(&_dx, &_dy);  // drain stale delta accumulated before click
    }
    if (s_rmb) {
        if (io.MouseDown[1]) {
            float dx, dy;
            SDL_GetRelativeMouseState(&dx, &dy);
            s_yaw   -= dx * 0.0018f;
            s_pitch += dy * 0.0014f;
            if (s_pitch < -0.3f) s_pitch = -0.3f;
            if (s_pitch >  1.3f) s_pitch =  1.3f;
        } else {
            s_rmb = false;
            SDL_SetWindowRelativeMouseMode(SDL_GetMouseFocus(), false);
        }
    }
    // WASD
    float sp = s_speed * dt;
    float sy = sinf(s_yaw), cy = cosf(s_yaw);
    if (ImGui::IsKeyDown(ImGuiKey_W)||ImGui::IsKeyDown(ImGuiKey_UpArrow))   { s_cx+=sp*sy; s_cz+=sp*cy; }
    if (ImGui::IsKeyDown(ImGuiKey_S)||ImGui::IsKeyDown(ImGuiKey_DownArrow)) { s_cx-=sp*sy; s_cz-=sp*cy; }
    if (ImGui::IsKeyDown(ImGuiKey_A))  { s_cx+=sp*cy; s_cz-=sp*sy; }
    if (ImGui::IsKeyDown(ImGuiKey_D))  { s_cx-=sp*cy; s_cz+=sp*sy; }
    if (ImGui::IsKeyDown(ImGuiKey_Q)||ImGui::IsKeyDown(ImGuiKey_PageDown)) s_cy-=sp;
    if (ImGui::IsKeyDown(ImGuiKey_E)||ImGui::IsKeyDown(ImGuiKey_PageUp))   s_cy+=sp;
    if (ImGui::IsKeyPressed(ImGuiKey_R))
        rebuild_inplace();
    if (ImGui::IsKeyPressed(ImGuiKey_T))
        travel_to_camera();
    // Scroll: zoom proportional to altitude
    if (io.MouseWheel != 0.f) {
        float step = s_cy * 0.05f * io.MouseWheel;
        s_cx += step * sy; s_cz += step * cy;
        if (io.MouseWheel > 0) s_cy = fmaxf(s_cy * 0.96f, 10.f);
        else                   s_cy = fminf(s_cy * 1.04f, 20000.f);
    }
    if (io.KeyCtrl && io.MouseWheel != 0.f) {
        s_speed *= powf(1.3f, io.MouseWheel);
        if (s_speed < 10.f)    s_speed = 10.f;
        if (s_speed > 20000.f) s_speed = 20000.f;
    }
    static constexpr float ATLAS_MAX = 63.f * CHUNK_SIZE;
    if (s_cx < 0.f) s_cx = 0.f; if (s_cx > ATLAS_MAX) s_cx = ATLAS_MAX;
    if (s_cz < 0.f) s_cz = 0.f; if (s_cz > ATLAS_MAX) s_cz = ATLAS_MAX;
    if (s_cy < 5.f) s_cy = 5.f;
}

// ── Render terrain to RTT (call AFTER ImGui build, BEFORE ImGui present) ────────
// ensure_rtt() is called from DrawImGui (during ImGui build) so s_color is stable.
static void RenderFrame(SDL_GPUCommandBuffer* cmd, float dt) {
    tick_chunk_build(); tick_chunk_build();
    bool overview_mode = (s_cy >= 2000.f);
    bool ready = overview_mode ? s_ov_gpu_ready : s_loaded;
    if (!ready || !s_color) return;
    int w = s_rtt_w, h = s_rtt_h;  // use already-created RTT dimensions
    if (w < 8 || h < 8) return;

    float asp = (float)w / (float)h;
    M4 proj = m4_persp(0.80f, asp, 5.f, 25000.f);
    M4 view = m4_view(s_cx, s_cy, s_cz, s_yaw, s_pitch);
    M4 vp   = m4_mul(proj, view);
    memcpy(s_last_vp, vp.m, 64);  // expose for brush cursor projection

    // Rebuild dirty chunks (marked by s_apply_brush)
    if (s_loaded) {
        for (int cz = 0; cz < EDITOR_TNKN; ++cz) for (int cx = 0; cx < EDITOR_TNKN; ++cx) {
            if (!s_chunk_dirty[cz][cx]) continue;
            int chunk_zx = s_zone_ox_saved + cx, chunk_zz = s_zone_oz_saved + cz;
            ChunkCoord coord = { chunk_zx, chunk_zz };
            TerrainGenParams p; p.zone_origin_x = 0; p.zone_origin_z = 0;
            p.amplitude = (chunk_zx>=0&&chunk_zx<64&&chunk_zz>=0&&chunk_zz<64)
                ? s_zone_amp[chunk_zz][chunk_zx] : 40.f;
            TerrainGen_Build(s_chunks[cz][cx], coord, p);
            TerrainGen_Upload(s_chunks[cz][cx]);
            s_chunk_dirty[cz][cx] = false;
        }
    }

    // ── Terrain render pass ──────────────────────────────────────────────────
    SDL_GPUColorTargetInfo ct = {};
    ct.texture     = s_color;
    ct.load_op     = SDL_GPU_LOADOP_CLEAR;
    ct.store_op    = SDL_GPU_STOREOP_STORE;
    ct.clear_color = { 0.42f, 0.52f, 0.62f, 1.f };  // sky

    SDL_GPUDepthStencilTargetInfo di = {};
    di.texture          = s_depth;
    di.clear_depth      = 1.f;
    di.load_op          = SDL_GPU_LOADOP_CLEAR;
    di.store_op         = SDL_GPU_STOREOP_STORE;
    di.stencil_load_op  = SDL_GPU_LOADOP_DONT_CARE;
    di.stencil_store_op = SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* rp = SDL_BeginGPURenderPass(cmd, &ct, 1, &di);
    if (rp) {
        if (overview_mode) {
            // High altitude: full 64×64 overview, no near-terrain skip
            s_draw_overview(rp, cmd, vp.m, false);
        } else if (s_loaded) {
            // Detail mode: ONLY near hi-res terrain — no ugly overview ring
            auto sun = TerrainRenderer::SunParams::Default();
            for (int cz = 0; cz < EDITOR_TNKN; ++cz)
                for (int cx = 0; cx < EDITOR_TNKN; ++cx)
                    s_terrain.DrawRawPOM(rp, cmd, s_chunks[cz][cx],
                                         vp.m, sun,
                                         s_cx, s_cy, s_cz,
                                         6000.f, 2000.f, 1.f / 8000.f);
            // GitHub #4: draw rock props over terrain if asset is loaded
            if (s_props.IsReady()) {
                float pos[3] = { s_cx, 0.f, s_cz };
                const float* sun32 = &sun.dir[0]; // SunParams is 32B contiguous
                s_props.DrawRaw(rp, cmd, pos, 1, vp.m, sun32);
            }
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

    bool overview_mode_ui = (s_cy >= 2000.f);
    bool ui_ready = overview_mode_ui ? s_ov_gpu_ready : s_loaded;
    if (!ui_ready) {
        // Progress display
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({W, H});
        ImGui::GetWindowDrawList()->AddRectFilled(p, {p.x+W, p.y+H}, IM_COL32(15,15,20,255));
        int built = s_chunks_built.load();
        int total = EDITOR_TNKN * EDITOR_TNKN;
        float pct  = !s_atlas_ready ? 0.f
                   : overview_mode_ui ? (s_ov_data_ready.load() ? 0.95f : 0.5f)
                   : (float)built / total;
        const char* msg = !s_atlas_ready ? "Loading heightmap (67 MB)..."
                        : overview_mode_ui ? "Building world overview..."
                        : "Building terrain chunks...";
        ImVec2 tc = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(
            {p.x + W*0.5f - tc.x*0.5f, p.y + H*0.5f - 20},
            IM_COL32(200,200,200,255), msg);
        // Progress bar
        float bw = W * 0.5f, bx = p.x + W*0.25f, by = p.y + H*0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled({bx,by}, {bx+bw,by+16}, IM_COL32(40,40,60,255), 4.f);
        ImGui::GetWindowDrawList()->AddRectFilled({bx,by}, {bx+bw*pct,by+16}, IM_COL32(80,140,220,255), 4.f);
        char pbuf[32]; snprintf(pbuf,sizeof(pbuf),"%d / %d",built,total);
        ImGui::GetWindowDrawList()->AddText({bx+bw*0.5f-20,by+1}, IM_COL32(255,255,255,180), pbuf);
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
    if (hov || s_rmb || s_focused) handle_input(dt);

    // ── Mouse → terrain ray cast (brush targeting) ───────────────────────────
    bool edit_mode = (s_cy < 2000.f) && s_loaded;
    if (hov && edit_mode && !s_rmb) {
        ImVec2 mouse = ImGui::GetMousePos();
        float ndc_x = ((mouse.x - origin.x) / W) * 2.f - 1.f;
        float ndc_y = 1.f - ((mouse.y - origin.y) / H) * 2.f;
        float thf = tanf(0.80f * 0.5f), asp = W / H;
        float sy = sinf(s_yaw), cy_c = cosf(s_yaw);
        float sp = sinf(s_pitch), cp = cosf(s_pitch);
        float fx = sy*cp, fy = -sp, fz = cy_c*cp;
        float rx = cy_c, rz = -sy;
        float ux = sp*sy, uy = cp, uz = sp*cy_c;
        float rdx = fx + ndc_x*thf*asp*rx + ndc_y*thf*ux;
        float rdy = fy +                    ndc_y*thf*uy;
        float rdz = fz + ndc_x*thf*asp*rz + ndc_y*thf*uz;
        s_brush_hit = s_ray_terrain(s_cx, s_cy, s_cz, rdx, rdy, rdz,
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
            if (ImGui::Button("Save edits (F5)", {-1, 0}))
                TerrainAtlas_SaveEdits(EDITS_PATH);
            ImGui::TextDisabled("LMB=paint  RMB=look");
        }
        ImGui::End();

        // F5 shortcut
        if (ImGui::IsKeyPressed(ImGuiKey_F5))
            TerrainAtlas_SaveEdits(EDITS_PATH);
    }

    // HUD — top-left
    ImGui::SetCursorScreenPos({origin.x+8, origin.y+8});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,255,200,220));
    int cur_zx = (int)(s_cx / CHUNK_SIZE);
    int cur_zy = (int)(s_cz / CHUNK_SIZE);
    bool hud_overview = (s_cy >= 2000.f);
    ImGui::Text("Zone: %d,%d  Alt: %.0fm  Speed: %.0fm/s  |  %s",
                cur_zx, cur_zy, s_cy, s_speed,
                hud_overview ? "OVERVIEW (scroll down for detail)"
                             : "DETAIL — near 7x7 zones hi-res");
    ImGui::Text("RMB=look  WASD=move  Q/E=alt  Scroll=zoom  R=rebuild  T=fly  F5=save");
    ImGui::PopStyleColor();

}

} // namespace WorldEditor3D_SDLGPU

#endif // MD_SDL_GPU
