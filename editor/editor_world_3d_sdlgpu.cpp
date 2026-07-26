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
#include <monkey_dust/render/terrain_quadtree_renderer.h>
#include <monkey_dust/world/terrain_quadtree.h>
#include <monkey_dust/world/terrain_quadtree_async.h>
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
#include <cstdlib>
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

// Quadtree-LOD terrain rewrite Phase 8 (see plan at
// /home/rdga1/.claude/plans/serene-pondering-teapot.md): coarse whole-world
// BACKDROP mesh — the far tier of a two-tier scheme, the near tier being a
// window-following TerrainQuadtree region (s_qt_* below, same engine classes
// the game's SceneRender uses). This backdrop replaces BOTH the old
// per-chunk-array-fed synthesis mesh AND the compact-VBO LOD2/LOD3 tier that
// used to sit between it and near-camera detail — with the quadtree handling
// its own internal LOD via CDLOD recursion, only two tiers are needed at
// all: this static backdrop (always drawn, depth-biased so the quadtree
// always wins the depth test against it) and the quadtree window itself.
//
// Reads TerrainAtlas_SampleWorld directly (the same always-resident,
// whole-world CPU heightmap every other engine height-query already uses —
// see terrain_query.h/physics_terrain_region.cpp) instead of a 4096-entry
// TerrainChunk array — removes the entire multi-frame chunk-build sweep this
// used to depend on (see git history for tick_chunk_build, deleted this
// phase): building this mesh is now a single synchronous ~513x513-sample
// CPU loop, done once in Init()'s loader thread right after ground textures
// are ready, not spread across ~32 frames.
static constexpr int   SYNTH_N          = 256;   // 256×256 quads, ~115m/quad
static GpuStaticBuffer s_synth_vbo;               // TerrainVertex, (SYNTH_N+1)²
static GpuStaticBuffer s_synth_ibo;               // uint32 IBO, SYNTH_N²×6
static bool            s_synth_built    = false;
static GpuPipeline     s_synth_pipeline;          // terrain_forward + depth bias (pushes behind quadtree)

