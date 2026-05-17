// md_flare_demo — Flare-style isometric RPG demo via SDL_GPU.
//
// Like Flare Empyrean Campaign:
//   • Camera follows player (WASD / arrows move player in isometric 4 dirs)
//   • Goblin NPCs patrol, detect player, chase, and melee-attack
//   • Left-click attacks goblin within melee range; goblins die and stop
//   • HP bar in title, kill count, FPS
//   • Q/E or scroll — zoom; R — reset; Escape — quit
//
// Usage:  md_flare_demo [mods_root] [mod_name] [map_name]

#include <monkey_dust/flare/flare_runtime.h>
#include <monkey_dust/flare/tile_map_2d_renderer.h>
#include <monkey_dust/flare/tile_collision.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
#include <monkey_dust/render/render_pass_graph.h>
#include <monkey_dust/render/cas_pass.h>
#include <monkey_dust/render/moc_culler.h>
#include <monkey_dust/render/oit_pass.h>
#include <monkey_dust/render/npc_gpu_culler.h>
#include <monkey_dust/render/evsm_shadow.h>
#include <monkey_dust/ecs/component_reflect.h>
#include <monkey_dust/spatial/world_bvh.h>
#include <monkey_dust/platform/math_types.h>
#include <monkey_dust/ai/sense_system.h>
#include <monkey_dust/ai/bt_system.h>
#include <monkey_dust/ai/bt_action_registry.h>
#include <monkey_dust/ai/bt_json_loader.h>
#include <monkey_dust/ai/fnv.h>
#include <monkey_dust/ai/sense_registry.h>
#include <monkey_dust/ai/squad_signal.h>
#include <monkey_dust/combat/projectile_system.h>
#include <monkey_dust/components/agent_state.h>
#include <monkey_dust/components/bt_components.h>
#include <monkey_dust/components/sense_component.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/engine_context.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/tools/hot_reload.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <signal.h>
#  include <sys/wait.h>
#endif

// ── Constants ─────────────────────────────────────────────────────────────────

static constexpr int   DEMO_MAX_NPCS     = 32;
static constexpr float LOGIC_TICK_S      = 0.1f;

// NPC speeds (tiles/s)
static constexpr float GUARD_CHASE_SPD   = 2.5f;
static constexpr float GUARD_INVEST_SPD  = 1.0f;
static constexpr float GUARD_PATROL_SPD  = 1.2f;
static constexpr float GUARD_MELEE_RANGE = 1.4f;

// Player
static constexpr float PLAYER_SPD        = 4.5f;   // tiles/s
static constexpr float PLAYER_ATK_RANGE  = 1.5f;   // melee range
static constexpr int   PLAYER_HP_MAX     = 100;
static constexpr int   PLAYER_DMG_LO     = 15;
static constexpr int   PLAYER_DMG_HI     = 35;

// NPC combat
static constexpr int   NPC_HP_INIT       = 30;
static constexpr int   NPC_DMG_LO        = 5;
static constexpr int   NPC_DMG_HI        = 12;
static constexpr float NPC_ATK_COOLDOWN  = 1.2f;   // s

// Camera
static constexpr float CAMERA_SCALE_INIT = 1.0f;   // 1 pixel = 1 atlas pixel

// Wander
static constexpr float WANDER_RADIUS     = 3.f;

// Paths
static const char* BT_JSON_PATH  = "data/bt/guard_npc.bt.json";
static const char* SENSE_JSON    = "data/ai/view_cone_sets.json";

// ── Demo state ────────────────────────────────────────────────────────────────

static entt::entity  s_player         = entt::null;
static entt::entity  s_npcs[DEMO_MAX_NPCS];
static int           s_npc_count      = 0;
static int           s_npc_hp[DEMO_MAX_NPCS]      = {};
static float         s_npc_atk_cd[DEMO_MAX_NPCS]  = {};  // per-NPC cooldown timer (s)
static int           s_kills          = 0;
static int           s_player_hp      = PLAYER_HP_MAX;
static bool          s_player_dead    = false;
static bool          s_player_moving  = false;
static float         s_player_tgt_x   = 0.f;   // click-to-move destination
static float         s_player_tgt_z   = 0.f;
static bool          s_player_has_tgt = false;
static int           s_player_atk_tgt = -1;    // index into s_npcs[] (-1 = none)
static float         s_player_atk_cd  = 0.f;   // player attack cooldown (s)
static BTSystem      s_bt_sys;
static md::EngineContext s_ctx;
static volatile bool s_reload_bt      = false;

// ── Geodata collision ─────────────────────────────────────────────────────────

// Collision layer values (FLARE geodata encoding):
//   0 = OPEN   — walkable floor
//   1 = FULL   — solid wall / cliff / object
//   2 = WATER  — water (impassable for normal move type)
//   3 = VOID   — outside map / completely blocked
static int CollisionAt(const md::flare::FlareMap& map, int col, int row) {
    if (col < 0 || col >= map.width || row < 0 || row >= map.height) return 3;
    for (int li = 0; li < map.layer_count; ++li) {
        if (map.layers[li].type == md::flare::LayerType::COLLISION)
            return (int)map.layers[li].tiles[row * md::flare::MAX_MAP_WIDTH + col];
    }
    return 0; // no collision layer — assume passable
}

// Returns true if the fractional tile position (x, z) is walkable.
// Rounds to nearest integer tile before lookup.
static bool IsPassable(const md::flare::FlareMap& map, float x, float z) {
    return CollisionAt(map, (int)floorf(x + 0.5f), (int)floorf(z + 0.5f)) == 0;
}

// ── Simple xorshift32 RNG for random damage.
static uint32_t s_rng = 0xdeadbeef;
static uint32_t RandU() {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return s_rng;
}
static int RandRange(int lo, int hi) {
    return lo + (int)(RandU() % (uint32_t)(hi - lo + 1));
}

// ── Camera-button recording ───────────────────────────────────────────────────

static constexpr int BTN_SZ     = 72;
static constexpr int BTN_MARGIN = 14;

static pid_t s_rec_pid   = -1;
static bool  s_recording = false;

// Draw camera icon into BTN_SZ×BTN_SZ RGBA8 buffer.
// Pixel layout designed at 72×72.
static void GenCamIcon(uint8_t* p, bool rec) {
    const int W = BTN_SZ;
    auto px = [&](int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (x<0||x>=W||y<0||y>=W) return;
        int i = (y*W+x)*4; p[i]=r; p[i+1]=g; p[i+2]=b; p[i+3]=255;
    };
    auto rect = [&](int x, int y, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
        for (int py=y; py<y+h; ++py)
        for (int px_=x; px_<x+w; ++px_) px(px_, py, r, g, b);
    };
    auto circ = [&](int cx, int cy, int rad, uint8_t r, uint8_t g, uint8_t b) {
        for (int py=cy-rad; py<=cy+rad; ++py)
        for (int px_=cx-rad; px_<=cx+rad; ++px_)
            if ((px_-cx)*(px_-cx)+(py-cy)*(py-cy) <= rad*rad)
                px(px_, py, r, g, b);
    };
    // Background (rounded feel via dark corners)
    rect(0, 0, W, W, 30, 30, 40);
    rect(3, 3, W-6, W-6, 45, 45, 55);
    // Camera body (scaled from 56px design × 72/56)
    rect(12, 26, 49, 31, 195, 195, 205);
    // Viewfinder notch
    rect(22, 17, 18, 12, 195, 195, 205);
    // Lens rings
    circ(36, 41, 13, 55, 55, 65);
    circ(36, 41,  9, 195, 195, 205);
    circ(36, 41,  5, 55, 55, 65);
    circ(36, 41,  3, 100, 130, 155);
    if (rec) {
        // Red dot indicator (top-right)
        circ(59, 12,  8, 180, 18, 18);
        circ(59, 12,  5, 235, 55, 55);
        // Red border (3 px)
        for (int i = 0; i < W; ++i) {
            px(i,0,195,20,20); px(i,1,195,20,20); px(i,2,195,20,20);
            px(i,W-1,195,20,20); px(i,W-2,195,20,20); px(i,W-3,195,20,20);
            px(0,i,195,20,20); px(1,i,195,20,20); px(2,i,195,20,20);
            px(W-1,i,195,20,20); px(W-2,i,195,20,20); px(W-3,i,195,20,20);
        }
    } else {
        // Subtle white border when idle
        for (int i = 0; i < W; ++i) {
            px(i,0,80,80,90); px(i,W-1,80,80,90);
            px(0,i,80,80,90); px(W-1,i,80,80,90);
        }
    }
}

