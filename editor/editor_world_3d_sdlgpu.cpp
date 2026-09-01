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
#include <monkey_dust/render/terrain_world_heightmap.h>
#include <monkey_dust/render/terrain_shading_projected.h>
#include <monkey_dust/render/terrain_vt_page_cache.h>
#include <monkey_dust/render/terrain_quadtree_renderer.h>
#include <monkey_dust/world/terrain_quadtree.h>
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
#include <algorithm>
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

// GEOCLIPMAP terrain geometry (plan: /home/rdga1/.claude/plans/serene-
// pondering-teapot.md), Phase 8 -- THE editor World3D viewport's sole
// terrain renderer. Replaces the flat-patch-grid system (s_granite_grid/
// s_granite_pr, "Granite terrain migration", deleted this phase after
// Phase 5-7's dual-run A/B confirmed pixel-identical output + ~21% lower
// GPU-ms), which had itself replaced the far backdrop mesh + near-tier
// TerrainQuadtree. No per-frame visible-patch list to cull/batch here --
// each of the 8 clip levels just draws its full fixed-topology mesh via
// TerrainClipmapRenderer::DrawLevel every frame.
//
// The editor's free-fly camera already lives in ABSOLUTE Kenshi metres
// (s_cx/s_cy/s_cz, see handle_input's ATLAS_MAX clamp) -- the SAME
// [0,extent) convention TerrainWorldHeightmap uses natively, so unlike
// the game side (SceneRender::GraniteAbsCam) no local-to-absolute camera
// translation is needed here at all.
// VT page-grid unit, metres -- purely a page-cache/debug-tool concept now
// (matched the deleted TerrainPatchGrid's patch size when that system fed
// VT's visibility; kept as a plain constant since terrain_vt_page_cache.h's
// page grid is still measured in these units).
static constexpr float kGranitePatchSize = 300.f;
static TerrainWorldHeightmap s_granite_hmap;
// Variant A (screen-space decoupled shading) -- matches the game's sole
// terrain shading path (game/src/render/scene_render.h's own field,
// TerrainShadingProjected class doc comment for the full history/why).
// The editor's World3D viewport previously kept the forward path
// (TerrainPatchRenderer::DrawBatch, Variant B) after the game switched --
// this consolidates both onto ONE shading path so there's only one to
// reason about/fix, per direct user request.
static TerrainShadingProjected s_terrain_shading;
static bool  s_granite_ready = false;

// terrain-vt Phase 1/2 -- see VtDebugFill/VtDebugDump's doc comment
// (editor_world_3d_sdlgpu.h). Its RequestPage/FlushFillQueue feed
// (previously driven by s_granite_grid's per-frame visibility loop) has
// no call site since this cutover, same as the game side -- see
// scene_render.h's terrain_vt_cache doc comment for why this was already
// fully inert before the cutover (shading resolve never actually reads
// the VT atlas/indirection, per a 2026-08-09 user directive baked into
// shaders/terrain_shading_screenspace.frag).
static TerrainVtPageCache s_vt_cache;
static bool s_vt_cache_ready = false;

// Ogre-quadtree (geomorph+skirts) terrain -- THE sole geometry system.
// No per-frame LOD-update step (unlike the old patch-grid's UpdateLOD):
// SelectVisible is called directly at draw time from camera+frustum.
static TerrainQuadtree         s_quadtree;
static TerrainQuadtreeRenderer s_quadtree_renderer;
static bool s_quadtree_ready = false;

