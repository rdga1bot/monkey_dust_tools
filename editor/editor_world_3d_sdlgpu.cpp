#include "editor_world_3d_sdlgpu.h"

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
#include <monkey_dust/world/biome_def.h>
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

// Compact VBOs for LOD2/LOD3: covers ALL 4096 chunks (slot = cz*64+cx).
// Built once after s_loaded — stable, no per-frame rebuild.
// li=0 → LOD2 (17×17=289 verts/chunk ~61MB), li=1 → LOD3 (9×9=81 verts/chunk ~17MB).
static GpuStaticBuffer  s_cvbo[2];
static GpuStaticBuffer  s_cvbo_ibo[2];
static bool             s_cvbo_built[2]  = {false, false};
static bool             s_cvbo_dirty     = false;  // set after brush edits
static float            s_cvbo_dirty_t   = 0.f;    // time since last dirty mark

static const int CVBO_STEPS[2]  = { TERRAIN_LOD_STEPS[1], TERRAIN_LOD_STEPS[2] }; // {4, 8}
static const int CVBO_ROWS[2]   = { TERRAIN_GRID/4+1, TERRAIN_GRID/8+1 };          // {33, 17} at TERRAIN_GRID=128
// MUST be derived from CVBO_ROWS, not a separate literal — this was hardcoded
// to {17*17, 9*9}={289,81} (correct only when TERRAIN_GRID was 64, giving
// CVBO_ROWS={17,9}); TERRAIN_GRID later became 128 (CVBO_ROWS={33,17}) but this
// literal was never updated, so the compact-VBO write loop (whose bounds use
// CVBO_ROWS, i.e. up to 33*33=1089 verts/chunk) wrote past the buffer sized by
// the stale vpc=289 — a heap-buffer-overflow confirmed via AddressSanitizer,
// exactly 0 bytes past the allocation, causing "malloc(): corrupted top size" /
// "double free or corruption" once enough chunks (~3840-4096) had overflowed
// into each other and past the buffer's true end.
static const int CVBO_VPC[2]    = { CVBO_ROWS[0]*CVBO_ROWS[0], CVBO_ROWS[1]*CVBO_ROWS[1] };

static void s_build_compact_vbo(int li)
{
    const int step  = CVBO_STEPS[li];
    const int rows  = CVBO_ROWS[li];
    const int vpc   = CVBO_VPC[li];
    const float cell = TERRAIN_STEP * (float)step;
    const int S = TERRAIN_GRID + 1;
    const int G = rows - 1;
    const int TOTAL = EDITOR_TNKN * EDITOR_TNKN;  // 4096 chunks

    TerrainVertex* buf = new TerrainVertex[(size_t)TOTAL * vpc];
    for (int cz = 0; cz < EDITOR_TNKN; ++cz) {
        for (int cx = 0; cx < EDITOR_TNKN; ++cx) {
            const TerrainChunk& ch = s_chunks[cz][cx];
            int ci = cz * EDITOR_TNKN + cx;          // fixed slot, independent of LOD
            float ox = ch.center_x - CHUNK_SIZE * 0.5f;
            float oz = ch.center_z - CHUNK_SIZE * 0.5f;
            for (int row = 0; row < rows; ++row) {
                for (int col = 0; col < rows; ++col) {
                    int vi = ci * vpc + row * rows + col;
                    int hi = row * step * S + col * step;
                    float x = ox + col * cell;
                    float z = oz + row * cell;
                    // heightmap_ready (not loaded — that means "GPU buffers uploaded",
                    // which most chunks never get now that per-chunk upload is windowed
                    // by camera distance, see s_update_chunk_gpu_window).
                    float y = ch.heightmap_ready ? ch.heightmap.h[hi] : 0.f;
                    float hL = (ch.heightmap_ready && col > 0) ? ch.heightmap.h[hi - step]     : y;
                    float hR = (ch.heightmap_ready && col < G) ? ch.heightmap.h[hi + step]     : y;
                    float hD = (ch.heightmap_ready && row > 0) ? ch.heightmap.h[hi - step * S] : y;
                    float hU = (ch.heightmap_ready && row < G) ? ch.heightmap.h[hi + step * S] : y;
                    float nx = (hL - hR) / (2.f * cell);
                    float ny = 1.f;
                    float nz = (hD - hU) / (2.f * cell);
                    float len = sqrtf(nx*nx + ny*ny + nz*nz);
                    if (len > 0.f) { nx/=len; ny/=len; nz/=len; }
                    // UV must match terrain_gen.cpp's real per-chunk convention (world*0.125,
                    // wrapped at 2048) — this was raw world x,z, i.e. 8x too dense relative to
                    // what terrain_forward.frag's `vUV * 4.0` tiling expects, aliasing the
                    // ground-texture-array detail into a flat, over-blurred average from any
                    // distance (the "почти нульова деталізація" complaint).
                    float u = fmodf(x * 0.125f, 2048.0f);
                    float v = fmodf(z * 0.125f, 2048.0f);
                    buf[vi] = { x, y, z,  nx, ny, nz,  u, v,
                                0.5f, 0.5f, 0.f, 0.f,  y };
                }
            }
        }
    }
    s_cvbo[li].Shutdown();
    s_cvbo[li].Init(0x8892u, buf, sizeof(TerrainVertex) * (size_t)TOTAL * vpc);
    s_cvbo_built[li] = true;
    delete[] buf;

    // Build compact IBO with correct stride (rows, not TERRAIN_GRID+1=65).
    // Built once per li — same for all chunks of this LOD level.
    if (!s_cvbo_ibo[li].SDLBuffer()) {
        const int iG = rows - 1;  // quad count per side
        const int idx_n = iG * iG * 6;
        uint16_t* ibuf = new uint16_t[idx_n];
        int ii = 0;
        for (int r = 0; r < iG; ++r)
            for (int c = 0; c < iG; ++c) {
                uint16_t bl=(uint16_t)(r*rows+c);
                uint16_t br=bl+1;
                uint16_t tl=(uint16_t)(bl+rows);
                uint16_t tr=tl+1;
                ibuf[ii++]=bl; ibuf[ii++]=br; ibuf[ii++]=tl;
                ibuf[ii++]=br; ibuf[ii++]=tr; ibuf[ii++]=tl;
            }
        s_cvbo_ibo[li].Init(0x8893u, ibuf, sizeof(uint16_t)*idx_n);
        delete[] ibuf;
    }
}
// ── Phase 3: Virtual Texturing — tiled kenshi colour overlay ─────────────────
// Splits the full overlay into VT_TILES×VT_TILES tiles loaded on demand.
// Page table: VT_TILES×VT_TILES uint8 texture mapping tile → atlas slot [0..VT_ATLAS_N²).
// Atlas: VT_ATLAS_N×VT_ATLAS_N slots each VT_TILE_PX×VT_TILE_PX pixels.
static constexpr int   VT_TILES    = 8;    // 8×8 = 64 tiles over the full world
static constexpr int   VT_TILE_PX  = 512;  // each tile = 512×512 pixels of kenshi overlay
static constexpr int   VT_ATLAS_N  = 4;    // atlas = 4×4 = 16 slots (fits ~8+spare tiles near camera)