static void s_build_synth_hmap() {
    static const float WORLD_SIZE = (float)(EDITOR_TNKN * CHUNK_SIZE); // 29491.2m
    const int   N1   = SYNTH_N + 1;           // verts per side = 257
    const float cell = WORLD_SIZE / SYNTH_N;

    TerrainVertex* verts = new TerrainVertex[(size_t)N1 * N1];
    for (int ty = 0; ty < N1; ++ty) {
        for (int tx = 0; tx < N1; ++tx) {
            float wx = tx * cell, wz = ty * cell;
            float y = TerrainAtlas_SampleWorld(wx, wz);
            // Finite-difference normal, same fixed-world-step convention as
            // terrain_gen.cpp's cross-chunk normal stitching (nx=-dhdx,
            // ny=1, nz=-dhdz after normalizing hL-hR/hD-hU below).
            float hL = TerrainAtlas_SampleWorld(wx - cell, wz);
            float hR = TerrainAtlas_SampleWorld(wx + cell, wz);
            float hD = TerrainAtlas_SampleWorld(wx, wz - cell);
            float hU = TerrainAtlas_SampleWorld(wx, wz + cell);
            float nx=(hL-hR)/(2.f*cell), ny=1.f, nz=(hD-hU)/(2.f*cell);
            float nl=sqrtf(nx*nx+ny*ny+nz*nz); if(nl>0.f){nx/=nl;ny/=nl;nz/=nl;}
            // Same UV-scale convention as terrain_gen.cpp's per-chunk bake
            // (world*0.125, wrapped at 2048) — matches terrain_forward.frag's
            // `vUV * 4.0` tiling density.
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

// Per-zone (64x64=4096) ground-layer lookup — fixes the synthesis/compact-LOD2
// "one biome for the whole world" bug: those two draw paths cover many zones
// in a single draw call and previously fell back to
// BiomeRegistry::Get().ForZone(nullptr) (always biomes_[0]) for the entire
// mesh. Built once, uploaded to TerrainRenderer's per-zone SSBO; consumed by
// terrain_forward.slang's fsMain only when use_zone_lookup=1 (see
// TerrainRenderer::SetBatchZoneLookup). Real per-chunk LOD0/1 draws are
// unaffected — they already resolve their own biome correctly via
// s_resolve_biome/TerrainGen_ResolveBiome at chunk-generation time.
static void s_build_zone_ground_layers() {
    static uint32_t s_layers[64 * 64 * 6];
    for (int zy = 0; zy < 64; ++zy) {
        for (int zx = 0; zx < 64; ++zx) {
            const BiomeDef& b = TerrainGen_ResolveBiome(zx, zy);
            int idx = (zy * 64 + zx) * 6;
            s_layers[idx + 0] = (uint32_t)b.tex_base;
            s_layers[idx + 1] = (uint32_t)b.tex_slope;
            s_layers[idx + 2] = (uint32_t)b.tex_cliff;
            s_layers[idx + 3] = (uint32_t)b.tex_grass;
            s_layers[idx + 4] = (uint32_t)b.tex_dirt;
            s_layers[idx + 5] = (uint32_t)b.tex_road;
        }
    }
    s_terrain.UploadZoneGroundLayers(s_layers, 64 * 64 * 6);
    // Sanity log — cross-check a few zones by hand against terrain_config.txt.
    fprintf(stdout, "[W3D-SDLGPU] zone ground-layer LUT built (4096 zones); "
                     "zone(0,0)=base%u zone(32,32)=base%u zone(63,63)=base%u\n",
            s_layers[(0*64+0)*6], s_layers[(32*64+32)*6], s_layers[(63*64+63)*6]);
}

static GpuPipeline        s_sky_pipeline;

// Prop scatter state — rebuilt once on init
static constexpr int  PROPS_PER_CHUNK = 8;
static float          s_prop_pos[PropRenderer::MAX_PROPS * 3] = {};
static int            s_prop_count = 0;
static bool           s_props_built = false;

// Async loading state
static std::atomic<bool>  s_master_ready{false};
static std::thread        s_loader_thread;
static constexpr int      s_zone_ox_saved = 0;  // whole-world prop scatter, unrelated to s_qt_* window below
static constexpr int      s_zone_oz_saved = 0;

// ── Near-tier: window-following TerrainQuadtree region ──────────────────────
// Same engine classes SceneRender::terrain_qt_* (game, Phase 7) uses. Unlike
// the game, the editor's free-fly camera already lives in ABSOLUTE Kenshi
// metres (s_cx/s_cy/s_cz, see handle_input's ATLAS_MAX clamp) — no session-
// local-window translation layer is needed here at all.
static TerrainQuadtree              s_qt_tree;
static TerrainQuadtreeAsyncSelector s_qt_async;
static TerrainQuadtreeRenderer      s_qt;
static bool  s_qt_ready      = false;
static float s_qt_height_min = 0.f;
static float s_qt_height_max = 0.f;
// 16 zones (~7372.8m) — near TerrainQuadtreeRenderer::UploadHeightmapRegion's
// kMaxRes=1025 cap (zone_span*64+1 <= 1025 => zone_span <= 16) at native
// per-zone resolution. Far beyond this window, the always-resident backdrop
// mesh above (s_build_synth_hmap) takes over — see this file's "Two-tier"
// doc comment near s_build_synth_hmap.
static constexpr int kQtZoneSpan  = 16;
static constexpr int kQtMaxDepth  = 4; // 16 zones wide -> depths 16,8,4,2,1 zone-widths
static float s_qt_lod_distances[kQtMaxDepth + 1] = {};
// Zone-space (bottom-left corner) of the region as of the last rebuild —
// used only to decide whether the camera has moved far enough to justify
// another rebuild (full CPU resample + GPU texture re-upload); NOT read by
// the draw call itself (TerrainQuadtreeRenderer tracks its own region
// origin/size internally). Sentinel -999999 forces the first rebuild.
static int s_qt_center_zx = -999999;
static int s_qt_center_zz = -999999;

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
                float wy = TerrainAtlas_SampleWorld(wx, wz);
                float* p3 = &s_prop_pos[s_prop_count * 3];
                p3[0] = wx; p3[1] = wy; p3[2] = wz;
                ++s_prop_count;
            }
        }
    }
    s_props_built = true;
}