// Builds/rebuilds the static world heightmap from TerrainAtlas's CURRENT
// contents -- called once at Init() and again whenever s_terrain_dirty's
// debounce fires (terrain edited) or the R key forces a refresh, mirroring
// the old s_build_synth_hmap's call sites exactly. Unlike that mesh
// rebuild (CPU triangulation loop), this re-inits the GPU texture
// (Shutdown+Init) from scratch -- the whole point of a single static
// texture is that this is the ONLY rebuild needed, no window/mesh to
// also re-triangulate.
static void s_rebuild_granite_hmap() {
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    s_granite_hmap.Shutdown(dev);
    bool hmap_ok = s_granite_hmap.Init(dev);
    s_granite_ready = hmap_ok;
    if (hmap_ok) {
        // terrain-vt Phase 2: init the page cache alongside the rest of
        // the terrain pipeline -- ONCE only (this function can re-run on
        // terrain edits/R-key refresh; re-Init()ing the cache each time
        // would either leak or need an explicit Shutdown+Init cycle for
        // no benefit, since a stale cached page after an edit is a known,
        // deferred problem -- Phase 7's real invalidation, not something
        // to half-solve here).
        if (!s_vt_cache_ready) s_vt_cache_ready = s_vt_cache.Init(dev, kGranitePatchSize, s_granite_hmap);
        s_granite_ready = s_granite_ready && s_quadtree_renderer.IsReady();

        // Ogre-quadtree needs real height data (relief-based skirt depth),
        // so Init here alongside the rest of the heightmap-dependent setup
        // (s_quadtree_renderer.Init() itself already ran earlier on this
        // same loader thread -- data-independent pipeline/mesh setup).
        if (!s_quadtree_ready && s_granite_ready) {
            // #400 (2026-08-23): 3.0->2.0, matches scene_render.cpp's own
            // tuned value -- see that call site's doc comment for the A/B
            // measurement (-23% node count, no measurable FPS/visual cost).
            s_quadtree.Init(0.f, 0.f, s_granite_hmap.WorldExtent(), CHUNK_SIZE,
                             /*max_depth=*/3, /*detail_multiplier=*/2.0f,
                             TerrainAtlas_SampleWorld);
            s_quadtree_ready = true;
        }
    }
}

// Per-zone (64x64=4096) ground-layer lookup, built once here and uploaded to
// TerrainRenderer's zone_layers_ssbo_ (see UploadZoneGroundLayers) --
// TerrainPatchRenderer/Granite's terrain_patch.frag reads this unconditionally
// for its live cliff-layer sampling (SampleZoneCliffColor), not gated behind
// any per-batch flag (the old use_zone_lookup toggle belonged to the now-
// removed dead draw pipeline, see terrain_renderer.h's class doc comment).
static void s_build_zone_ground_layers() {
    // Stride 9 (was 8, task terrain-brightness) -- see scene_render.cpp's
    // matching loop for slot 8 (brightness_fix) rationale.
    static uint32_t s_layers[64 * 64 * 9];
    for (int zy = 0; zy < 64; ++zy) {
        for (int zx = 0; zx < 64; ++zx) {
            const BiomeDef& b = TerrainGen_ResolveBiome(zx, zy);
            int idx = (zy * 64 + zx) * 9;
            s_layers[idx + 0] = (uint32_t)b.tex_base;
            s_layers[idx + 1] = (uint32_t)b.tex_slope;
            s_layers[idx + 2] = (uint32_t)b.tex_cliff;
            s_layers[idx + 3] = (uint32_t)b.tex_grass;
            s_layers[idx + 4] = (uint32_t)b.tex_dirt;
            s_layers[idx + 5] = (uint32_t)b.tex_road;
            float ctx = b.cliff_tiling_x, cty = b.cliff_tiling_y;
            memcpy(&s_layers[idx + 6], &ctx, sizeof(uint32_t));
            memcpy(&s_layers[idx + 7], &cty, sizeof(uint32_t));
            float bf = b.brightness_fix;
            memcpy(&s_layers[idx + 8], &bf, sizeof(uint32_t));
        }
    }
    s_terrain.UploadZoneGroundLayers(s_layers, 64 * 64 * 9);
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
        // One-time pipeline+mesh init for the quadtree renderer (replaces
        // the old backdrop pipeline above) -- doesn't depend on terrain
        // data, never needs rebuilding, unlike the heightmap texture
        // (s_rebuild_granite_hmap).
        s_quadtree_renderer.Init(md::GpuDevice::Get().SDLDevice());
        // Placeholder size -- DrawImGui's ensure_rtt-adjacent EnsureSize call
        // resizes this to the real viewport dims on the first frame the
        // panel is actually shown (this thread doesn't know the ImGui
        // panel's size yet, same reason ensure_rtt itself isn't called here).
        s_terrain_shading.Init(md::GpuDevice::Get().SDLDevice(), 64, 64);
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
        // init call. See game/src/render/scene_render.cpp's InitGroundBaked/
        // InitBiomeBlend/InitAlbedoBake call sites (~line 264-291) this
        // mirrors.
        s_terrain.InitGroundBaked("game/data/textures/md_ground_baked.dds");
        s_terrain.InitBiomeBlend("game/data/textures/md_biome_blend.png");
        s_terrain.InitOverlayMask("game/data/textures/md_overlay_mask.png");
        // Zone-layer LUT + the static world heightmap are atlas-sourced
        // (TerrainAtlas is already loaded by main.cpp before Init() is
        // called — see this file's header comment) and build
        // synchronously in a single sweep -- no per-window rebuild needed
        // at all for the heightmap (see s_rebuild_granite_hmap's doc
        // comment).
        s_build_zone_ground_layers();
        s_rebuild_granite_hmap();
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

    // audit S2 (2026-08-27, Phase 2 sanitizer follow-up): this used to only
    // join the loader thread -- none of the GPU-resource-owning statics
    // below ever had their Shutdown() called before editor_panels_shutdown()
    // (this function's only caller) returns and EditorModule::Unload()
    // dlclose()s this .so out from under them. LeakSanitizer measured a
    // real, reproducible ~34KB/75-allocation leak per hot-reload cycle
    // (textures/buffers/samplers from Init(), never freed), traced to
    // exactly this gap. Order matters: the loader thread above must finish
    // first (it can still be touching s_terrain/s_props), and GetSDLDevice()
    // must still be valid (this runs before EditorModule::Unload()'s
    // dlclose(), which is well before GpuDevice::Shutdown() at real process
    // exit -- see this function's own doc comment above for that separate,
    // already-fixed race).
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    s_quadtree_renderer.Shutdown(dev);
    s_quadtree_ready = false;
    s_vt_cache.Shutdown(dev);
    s_vt_cache_ready = false;
    s_terrain_shading.Shutdown();
    s_props.Shutdown();
    s_terrain.Shutdown();
    s_granite_hmap.Shutdown(dev);
    s_granite_ready = false;
}