// Create a BTN_SZ×BTN_SZ RGBA8 SDL_GPU texture from pixel data.
static SDL_GPUTexture* MakeCamTex(SDL_GPUDevice* dev, const uint8_t* pixels) {
    SDL_GPUTextureCreateInfo ti {};
    ti.type                 = SDL_GPU_TEXTURETYPE_2D;
    ti.format               = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ti.width                = (uint32_t)BTN_SZ;
    ti.height               = (uint32_t)BTN_SZ;
    ti.layer_count_or_depth = 1;
    ti.num_levels           = 1;
    ti.usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER;
    SDL_GPUTexture* tex = SDL_CreateGPUTexture(dev, &ti);
    if (!tex) return nullptr;

    SDL_GPUTransferBufferCreateInfo tbi {};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
    tbi.size  = (uint32_t)(BTN_SZ * BTN_SZ * 4);
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!tb) { SDL_ReleaseGPUTexture(dev, tex); return nullptr; }

    void* ptr = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (ptr) memcpy(ptr, pixels, (size_t)(BTN_SZ * BTN_SZ * 4));
    SDL_UnmapGPUTransferBuffer(dev, tb);

    SDL_GPUCommandBuffer* cmd = SDL_AcquireGPUCommandBuffer(dev);
    SDL_GPUCopyPass*      cp  = SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src {};
    src.transfer_buffer = tb;
    src.pixels_per_row  = (uint32_t)BTN_SZ;
    src.rows_per_layer  = (uint32_t)BTN_SZ;
    SDL_GPUTextureRegion dst {};
    dst.texture = tex;
    dst.w = (uint32_t)BTN_SZ;
    dst.h = (uint32_t)BTN_SZ;
    dst.d = 1;
    SDL_UploadToGPUTexture(cp, &src, &dst, false);
    SDL_EndGPUCopyPass(cp);
    SDL_SubmitGPUCommandBuffer(cmd);
    SDL_ReleaseGPUTransferBuffer(dev, tb);
    return tex;
}

static void StartRecording() {
    if (s_recording) return;
#ifndef _WIN32
    pid_t pid = fork();
    if (pid == 0) {
        // Child: redirect stderr to /dev/null, launch demo_capture --no-launch
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) { dup2(devnull, 2); close(devnull); }
        execlp("python3", "python3", "scripts/demo_capture.py",
               "--no-launch", "--record-fps", "10", nullptr);
        _exit(1);
    } else if (pid > 0) {
        s_rec_pid = pid;
        s_recording = true;
        fprintf(stderr, "[rec] Recording started (pid=%d)\n", (int)pid);
    }
#endif
}

static void StopRecording() {
    if (!s_recording) return;
#ifndef _WIN32
    if (s_rec_pid > 0) {
        kill(s_rec_pid, SIGTERM);
        // Reap child asynchronously — don't block the game loop.
        waitpid(s_rec_pid, nullptr, WNOHANG);
        s_rec_pid = -1;
    }
    s_recording = false;
    fprintf(stderr, "[rec] Recording stopped\n");
#endif
}

// ── World 3D renderer (Step 4) ────────────────────────────────────────────────
// Toggle 2D / 3D view with Tab.
// In 3D mode: orbit camera (arrow keys = rotate, scroll = zoom).

static bool s_view_3d  = false;
static float s_cam_az  = 0.f;     // orbit azimuth  (radians)
static float s_cam_el  = 0.9f;    // orbit elevation (radians, 0=horizon 1.57=top)
static float s_cam_dist = 40.f;   // distance from map center

static GpuPipeline    s_w3d_pipeline;
static GpuStaticBuffer s_w3d_vbuf;
static GpuDepthTexture s_w3d_depth;
static md::CasPass    s_cas;
static md::OitPass    s_oit;
static int            s_w3d_tri_count = 0;
static Vec3           s_w3d_target    = {0.f,0.f,0.f};  // look-at (map center, world space)
static SDL_Window*    s_w3d_window    = nullptr;

// Geometry scratch — static (BSS), not on stack.
static float s_w3d_raw_verts[md::flare::GEO_MAX_VERTS * 3];
static int   s_w3d_raw_tris [md::flare::GEO_MAX_TRIS  * 3];
static float s_w3d_flat     [md::flare::GEO_MAX_TRIS  * 9];  // expanded flat VB

static void World3DInit(SDL_Window* window,
                        const md::flare::FlareMap& map,
                        float tile_world_size = 1.f) {
    s_w3d_window = window;

    // Build indexed triangle soup.
    int nv = 0, nt = 0;
    if (!md::flare::BuildWorldGeometry(map, tile_world_size,
                                       s_w3d_raw_verts, md::flare::GEO_MAX_VERTS, nv,
                                       s_w3d_raw_tris,  md::flare::GEO_MAX_TRIS,  nt)) {
        fprintf(stderr, "[World3D] geometry build failed\n");
        return;
    }

    // Expand indexed → flat: 3 verts × float[3] per triangle.
    for (int i = 0; i < nt; ++i) {
        for (int k = 0; k < 3; ++k) {
            int vi = s_w3d_raw_tris[i * 3 + k];
            s_w3d_flat[i * 9 + k * 3 + 0] = s_w3d_raw_verts[vi * 3 + 0];
            s_w3d_flat[i * 9 + k * 3 + 1] = s_w3d_raw_verts[vi * 3 + 1];
            s_w3d_flat[i * 9 + k * 3 + 2] = s_w3d_raw_verts[vi * 3 + 2];
        }
    }
    s_w3d_tri_count = nt;

    // Upload to GPU (one-shot static buffer, GL_ARRAY_BUFFER = 0x8892 → VERTEX usage).
    const uint32_t flat_bytes = (uint32_t)(nt * 9 * sizeof(float));
    s_w3d_vbuf.Init(0x8892u, s_w3d_flat, flat_bytes);

    // Isometric world-space map center:
    //   world_z_center = (map_w-1 + map_h-1) * 0.5 * h = (W+H-2)*0.25*tsz
    const float h_half = tile_world_size * 0.5f;
    s_w3d_target = { 0.f,
                     0.f,
                     (float)(map.width + map.height - 2) * h_half * 0.5f };

    // 3D pipeline — simple MVP + height-coloured geometry.
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/world3d.vert";
    pd.frag_path = "shaders/world3d.frag";
    pd.layout.count      = 1;
    pd.layout.stride     = 12;                       // float[3]
    pd.layout.attribs[0] = { 0, 0, GpuAttribFmt::F3 };
    pd.raster.depth_test  = true;
    pd.raster.depth_write = true;
    pd.raster.cull_back   = false;  // see inside walls from any angle
    pd.has_depth_target   = true;
    pd.vert_uniform_bufs  = 1;      // WorldUBO (MVP matrix)
    if (!s_w3d_pipeline.Create(pd))
        fprintf(stderr, "[World3D] pipeline create failed\n");
    else
        fprintf(stdout, "[World3D] ready: %d tris, %.1f KB VB\n",
                nt, flat_bytes / 1024.f);

    // CAS post-process pass — actual viewport size passed later in World3DRender.
    s_cas.Init(1, 1, 0.5f);

    // OIT pass — investigation: composite pipeline created FIRST (before accum).
    // Testing if Intel HD 520 driver crash was order-dependent.
    s_oit.Init(1, 1);

    // MaskedOcclusionCulling — CPU occlusion culling for NPC visibility.
    md::MocCuller::Get().Init(320, 160);

    // GPU frustum culling via Vulkan compute (3D mode).
    md::NpcGpuCuller::Get().Init();

    // EVSM shadow infrastructure (used by the main game's forward pass).
    // Demo: init with smaller map (512) since no actual shadow casters here.
    md::EvsmShadow::Get().Init(512, 40.f);
}