// Re-centres the near-tier quadtree window on (cam_x, cam_z) and re-uploads
// its height texture — mirrors game/src/render/scene_render.cpp's
// SceneRender::RebuildQuadtreeRegion (Phase 7), simplified since the editor
// camera is already in absolute Kenshi metres (no tnoff-style translation).
// First call (s_qt_ready == false) does the one-time pipeline+mesh Init();
// later calls use the cheaper RebuildRegion (height texture only).
static void s_rebuild_quadtree_region(float cam_x, float cam_z) {
    static constexpr int kZoneMax = EDITOR_TNKN - kQtZoneSpan; // valid zx0/zy0 upper bound
    int zx0 = (int)(cam_x / CHUNK_SIZE) - kQtZoneSpan / 2;
    int zy0 = (int)(cam_z / CHUNK_SIZE) - kQtZoneSpan / 2;
    if (zx0 < 0) zx0 = 0; if (zx0 > kZoneMax) zx0 = kZoneMax;
    if (zy0 < 0) zy0 = 0; if (zy0 > kZoneMax) zy0 = kZoneMax;

    // local_origin_x/z == absolute zx0*CHUNK_SIZE here (no-op translation) —
    // the editor's camera/tree already live in absolute Kenshi metres, see
    // TerrainQuadtreeRenderer::Init's doc comment for why this must match
    // whatever space s_qt_tree.Init() below uses.
    float local_ox = (float)zx0 * CHUNK_SIZE, local_oz = (float)zy0 * CHUNK_SIZE;
    bool ok;
    if (!s_qt_ready) {
        ok = s_qt.Init(zx0, zy0, kQtZoneSpan, local_ox, local_oz, s_qt_height_min, s_qt_height_max);
        if (ok) {
            for (int d = 0; d <= kQtMaxDepth; ++d) {
                float node_size = ((float)kQtZoneSpan * CHUNK_SIZE) / (float)(1 << d);
                s_qt_lod_distances[d] = node_size * 2.f;
            }
            s_qt_async.Init(&s_qt_tree);
        }
    } else {
        ok = s_qt.RebuildRegion(zx0, zy0, kQtZoneSpan, local_ox, local_oz, s_qt_height_min, s_qt_height_max);
    }
    if (!ok) return;
    s_qt_ready = true;
    s_qt_center_zx = zx0;
    s_qt_center_zz = zy0;
    float region_size = (float)kQtZoneSpan * CHUNK_SIZE;
    s_qt_tree.Init((float)zx0 * CHUNK_SIZE, (float)zy0 * CHUNK_SIZE, region_size, kQtMaxDepth);
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

// Debounced "TerrainAtlas may have changed" flag — replaces the old
// per-chunk s_chunk_dirty[][] array (Phase 8 rewrite). Set by
// UploadTerrainHeightmap (PCG hook) and the R key; RenderFrame rebuilds
// both terrain tiers 0.5s after the last mark, same debounce timing the old
// compact-VBO dirty flag used.
static bool  s_terrain_dirty   = false;
static float s_terrain_dirty_t = 0.f;

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

// ── UploadTerrainHeightmap — mark terrain dirty for a PCG-generated tile ────────
// hmap/W/H/chunk_x/chunk_z: unused (see 2026-07-19 note below) — kept in the
// signature so EditorTerrainPanel's PCG Generate panel (game/src/editor/
// editor_terrain_panel.cpp) keeps compiling unchanged.
// 2026-07-19: used to also write the PCG tile into the (now-removed)
// TerrainMaster macro layer, which real Kenshi zone chunks never actually
// sampled from (TerrainGen_Build's atlas path always wins when zone_origin_x
// is in-bounds and the atlas is loaded — the case for every real chunk).
// The actual heightmap data passed here has no real-terrain target to land
// on anymore; this hook now only marks BOTH terrain tiers (backdrop +
// near-tier quadtree, Phase 8) for a debounced refresh (see s_terrain_dirty),
// in case a future PCG rewrite does start writing into TerrainAtlas for real.
void UploadTerrainHeightmap(const float* /*hmap*/, int /*W*/, int /*H*/,
                                    float /*world_size_m*/, int /*chunk_x*/, int /*chunk_z*/) {
    s_terrain_dirty   = true;
    s_terrain_dirty_t = 0.f;
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

// ── Init — background: renderer thread does the heavy chunk build ───────────
bool Init(const char* overlay_path, int /*zone_ox*/, int /*zone_oz*/) {
    s_cx = 11750.f;
    s_cy = 8000.f;   // start high to see whole world
    s_cz = 14250.f;
    s_yaw = 0.f; s_pitch = 0.38f;

    // Real per-biome data lives in a private data file, not compiled into
    // the public engine/tools repos — see docs/ENGINE_AUDIT.md /
    // TERRAIN_FIX_PROMPT.md. Must load before any TerrainGen_Build call.
    BiomeRegistry::Get().LoadFromFile("game/data/biome_table.txt");

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
        // Backdrop pipeline: same as terrain_forward but with depth bias so
        // the backdrop always loses depth test against the near-tier
        // quadtree at the same world position (no world-Y offset needed).
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
            // terrain_forward.frag samples 4 textures (tex_colour, tex_ground,
            // tex_overlay_mask, tex_biome_blend — tex_detail removed
            // 2026-07-25). Must track TerrainRenderer::Init()'s own pipeline
            // for the same shader exactly (terrain_renderer.cpp) — falling
            // behind shifts every descriptor slot, producing solid-colour
            // garbage quads on the backdrop mesh (which always renders, not
            // just at high altitude). Was wrongly 5 here (stale from before
            // the 2026-07-25 tex_detail removal — never updated when
            // terrain_forward.slang and TerrainRenderer's own pipeline both
            // were), fixed as part of this Phase 8 rewrite.
            sd.frag_samplers      = 4;
            // Must also track TerrainRenderer::Init()'s frag_storage_bufs=1
            // (per-zone ground-layer lookup SSBO, see
            // TerrainRenderer::UploadZoneGroundLayers/SetBatchZoneLookup) —
            // same binding-count-sync rule as frag_samplers above.
            sd.frag_storage_bufs  = 1;
            s_synth_pipeline.Create(sd);
        }
        s_props.Init("game/data/props/rock_01.glb", 0.f); // no-op if missing; 0=rock diffuse
        s_terrain.InitKenshiOverlay(op);
        s_terrain.InitGroundTextureArray();
        // task #158c: editor's 3D World viewport never called these three —
        // scene_render.cpp (game) does, so terrain_pom.slang's tex_overlay_
        // mask/tex_biome_blend samplers (and, via BakeAlbedo below, the baked
        // per-chunk albedo itself) were silently sampling fallback textures
        // for every editor chunk, producing the black/white "прогалини
        // текстур" blotch pattern (task #158, re-confirmed at zone 25,30/
        // 13,15/34,36 this session) — not a POM/altitude bug, a missing
        // init call. See game/src/render/scene_render.cpp's InitOverlayMask/
        // InitBiomeBlend/InitAlbedoBake call sites (~line 264-291) this
        // mirrors.
        s_terrain.InitOverlayMask("game/data/textures/md_overlay_mask.png");
        s_terrain.InitBiomeBlend("game/data/textures/md_biome_blend.png");
        // Phase 8: zone-layer LUT + backdrop mesh are now atlas-sourced
        // (TerrainAtlas is already loaded by main.cpp before Init() is
        // called — see this file's header comment) and build synchronously
        // in a single sweep, no multi-frame chunk-build sweep needed
        // anymore (see s_build_synth_hmap's doc comment).
        s_build_zone_ground_layers();
        s_build_synth_hmap();
        s_rebuild_quadtree_region(s_cx, s_cz);
        s_master_ready = true;
        s_build_prop_positions();
    });
    return true;
}

