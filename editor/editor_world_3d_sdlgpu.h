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

// Prop scatter state — rebuilt once per zone change when atlas is ready
static constexpr int  PROPS_PER_CHUNK = 8;   // rocks per 500×500m chunk
static float          s_prop_pos[PropRenderer::MAX_PROPS * 3] = {};
static int            s_prop_count = 0;
static bool           s_props_built = false;

// Async loading state
static std::atomic<bool>  s_master_ready{false};  // terrain renderer ready (master hmap loaded sync)
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
static bool                s_ov_rebuild_needed = false;

// Staging in BSS (~9.3 MB + 768 B; OS lazy-maps pages)
static OvVtx    s_ov_stage[OV_TOTAL_V];
static uint16_t s_ov_ibo_data[OV_IDX];

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

    const float step    = CHUNK_SIZE / (float)OV_GRID;  // 62.5 m/quad
    const float H_RANGE = TerrainMaster_HMax() > 1.f ? TerrainMaster_HMax() : 100.f;

    auto get_h = [](float wx, float wz) -> float {
        return TerrainMaster_SampleWorld(wx, wz);
    };

    for (int zz = 0; zz < OV_ZONES; ++zz) {
        for (int zx = 0; zx < OV_ZONES; ++zx) {
            int   base = (zz*OV_ZONES + zx)*OV_VERTS;
            float ox   = (float)zx * CHUNK_SIZE;
            float oz   = (float)zz * CHUNK_SIZE;
            for (int r = 0; r <= OV_GRID; ++r) {
                for (int c = 0; c <= OV_GRID; ++c) {
                    float wx   = ox + c*step;
                    float wz   = oz + r*step;
                    float wy   = get_h(wx, wz);
                    float dhdx = (get_h(wx+step, wz) - get_h(wx-step, wz)) / (2.f*step);
                    float dhdz = (get_h(wx, wz+step) - get_h(wx, wz-step)) / (2.f*step);
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
static float s_cx = 16000.f, s_cy = 400.f, s_cz = 11000.f;
static float s_yaw = 0.f, s_pitch = 0.38f;  // ~22° down — game-like angle
static float s_speed = 200.f;
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
        s_ov_rebuild_needed = true;
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
    s_ov_rebuild_needed = true;
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

// ── Init — loads master hmap synchronously (257 KiB), background: renderer + overview ──
static bool Init(const char* overlay_path, int zone_ox = 28, int zone_oz = 28) {
    s_zone_ox_saved = zone_ox;
    s_zone_oz_saved = zone_oz;
    s_cx = 32.f * CHUNK_SIZE;  // center of world
    s_cy = 400.f;              // ~game altitude: terrain detail visible
    s_pitch = 0.38f;           // ~22° down — similar to game camera angle
    s_cz = 32.f * CHUNK_SIZE;

    s_load_zone_amplitudes("game/data/terrain_config.txt");

    // Master hmap is 257 KiB — load synchronously before spinning the thread
    if (!TerrainMaster_Load(MASTER_PATH, 64.f * CHUNK_SIZE, 64.f * CHUNK_SIZE))
        fprintf(stderr, "[W3D-SDLGPU] master hmap load failed: %s\n", MASTER_PATH);

    // Background: init TerrainRenderer (GPU shader compilation) + build overview mesh
    const char* op = overlay_path;
    s_loader_thread = std::thread([op]() {
        if (!s_terrain.Init()) {
            fprintf(stderr, "[W3D-SDLGPU] TerrainRenderer init failed\n"); return;
        }
        s_props.Init("game/data/props/rocks/rock_01.glb"); // no-op if missing
        s_terrain.InitKenshiOverlay(op);
        s_terrain.InitPOM("game/data/terrain/pom_detail.png");
        s_master_ready = true;
        s_build_prop_positions();
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
    if (s_master_ready && s_loaded) return;
    if (!s_master_ready || s_loaded) return;
    int idx = s_chunks_built.load();
    if (idx >= EDITOR_TNKN * EDITOR_TNKN) { s_loaded = true; return; }

    int cz = idx / EDITOR_TNKN, cx = idx % EDITOR_TNKN;
    ChunkCoord coord = { s_zone_ox_saved + cx, s_zone_oz_saved + cz };
    TerrainGenParams p;
    p.zone_origin_x = 0;
    p.zone_origin_z = 0;
    // amplitude=0 when master loaded: h = master_h + noise*0 → clean master hmap, no noise
    p.amplitude = TerrainMaster_Loaded() ? 0.f :
        ((coord.x >= 0 && coord.x < 64 && coord.z >= 0 && coord.z < 64)
            ? s_zone_amp[coord.z][coord.x] : 40.f);
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
    // WASD — speed scales with altitude so pan feel is consistent at any zoom
    float sp = s_cy * dt;
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
        if (io.MouseWheel > 0) s_cy = fmaxf(s_cy * 0.92f, 10.f);
        else                   s_cy = fminf(s_cy * 1.08f, 150000.f);
    }
    if (io.KeyCtrl && io.MouseWheel != 0.f) {
        s_speed *= powf(1.3f, io.MouseWheel);
        if (s_speed < 10.f)    s_speed = 10.f;
        if (s_speed > 20000.f) s_speed = 20000.f;
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
    bool ready = s_ov_gpu_ready;
    if (!ready || !s_color) return;
    // Rebuild overview VBO on main thread after brush stroke
    if (s_ov_rebuild_needed) {
        s_build_overview_cpu();
        s_ov_vbo.Shutdown();
        s_ov_vbo.Init(0x8892u, s_ov_stage, sizeof(s_ov_stage));
        s_ov_rebuild_needed = false;
    }
    int w = s_rtt_w, h = s_rtt_h;  // use already-created RTT dimensions
    if (w < 8 || h < 8) return;

    float asp = (float)w / (float)h;
    M4 proj = m4_persp(0.80f, asp, 5.f, 350000.f);
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
            p.amplitude = TerrainMaster_Loaded() ? 0.f :
                ((chunk_zx>=0&&chunk_zx<64&&chunk_zz>=0&&chunk_zz<64)
                    ? s_zone_amp[chunk_zz][chunk_zx] : 40.f);
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
        s_draw_overview(rp, cmd, vp.m, false);
        SDL_EndGPURenderPass(rp);
    }

    (void)dt;
}

// ── ImGui draw (call inside "3D World" tab) ────────────────────────────────────
static void DrawImGui(float W, float H, float dt) {
    s_last_w = (int)W > 4 ? (int)W : s_last_w;
    s_last_h = (int)H > 4 ? (int)H : s_last_h;

    // (chunk building handled in RenderFrame, called every frame)

    if (!s_ov_gpu_ready) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy({W, H});
        ImGui::GetWindowDrawList()->AddRectFilled(p, {p.x+W, p.y+H}, IM_COL32(15,15,20,255));
        float pct  = !s_master_ready ? 0.f : s_ov_data_ready.load() ? 0.9f : 0.5f;
        const char* msg = !s_master_ready ? "Initialising terrain renderer..." : "Building world overview...";
        ImVec2 tc = ImGui::CalcTextSize(msg);
        ImGui::GetWindowDrawList()->AddText(
            {p.x + W*0.5f - tc.x*0.5f, p.y + H*0.5f - 20},
            IM_COL32(200,200,200,255), msg);
        float bw = W * 0.5f, bx = p.x + W*0.25f, by = p.y + H*0.5f;
        ImGui::GetWindowDrawList()->AddRectFilled({bx,by}, {bx+bw,by+16}, IM_COL32(40,40,60,255), 4.f);
        ImGui::GetWindowDrawList()->AddRectFilled({bx,by}, {bx+bw*pct,by+16}, IM_COL32(80,140,220,255), 4.f);
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
    bool edit_mode = s_ov_gpu_ready;
    if (hov && edit_mode && !s_rmb) {
        ImVec2 mouse = ImGui::GetMousePos();
        float ndc_x = ((mouse.x - origin.x) / W) * 2.f - 1.f;
        float ndc_y = 1.f - ((mouse.y - origin.y) / H) * 2.f;
        float thf = tanf(0.80f * 0.5f), asp = W / H;
        float sy = sinf(s_yaw), cy_c = cosf(s_yaw);
        float sp = sinf(s_pitch), cp = cosf(s_pitch);
        float fx = sy*cp, fy = -sp, fz = cy_c*cp;
        float rx = -cy_c, rz = sy;   // matches m4_view right vector
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
    ImGui::Text("Zone: %d,%d  Alt: %.0fm  Speed: %.0fm/s", cur_zx, cur_zy, s_cy, s_speed);
    ImGui::Text("RMB=look  WASD=move  Q/E=alt  Scroll=zoom  F5=save");
    ImGui::PopStyleColor();

}

} // namespace WorldEditor3D_SDLGPU

#endif // MD_SDL_GPU