static void World3DRender(int vp_w, int vp_h, float dt) {
    if (s_w3d_tri_count == 0 || !s_w3d_pipeline.SDLPipeline()) return;

    // Orbit camera: arrow keys update azimuth/elevation each frame.
    const bool* ks = SDL_GetKeyboardState(nullptr);
    if (ks[SDL_SCANCODE_LEFT])  s_cam_az -= 1.4f * dt;
    if (ks[SDL_SCANCODE_RIGHT]) s_cam_az += 1.4f * dt;
    if (ks[SDL_SCANCODE_UP])    s_cam_el  = fminf(s_cam_el + 1.0f * dt, 1.55f);
    if (ks[SDL_SCANCODE_DOWN])  s_cam_el  = fmaxf(s_cam_el - 1.0f * dt, 0.05f);

    // Eye position from orbit angles.
    Vec3 eye = {
        s_w3d_target.x + s_cam_dist * cosf(s_cam_el) * sinf(s_cam_az),
        s_w3d_target.y + s_cam_dist * sinf(s_cam_el),
        s_w3d_target.z - s_cam_dist * cosf(s_cam_el) * cosf(s_cam_az),
    };
    Vec3 up = {0.f, 1.f, 0.f};

    Mat4 view = mat4_lookat(eye, s_w3d_target, up);
    Mat4 proj = mat4_perspective(0.80f,
                                  (float)vp_w / (float)vp_h,
                                  0.1f, 500.f);
    Mat4 mvp  = mat4_mul(proj, view);

    // ── MocCuller: render world occluders, then NPC visibility is tested
    // in the NPC sprite loop (above, before SetNpcSprites is called).
    // We call BeginFrame here so the depth buffer is fresh for this frame.
    // RenderOccluders submits ground tiles as occluders for the NPC test.
    if (md::MocCuller::Get().IsReady() && s_w3d_tri_count > 0) {
        md::MocCuller::Get().BeginFrame(mat4_ptr(mvp), vp_w, vp_h);
        // Use only ground/water triangles (first quarter of flat buffer) as occluders.
        // Wall prisms are occluders too but we only need approximate coverage.
        int occ_tri_count = s_w3d_tri_count / 4;  // sample subset for speed
        if (occ_tri_count > 0) {
            // Expand flat buffer to indexed form for MOC (build index array).
            static uint32_t s_occ_idx[1024 * 3];
            int n = occ_tri_count < 1024 ? occ_tri_count : 1024;
            for (int i = 0; i < n * 3; ++i) s_occ_idx[i] = (uint32_t)i;
            md::MocCuller::Get().RenderOccluders(s_w3d_flat, n * 3, s_occ_idx, n);
        }
    }

    // Depth texture — recreate on viewport resize.
    if (s_w3d_depth.Width() != vp_w || s_w3d_depth.Height() != vp_h) {
        s_w3d_depth.Shutdown();
        s_w3d_depth.Init(vp_w, vp_h);
    }

    // OIT manual-blend composite requires scene_tex != output_tex (Vulkan constraint).
    // Route scene through an intermediate (cas.SceneTex()) whenever OIT or CAS is active.
    const bool use_cas = s_cas.IsReady() &&
                         md::RenderPassGraph::Get().IsEnabled("cas_sharpening");
    const bool use_oit_check = s_oit.IsReady() &&
                               md::RenderPassGraph::Get().IsEnabled("oit_transparency");
    const bool need_intermediate = use_cas || use_oit_check;

    // Resize CAS/OIT intermediate textures if viewport changed.
    if (need_intermediate && s_cas.IsReady()) s_cas.Resize(vp_w, vp_h);

    // Acquire command buffer + swapchain texture.
    SDL_GPUCommandBuffer* cmd = md::GpuDevice::Get().AcquireCommandBuffer();
    SDL_GPUTexture* swap = nullptr;
    if (!SDL_AcquireGPUSwapchainTexture(cmd, s_w3d_window, &swap, nullptr, nullptr)
        || !swap) {
        md::GpuDevice::Get().Submit(cmd);
        return;
    }

    // When OIT or CAS is active, scene always goes to cas.SceneTex() so OIT
    // composite can read scene_target (cas.SceneTex()) and write to swap without
    // the Vulkan same-texture read+write restriction.
    SDL_GPUTexture* scene_target = (need_intermediate && s_cas.IsReady()) ? s_cas.SceneTex() : swap;
    SDL_GPUTexture* scene_depth  = (need_intermediate && s_cas.IsReady()) ? s_cas.DepthTex() : s_w3d_depth.SDLTexture();

    // Scene render pass.
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd            = cmd;
    cpd.color_tex      = scene_target;
    cpd.depth_tex      = scene_depth;
    cpd.clear_color[0] = 0.12f;
    cpd.clear_color[1] = 0.16f;
    cpd.clear_color[2] = 0.24f;
    cpd.clear_color[3] = 1.0f;
    cpd.load_color     = false;
    cpd.load_depth     = false;
    cb.BeginColorPass(cpd);

    cb.BindPipeline(&s_w3d_pipeline);
    SDL_GPUBufferBinding vb { s_w3d_vbuf.SDLBuffer(), 0u };
    SDL_BindGPUVertexBuffers(cb.SDLPass(), 0, &vb, 1);
    cb.PushVertexUniforms(0, mat4_ptr(mvp), 64);
    cb.Draw((uint32_t)(s_w3d_tri_count * 3));
    cb.EndPass();

    // OIT pass: transparent geometry (water, leaves, particles) over opaque scene.
    // Must run BEFORE CAS apply so scene is still in scene_target (cas.SceneTex())
    // and we can pass it as scene_tex to OIT Composite (scene_tex != output=swap).
    const bool use_oit = use_oit_check;
    if (use_oit) {
        s_oit.Resize(vp_w, vp_h);

        // Demo: 8 semi-transparent quads at fixed world positions (simulate leaves).
        // Each quad = 2 triangles = 6 verts × 28 bytes.
        struct OitVert { float x,y,z; float r,g,b,a; };
        static OitVert s_oit_verts[6 * 8];
        static uint32_t s_oit_idx[6 * 8];
        int nv = 0;

        auto push_quad = [&](float wx, float wz, float wy,
                              float r, float g, float b, float a) {
            const float hs = 0.6f;
            OitVert quad[4] = {
                {wx-hs, wy, wz-hs, r,g,b,a},
                {wx+hs, wy, wz-hs, r,g,b,a},
                {wx+hs, wy, wz+hs, r,g,b,a},
                {wx-hs, wy, wz+hs, r,g,b,a},
            };
            // Two triangles: 0-1-2, 0-2-3
            int b0 = nv;
            for (int k = 0; k < 4; ++k) s_oit_verts[nv++] = quad[k];
            (void)b0;
        };

        // Place quads near the goblin camp centre in 3D isometric world coords.
        // World: x=(col-row)*0.5, z=(col+row)*0.5 with tile_world_size=1.
        push_quad( 3.f, 13.f, 1.0f, 0.2f, 0.8f, 0.2f, 0.5f);  // green leaves
        push_quad(-2.f, 14.f, 0.8f, 0.3f, 0.7f, 0.1f, 0.4f);
        push_quad( 5.f, 16.f, 1.2f, 0.2f, 0.6f, 0.3f, 0.6f);
        push_quad( 0.f, 18.f, 0.9f, 0.1f, 0.9f, 0.2f, 0.35f);
        push_quad(-4.f, 20.f, 1.1f, 0.3f, 0.7f, 0.15f, 0.45f);
        push_quad( 2.f, 22.f, 0.7f, 0.15f,0.8f, 0.25f, 0.55f);
        push_quad(-1.f, 12.f, 1.3f, 0.2f, 0.75f,0.3f, 0.5f);
        push_quad( 4.f, 19.f, 1.0f, 0.25f,0.7f, 0.2f, 0.4f);

        // Build index array for triangles (2 tris per quad = 6 indices per quad).
        int ni = 0;
        int nquads = nv / 4;
        for (int q = 0; q < nquads; ++q) {
            int b0 = q * 4;
            s_oit_idx[ni++] = b0+0; s_oit_idx[ni++] = b0+1; s_oit_idx[ni++] = b0+2;
            s_oit_idx[ni++] = b0+0; s_oit_idx[ni++] = b0+2; s_oit_idx[ni++] = b0+3;
        }

        // Upload vertex data via transfer buffer.
        SDL_GPUDevice* sdl_dev = md::GpuDevice::Get().SDLDevice();
        uint32_t vb_size = (uint32_t)(nv * sizeof(OitVert));

        SDL_GPUBufferCreateInfo bci{};
        bci.usage = SDL_GPU_BUFFERUSAGE_VERTEX;
        bci.size  = vb_size;
        SDL_GPUBuffer* oit_vbuf = SDL_CreateGPUBuffer(sdl_dev, &bci);

        SDL_GPUTransferBufferCreateInfo tci{};
        tci.usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
        tci.size  = vb_size;
        SDL_GPUTransferBuffer* oit_tbuf = SDL_CreateGPUTransferBuffer(sdl_dev, &tci);
        void* ptr = SDL_MapGPUTransferBuffer(sdl_dev, oit_tbuf, false);
        if (ptr) memcpy(ptr, s_oit_verts, vb_size);
        SDL_UnmapGPUTransferBuffer(sdl_dev, oit_tbuf);

        // Copy pass: transfer → vertex buffer.
        SDL_GPUCopyPass* cp = SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTransferBufferLocation src_loc{ oit_tbuf, 0 };
        SDL_GPUBufferRegion dst_reg{ oit_vbuf, 0, vb_size };
        SDL_UploadToGPUBuffer(cp, &src_loc, &dst_reg, false);
        SDL_EndGPUCopyPass(cp);
        SDL_ReleaseGPUTransferBuffer(sdl_dev, oit_tbuf);

        // OIT accumulation pass.
        SDL_GPURenderPass* oit_rp = s_oit.BeginAccum(cmd, nullptr);
        if (oit_rp && s_oit.AccumPipeline()) {
            SDL_BindGPUGraphicsPipeline(oit_rp, s_oit.AccumPipeline());

            // Push MVP + View uniforms (slot 0 = set=1, binding=0).
            struct OitUBO { float mvp[16]; float view[16]; } ubo;
            memcpy(ubo.mvp,  mat4_ptr(mvp),  64);
            memcpy(ubo.view, mat4_ptr(view), 64);
            SDL_PushGPUVertexUniformData(cmd, 0, &ubo, sizeof(ubo));

            SDL_GPUBufferBinding vb_bind{ oit_vbuf, 0 };
            SDL_BindGPUVertexBuffers(oit_rp, 0, &vb_bind, 1);
            SDL_DrawGPUPrimitives(oit_rp, (uint32_t)ni, 1, 0, 0);
        }
        s_oit.EndAccum();
        SDL_ReleaseGPUBuffer(sdl_dev, oit_vbuf);

        // OIT manual composite: reads scene_target (cas.SceneTex()) + accum_tex_,
        // writes merged result to swap. No same-texture conflict.
        // CAS sharpening is skipped this frame (swap already has final output).
        s_oit.Composite(cmd, scene_target, swap, vp_w, vp_h);
    } else {
        // No OIT: apply CAS sharpening normally (scene_target → swap).
        if (use_cas) s_cas.Apply(cmd, swap, vp_w, vp_h);
    }

    md::GpuDevice::Get().Submit(cmd);
}