// R key: force an immediate refresh of both terrain tiers (was "rebuild all
// chunks after sculpting" — the editor's World3D viewport is view-only,
// brush editing lives in EditorTerrainPanel/game F3, see CLAUDE.md's "F3
// Flythrough camera" section — this now just re-syncs both tiers with
// TerrainAtlas's current contents on demand, useful after an external edit
// to the atlas file between sessions).
static void s_force_refresh() {
    s_rebuild_granite_hmap();
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

    // Granite terrain migration, Phase 7: no per-window rebuild/re-centre
    // trigger needed at all -- the static world heightmap is always fully
    // resident, so unlike the old near-tier quadtree there is no "camera
    // drifted past N% of window width" class of check to run every frame
    // (the whole reason that class existed — and the "GPU load >80%" bug
    // it caused, bug #3 third follow-up — doesn't apply to a system with
    // no window at all).
    bool granite_rebuilt_this_frame = false;

    // Debounced terrain-dirty refresh (UploadTerrainHeightmap / R key) —
    // rebuilds the heightmap texture 0.5s after the last mark.
    if (s_terrain_dirty) {
        s_terrain_dirty_t += dt;
        if (s_terrain_dirty_t >= 0.5f) {
            s_rebuild_granite_hmap();
            s_terrain_dirty = false;
            granite_rebuilt_this_frame = true;
        }
    }

    // Idle skip: if camera and scene unchanged, reuse last RTT (LOAD_OP_CLEAR not called
    // → s_color keeps previous content → ImGui image shows last rendered frame).
    // Allow 2 stable frames before skipping so final position is fully rendered.
    // Must also stay awake the frame a heightmap rebuild happened — its new
    // texture needs an actual draw to show up in the RTT.
    {
        static float s_pcx=-1e9f,s_pcy=-1e9f,s_pcz=-1e9f,s_pyaw=-1e9f,s_ppit=-1e9f;
        static int   s_idle=0;
        bool cam_same = fabsf(s_cx-s_pcx)<0.5f && fabsf(s_cy-s_pcy)<0.5f &&
                        fabsf(s_cz-s_pcz)<0.5f && fabsf(s_yaw-s_pyaw)<0.001f &&
                        fabsf(s_pitch-s_ppit)<0.001f;
        if (cam_same && !s_terrain_dirty && !granite_rebuilt_this_frame) {
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

    // Variant A G-buffer pass -- must run in its OWN render pass, BEFORE
    // the main color pass below (TerrainShadingProjected's doc comment).
    // Nothing else in this viewport draws into the shared depth before or
    // after terrain (s_props is loaded but never drawn -- see this file's
    // RenderFrame), so unlike game/src/render/npc_render.cpp's
    // CullAndPrepass this doesn't need a separate depth-only Early-Z
    // prepass into the shared `s_depth`: it's still at its LOAD_OP_CLEAR
    // 1.0 when the resolve pass below runs, so its gl_FragDepth-forwarded
    // depth test trivially passes everywhere real terrain exists.
    if (s_granite_ready && s_terrain.IsReady()) {
        SDL_GPURenderPass* gbuf_pass = s_terrain_shading.BeginGBufferPass(cmd);
        if (gbuf_pass) {
            // Minimal-variant Part B (per-tile culling): same Gribb &
            // Hartmann plane extraction as MdCamera::FrustumPlanes
            // (engine/include/monkey_dust/render/md_camera.h) applied
            // directly to this viewport's already-built vp.m -- no
            // MdCamera object exists here (eye_x/y/z + vp.m is this
            // file's own camera representation), so inlined rather than
            // constructing one just to call that method.
            float frustum_planes[16];
            {
                const float* m = vp.m;
                frustum_planes[ 0]=m[3]+m[0]; frustum_planes[ 1]=m[7]+m[4]; frustum_planes[ 2]=m[11]+m[ 8]; frustum_planes[ 3]=m[15]+m[12];
                frustum_planes[ 4]=m[3]-m[0]; frustum_planes[ 5]=m[7]-m[4]; frustum_planes[ 6]=m[11]-m[ 8]; frustum_planes[ 7]=m[15]-m[12];
                frustum_planes[ 8]=m[3]-m[1]; frustum_planes[ 9]=m[7]-m[5]; frustum_planes[10]=m[11]-m[ 9]; frustum_planes[11]=m[15]-m[13];
                frustum_planes[12]=m[3]+m[1]; frustum_planes[13]=m[7]+m[5]; frustum_planes[14]=m[11]+m[ 9]; frustum_planes[15]=m[15]+m[13];
            }
            // Ogre-quadtree: static, not a stack array -- see the game-side
            // call site's own comment (npc_render_frame_prep.cpp) for why
            // kMaxNodesPublic(16384) entries must not live on the stack.
            static TerrainQuadtree::VisibleNode s_visible_nodes[TerrainQuadtree::kMaxNodesPublic];
            float cam_pos[3] = { eye_x, eye_y, eye_z };
            int qt_count = s_quadtree.SelectVisible(cam_pos, frustum_planes,
                                                      s_visible_nodes, TerrainQuadtree::kMaxNodesPublic);
            for (int i = 0; i < qt_count; ++i) {
                s_quadtree_renderer.DrawNode(gbuf_pass, cmd, s_granite_hmap, vp.m,
                    s_visible_nodes[i], eye_x, eye_y, eye_z);
            }
            SDL_EndGPURenderPass(gbuf_pass);
        }
        s_terrain_shading.EndGBufferPass();
    }

    // ── Terrain render pass ──────────────────────────────────────────────────
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd            = cmd;
    cpd.color_tex[0]      = s_color;
    cpd.depth_tex      = s_depth;
    cpd.clear_color[0] = kSkyR; cpd.clear_color[1] = kSkyG;
    cpd.clear_color[2] = kSkyB; cpd.clear_color[3] = 1.f;
    cpd.clear_depth    = 1.f;
    cpd.load_color     = false; // CLEAR, matches original exactly
    cpd.load_depth     = false; // CLEAR
    cb.BeginColorPass(cpd);
    SDL_GPURenderPass* rp = cb.SDLPass();
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
            GpuPassView pv = GpuPassView::FromRaw(rp, cmd);
            pv.BindPipeline(&s_sky_pipeline);
            pv.PushVertexUniforms(0, &sky, sizeof(sky));
            pv.PushFragmentUniforms(0, &sky, sizeof(sky));
            pv.Draw(3, 1, 0, 0);
        }

        if (s_granite_ready && s_terrain.IsReady()) {
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

            // Variant A resolve: fullscreen draw reading back the G-buffer
            // pass rasterized above (before this render pass opened).
            // fog_far=60000m / fog_near=0: same aerial-altitude fog
            // override the old forward draw used (WCX/WCZ/W2UV, an
            // absolute-space world-centre convention, is exactly this
            // file's own kWorldCenter equivalent -- the editor camera is
            // already absolute, so no COL_OX/COL_OZ-style local conversion
            // is needed the way game/src/render/npc_render.cpp needs for
            // its own granite draw).
            static const float kFogColor[3] = { kSkyR, kSkyG, kSkyB };
            s_terrain_shading.DrawShadingResolve(rp, cmd, sun, eye_x, eye_y, eye_z,
                WCX, WCZ, W2UV,
                60000.f, kFogColor, 0.f,
                s_terrain, s_vt_cache);
        }

        cb.EndPass();
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
    if ((int)W > 4 && (int)H > 4) {
        ensure_rtt((int)W, (int)H);
        // Same "before AcquireCommandBuffer" safety requirement as
        // ensure_rtt above -- see TerrainShadingProjected::EnsureSize's
        // doc comment / game/src/render/npc_render.cpp's own call site
        // for the diagnosed SIGSEGV this ordering avoids (destroying a
        // render-target texture referenced by an already-forming command
        // buffer). DrawImGui runs during the UI-build phase, strictly
        // before editor_panels_render acquires this frame's command
        // buffer, so this is the correct, already-established hook point.
        s_terrain_shading.EnsureSize(md::GpuDevice::Get().SDLDevice(), (int)W, (int)H);
    }

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

int VtDebugFill() {
    if (!s_granite_ready) return -1;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return -1;
    if (!s_vt_cache_ready) {
        s_vt_cache_ready = s_vt_cache.Init(dev, kGranitePatchSize, s_granite_hmap);
        if (!s_vt_cache_ready) return -1;
    }

    // Small neighborhood of pages around the current camera position, all
    // at tier 0 -- enough to exercise page allocation + compute dispatch +
    // indirection upload end-to-end without real visibility wiring (Phase 2).
    // terrain-vt clipmap fix: tier 0 now exercises the NEW subdivided
    // (FINE_SUBDIV x FINE_SUBDIV sub-page) path -- the most-changed code
    // path this debug helper should actually be smoke-testing, now that
    // every tier is cacheable (no more MIN_CACHEABLE_TIER floor).
    int ix0 = (int)(s_cx / kGranitePatchSize);
    int iz0 = (int)(s_cz / kGranitePatchSize);
    for (int dz = -3; dz <= 3; ++dz)
        for (int dx = -3; dx <= 3; ++dx)
            s_vt_cache.RequestPage(ix0 + dx, iz0 + dz, /*tier=*/0);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    if (!cmd) return -1;
    s_vt_cache.FlushFillQueue(dev, cmd, s_granite_hmap, s_terrain);
    // Debug-only diagnostic: fence-wait so a subsequent VtDebugDump call
    // (separate command buffer) can never race the compute writes above --
    // isolates whether a visible-content bug is really about the dispatch
    // itself vs. cross-command-buffer ordering.
    SDL_GPUFence* fence = md::GpuDevice::Get().SubmitAndAcquireFence(cmd);
    if (fence) {
        md::GpuDevice::Get().WaitForFence(fence);
        md::GpuDevice::Get().ReleaseFence(fence);
    }
    return s_vt_cache.ResidentCount();
}

bool VtDebugDump(const char* out_png_path) {
    if (!s_vt_cache_ready) return false;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (!dev) return false;
    return s_vt_cache.DebugDumpAtlas(dev, out_png_path);
}

int VtResidentCount() {
    return s_vt_cache_ready ? s_vt_cache.ResidentCount() : -1;
}

int VtEvictionCount() {
    return s_vt_cache_ready ? (int)s_vt_cache.EvictionCount() : -1;
}

} // namespace WorldEditor3D_SDLGPU

#endif // MD_SDL_GPU