// Virtual Texturing state — CPU overlay + local composite GPU texture.
// Composite: 1024×1024px covering 8000m×8000m centered on camera.
// Built from CPU overlay when camera moves; swapped in for close terrain render.
static uint8_t*   s_vt_img       = nullptr;  // full overlay CPU (4096×4096×4 = 64MB)
static int        s_vt_img_w     = 0;
static int        s_vt_img_h     = 0;
static GpuTexture s_vt_composite;             // 1024×1024 local composite GPU texture
static float      s_vt_comp_ox   = -1e9f;     // world origin X of current composite
static float      s_vt_comp_oz   = -1e9f;
static bool       s_vt_ready     = false;

// stb_image forward declarations (implementation in engine stb_image_impl.cpp)
extern "C" {
    extern unsigned char* stbi_load(const char* filename, int* x, int* y,
                                     int* channels_in_file, int desired_channels);
    extern void stbi_image_free(void* retval_from_stbi_load);
}

static void s_vt_load_source(const char* overlay_path) {
    if (s_vt_img) { stbi_image_free(s_vt_img); s_vt_img = nullptr; }
    int ch = 0;
    s_vt_img = stbi_load(overlay_path, &s_vt_img_w, &s_vt_img_h, &ch, 4);
    if (!s_vt_img)
        fprintf(stderr, "[VT] failed to load overlay: %s\n", overlay_path);
    else
        fprintf(stdout, "[VT] overlay loaded CPU: %dx%d\n", s_vt_img_w, s_vt_img_h);
}

// Build 1024×1024 composite from CPU overlay, covering [cx-4000 .. cx+4000] world space.
static void s_vt_build_composite(float cam_x, float cam_z) {
    if (!s_vt_img) return;
    static const int COMP_PX  = 1024;           // composite pixel size
    static const float HALF_W = 4000.f;          // half world coverage = 4km
    static const float WORLD  = (float)(EDITOR_TNKN * CHUNK_SIZE); // 32000m

    float ox = cam_x - HALF_W;  // world-space left edge
    float oz = cam_z - HALF_W;

    // Map world coords to source image pixels
    float px_per_m = (float)s_vt_img_w / WORLD;  // typically 4096/32000 = 0.128
    int src_x0 = (int)(ox * px_per_m);
    int src_z0 = (int)(oz * px_per_m);
    int src_sz = (int)(HALF_W * 2.f * px_per_m);  // typically 8000*0.128 = 1024

    uint8_t* comp = new uint8_t[(size_t)COMP_PX * COMP_PX * 4];

    // Nearest-neighbour resample src_sz×src_sz → COMP_PX×COMP_PX
    for (int cy = 0; cy < COMP_PX; ++cy) {
        for (int cx2 = 0; cx2 < COMP_PX; ++cx2) {
            int sx = src_x0 + (int)((float)cx2 / COMP_PX * src_sz);
            int sz = src_z0 + (int)((float)cy / COMP_PX * src_sz);
            // clamp to source bounds
            sx = (sx < 0) ? 0 : (sx >= s_vt_img_w) ? s_vt_img_w-1 : sx;
            sz = (sz < 0) ? 0 : (sz >= s_vt_img_h) ? s_vt_img_h-1 : sz;
            int si = (sz * s_vt_img_w + sx) * 4;
            int di = (cy * COMP_PX + cx2) * 4;
            comp[di+0] = s_vt_img[si+0];
            comp[di+1] = s_vt_img[si+1];
            comp[di+2] = s_vt_img[si+2];
            comp[di+3] = s_vt_img[si+3];
        }
    }

    GpuSamplerDesc sd;
    sd.min_filter = GpuSamplerDesc::Filter::LINEAR_MIPMAP;
    sd.mag_filter = GpuSamplerDesc::Filter::LINEAR;
    sd.wrap_s = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    sd.wrap_t = GpuSamplerDesc::Wrap::CLAMP_TO_EDGE;
    sd.gen_mipmap = true;
    sd.flip_v = false;
    s_vt_composite.Shutdown();
    s_vt_composite.InitFromMemory(comp, COMP_PX, COMP_PX, sd);
    delete[] comp;

    s_vt_comp_ox = ox;
    s_vt_comp_oz = oz;
    s_vt_ready   = true;
}

// Call per-frame when camera moves; rebuilds composite if camera crossed 1km boundary.
static void s_vt_update(float cam_x, float cam_z) {
    if (!s_vt_img) return;
    float dx = cam_x - (s_vt_comp_ox + 4000.f);  // distance from composite center
    float dz = cam_z - (s_vt_comp_oz + 4000.f);
    if (dx*dx + dz*dz > 1000.f*1000.f)            // rebuild if moved >1km from center
        s_vt_build_composite(cam_x, cam_z);
}