static void World3DShutdown() {
    md::EvsmShadow::Get().Shutdown();
    md::NpcGpuCuller::Get().Shutdown();
    md::MocCuller::Get().Shutdown();
    md::WorldBVH::Get().Shutdown();
    s_oit.Shutdown();
    s_cas.Shutdown();
    s_w3d_depth.Shutdown();
    s_w3d_vbuf.Shutdown();
    s_w3d_pipeline.Destroy();
}

// ── Repo root ─────────────────────────────────────────────────────────────────

static void ChdirToRepoRoot() {
#ifdef _WIN32
    char exe[512] = {};
    DWORD n = GetModuleFileNameA(nullptr, exe, sizeof(exe) - 1);
    if (!n) return;
    for (int i = 0; i < 3; ++i) { char* p = strrchr(exe, '\\'); if (!p) return; *p = '\0'; }
    SetCurrentDirectoryA(exe);
#else
    char exe[512] = {};
    if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) <= 0) return;
    for (int i = 0; i < 3; ++i) { char* p = strrchr(exe, '/'); if (!p) return; *p = '\0'; }
    if (exe[0]) chdir(exe);
#endif
}

// ── Blackboard keys ───────────────────────────────────────────────────────────

static constexpr uint32_t kSX = md::fnv1a("spawn_x");
static constexpr uint32_t kSZ = md::fnv1a("spawn_z");
static constexpr uint32_t kWX = md::fnv1a("wx");
static constexpr uint32_t kWZ = md::fnv1a("wz");

// ── Move helper ───────────────────────────────────────────────────────────────

static float MoveToward(WorldTransform& wt, float tx, float tz, float speed_mps) {
    float dx = tx - wt.x, dz = tz - wt.z;
    float dist = sqrtf(dx * dx + dz * dz);
    if (dist > 0.01f) {
        float step = speed_mps * LOGIC_TICK_S / dist;
        if (step > 1.f) step = 1.f;
        wt.x += dx * step;
        wt.z += dz * step;
        wt.rot_y = atan2f(dx, dz);
    }
    return dist;
}

// ── BT action: CHASE + melee attack ──────────────────────────────────────────

static BTStatus actGuardChase(md::EngineContext&, entt::entity e) {
    auto& reg = Registry::Get();
    auto* wt  = reg.try_get<WorldTransform>(e);
    auto* sc  = reg.try_get<SenseComponent>(e);
    if (!wt || !sc) return BTStatus::Failure;

    MoveToward(*wt, sc->last_known_x, sc->last_known_z, GUARD_CHASE_SPD);

    // Melee attack: deal damage to player if within range and cooldown expired.
    if (!s_player_dead && s_player != entt::null) {
        auto* pwt = reg.try_get<WorldTransform>(s_player);
        if (pwt) {
            float ddx = pwt->x - wt->x, ddz = pwt->z - wt->z;
            float dist = sqrtf(ddx*ddx + ddz*ddz);
            for (int i = 0; i < s_npc_count; ++i) {
                if (s_npcs[i] != e) continue;
                if (dist <= GUARD_MELEE_RANGE && s_npc_atk_cd[i] <= 0.f) {
                    int dmg = RandRange(NPC_DMG_LO, NPC_DMG_HI);
                    s_player_hp -= dmg;
                    if (s_player_hp < 0) s_player_hp = 0;
                    s_npc_atk_cd[i] = NPC_ATK_COOLDOWN;
                    fprintf(stderr, "[combat] NPC hits player -%d  (HP=%d)\n", dmg, s_player_hp);
                    if (s_player_hp == 0) {
                        s_player_dead = true;
                        fprintf(stderr, "[combat] PLAYER DEAD — press R to restart\n");
                    }
                }
                break;
            }
        }
    }
    return BTStatus::Running;
}

// ── BT action: INVESTIGATE ────────────────────────────────────────────────────

static BTStatus actGuardInvestigate(md::EngineContext&, entt::entity e) {
    auto& reg = Registry::Get();
    auto* wt  = reg.try_get<WorldTransform>(e);
    auto* sc  = reg.try_get<SenseComponent>(e);
    if (!wt || !sc) return BTStatus::Failure;
    MoveToward(*wt, sc->last_known_x, sc->last_known_z, GUARD_INVEST_SPD);
    return BTStatus::Running;
}

// ── BT action: PATROL ─────────────────────────────────────────────────────────

static BTStatus actGuardPatrol(md::EngineContext& ctx, entt::entity e) {
    auto& reg = Registry::Get();
    auto* wt  = reg.try_get<WorldTransform>(e);
    auto* ab  = reg.try_get<AgentBlackboard>(e);
    if (!wt || !ab) return BTStatus::Failure;

    float sx = bb_get_float(*ab, kSX, wt->x);
    float sz = bb_get_float(*ab, kSZ, wt->z);
    float tx = bb_get_float(*ab, kWX, sx);
    float tz = bb_get_float(*ab, kWZ, sz);

    float dist = MoveToward(*wt, tx, tz, GUARD_PATROL_SPD);

    if (dist < 0.3f) {
        uint32_t r = ctx.frame_index * 2654435761u ^ static_cast<uint32_t>(entt::to_integral(e));
        float angle  = (float)((r & 0xFFu)) / 255.f * 6.28318f;
        float radius = (float)(((r >> 8) & 0xFFu)) / 255.f * WANDER_RADIUS;
        bb_set_float(*ab, kWX, sx + cosf(angle) * radius);
        bb_set_float(*ab, kWZ, sz + sinf(angle) * radius);
    }
    return BTStatus::Running;
}

// ── HotReload ─────────────────────────────────────────────────────────────────

static void OnBTFileChanged(const char*) { s_reload_bt = true; }

// ── BT setup ──────────────────────────────────────────────────────────────────

static void RegisterDemoActions() {
    auto& r = md::BTActionRegistry::Get();
    r.Clear();
    r.RegisterAction("actGuardChase",       actGuardChase);
    r.RegisterAction("actGuardInvestigate", actGuardInvestigate);
    r.RegisterAction("actGuardPatrol",      actGuardPatrol);
}

static void LoadNpcBT(BehaviorTree& bt) {
    RegisterDemoActions();
    if (!BTJsonLoader::LoadFromFile(bt, BT_JSON_PATH))
        fprintf(stderr, "[demo] BT load failed: %s\n", BT_JSON_PATH);
}

static void RespawnNpcBT(entt::entity e) {
    auto& reg = Registry::Get();
    auto* old = reg.try_get<BehaviorTreeComponent>(e);
    if (old && old->owning && old->tree) { delete old->tree; old->tree = nullptr; }
    auto* tree = new BehaviorTree();
    LoadNpcBT(*tree);
    auto& btc = reg.emplace_or_replace<BehaviorTreeComponent>(e);
    btc.tree = tree; btc.owning = true; btc.enabled = true;
}

