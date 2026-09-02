#include "flare_demo_internal.h"

void World3DInit(SDL_Window* window,
                        const md::flare::FlareMap& map,
                        float tile_world_size) {
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

    // Terrain-mesh view removed (found while splitting this file into
    // per-concern TUs, unrelated to the split itself): TerrainRenderer::Draw
    // no longer exists (task #309 "Remove dead TerrainRenderer draw
    // pipeline") -- this single-chunk demo toggle was never updated to the
    // modern terrain path (TerrainPatchRenderer + TerrainWorldHeightmap),
    // which needs a whole heightmap-texture/patch-grid setup this demo
    // never had. Out of scope to port here; the working flat Flare
    // world-geometry view (World3DRender's else branch) is unaffected.

    // CAS post-process pass — actual viewport size passed later in World3DRender.
    s_cas.Init(1, 1, 0.5f);

    // MaskedOcclusionCulling — CPU occlusion culling for NPC visibility.
    md::MocCuller::Get().Init(320, 160);

    // GPU frustum culling via Vulkan compute (3D mode).
    md::NpcGpuCuller::Get().Init();

    // EVSM shadow infrastructure (used by the main game's forward pass).
    // Demo: init with smaller map (512) since no actual shadow casters here.
    md::EvsmShadow::Get().Init(512, 40.f);
}

void World3DRender(int vp_w, int vp_h, float dt) {
    if (s_w3d_tri_count == 0 || !s_w3d_pipeline.SDLPipeline()) return;

    // Orbit camera: arrow keys update azimuth/elevation each frame.
    const bool* ks = SDL_GetKeyboardState(nullptr);
    if (ks[SDL_SCANCODE_LEFT])  s_cam_az -= 1.4f * dt;
    if (ks[SDL_SCANCODE_RIGHT]) s_cam_az += 1.4f * dt;
    if (ks[SDL_SCANCODE_UP])    s_cam_el  = fminf(s_cam_el + 1.0f * dt, 1.55f);
    if (ks[SDL_SCANCODE_DOWN])  s_cam_el  = fmaxf(s_cam_el - 1.0f * dt, 0.05f);

    // 'T' terrain-view toggle removed along with the dead TerrainRenderer::Draw
    // call below (see World3DInit's comment) -- always the flat world-geometry view now.
    Vec3  cam_target = s_w3d_target;
    float cam_dist   = s_cam_dist;
    float cam_el     = s_cam_el;

    // Eye position from orbit angles.
    Vec3 eye = {
        cam_target.x + cam_dist * cosf(cam_el) * sinf(s_cam_az),
        cam_target.y + cam_dist * sinf(cam_el),
        cam_target.z - cam_dist * cosf(cam_el) * cosf(s_cam_az),
    };
    Vec3 up = {0.f, 1.f, 0.f};

    Mat4 view = mat4_lookat(eye, cam_target, up);
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

    const bool use_cas = s_cas.IsReady() &&
                         md::RenderPassGraph::Get().IsEnabled("cas_sharpening");

    // Resize CAS intermediate texture if viewport changed.
    if (use_cas) s_cas.Resize(vp_w, vp_h);

    // Acquire command buffer + swapchain texture.
    md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
    md::GpuTextureHandle swap = nullptr;
    if (!SDL_AcquireGPUSwapchainTexture(cmd, s_w3d_window, &swap, nullptr, nullptr)
        || !swap) {
        md::GpuDevice::Get().Submit(cmd);
        return;
    }

    md::GpuTextureHandle scene_target = use_cas ? s_cas.SceneTex() : swap;
    md::GpuTextureHandle scene_depth  = use_cas ? s_cas.DepthTex() : s_w3d_depth.SDLTexture();

    // Scene render pass.
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd            = cmd;
    cpd.color_tex[0]      = scene_target;
    cpd.depth_tex      = scene_depth;
    cpd.clear_color[0] = 0.12f;
    cpd.clear_color[1] = 0.16f;
    cpd.clear_color[2] = 0.24f;
    cpd.clear_color[3] = 1.0f;
    cpd.load_color     = false;
    cpd.load_depth     = false;
    cb.BeginColorPass(cpd);

    {
        // Flare world-geometry view (flat tile extrusion). Terrain-mesh view
        // removed (see World3DInit's comment) -- always this path now.
        cb.BindPipeline(&s_w3d_pipeline);
        // s_w3d_vbuf is GpuStaticBuffer -- GpuCommandBuffer::BindVertexBuffer
        // only overloads GpuVertexBuffer*, so route through GpuPassView
        // (which has the GpuStaticBuffer overload, added in the M1 pilot)
        // via its already-open pass rather than adding a redundant overload
        // to GpuCommandBuffer for a single call site.
        GpuPassView::FromRaw(cb.SDLPass(), cb.SDLCmd()).BindVertexBuffer(&s_w3d_vbuf);
        cb.PushVertexUniforms(0, mat4_ptr(mvp), 64);
        cb.Draw((uint32_t)(s_w3d_tri_count * 3));
    }
    cb.EndPass();

    // OIT (transparent leaf-quad demo) removed 2026-08-09 -- unused by the
    // actual game, was demo-only synthetic content here. CAS sharpening
    // still applies normally.
    if (use_cas) s_cas.Apply(cmd, swap, vp_w, vp_h);

    md::GpuDevice::Get().Submit(cmd);
}

void World3DShutdown() {
    md::EvsmShadow::Get().Shutdown();
    md::NpcGpuCuller::Get().Shutdown();
    md::MocCuller::Get().Shutdown();
    md::WorldBVH::Get().Shutdown();
    s_cas.Shutdown();
    s_w3d_depth.Shutdown();
    s_w3d_vbuf.Shutdown();
    s_w3d_pipeline.Destroy();
}

// ── Repo root ─────────────────────────────────────────────────────────────────