// GPU Synthesis: activated when camera altitude > SYNTH_ALT_THRESH.
// Uses standard VBO (no vertex texture sampling — safe on Intel Gen9).
// SYNTH_N×SYNTH_N quads = 1 draw call for the full world.
static constexpr float SYNTH_ALT_THRESH = 2000.f;
static constexpr int   SYNTH_N          = 256;   // 256×256 quads, 125m/quad — 4× fewer tris/samples
static GpuStaticBuffer s_synth_vbo;               // TerrainVertex, (SYNTH_N+1)²
static GpuStaticBuffer s_synth_ibo;               // uint32 IBO, SYNTH_N²×6
static bool            s_synth_built    = false;
static GpuPipeline     s_synth_pipeline;          // terrain_forward + depth bias (pushes behind LOD)

static void s_build_synth_hmap() {
    static const float WORLD_SIZE = (float)(EDITOR_TNKN * CHUNK_SIZE); // 32000m
    const int   N1   = SYNTH_N + 1;           // verts per side = 513
    const float cell = WORLD_SIZE / SYNTH_N;  // 62.5m per quad

    // Build VBO from chunk heightmaps
    TerrainVertex* verts = new TerrainVertex[(size_t)N1 * N1];
    for (int ty = 0; ty < N1; ++ty) {
        for (int tx = 0; tx < N1; ++tx) {
            float wx = tx * cell, wz = ty * cell;
            int cx = (int)(wx / CHUNK_SIZE); if (cx >= EDITOR_TNKN) cx = EDITOR_TNKN-1;
            int cz = (int)(wz / CHUNK_SIZE); if (cz >= EDITOR_TNKN) cz = EDITOR_TNKN-1;
            int col = (int)((wx - cx*CHUNK_SIZE) / TERRAIN_STEP); if (col >= TERRAIN_GRID) col = TERRAIN_GRID-1;
            int row = (int)((wz - cz*CHUNK_SIZE) / TERRAIN_STEP); if (row >= TERRAIN_GRID) row = TERRAIN_GRID-1;
            float y = (s_chunks[cz][cx].heightmap_ready
                    ? s_chunks[cz][cx].heightmap.h[row*(TERRAIN_GRID+1)+col] : 0.f);
            // Finite-difference normal
            auto h_at = [&](int ttx, int tty) -> float {
                float wwx = ttx*cell, wwz = tty*cell;
                int ccx=(int)(wwx/CHUNK_SIZE); if(ccx<0)ccx=0; if(ccx>=EDITOR_TNKN)ccx=EDITOR_TNKN-1;
                int ccz=(int)(wwz/CHUNK_SIZE); if(ccz<0)ccz=0; if(ccz>=EDITOR_TNKN)ccz=EDITOR_TNKN-1;
                int co=(int)((wwx-ccx*CHUNK_SIZE)/TERRAIN_STEP); if(co>=TERRAIN_GRID)co=TERRAIN_GRID-1;
                int ro=(int)((wwz-ccz*CHUNK_SIZE)/TERRAIN_STEP); if(ro>=TERRAIN_GRID)ro=TERRAIN_GRID-1;
                return s_chunks[ccz][ccx].heightmap_ready ? s_chunks[ccz][ccx].heightmap.h[ro*(TERRAIN_GRID+1)+co] : 0.f;
            };
            float hL=h_at(tx-1,ty), hR=h_at(tx+1,ty), hD=h_at(tx,ty-1), hU=h_at(tx,ty+1);
            float nx=(hL-hR)/(2.f*cell), ny=1.f, nz=(hD-hU)/(2.f*cell);
            float nl=sqrtf(nx*nx+ny*ny+nz*nz); if(nl>0.f){nx/=nl;ny/=nl;nz/=nl;}
            // Same UV-scale fix as s_build_compact_vbo — must match terrain_gen.cpp's
            // world*0.125 convention, not raw world position (8x too dense otherwise).
            float su = fmodf(wx * 0.125f, 2048.0f);
            float sv = fmodf(wz * 0.125f, 2048.0f);
            verts[ty*N1+tx] = { wx, y, wz,  nx, ny, nz,  su, sv,
                                 0.5f, 0.5f, 0.f, 0.f,  y };
        }
    }
    s_synth_vbo.Shutdown();
    s_synth_vbo.Init(0x8892u, verts, sizeof(TerrainVertex)*(size_t)N1*N1);
    delete[] verts;

    // Build uint32 IBO (N1² > 65535 → must be uint32)
    const int idx_n = SYNTH_N * SYNTH_N * 6;
    uint32_t* idx = new uint32_t[idx_n];
    int ii = 0;
    for (int r = 0; r < SYNTH_N; ++r)
        for (int c = 0; c < SYNTH_N; ++c) {
            uint32_t bl=(uint32_t)(r*N1+c), br=bl+1, tl=bl+N1, tr=tl+1;
            idx[ii++]=bl; idx[ii++]=br; idx[ii++]=tl;
            idx[ii++]=br; idx[ii++]=tr; idx[ii++]=tl;
        }
    s_synth_ibo.Shutdown();
    s_synth_ibo.Init(0x8893u, idx, sizeof(uint32_t)*idx_n);
    delete[] idx;
    s_synth_built = true;
}

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
// Default at grid(23,28) = world(11750,14250) — vivid orange Great Desert area
// (avoid Vain/Ashlands center at 16000,14000 which is sat=0 gray in Kenshi colour map)
static float s_cam_x    = 11750.f;
static float s_cam_z    = 14250.f;
static float s_cam_az   = 0.f;
static float s_cam_el   = 0.70f;
static float s_cam_dist = 22.f;
// Free-fly state
static float s_cx = 11750.f, s_cy = 1500.f, s_cz = 14250.f;
static float s_yaw = 0.f, s_pitch = 0.38f;
static float s_speed       = 1000.f;  // m/s; Shift+Scroll to adjust
static float s_scroll_step = 0.03f;   // step = s_cy * s_scroll_step * wheel
static float s_zoom_in     = 0.94f;   // s_cy *= s_zoom_in  on scroll up
static float s_zoom_out    = 1.06f;   // s_cy *= s_zoom_out on scroll down
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
void UploadTerrainHeightmap(const float* hmap, int W, int H,
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
bool Init(const char* overlay_path, int /*zone_ox*/, int /*zone_oz*/) {
    s_cx = 11750.f;
    s_cy = 8000.f;   // start high to see whole world
    s_cz = 14250.f;
    s_yaw = 0.f; s_pitch = 0.38f;

    s_load_zone_amplitudes("game/data/terrain_config.txt");

    // Real per-biome data lives in a private data file, not compiled into
    // the public engine/tools repos — see docs/ENGINE_AUDIT.md /
    // TERRAIN_FIX_PROMPT.md. Must load before any TerrainGen_Build call.
    BiomeRegistry::Get().LoadFromFile("game/data/biome_table.txt");

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
    s_vt_load_source(op);  // load overlay CPU-side for VT composite (64MB, one-time)
    s_loader_thread = std::thread([op]() {
        if (!s_terrain.Init()) {
            fprintf(stderr, "[W3D-SDLGPU] TerrainRenderer init failed\n"); return;
        }
        // Synthesis pipeline: same as terrain_forward but with depth bias so synthesis always
        // loses depth test against LOD terrain at the same world position (no world-Y offset needed).
        {
            GpuPipeline::Desc sd;
            sd.vert_path = "shaders/terrain_forward.vert";
            sd.frag_path = "shaders/terrain_forward.frag";
            sd.layout.count      = 4;
            sd.layout.stride     = 52;
            sd.layout.attribs[0] = { 0,  0, GpuAttribFmt::F3 };
            sd.layout.attribs[1] = { 1, 12, GpuAttribFmt::F3 };
            sd.layout.attribs[2] = { 2, 24, GpuAttribFmt::F2 };
            sd.layout.attribs[3] = { 3, 32, GpuAttribFmt::F4 };
            sd.raster.depth_test        = true;
            sd.raster.depth_write       = true;
            sd.raster.cull_back         = false;
            sd.has_depth_target   = true;
            sd.vert_uniform_bufs  = 1;
            sd.frag_uniform_bufs  = 1;
            // terrain_forward.frag samples 5 textures (tex_colour, tex_ground,
            // tex_detail, tex_overlay_mask, tex_biome_blend). This pipeline's
            // frag_samplers count must track that exactly (engine's own
            // TerrainRenderer pipeline for the same shader already does, see
            // terrain_renderer.cpp:38) — falling behind by even one binding
            // shifts every descriptor slot, producing solid-colour garbage
            // quads on the synthesis background mesh (which always renders,
            // not just at high altitude).
            sd.frag_samplers      = 5;
            s_synth_pipeline.Create(sd);
        }
        s_props.Init("game/data/props/rock_01.glb", 0.f); // no-op if missing; 0=rock diffuse
        s_terrain.InitKenshiOverlay(op);
        s_terrain.InitGroundTextureArray();
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

// Same TerrainGenParams every chunk build needs (initial bulk build AND the
// windowed re-build in s_update_chunk_gpu_window) — factored out so both
// stay in sync.
static TerrainGenParams s_make_gen_params(int cx, int cz) {
    TerrainGenParams p;
    p.zone_origin_x = 0;
    p.zone_origin_z = 0;
    p.amplitude = TerrainMaster_Loaded() ? 0.f :
        ((cx >= 0 && cx < 64 && cz >= 0 && cz < 64) ? s_zone_amp[cz][cx] : 40.f);
    return p;
}

// ── Tick: build chunks until all 64×64 are loaded ────────────────────────────
static void tick_chunk_build() {
    if (s_loaded) return;
    if (!s_master_ready) return;

    for (int b = 0; b < 64; ++b) {  // 64/call × 2 calls/frame = ~0.5s for all 4096
    int idx = s_chunks_built.load();
    if (idx >= EDITOR_TNKN * EDITOR_TNKN) { s_loaded = true; return; }

    int cz = idx / EDITOR_TNKN, cx = idx % EDITOR_TNKN;
    ChunkCoord coord = { cx, cz };
    TerrainGenParams p = s_make_gen_params(cx, cz);
    TerrainGen_Build(s_chunks[cz][cx], coord, p);
    // GPU buffers (vbo/ibo/ibo_lod/skirt) are NOT uploaded here anymore — see
    // s_update_chunk_gpu_window's doc comment for why eager upload for all
    // 4096 chunks crashes (28,672 individually-allocated GPU buffers exhausts
    // Intel ANV driver-internal resource tracking, aborts mid-vkAllocateCommandBuffers).
    // heightmap_ready (set inside TerrainGen_Build) is enough for the compact
    // VBO / synth background mesh below, which only need CPU height data.
    s_chunks[cz][cx].center_x = (float)cx * CHUNK_SIZE + CHUNK_SIZE * 0.5f;
    s_chunks[cz][cx].center_z = (float)cz * CHUNK_SIZE + CHUNK_SIZE * 0.5f;

    ++s_chunks_built;
    if (s_chunks_built >= EDITOR_TNKN * EDITOR_TNKN) {
        s_loaded = true;
        s_build_compact_vbo(0);   // LOD2 compact VBO — all 4096 chunks, ~61MB, one-time
        s_build_compact_vbo(1);   // LOD3 compact VBO — all 4096 chunks, ~17MB, one-time
        s_build_synth_hmap();
        fprintf(stdout, "[W3D-SDLGPU] %dx%d chunks ready\n", EDITOR_TNKN, EDITOR_TNKN);
    }
    } // for b
}

// Lazily upload/release individual per-chunk GPU buffers (vbo/ibo/ibo_lod/skirt)
// for a small camera-centred window, instead of eagerly uploading all 4096
// chunks at load time. Root cause (confirmed via coredumpctl + gdb backtrace
// on a real crash): eager upload created 4096 chunks × 7 GpuStaticBuffer
// objects each = ~28,672 individually-allocated GPU buffers, all resident at
// once — this exhausted Intel ANV driver-internal resource tracking and
// aborted with "malloc(): corrupted top size" inside vkAllocateCommandBuffers
// (called from GpuStaticBuffer::Init via s_build_compact_vbo, ~chunk 3840-4096
// in load order — first big allocation after the driver state was already
// corrupted). Batching the per-chunk upload into one transfer buffer + one
// submit (GpuUploadBatch, gpu_hal.h) did NOT fix it — same exact crash,
// proving the problem is buffer OBJECT count, not submit count.
// This is safe because the render loop already only uses a chunk's individual
// buffers within d1sq=3500m (DrawRawPOM, lod=0 or 1 by distance — real Kenshi/
// Ogre never switches shader for near terrain, see RenderFrame's comment);
// beyond that it draws via the compact VBO (s_cvbo), which only needs
// heightmap_ready.
static void s_update_chunk_gpu_window(float eye_x, float eye_z) {
    static constexpr float UPLOAD_R2  = 4000.f * 4000.f;  // margin past d1sq=3500m
    static constexpr float RELEASE_R2 = 5500.f * 5500.f;  // hysteresis — avoid thrash at the boundary
    static constexpr int   MAX_UPLOADS_PER_CALL = 8;      // bound per-frame GPU work

    int uploads = 0;
    for (int cz = 0; cz < EDITOR_TNKN && uploads < MAX_UPLOADS_PER_CALL; ++cz) {
        for (int cx = 0; cx < EDITOR_TNKN && uploads < MAX_UPLOADS_PER_CALL; ++cx) {
            TerrainChunk& ch = s_chunks[cz][cx];
            if (!ch.heightmap_ready) continue;
            float ddx = ch.center_x - eye_x, ddz = ch.center_z - eye_z;
            float d2  = ddx * ddx + ddz * ddz;
            if (!ch.loaded && d2 < UPLOAD_R2) {
                // TerrainGen_Upload() reads module-static staging buffers in
                // terrain_gen.cpp that are only valid for whichever chunk
                // TerrainGen_Build() last populated — rebuild (cheap: atlas
                // path is a direct array copy, no procedural noise) right
                // before uploading so we upload THIS chunk's data, not
                // whatever the bulk-build sweep left behind.
                ChunkCoord coord = { cx, cz };
                TerrainGenParams p = s_make_gen_params(cx, cz);
                TerrainGen_Build(ch, coord, p);
                TerrainGen_Upload(ch);
                ++uploads;
            } else if (ch.loaded && d2 > RELEASE_R2) {
                ch.vbo.Shutdown();
                ch.ibo.Shutdown();
                for (int li = 0; li < TERRAIN_LOD_LEVELS; ++li) ch.ibo_lod[li].Shutdown();
                ch.skirt_vbo.Shutdown();
                ch.skirt_ibo.Shutdown();
                ch.loaded = false;
            }
        }
    }
}

// ── Camera input (original free-fly) ─────────────────────────────────────────
static void handle_input(float dt) {
    ImGuiIO& io = ImGui::GetIO();
    // Relative mouse mode toggle — tracks state across calls.
    static bool s_rel_active = false;
    if (io.MouseDown[1]) {
        if (!s_rel_active) {
            SDL_SetWindowRelativeMouseMode(SDL_GetMouseFocus(), true);
            float _dx, _dy;
            SDL_GetRelativeMouseState(&_dx, &_dy); // drain stale delta on enter
            s_rel_active = true;
        }
        s_rmb = true;
        ImGui::SetMouseCursor(ImGuiMouseCursor_None);
        float rdx = 0.f, rdy = 0.f;
        SDL_GetRelativeMouseState(&rdx, &rdy);
        s_yaw   -= rdx * 0.003f;
        s_pitch += rdy * 0.002f;
        if (s_pitch < -0.3f) s_pitch = -0.3f;
        if (s_pitch >  1.3f) s_pitch =  1.3f;
    } else {
        if (s_rel_active) {
            SDL_SetWindowRelativeMouseMode(SDL_GetMouseFocus(), false);
            s_rel_active = false;
        }
        s_rmb = false;
    }
    float sy = sinf(s_yaw), cy2 = cosf(s_yaw);
    // SDL raw key state — always works regardless of ImGui keyboard focus.
    const bool* kb = (const bool*)SDL_GetKeyboardState(nullptr);
    bool shift = kb[SDL_SCANCODE_LSHIFT] || kb[SDL_SCANCODE_RSHIFT];
    // Shift adjusts s_speed (Ctrl+Scroll also works); WASD uses s_speed * dt.
    float sp = s_speed * dt;
    if (kb[SDL_SCANCODE_W]||kb[SDL_SCANCODE_UP])   { s_cx+=sp*sy; s_cz+=sp*cy2; }
    if (kb[SDL_SCANCODE_S]||kb[SDL_SCANCODE_DOWN]) { s_cx-=sp*sy; s_cz-=sp*cy2; }
    if (kb[SDL_SCANCODE_A])  { s_cx+=sp*cy2; s_cz-=sp*sy; }
    if (kb[SDL_SCANCODE_D])  { s_cx-=sp*cy2; s_cz+=sp*sy; }
    if (kb[SDL_SCANCODE_Q]||kb[SDL_SCANCODE_PAGEDOWN]) s_cy-=sp;
    if (kb[SDL_SCANCODE_E]||kb[SDL_SCANCODE_PAGEUP])   s_cy+=sp;
    if (ImGui::IsKeyPressed(ImGuiKey_R)) rebuild_inplace();
    if (ImGui::IsKeyPressed(ImGuiKey_T)) { s_cx=11750.f; s_cy=8000.f; s_cz=14250.f; }
    if (io.MouseWheel != 0.f) {
        if (shift) {
            // Shift+Scroll = adjust WASD speed
            if (io.MouseWheel > 0) s_speed = fminf(s_speed * 1.25f, 80000.f);
            else                   s_speed = fmaxf(s_speed * 0.80f,    10.f);
        } else {
            // Scroll = zoom (move forward + change altitude)
            float step = s_cy * s_scroll_step * io.MouseWheel;
            s_cx += step * sy; s_cz += step * cy2;
            if (io.MouseWheel > 0) s_cy = fmaxf(s_cy * s_zoom_in,  10.f);
            else                   s_cy = fminf(s_cy * s_zoom_out, 150000.f);
        }
    }
    static constexpr float ATLAS_MAX = 63.f * CHUNK_SIZE;
    if (s_cx < 0.f) s_cx = 0.f; if (s_cx > ATLAS_MAX) s_cx = ATLAS_MAX;
    if (s_cz < 0.f) s_cz = 0.f; if (s_cz > ATLAS_MAX) s_cz = ATLAS_MAX;
    if (s_cy > 150000.f) s_cy = 150000.f;

    // Terrain floor clamp — prevent camera clipping through surface.
    // TerrainMaster gives world-space height; add clearance so viewport stays above.
    static constexpr float MIN_ABOVE_TERRAIN = 2.f;
    if (TerrainMaster_Loaded()) {
        float th = TerrainMaster_SampleWorld(s_cx, s_cz);
        if (s_cy < th + MIN_ABOVE_TERRAIN) s_cy = th + MIN_ABOVE_TERRAIN;
    } else {
        if (s_cy < 5.f) s_cy = 5.f;
    }
}

// ── Render terrain to RTT (call AFTER ImGui build, BEFORE ImGui present) ────────
// ensure_rtt() is called from DrawImGui (during ImGui build) so s_color is stable.
void RenderFrame(SDL_GPUCommandBuffer* cmd, float dt, bool tab_active) {
    if (!tab_active) return;
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

    // Windowed per-chunk GPU buffer upload/release around the camera (see
    // s_update_chunk_gpu_window's doc comment) — must run every frame so
    // nearby chunks are ready for the LOD0/LOD1 draw loop below.
    s_update_chunk_gpu_window(eye_x, eye_z);

    // Rebuild dirty chunks (marked by s_apply_brush)
    bool was_chunk_dirty = false;
    if (s_loaded) {
        for (int cz = 0; cz < EDITOR_TNKN; ++cz) for (int cx = 0; cx < EDITOR_TNKN; ++cx) {
            if (!s_chunk_dirty[cz][cx]) continue;
            was_chunk_dirty = true;
            int chunk_zx = s_zone_ox_saved + cx, chunk_zz = s_zone_oz_saved + cz;
            ChunkCoord coord = { chunk_zx, chunk_zz };
            TerrainGenParams p; p.zone_origin_x = 0; p.zone_origin_z = 0;
            p.amplitude = TerrainMaster_Loaded() ? 0.f :
                ((chunk_zx>=0&&chunk_zx<64&&chunk_zz>=0&&chunk_zz<64)
                    ? s_zone_amp[chunk_zz][chunk_zx] : 40.f);
            TerrainGen_Build(s_chunks[cz][cx], coord, p);
            TerrainGen_Upload(s_chunks[cz][cx]);
            s_chunk_dirty[cz][cx] = false;
            s_cvbo_dirty   = true;   // compact VBO needs refresh after edits
            s_cvbo_dirty_t = 0.f;
        }
    }

    // Idle skip: if camera and scene unchanged, reuse last RTT (LOAD_OP_CLEAR not called
    // → s_color keeps previous content → ImGui image shows last rendered frame).
    // Allow 2 stable frames before skipping so final position is fully rendered.
    {
        static float s_pcx=-1e9f,s_pcy=-1e9f,s_pcz=-1e9f,s_pyaw=-1e9f,s_ppit=-1e9f;
        static int   s_idle=0;
        bool cam_same = fabsf(s_cx-s_pcx)<0.5f && fabsf(s_cy-s_pcy)<0.5f &&
                        fabsf(s_cz-s_pcz)<0.5f && fabsf(s_yaw-s_pyaw)<0.001f &&
                        fabsf(s_pitch-s_ppit)<0.001f;
        if (cam_same && !was_chunk_dirty && !s_cvbo_dirty) {
            if (++s_idle > 2) return;  // RTT retained → no GPU work needed
        } else {
            s_idle=0;
            s_pcx=s_cx; s_pcy=s_cy; s_pcz=s_cz; s_pyaw=s_yaw; s_ppit=s_pitch;
        }
    }

    // Sun direction from LightSystem. Sky/fog/ambient colour used to use
    // per-biome BiomeDef::fog_r/g/b + sky_horizon_r/g/b, but real Kenshi's
    // fog/sky is a global screen-space post-process driven by sun angle and
    // time of day (confirmed from the real uncompiled shader source,
    // tmp_/kenshi/data/materials/post/{fog,atmospherefog}.hlsl — no per-biome
    // colour field exists there at all) -- so this uses one fixed neutral
    // sky-blue constant for every zone instead, matching
    // GraphicsSettings::fog_color's default.
    const auto& ls  = LightSystem::Get();
    static constexpr float kSkyR = 0.38f, kSkyG = 0.58f, kSkyB = 0.82f;

    // ── Terrain render pass ──────────────────────────────────────────────────
    SDL_GPUColorTargetInfo ct = {};
    ct.texture     = s_color;
    ct.load_op     = SDL_GPU_LOADOP_CLEAR;
    ct.store_op    = SDL_GPU_STOREOP_STORE;
    ct.clear_color = { kSkyR, kSkyG, kSkyB, 1.f };

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
            sky.horizon_col[0] = kSkyR;
            sky.horizon_col[1] = kSkyG;
            sky.horizon_col[2] = kSkyB;
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
            // terrain shaders expect surface→sun (positive Y up); LightSystem = sun→surface → negate.
            sun.dir[0] = -ls.sun_dir.x; sun.dir[1] = -ls.sun_dir.y; sun.dir[2] = -ls.sun_dir.z;
            sun.strength   = 1.1f;
            sun.ambient[0] = kSkyR * 0.2f + 0.22f;
            sun.ambient[1] = kSkyG * 0.2f + 0.23f;
            sun.ambient[2] = kSkyB * 0.2f + 0.26f;

            // Rebuild compact VBOs 0.5s after last brush edit (debounced).
            if (s_cvbo_dirty) {
                s_cvbo_dirty_t += dt;
                if (s_cvbo_dirty_t >= 0.5f) {
                    s_build_compact_vbo(0);
                    s_build_compact_vbo(1);
                    s_cvbo_dirty = false;
                }
            }

            // Phase 3: Virtual Texturing — update local composite (1km threshold).
            s_vt_update(eye_x, eye_z);

            // Phase 2: Synthesis VBO — always render as full-world background.
            // Near chunks render on top via depth test (no z-fight: chunks are
            // more precise, synthesis is background filler beyond LOD2 range).
            if (s_synth_built && s_synth_vbo.SDLBuffer() && s_synth_ibo.SDLBuffer()
                    && s_synth_pipeline.SDLPipeline()) {
                s_terrain.BeginRawBatch(rp, cmd, vp.m, sun, eye_x, eye_y, eye_z, WCX, WCZ, W2UV, 1); // set uniforms
                // BeginRawBatch never sets ground_layers (only DrawRawChunk does, per real
                // chunk) — this manual draw needs its own push or it samples whatever was
                // left from the last DrawRawChunk call (often all-zero => solid wrong colour).
                {
                    // grass/dirt/road are genuinely per-biome (real FCS data via
                    // BiomeRegistry, loaded at runtime from a private data file) --
                    // this whole-world background mesh has no single "current biome"
                    // to reference, so it uses ForZone(nullptr)'s registry default
                    // as a reasonable flat default.
                    const BiomeDef& synth_biome = BiomeRegistry::Get().ForZone(nullptr);
                    const float kSynthGroundLayers[6] = {
                        (float)synth_biome.tex_base, (float)synth_biome.tex_slope, (float)synth_biome.tex_cliff,
                        (float)synth_biome.tex_grass, (float)synth_biome.tex_dirt, (float)synth_biome.tex_road
                    };
                    // fog_far tuned for normal ~km-scale gameplay view distance saturates
                    // fog_t=1.0 across this whole-world mesh from an aerial camera (tens of
                    // km away), washing everything to solid fog colour — override with a
                    // value past the world's diagonal (~42km for the 29.5km×29.5km world).
                    s_terrain.SetBatchGroundLayers(cmd, kSynthGroundLayers, 60000.f);
                }
                SDL_BindGPUGraphicsPipeline(rp, s_synth_pipeline.SDLPipeline()); // override: depth bias
                SDL_GPUBufferBinding sib { s_synth_ibo.SDLBuffer(), 0u };
                SDL_BindGPUIndexBuffer(rp, &sib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_GPUBufferBinding svb { s_synth_vbo.SDLBuffer(), 0u };
                SDL_BindGPUVertexBuffers(rp, 0, &svb, 1);
                SDL_DrawGPUIndexedPrimitives(rp, (uint32_t)(SYNTH_N*SYNTH_N*6), 1, 0, 0, 0);
            }
            // Phase 1: Chunk LOD (LOD0/1/2 only — synthesis covers > 8km).
            {
            // d0sq/d1sq: both now render via DrawRawPOM (real Kenshi/Ogre never
            // switches shader for "near" terrain — see the identical fix +
            // rationale in game/src/render/npc_render.cpp; d1sq=3500m already
            // sat almost exactly at Kenshi's own real ~3000m composite-map
            // distance, so this tier boundary needed no change, only which
            // shader draws it). LOD only decimates the mesh (lod=0 within
            // d0sq, lod=1 from d0sq to d1sq) via DrawRawPOM's existing lod
            // param — no separate DrawRawChunk(forward) tier anymore.
            static constexpr float d0sq =  1200.f *  1200.f;
            static constexpr float d1sq =  3500.f *  3500.f;
            static constexpr float d2sq =  8000.f *  8000.f;
            static constexpr float d3sq =  8000.f *  8000.f;  // same as d2sq: no LOD3 needed

            // VBfA pattern: bitmask presence table replaces lod_count[]+pointer arrays.
            // 64×64=4096 chunks → 64 uint64_t per LOD tier (one bit per chunk slot).
            // ctzll iteration is branch-free and cache-friendly for sparse visible sets.
            static constexpr int LOD_WORDS = (EDITOR_TNKN * EDITOR_TNKN + 63) / 64; // 64
            uint64_t lod_mask[2][LOD_WORDS] = {};  // [0]=compact-VBO LOD2, [1]=compact-VBO LOD3 (beyond d1sq — real Kenshi DistantTerrain/composite-map tier)

            for (int cz = 0; cz < EDITOR_TNKN; ++cz) {
                for (int cx = 0; cx < EDITOR_TNKN; ++cx) {
                    const TerrainChunk& ch = s_chunks[cz][cx];
                    if (!ch.loaded) continue;
                    float ddx = ch.center_x - eye_x;
                    float ddz = ch.center_z - eye_z;
                    float d2  = ddx*ddx + ddz*ddz;
                    if (d2 > d3sq) continue;
                    if (d2 < d1sq) {
                        // Use global kenshi colour map (WCX/WCZ/W2UV) for correct vivid colours.
                        // VT composite is built for future height-based detail but not applied
                        // for colour — the local 8km patch can land on muted/grey kenshi zones.
                        // fog_far_override: see DrawRawPOM's doc comment — normal gameplay
                        // fog_far is tuned for ground-level view distance; the editor's
                        // aerial camera (kilometres up) needs a much larger value or every
                        // POM chunk renders as solid fog colour (the "gear/waffle" artifact).
                        int lod = (d2 < d0sq) ? 0 : 1;
                        s_terrain.DrawRawPOM(rp, cmd, ch, vp.m,
                                            sun, eye_x, eye_y, eye_z, WCX, WCZ, W2UV, lod, 60000.f);
                    } else {
                        int flat = cz * EDITOR_TNKN + cx;
                        int tier = (d2 < d2sq) ? 0 : 1;
                        lod_mask[tier][flat >> 6] |= 1ULL << (flat & 63);
                    }
                }
            }
            // LOD2/LOD3: compact VBO — bitmask → DrawGPUIndexedPrimitives by flat slot.
            static const int IDX_N[2] = { 16*16*6, 8*8*6 };  // 1536, 384
            for (int li = 0; li < 2; ++li) {
                if (!s_cvbo_built[li] || !s_cvbo_ibo[li].SDLBuffer()) continue;
                bool batch_open = false;
                const int vpc = CVBO_VPC[li];
                SDL_GPUBufferBinding cib { s_cvbo_ibo[li].SDLBuffer(), 0u };
                SDL_GPUBufferBinding vb  { s_cvbo[li].SDLBuffer(), 0u };
                for (int w = 0; w < LOD_WORDS; ++w) {
                    uint64_t bits = lod_mask[li][w];
                    while (bits) {
                        if (!batch_open) {
                            s_terrain.BeginRawBatch(rp, cmd, vp.m, sun, eye_x, eye_y, eye_z, WCX, WCZ, W2UV, li + 2);
                            // Same bug as synthesis (see SetBatchGroundLayers doc comment):
                            // this manual draw bypasses DrawRawChunk, so BeginRawBatch alone
                            // leaves ground_layers at whatever was last pushed (often zero) —
                            // and since ALL chunks in this compact-VBO batch share ONE push
                            // (no per-chunk re-push like DrawRawChunk), they can only ever
                            // show ONE biome's texture here, not real per-chunk variety — a
                            // reasonable simplification for the coarsest background LOD, and
                            // fixes the "waffle" pattern of correct-vs-stale-colour chunks at
                            // the LOD1/LOD2 boundary (real biome colour vs previous garbage).
                            const BiomeDef& compact_biome = BiomeRegistry::Get().ForZone(nullptr);
                            const float kCompactGroundLayers[6] = {
                                (float)compact_biome.tex_base, (float)compact_biome.tex_slope, (float)compact_biome.tex_cliff,
                                (float)compact_biome.tex_grass, (float)compact_biome.tex_dirt, (float)compact_biome.tex_road
                            };
                            // Same aerial-altitude fog_far problem as LOD1/synthesis (see their
                            // comments) — override here too, not just ground_layers.
                            s_terrain.SetBatchGroundLayers(cmd, kCompactGroundLayers, 60000.f);
                            SDL_BindGPUIndexBuffer(rp, &cib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
                            SDL_BindGPUVertexBuffers(rp, 0, &vb, 1);
                            batch_open = true;
                        }
                        int bit  = __builtin_ctzll(bits);
                        int flat = w * 64 + bit;
                        SDL_DrawGPUIndexedPrimitives(rp, (uint32_t)IDX_N[li],
                                                     1, 0, (Sint32)(flat * vpc), 0);
                        bits &= bits - 1;
                    }
                }
            }
            } // chunk-based LOD
        }

        SDL_EndGPURenderPass(rp);
    }

    (void)dt;
}

// ── ImGui draw (call inside "3D World" tab) ────────────────────────────────────
void DrawImGui(float W, float H, float dt) {
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
    bool hov = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) || ImGui::IsItemClicked(ImGuiMouseButton_Right))
        s_focused = true;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !hov)
        s_focused = false;
    // Always process camera input when any movement key is held — prevents
    // freeze when mouse moves off the viewport without releasing keys.
    {
        const bool* kb = (const bool*)SDL_GetKeyboardState(nullptr);
        bool any_key = kb[SDL_SCANCODE_W]||kb[SDL_SCANCODE_A]||
                       kb[SDL_SCANCODE_S]||kb[SDL_SCANCODE_D]||
                       kb[SDL_SCANCODE_Q]||kb[SDL_SCANCODE_E];
        if (hov || s_rmb || s_focused || any_key || ImGui::GetIO().MouseWheel != 0.f)
            handle_input(dt);
    }

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
        // Only cast when ray points at least 20° below horizontal.
        float rdlen = sqrtf(rdx*rdx + rdy*rdy + rdz*rdz);
        bool ray_ok = rdlen > 1e-4f && (rdy / rdlen) < -0.34f;  // sin(20°)≈0.34
        s_brush_hit = ray_ok && s_ray_terrain(s_last_eye[0], s_last_eye[1], s_last_eye[2],
                                              rdx, rdy, rdz,
                                              s_brush_wx, s_brush_wy, s_brush_wz);

        // LMB held → paint (only when close enough to terrain)
        if (s_brush_hit && s_cy < 500.f && ImGui::GetIO().MouseDown[0])
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

    // ── Brush cursor — crosshair at mouse position (no world-space projection) ──
    if (s_brush_hit && edit_mode && !s_rmb && s_cy < 500.f) {
        ImVec2 mp = ImGui::GetMousePos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        uint32_t col = (s_brush_mode == BrushMode::Lower) ? IM_COL32(255,80,60,220)
                     : (s_brush_mode == BrushMode::Smooth || s_brush_mode == BrushMode::Flatten)
                       ? IM_COL32(80,200,255,220) : IM_COL32(255,210,60,220);
        const float R = 10.f;
        dl->AddLine({mp.x-R, mp.y}, {mp.x+R, mp.y}, col, 2.f);
        dl->AddLine({mp.x, mp.y-R}, {mp.x, mp.y+R}, col, 2.f);
        dl->AddCircle(mp, 4.f, col, 8, 1.5f);
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
        ImGui::Text("Zone: %d,%d  Alt: %.0fm  Speed: %.0fm/s  LOD: %s",
            cur_zx, cur_zy, s_cy, s_speed,
            s_cy > 4000.f ? "8x8" : s_cy > 1500.f ? "16x16" : s_cy > 500.f ? "32x32" : "64x64");
    ImGui::Text("RMB=look  WASD=fly  Q/E=up/down  Scroll=zoom  Shift+Scroll=speed  T=reset");
    ImGui::PopStyleColor();

}

void ApplyCameraConfig(float wasd_speed, float scroll_step, float zoom_in, float zoom_out) {
    s_speed       = wasd_speed;
    s_scroll_step = scroll_step;
    s_zoom_in     = zoom_in;
    s_zoom_out    = zoom_out;
}

float GetWasdSpeed()    { return s_speed; }
float GetScrollStep()   { return s_scroll_step; }
float GetZoomIn()       { return s_zoom_in; }
float GetZoomOut()      { return s_zoom_out; }

} // namespace WorldEditor3D_SDLGPU

#endif // MD_SDL_GPU