// ── Spawn ─────────────────────────────────────────────────────────────────────

static void SpawnDemoEntities(const md::flare::FlareRuntime& rt) {
    auto& reg       = Registry::Get();
    const auto& map = rt.GetMap();

    // Player — spawn at map's hero_pos, but for this demo use a position
    // that's in the middle of the goblin camp (near NPC spawn groups).
    // goblin_camp: hero_pos=5,2 is a dead-end corner; spawn at (18,26)
    // which is passable and surrounded by the first two goblin groups.
    s_player = reg.create();
    auto& pas = reg.emplace<AgentState>(s_player);
    pas.lcflags.set(lcf::IS_PLAYER);
    auto& pwt = reg.emplace<WorldTransform>(s_player);
    pwt.x = 18.f;
    pwt.z = 26.f;
    pwt.y = 0.f; pwt.rot_y = 0.f;
    s_player_hp      = PLAYER_HP_MAX;
    s_player_dead    = false;
    s_player_moving  = false;
    s_player_has_tgt = false;
    s_player_atk_tgt = -1;
    s_player_atk_cd  = 0.f;
    s_kills = 0;

    // NPCs from Flare [enemy] spawns.
    s_npc_count = 0;
    for (int i = 0; i < map.spawn_count && s_npc_count < DEMO_MAX_NPCS; ++i) {
        const auto& sp = map.spawns[i];
        int n = (sp.number_min < 1 ? 1 : sp.number_min);
        for (int j = 0; j < n && s_npc_count < DEMO_MAX_NPCS; ++j) {
            entt::entity e = reg.create();
            int idx = s_npc_count++;
            s_npcs[idx]        = e;
            s_npc_hp[idx]      = NPC_HP_INIT;
            s_npc_atk_cd[idx]  = 0.f;

            reg.emplace<AgentState>(e);
            auto& ab = reg.emplace<AgentBlackboard>(e);
            reg.emplace<SquadMemberComponent>(e).squad_id = 0;

            float spx = sp.center_x + (float)j * 0.8f;
            float spz = sp.center_y + (float)j * 0.8f;

            auto& wt = reg.emplace<WorldTransform>(e);
            wt.x = spx; wt.z = spz; wt.y = 0.f; wt.rot_y = 0.f;

            bb_set_float(ab, kSX, spx);
            bb_set_float(ab, kSZ, spz);

            auto& sc = reg.emplace<SenseComponent>(e);
            sc.cone_set_idx = 0;
            sc.threshold_lo = 0.3f;
            sc.threshold_hi = 0.7f;
            for (int s = 0; s < MAX_SENSES; ++s) {
                sc.activation[s]        = 0.f;
                sc.last_activated_ms[s] = 0u;
            }
            sc.last_known_x = 0.f;
            sc.last_known_z = 0.f;

            RespawnNpcBT(e);
        }
    }
    fprintf(stderr, "[demo] Player at (%.0f,%.0f) | %d NPCs from %d spawn entries\n",
            pwt.x, pwt.z, s_npc_count, map.spawn_count);
}

static void DestroyDemoEntities() {
    auto& reg = Registry::Get();
    for (int i = 0; i < s_npc_count; ++i)
        if (reg.valid(s_npcs[i])) reg.destroy(s_npcs[i]);
    if (s_player != entt::null && reg.valid(s_player)) reg.destroy(s_player);
    s_npc_count = 0;
    s_player    = entt::null;
}

// ── Logic tick (10 TPS) ───────────────────────────────────────────────────────