// ── Shutdown ────────────────────────────────────────────────────────────────
// Joins the background loader thread. MUST run before GpuDevice::Shutdown()
// destroys the Vulkan device: this thread was previously detach()'d, which let
// main() reach vkDestroyDevice() while the thread was still mid-upload of the
// 699MB ground-texture array (InitGroundTextureArray → SDL3 GPU transfer-
// buffer realloc) — a destroy-during-upload race that corrupted the Vulkan
// driver's internal allocator. Confirmed via 5 identical --exec-mode
// coredumps: crashing thread always mid-realloc inside InitFromDDSArray,
// main thread always concurrently inside GpuDevice::Shutdown → vkDestroyDevice
// (see CLAUDE.md Hardware Checklist's "ВІДКРИТИЙ БАГ" entry, now closed).
// Never reproduced interactively because a human never quits within the
// ~1s load window; --exec scenarios that render+screenshot take just long
// enough for main() to race ahead to shutdown before the load finishes.
void Shutdown() {
    if (s_loader_thread.joinable()) s_loader_thread.join();
}

// R key: force an immediate refresh of both terrain tiers (was "rebuild all
// chunks after sculpting" — the editor's World3D viewport is view-only,
// brush editing lives in EditorTerrainPanel/game F3, see CLAUDE.md's "F3
// Flythrough camera" section — this now just re-syncs both tiers with
// TerrainAtlas's current contents on demand, useful after an external edit
// to the atlas file between sessions).
static void s_force_refresh() {
    s_build_synth_hmap();
    s_qt_center_zx = -999999; s_qt_center_zz = -999999;  // force next rebuild
    s_rebuild_quadtree_region(s_cx, s_cz);
    if (s_rmb) {
        SDL_SetWindowRelativeMouseMode(SDL_GetMouseFocus(), false);
        s_rmb = false;
    }
    float _dx, _dy;
    SDL_GetRelativeMouseState(&_dx, &_dy);  // drain accumulated delta
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
    if (ImGui::IsKeyPressed(ImGuiKey_R)) s_force_refresh();
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
    static constexpr float MIN_ABOVE_TERRAIN = 2.f;
    if (TerrainAtlas_Loaded()) {
        float th = TerrainAtlas_SampleWorld(s_cx, s_cz);
        if (s_cy < th + MIN_ABOVE_TERRAIN) s_cy = th + MIN_ABOVE_TERRAIN;
    } else {
        if (s_cy < 5.f) s_cy = 5.f;
    }
}

// ── Render terrain to RTT (call AFTER ImGui build, BEFORE ImGui present) ────────
// ensure_rtt() is called from DrawImGui (during ImGui build) so s_color is stable.
void RenderFrame(SDL_GPUCommandBuffer* cmd, float dt, bool tab_active) {
    if (!tab_active) return;
    if (!s_master_ready.load() || !s_color) return;
    int w = s_rtt_w, h = s_rtt_h;  // use already-created RTT dimensions
    if (w < 8 || h < 8) return;

    float asp = (float)w / (float)h;
    M4 proj = m4_persp(0.80f, asp, 5.f, 350000.f);
    M4 view = m4_view(s_cx, s_cy, s_cz, s_yaw, s_pitch);
    M4 vp   = m4_mul(proj, view);
    float eye_x = s_cx, eye_y = s_cy, eye_z = s_cz;

    // Re-centre the near-tier quadtree window once the camera has drifted
    // past 1/4 of the window's width from its last-rebuilt centre — mirrors
    // game/src/render/scene_render.cpp's RebuildQuadtreeRegion trigger
    // (Phase 7), continuous-distance here since the editor camera isn't
    // gated to gameplay's discrete zone-crossing streaming ticks.
    bool qt_rebuilt_this_frame = false;
    if (s_qt_ready) {
        float win_cx = ((float)s_qt_center_zx + (float)kQtZoneSpan * 0.5f) * CHUNK_SIZE;
        float win_cz = ((float)s_qt_center_zz + (float)kQtZoneSpan * 0.5f) * CHUNK_SIZE;
        float dx = eye_x - win_cx, dz = eye_z - win_cz;
        static constexpr float kRebuildR = (float)kQtZoneSpan * CHUNK_SIZE * 0.25f;
        if (dx*dx + dz*dz > kRebuildR*kRebuildR) {
            s_rebuild_quadtree_region(eye_x, eye_z);
            qt_rebuilt_this_frame = true;
        }
    }

    // Debounced terrain-dirty refresh (UploadTerrainHeightmap / R key) —
    // rebuilds both tiers 0.5s after the last mark.
    if (s_terrain_dirty) {
        s_terrain_dirty_t += dt;
        if (s_terrain_dirty_t >= 0.5f) {
            s_build_synth_hmap();
            s_qt_center_zx = -999999; s_qt_center_zz = -999999;
            s_rebuild_quadtree_region(eye_x, eye_z);
            s_terrain_dirty = false;
            qt_rebuilt_this_frame = true;
        }
    }

    // Idle skip: if camera and scene unchanged, reuse last RTT (LOAD_OP_CLEAR not called
    // → s_color keeps previous content → ImGui image shows last rendered frame).
    // Allow 2 stable frames before skipping so final position is fully rendered.
    // Must also stay awake the frame a quadtree rebuild happened — its new
    // height texture needs an actual draw to show up in the RTT.
    {
        static float s_pcx=-1e9f,s_pcy=-1e9f,s_pcz=-1e9f,s_pyaw=-1e9f,s_ppit=-1e9f;
        static int   s_idle=0;
        bool cam_same = fabsf(s_cx-s_pcx)<0.5f && fabsf(s_cy-s_pcy)<0.5f &&
                        fabsf(s_cz-s_pcz)<0.5f && fabsf(s_yaw-s_pyaw)<0.001f &&
                        fabsf(s_pitch-s_ppit)<0.001f;
        if (cam_same && !s_terrain_dirty && !qt_rebuilt_this_frame) {
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

        if (s_synth_built && s_terrain.IsReady()) {
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

            // ── Far tier: always-resident whole-world backdrop, depth-biased
            // so it always loses against the near-tier quadtree below.
            if (s_synth_vbo.SDLBuffer() && s_synth_ibo.SDLBuffer() && s_synth_pipeline.SDLPipeline()) {
                s_terrain.BeginRawBatch(rp, cmd, vp.m, sun, eye_x, eye_y, eye_z, WCX, WCZ, W2UV, 1); // set uniforms
                // BeginRawBatch never sets ground_layers (only DrawRawChunk does, per real
                // chunk) — this manual draw needs its own push or it samples whatever was
                // left from the last DrawRawChunk call (often all-zero => solid wrong colour).
                {
                    // Per-zone ground-layer lookup (see s_build_zone_ground_layers) —
                    // this whole-world background mesh spans many zones/biomes in one
                    // draw call, so a single fixed ground_layers push (the old
                    // ForZone(nullptr) default-biome-everywhere bug) can't show real
                    // per-zone variety. use_zone_lookup=1 makes terrain_forward.slang's
                    // fsMain resolve ground_layers per-fragment from world position
                    // instead (no biome crossfade on this path — hard zone boundary,
                    // acceptable simplification at this distance, see terrain_forward.slang).
                    // fog_far tuned for normal ~km-scale gameplay view distance
                    // (terrain_cr_m) saturates fog_t=1.0 across this whole-world mesh
                    // from an aerial camera (tens of km away), washing everything to
                    // solid fog colour — override with a much larger fog_far (linear
                    // fog, 2026-07-12; this arg was an EXP2 density override before,
                    // now overrides fog_far directly). 60000m safely exceeds the
                    // 29.5km×29.5km world's diagonal (~41.7km).
                    s_terrain.SetBatchZoneLookup(cmd, true, 60000.f);
                }
                SDL_BindGPUGraphicsPipeline(rp, s_synth_pipeline.SDLPipeline()); // override: depth bias
                SDL_GPUBufferBinding sib { s_synth_ibo.SDLBuffer(), 0u };
                SDL_BindGPUIndexBuffer(rp, &sib, SDL_GPU_INDEXELEMENTSIZE_32BIT);
                SDL_GPUBufferBinding svb { s_synth_vbo.SDLBuffer(), 0u };
                SDL_BindGPUVertexBuffers(rp, 0, &svb, 1);
                SDL_DrawGPUIndexedPrimitives(rp, (uint32_t)(SYNTH_N*SYNTH_N*6), 1, 0, 0, 0);
            }

            // ── Near tier: window-following quadtree (Phase 8). Traversal
            // runs on a JobSystem worker (kicked here, consumed next time it
            // completes — never blocks this thread); draws whatever the most
            // recently COMPLETED traversal picked, with the matching CDLOD
            // morph factor — same pattern game/src/render/npc_render.cpp uses.
            if (s_qt_ready) {
                float qfp[16];
                // Gribb & Hartmann side-plane extraction — same formula as
                // MdCamera::FrustumPlanes (md_camera.h), replicated locally
                // since this file uses its own raw M4, not MdCamera.
                const float* m = vp.m;
                qfp[ 0]=m[3]+m[0]; qfp[ 1]=m[7]+m[4]; qfp[ 2]=m[11]+m[ 8]; qfp[ 3]=m[15]+m[12];
                qfp[ 4]=m[3]-m[0]; qfp[ 5]=m[7]-m[4]; qfp[ 6]=m[11]-m[ 8]; qfp[ 7]=m[15]-m[12];
                qfp[ 8]=m[3]-m[1]; qfp[ 9]=m[7]-m[5]; qfp[10]=m[11]-m[ 9]; qfp[11]=m[15]-m[13];
                qfp[12]=m[3]+m[1]; qfp[13]=m[7]+m[5]; qfp[14]=m[11]+m[ 9]; qfp[15]=m[15]+m[13];
                float qcam[3] = { eye_x, eye_y, eye_z };
                s_qt_async.KickAsync(qcam, qfp, s_qt_lod_distances);
                int qn = 0;
                const TerrainQuadVisibleNode* qnodes = s_qt_async.ActiveVisible(qn);
                for (int i = 0; i < qn; ++i) {
                    float ndx = qcam[0] - (qnodes[i].origin_x + qnodes[i].size * 0.5f);
                    float ndz = qcam[2] - (qnodes[i].origin_z + qnodes[i].size * 0.5f);
                    float ndist = sqrtf(ndx * ndx + ndz * ndz);
                    float morph_t = TerrainQuadtreeRenderer::ComputeMorphT(
                        ndist, qnodes[i].depth, s_qt_lod_distances);
                    // fog_far=60000m / fog_near=0: same aerial-altitude fog
                    // override as the backdrop draw above (see its comment);
                    // fog_color matches the sky's own horizon colour.
                    static const float kFogColor[3] = { kSkyR, kSkyG, kSkyB };
                    s_qt.Draw(rp, cmd, vp.m,
                        qnodes[i].origin_x, qnodes[i].origin_z, qnodes[i].size, morph_t,
                        s_qt_height_min, s_qt_height_max,
                        sun, eye_x, eye_y, eye_z,
                        WCX, WCZ, W2UV,
                        60000.f, kFogColor, 0.f,
                        s_terrain);
                }
            }
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

    // HUD — top-left
    ImGui::SetCursorScreenPos({origin.x+8, origin.y+8});
    ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,255,200,220));
    int cur_zx = (int)(s_cx / CHUNK_SIZE);
    int cur_zy = (int)(s_cz / CHUNK_SIZE);
    // Both terrain tiers build synchronously inside the loader thread before
    // s_master_ready is set (Phase 8 — no more multi-frame chunk-build
    // sweep), so by the time this HUD renders both are already ready.
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

void TeleportToZone(int zone_x, int zone_z) {
    static constexpr float ATLAS_MAX = 63.f * CHUNK_SIZE;
    s_cx = ((float)zone_x + 0.5f) * CHUNK_SIZE;
    s_cz = ((float)zone_z + 0.5f) * CHUNK_SIZE;
    if (s_cx < 0.f) s_cx = 0.f; if (s_cx > ATLAS_MAX) s_cx = ATLAS_MAX;
    if (s_cz < 0.f) s_cz = 0.f; if (s_cz > ATLAS_MAX) s_cz = ATLAS_MAX;
}

// Repurposed for Phase 8's autonomy-system callers (md.editor_terrain_op
// ("chunks_loaded")): no more literal 4096-chunk sweep to report progress
// on (see s_build_synth_hmap's doc comment) — both values collapse to a
// simple ready/not-ready pair now that both tiers build synchronously.
int GetChunksLoaded() { return s_master_ready.load() ? 1 : 0; }
int GetChunksTotal()  { return 1; }

void SetCameraPos(float x, float y, float z, float yaw, float pitch) {
    s_cx = x; s_cy = y; s_cz = z;
    s_yaw = yaw; s_pitch = pitch;
}

} // namespace WorldEditor3D_SDLGPU

#endif // MD_SDL_GPU