static void LogicTick(float now_ms) {
    ++s_ctx.logic_tick;
    ++s_ctx.frame_index;
    s_ctx.delta_time = LOGIC_TICK_S;
    s_ctx.now_s      = now_ms * 0.001f;

    SquadSignalBus::Get().ClearAll();
    SenseSystemUpdate(now_ms);
    s_bt_sys.Tick(s_ctx, Registry::Get(), static_cast<uint32_t>(now_ms));
    md::ProjectileSystem::Get().Tick(LOGIC_TICK_S);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    ChdirToRepoRoot();

    const char* mods_root = (argc > 1) ? argv[1] : "third_party/flare-game/mods";
    const char* mod_name  = (argc > 2) ? argv[2] : "empyrean_campaign";
    const char* map_name  = (argc > 3) ? argv[3] : "maps/goblin_camp.txt";

    // SDL3
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[demo] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }
    const int WIN_W = 1280, WIN_H = 720;
    SDL_Window* window = SDL_CreateWindow("md_flare_demo", WIN_W, WIN_H, SDL_WINDOW_RESIZABLE);
    if (!window) { fprintf(stderr, "[demo] Window failed\n"); SDL_Quit(); return 1; }

    if (!md::GpuDevice::Get().Init(window)) {
        fprintf(stderr, "[demo] GpuDevice failed\n");
        SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }
    fprintf(stderr, "[demo] GPU: %s\n", md::GpuDevice::Get().DriverName());

    // Camera-button textures (idle + recording state).
    SDL_GPUDevice*  sdl_dev      = md::GpuDevice::Get().SDLDevice();
    SDL_GPUTexture* cam_idle_tex = nullptr;
    SDL_GPUTexture* cam_rec_tex  = nullptr;
    {
        uint8_t px[BTN_SZ * BTN_SZ * 4] = {};
        GenCamIcon(px, false); cam_idle_tex = MakeCamTex(sdl_dev, px);
        GenCamIcon(px, true);  cam_rec_tex  = MakeCamTex(sdl_dev, px);
    }

    if (!SenseRegistry::Get().Load(SENSE_JSON))
        fprintf(stderr, "[demo] No sense cones — NPCs use fallback\n");

    // Load Flare mod + map.
    auto& rt = md::flare::FlareRuntime::Get();
    if (!rt.LoadMod(mod_name, mods_root, map_name)) {
        fprintf(stderr, "[demo] LoadMod failed: %s / %s / %s\n", mods_root, mod_name, map_name);
        md::GpuDevice::Get().Shutdown(); SDL_DestroyWindow(window); SDL_Quit(); return 1;
    }
    const auto& map = rt.GetMap();
    fprintf(stderr, "[demo] Map: %s  (%dx%d)\n", map.title, map.width, map.height);

    // Tile renderer.
    auto& tmr2d = md::flare::TileMap2DRenderer::Get();
    tmr2d.Init();
    tmr2d.SetAtlases(map);

    // Load goblin sprite sheet (fantasycore).
    tmr2d.SetNpcSpriteSheet(
        "third_party/flare-game/mods/fantasycore/images/enemies/goblin.png");

    // ECS + BT.
    BTSystem::ConnectRegistry(Registry::Get());
    SpawnDemoEntities(rt);

    // ── Component reflection ──────────────────────────────────────────────────
    md::RegisterCoreComponents();

    HotReload::Get().Watch(BT_JSON_PATH, OnBTFileChanged);
    HotReload::Get().Start(500);

    // ── Render pass graph ─────────────────────────────────────────────────────
    {
        auto& rpg = md::RenderPassGraph::Get();
        rpg.Register("tiles_2d",  true);   // 2D Flare isometric renderer
        rpg.Register("npc_sprites",     true);   // goblin sprite overlay
        rpg.Register("evsm_shadows",    true);   // EVSM soft shadow pass (before world_3d)
        rpg.Register("world_3d",        false);  // 3D geometry view (Tab toggle)
        rpg.Register("overlay",         true);   // camera button + UI overlays
        rpg.Register("cas_sharpening",  true);   // CAS post-process (3D mode)
        // OIT split into two sub-passes for correct resource tracking:
        //   oit_accum     — transparent geometry → writes oit_accum_tex
        //   oit_composite — reads oit_accum_tex → writes to swapchain
        rpg.Register("oit_accum",      true);   // OIT accumulation pass
        rpg.Register("oit_composite",  true);   // OIT composite pass
        rpg.Register("oit_transparency",true);  // legacy combined alias (backward compat)
        rpg.LoadFromJSON("data/render_settings.json");

        // ── Resource dependency declarations (Step 7 — FrameGraph) ────────────
        // Declares data-flow between passes.  SDL_GPU handles VkImageLayout
        // barriers internally; this layer adds documentation + ordering validation.
        //
        // 3D pipeline:  world_3d → [scene_color] → cas_sharpening → swapchain
        // OIT pipeline: oit_accum → [oit_accum_tex] → oit_composite → swapchain
        rpg.DeclareWrite("evsm_shadows",  "evsm_moment_tex");  // shadow pass
        rpg.DeclareRead ("world_3d",      "evsm_moment_tex");  // main pass reads shadow
        rpg.DeclareWrite("world_3d",      "scene_color");
        rpg.DeclareRead ("cas_sharpening","scene_color");
        rpg.DeclareWrite("cas_sharpening","swapchain");
        rpg.DeclareWrite("tiles_2d",      "swapchain");
        rpg.DeclareWrite("overlay",       "swapchain");
        rpg.DeclareWrite("oit_accum",     "oit_accum_tex");
        rpg.DeclareRead ("oit_composite", "oit_accum_tex");
        rpg.DeclareWrite("oit_composite", "swapchain");
        rpg.Validate();
    }

    // ── 3D world renderer + BVH for ray picking ───────────────────────────────
    World3DInit(window, map, 1.0f);
    if (s_w3d_tri_count > 0)
        md::WorldBVH::Get().Build(s_w3d_flat, s_w3d_tri_count);

    // ── Camera state ──────────────────────────────────────────────────────────
    int   vp_w = WIN_W, vp_h = WIN_H;
    float scale    = CAMERA_SCALE_INIT;
    float origin_x = 0.f, origin_y = 0.f;

    // Compute origin so camera centers on player's tile position,
    // then clamp so the map diamond fills at least half the viewport.
    auto ComputeOrigin = [&]() {
        if (s_player == entt::null) return;
        auto* pwt = Registry::Get().try_get<WorldTransform>(s_player);
        if (!pwt) return;
        origin_x = (float)vp_w * 0.5f - (pwt->x - pwt->z) * 96.f * scale;
        origin_y = (float)vp_h * 0.5f - (pwt->x + pwt->z) * 48.f * scale;

        // Clamp origin so the map diamond's AABB exactly fills the viewport
        // (diamond edge never visibly inside the screen rect).
        // Diamond pixel extents (origin = screen pos of tile 0,0):
        //   left:   origin_x - (H-1)*96*scale  (tile 0,H-1)
        //   right:  origin_x + (W-1)*96*scale  (tile W-1,0)
        //   top:    origin_y                    (tile 0,0)
        //   bottom: origin_y + (W+H-2)*48*scale (tile W-1,H-1)
        const float ext_x_l = (float)(map.height - 1) * 96.f * scale;  // left AABB half
        const float ext_x_r = (float)(map.width  - 1) * 96.f * scale;  // right AABB half
        const float full_h  = (float)(map.width + map.height - 2) * 48.f * scale;
        // Keep left diamond edge ≤ viewport left edge (0)
        origin_x = fminf(origin_x, ext_x_l);
        // Keep right diamond edge ≥ viewport right edge (vp_w)
        origin_x = fmaxf(origin_x, (float)vp_w - ext_x_r);
        // Keep top diamond edge ≤ viewport top (0)
        origin_y = fminf(origin_y, 0.f);
        // Keep bottom diamond edge ≥ viewport bottom (vp_h)
        origin_y = fmaxf(origin_y, (float)vp_h - full_h);
    };

    auto ResetCamera = [&]() {
        SDL_GetWindowSize(window, &vp_w, &vp_h);
        scale = CAMERA_SCALE_INIT;
        ComputeOrigin();
    };
    ResetCamera();

    // ── Game loop ─────────────────────────────────────────────────────────────
    float    logic_accum = 0.f;
    uint64_t frame_count = 0;
    uint64_t prev_ms     = SDL_GetTicks();
    bool     quit        = false;
    bool     prev_lmb    = false;

    while (!quit) {
        uint64_t now_ms = SDL_GetTicks();
        float dt = (float)(now_ms - prev_ms) * 0.001f;
        if (dt > 0.1f) dt = 0.1f;
        prev_ms = now_ms;
        ++frame_count;

        float scroll_y   = 0.f;
        bool  do_zoom_in = false, do_zoom_out = false, do_reset = false;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { quit = true; break; }
            if (ev.type == SDL_EVENT_MOUSE_WHEEL) scroll_y = ev.wheel.y;
            if (ev.type == SDL_EVENT_WINDOW_RESIZED)
                SDL_GetWindowSize(window, &vp_w, &vp_h);
            if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                switch (ev.key.scancode) {
                    case SDL_SCANCODE_ESCAPE: quit = true; break;
                    case SDL_SCANCODE_Q:      do_zoom_out = true; break;
                    case SDL_SCANCODE_E:      do_zoom_in  = true; break;
                    case SDL_SCANCODE_I: {
                        // Dump reflected fields of the player entity.
                        if (s_player != entt::null) {
                            auto* pwt = Registry::Get().try_get<WorldTransform>(s_player);
                            if (pwt) {
                                const auto* d = md::ComponentReflect::Get()
                                                    .Find("world_transform");
                                if (d) md::ComponentReflect::Dump(pwt, *d);
                            }
                        }
                        break;
                    }
                    case SDL_SCANCODE_TAB:
                        s_view_3d = !s_view_3d;
                        fprintf(stdout, "[demo] View: %s\n", s_view_3d ? "3D" : "2D");
                        break;
                    case SDL_SCANCODE_O:
                        md::flare::ExportWorldOBJ(map, 1.0f, "qa/world.obj");
                        break;
                    case SDL_SCANCODE_R:
                        // Restart if dead, else reset camera.
                        if (s_player_dead) {
                            DestroyDemoEntities();
                            SpawnDemoEntities(rt);
                            ResetCamera();
                        } else {
                            do_reset = true;
                        }
                        break;
                    default: break;
                }
            }
        }
        if (quit) break;

        // ── LMB: click-to-move / hold-to-move / click-to-attack / camera btn ──
        {
            float mx_f = 0.f, my_f = 0.f;
            bool  lmb       = (SDL_GetMouseState(&mx_f, &my_f) & SDL_BUTTON_LMASK) != 0;
            bool  lmb_click = lmb && !prev_lmb;
            prev_lmb = lmb;

            // Camera button occupies bottom-right corner — handle on click only.
            const int bx = vp_w - BTN_SZ - BTN_MARGIN;
            const int by = vp_h - BTN_SZ - BTN_MARGIN;
            const bool over_btn = ((int)mx_f >= bx && (int)mx_f < bx + BTN_SZ &&
                                   (int)my_f >= by && (int)my_f < by + BTN_SZ);

            if (lmb_click && over_btn) {
                s_recording ? StopRecording() : StartRecording();
            } else if (lmb_click && s_view_3d && md::WorldBVH::Get().IsBuilt()) {
                // ── 3D mode: ray pick into world geometry ─────────────────────
                // Reconstruct camera matrices for unproject.
                Vec3 eye3d = {
                    s_w3d_target.x + s_cam_dist * cosf(s_cam_el) * sinf(s_cam_az),
                    s_w3d_target.y + s_cam_dist * sinf(s_cam_el),
                    s_w3d_target.z - s_cam_dist * cosf(s_cam_el) * cosf(s_cam_az),
                };
                Mat4 view3d = mat4_lookat(eye3d, s_w3d_target, {0,1,0});
                Mat4 proj3d = mat4_perspective(0.80f, (float)vp_w/(float)vp_h, 0.1f, 500.f);
                Mat4 vp_inv = mat4_mul(
                    // Invert proj*view via separate inv (simplified: NDC→world)
                    mat4_identity(), mat4_identity());  // placeholder — use NDC unproject

                // NDC mouse position [-1, 1]
                float ndcx = (mx_f / (float)vp_w) * 2.f - 1.f;
                float ndcy = 1.f - (my_f / (float)vp_h) * 2.f;

                // Unproject: near plane point and far plane point in world space.
                // Use inverse of (proj * view) applied to NDC clip positions.
                Mat4 mvp3d = mat4_mul(proj3d, view3d);
                const float* m = mat4_ptr(mvp3d);
                // Simple unproject for ray direction using camera frustum.
                // Ray origin = eye position.
                float orig[3] = { eye3d.x, eye3d.y, eye3d.z };

                // Forward direction at NDC position (simplified: use perspective divide).
                // Proper unproject: clip_pos → divide by W → view → world.
                // For simplicity use the forward-at-pixel approach:
                //   ray_dir = normalize(pixel_world_pos - eye)
                // pixel_world_pos at near plane from NDC:
                float fov_y = 0.80f;
                float tan_half = tanf(fov_y * 0.5f);
                float aspect = (float)vp_w / (float)vp_h;
                // Camera right and up in world space from view matrix.
                // View matrix columns are right, up, -forward (row-major vs col-major).
                // Compute from eye + target.
                Vec3 fwd3d = {s_w3d_target.x - eye3d.x,
                              s_w3d_target.y - eye3d.y,
                              s_w3d_target.z - eye3d.z};
                // Normalise forward
                float fl = sqrtf(fwd3d.x*fwd3d.x + fwd3d.y*fwd3d.y + fwd3d.z*fwd3d.z);
                if (fl > 1e-6f) { fwd3d.x/=fl; fwd3d.y/=fl; fwd3d.z/=fl; }
                Vec3 worldup = {0,1,0};
                Vec3 right3d = {
                    fwd3d.y*worldup.z - fwd3d.z*worldup.y,
                    fwd3d.z*worldup.x - fwd3d.x*worldup.z,
                    fwd3d.x*worldup.y - fwd3d.y*worldup.x };
                float rl = sqrtf(right3d.x*right3d.x + right3d.y*right3d.y + right3d.z*right3d.z);
                if (rl > 1e-6f) { right3d.x/=rl; right3d.y/=rl; right3d.z/=rl; }
                Vec3 up3d = {
                    right3d.y*fwd3d.z - right3d.z*fwd3d.y,
                    right3d.z*fwd3d.x - right3d.x*fwd3d.z,
                    right3d.x*fwd3d.y - right3d.y*fwd3d.x };

                float dir[3] = {
                    fwd3d.x + right3d.x * ndcx * tan_half * aspect + up3d.x * ndcy * tan_half,
                    fwd3d.y + right3d.y * ndcx * tan_half * aspect + up3d.y * ndcy * tan_half,
                    fwd3d.z + right3d.z * ndcx * tan_half * aspect + up3d.z * ndcy * tan_half,
                };
                float dlen = sqrtf(dir[0]*dir[0]+dir[1]*dir[1]+dir[2]*dir[2]);
                if (dlen > 1e-6f) { dir[0]/=dlen; dir[1]/=dlen; dir[2]/=dlen; }

                auto hit3d = md::WorldBVH::Get().RayIntersect(orig, dir);
                if (hit3d.hit()) {
                    // Convert world hit position back to tile coordinates.
                    // world_x = (col-row)*0.5, world_z = (col+row)*0.5
                    float tile_col = hit3d.world_x + hit3d.world_z;
                    float tile_row = hit3d.world_z - hit3d.world_x;
                    fprintf(stdout, "[3D Pick] hit world=(%.2f,%.2f,%.2f) "
                            "tile≈(col=%.1f, row=%.1f)\n",
                            hit3d.world_x, hit3d.world_y, hit3d.world_z,
                            tile_col, tile_row);
                }
            } else if (lmb && !over_btn && !s_player_dead && s_player != entt::null) {
                // Mouse → tile coords (inverse isometric formula).
                float xmz = (mx_f - origin_x) / (96.f * scale);
                float xpz = (my_f - origin_y) / (48.f * scale);
                float mtx = (xmz + xpz) * 0.5f;
                float mtz = (xpz - xmz) * 0.5f;

                if (lmb_click) {
                    // ── Rising edge: check NPC targeting first ────────────────
                    float best = 1.5f;
                    int   hit  = -1;
                    auto& reg  = Registry::Get();
                    for (int i = 0; i < s_npc_count; ++i) {
                        if (s_npc_hp[i] <= 0) continue;
                        if (!reg.valid(s_npcs[i])) continue;
                        auto* nwt = reg.try_get<WorldTransform>(s_npcs[i]);
                        if (!nwt) continue;
                        float cx = nwt->x - mtx, cz = nwt->z - mtz;
                        float d  = sqrtf(cx*cx + cz*cz);
                        if (d < best) { best = d; hit = i; }
                    }
                    if (hit >= 0) {
                        // Clicked on enemy → chase + attack.
                        s_player_atk_tgt = hit;
                        s_player_has_tgt = false;
                    } else {
                        // Clicked on ground → clear attack target, set move target.
                        s_player_atk_tgt = -1;
                        if (IsPassable(map, mtx, mtz)) {
                            s_player_tgt_x   = mtx;
                            s_player_tgt_z   = mtz;
                            s_player_has_tgt = true;
                        }
                    }
                } else {
                    // ── Hold (not initial click): stream move target to cursor ──
                    // Only while no attack target is active (attack cancels hold-move).
                    if (s_player_atk_tgt < 0 && IsPassable(map, mtx, mtz)) {
                        s_player_tgt_x   = mtx;
                        s_player_tgt_z   = mtz;
                        s_player_has_tgt = true;
                    }
                }
            }
        }

        // ── Player movement toward target ─────────────────────────────────────
        if (!s_player_dead && s_player != entt::null) {
            auto* pwt = Registry::Get().try_get<WorldTransform>(s_player);
            if (pwt) {
                float tx = pwt->x, tz = pwt->z;  // default: stay
                bool  should_move = false;
                auto& reg = Registry::Get();

                if (s_player_atk_tgt >= 0) {
                    // Validate attack target.
                    if (s_npc_hp[s_player_atk_tgt] <= 0 ||
                        !reg.valid(s_npcs[s_player_atk_tgt])) {
                        s_player_atk_tgt = -1;  // target died
                    } else {
                        auto* nwt = reg.try_get<WorldTransform>(s_npcs[s_player_atk_tgt]);
                        if (nwt) {
                            float dx = nwt->x - pwt->x, dz = nwt->z - pwt->z;
                            float dist = sqrtf(dx*dx + dz*dz);
                            if (dist > PLAYER_ATK_RANGE) {
                                // Walk toward enemy.
                                tx = nwt->x; tz = nwt->z;
                                should_move = true;
                            } else {
                                // In range — auto-attack on cooldown.
                                if (s_player_atk_cd <= 0.f) {
                                    int dmg = RandRange(PLAYER_DMG_LO, PLAYER_DMG_HI);
                                    s_npc_hp[s_player_atk_tgt] -= dmg;
                                    s_player_atk_cd = 0.6f;  // 0.6s attack speed
                                    fprintf(stderr, "[combat] Player hits NPC[%d] -%d (hp=%d)\n",
                                            s_player_atk_tgt, dmg,
                                            s_npc_hp[s_player_atk_tgt]);
                                    if (s_npc_hp[s_player_atk_tgt] <= 0) {
                                        auto* nas = reg.try_get<AgentState>(
                                            s_npcs[s_player_atk_tgt]);
                                        if (nas) nas->lcflags.set(lcf::IS_DEAD);
                                        ++s_kills;
                                        fprintf(stderr, "[combat] NPC[%d] killed! kills=%d\n",
                                                s_player_atk_tgt, s_kills);
                                        s_player_atk_tgt = -1;
                                    }
                                }
                            }
                        }
                    }
                } else if (s_player_has_tgt) {
                    float dx = s_player_tgt_x - pwt->x, dz = s_player_tgt_z - pwt->z;
                    float dist = sqrtf(dx*dx + dz*dz);
                    if (dist < 0.2f) {
                        s_player_has_tgt = false;  // arrived
                    } else {
                        tx = s_player_tgt_x; tz = s_player_tgt_z;
                        should_move = true;
                    }
                }

                if (should_move) {
                    float dx = tx - pwt->x, dz = tz - pwt->z;
                    float dist = sqrtf(dx*dx + dz*dz);
                    if (dist > 0.01f) {
                        float step = fminf(PLAYER_SPD * dt / dist, 1.f);
                        float nx = pwt->x + dx * step;
                        float nz = pwt->z + dz * step;
                        nx = fmaxf(0.f, fminf(nx, (float)(map.width  - 1)));
                        nz = fmaxf(0.f, fminf(nz, (float)(map.height - 1)));
                        pwt->rot_y = atan2f(dx, dz);

                        if (IsPassable(map, nx, nz)) {
                            pwt->x = nx;
                            pwt->z = nz;
                        } else {
                            // Wall-slide: try each axis separately.
                            float sx = fmaxf(0.f, fminf(pwt->x + dx * step, (float)(map.width-1)));
                            float sz = fmaxf(0.f, fminf(pwt->z + dz * step, (float)(map.height-1)));
                            if (IsPassable(map, sx, pwt->z))      pwt->x = sx;
                            else if (IsPassable(map, pwt->x, sz)) pwt->z = sz;
                            // else fully blocked — stay in place
                        }
                    }
                }
                s_player_moving = should_move;
            }
        }

        // ── Zoom (2D) / distance (3D) ────────────────────────────────────────
        if (s_view_3d) {
            // Scroll changes orbit distance in 3D mode.
            if (scroll_y != 0.f)
                s_cam_dist = fmaxf(fminf(s_cam_dist - scroll_y * 2.f, 200.f), 3.f);
        } else {
            float old_scale = scale;
            if (do_zoom_out)    scale = fmaxf(scale * 0.92f, 0.1f);
            if (do_zoom_in)     scale = fminf(scale * 1.08f, 4.0f);
            if (scroll_y != 0.f) {
                float f = powf(1.05f, scroll_y);
                scale = fmaxf(fminf(scale * f, 4.0f), 0.1f);
            }
            if (scale != old_scale) {
                (void)old_scale;
                ComputeOrigin();
            }
        }
        if (do_reset) ResetCamera();

        // Camera always follows player.
        ComputeOrigin();

        // ── Attack cooldowns ───────────────────────────────────────────────────
        if (s_player_atk_cd > 0.f) s_player_atk_cd -= dt;
        for (int i = 0; i < s_npc_count; ++i)
            if (s_npc_atk_cd[i] > 0.f) s_npc_atk_cd[i] -= dt;

        // ── Logic tick (10 TPS) ───────────────────────────────────────────────
        logic_accum += dt;
        while (logic_accum >= LOGIC_TICK_S) {
            logic_accum -= LOGIC_TICK_S;
            LogicTick((float)now_ms);
        }

        // ── Hot-reload BT ─────────────────────────────────────────────────────
        if (s_reload_bt) {
            s_reload_bt = false;
            for (int i = 0; i < s_npc_count; ++i)
                RespawnNpcBT(s_npcs[i]);
        }

        // ── Title bar (≈1/s) ──────────────────────────────────────────────────
        if (frame_count % 60 == 0) {
            int alive = 0;
            for (int i = 0; i < s_npc_count; ++i)
                if (s_npc_hp[i] > 0) ++alive;
            float fps = (dt > 0.f) ? 1.f / dt : 0.f;
            s_ctx.fps = fps;
            char title[256];
            if (s_player_dead) {
                snprintf(title, sizeof(title),
                         "md_flare_demo | DEAD — press R to restart | Kills:%d | Map:%s",
                         s_kills, map.title);
            } else {
                snprintf(title, sizeof(title),
                         "md_flare_demo | FPS:%.0f | HP:%d/%d | NPCs:%d | Kills:%d | Map:%s",
                         fps, s_player_hp, PLAYER_HP_MAX, alive, s_kills, map.title);
            }
            SDL_SetWindowTitle(window, title);
        }

        rt.Tick(dt);

        // ── GPU frustum cull (3D mode only, before sprite list) ──────────────
        // Dispatch npc_cull.comp: tests all NPC positions against camera frustum.
        // Result used below to skip off-screen NPCs.
        if (s_view_3d && md::NpcGpuCuller::Get().IsReady()) {
            // Collect raw NPC positions for the GPU cull pass.
            static float cull_x[64], cull_z[64];
            int cull_n = 0;
            auto& creg = Registry::Get();
            for (int i = 0; i < s_npc_count && cull_n < 64; ++i) {
                if (s_npc_hp[i] <= 0 || !creg.valid(s_npcs[i])) continue;
                auto* wt = creg.try_get<WorldTransform>(s_npcs[i]);
                if (!wt) continue;
                cull_x[cull_n] = wt->x;
                cull_z[cull_n] = wt->z;
                ++cull_n;
            }
            // Rebuild MVP for the current frame's orbit camera.
            Vec3 eye3d = {
                s_w3d_target.x + s_cam_dist * cosf(s_cam_el) * sinf(s_cam_az),
                s_w3d_target.y + s_cam_dist * sinf(s_cam_el),
                s_w3d_target.z - s_cam_dist * cosf(s_cam_el) * cosf(s_cam_az),
            };
            Mat4 v3d = mat4_lookat(eye3d, s_w3d_target, {0,1,0});
            Mat4 p3d = mat4_perspective(0.80f, (float)vp_w/(float)vp_h, 0.1f, 500.f);
            Mat4 mvp3d = mat4_mul(p3d, v3d);
            int n_visible = md::NpcGpuCuller::Get().Cull(
                cull_x, cull_z, cull_n, mat4_ptr(mvp3d));
            (void)n_visible;  // result used per-NPC via IsVisible() below
        }

        // ── Collect sprites: player first, then living NPCs ───────────────────
        {
            static constexpr int kMax = 64;
            static float   sp_x[kMax], sp_z[kMax], sp_rot[kMax];
            static uint8_t sp_mov[kMax];
            int sp_n = 0;
            auto& reg = Registry::Get();

            // Player sprite.
            if (!s_player_dead && s_player != entt::null) {
                auto* pwt = reg.try_get<WorldTransform>(s_player);
                if (pwt && sp_n < kMax) {
                    sp_x[sp_n]   = pwt->x;
                    sp_z[sp_n]   = pwt->z;
                    sp_rot[sp_n] = pwt->rot_y;
                    sp_mov[sp_n] = s_player_moving ? 1 : 0;
                    ++sp_n;
                }
            }

            // NPC sprites — skip dead ones, apply culling in 3D mode.
            // Two complementary cullers run in 3D mode:
            //   GPU frustum cull (npc_cull.comp): off-screen NPCs → skip
            //   CPU MOC (MaskedOcclusionCulling): occluded NPCs → skip
            const bool use_gpu_cull = s_view_3d && md::NpcGpuCuller::Get().IsReady();
            const bool use_moc      = s_view_3d && md::MocCuller::Get().IsReady();
            int gpu_cull_idx = 0;  // parallel index into NpcGpuCuller results
            int culled = 0;
            for (int i = 0; i < s_npc_count && sp_n < kMax; ++i) {
                if (s_npc_hp[i] <= 0) continue;
                if (!reg.valid(s_npcs[i])) continue;
                auto* wt = reg.try_get<WorldTransform>(s_npcs[i]);
                if (!wt) continue;

                // GPU frustum cull result (if available).
                if (use_gpu_cull) {
                    bool gpu_vis = md::NpcGpuCuller::Get().IsVisible(gpu_cull_idx);
                    ++gpu_cull_idx;
                    if (!gpu_vis) { ++culled; continue; }
                }
                // CPU MOC occlusion cull.
                if (use_moc) {
                    if (!md::MocCuller::Get().IsBoxVisible(wt->x, 0.5f, wt->z, 0.5f)) {
                        ++culled;
                        continue;
                    }
                }
                sp_x[sp_n]   = wt->x;
                sp_z[sp_n]   = wt->z;
                sp_rot[sp_n] = wt->rot_y;
                sp_mov[sp_n] = 1;
                ++sp_n;
            }
            (void)culled;

            tmr2d.SetNpcSprites(sp_x, sp_z, sp_rot, sp_mov, sp_n,
                                (float)now_ms * 0.001f);
        }

        {
            auto& rpg = md::RenderPassGraph::Get();

            if (s_view_3d && rpg.IsEnabled("world_3d")) {
                // ── Pass: world_3d ────────────────────────────────────────────
                World3DRender(vp_w, vp_h, dt);
            } else if (!s_view_3d) {
                // ── Pass: npc_sprites ─────────────────────────────────────────
                if (rpg.IsEnabled("npc_sprites")) {
                    // (NPC sprite data already filled above — just keep in SetNpcSprites)
                }

                // ── Pass: overlay (camera button + UI) ────────────────────────
                if (rpg.IsEnabled("overlay")) {
                    void* btn_tex = (void*)cam_idle_tex;
                    if (s_recording)
                        btn_tex = ((now_ms / 500) & 1)
                                  ? (void*)cam_idle_tex : (void*)cam_rec_tex;
                    tmr2d.SetOverlayBlit(0, btn_tex,
                                          vp_w - BTN_SZ - BTN_MARGIN,
                                          vp_h - BTN_SZ - BTN_MARGIN,
                                          BTN_SZ, BTN_SZ);
                } else {
                    tmr2d.ClearOverlayBlit(0);
                }

                // ── Pass: tiles_2d ────────────────────────────────────────────
                if (rpg.IsEnabled("tiles_2d")) {
                    uint8_t layer_mask = 0xFF;
                    tmr2d.Render(map, (float)now_ms * 0.001f,
                                 origin_x, origin_y, scale, vp_w, vp_h,
                                 layer_mask);
                }
            }
        }
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    StopRecording();
    World3DShutdown();
    tmr2d.ClearOverlayBlit(0);
    SDL_WaitForGPUIdle(sdl_dev);
    if (cam_idle_tex) SDL_ReleaseGPUTexture(sdl_dev, cam_idle_tex);
    if (cam_rec_tex)  SDL_ReleaseGPUTexture(sdl_dev, cam_rec_tex);
    HotReload::Get().Stop();
    HotReload::Get().Unwatch(BT_JSON_PATH);
    DestroyDemoEntities();
    tmr2d.Shutdown();
    MdTextureCache_Shutdown();
    MdSpvCache_Shutdown();
    md::GpuDevice::Get().Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
