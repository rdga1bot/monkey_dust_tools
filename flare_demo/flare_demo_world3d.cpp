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

    {
        // Flare world-geometry view (flat tile extrusion). Terrain-mesh view
        // removed (see World3DInit's comment) -- always this path now.
        cb.BindPipeline(&s_w3d_pipeline);
        SDL_GPUBufferBinding vb { s_w3d_vbuf.SDLBuffer(), 0u };
        SDL_BindGPUVertexBuffers(cb.SDLPass(), 0, &vb, 1);
        cb.PushVertexUniforms(0, mat4_ptr(mvp), 64);
        cb.Draw((uint32_t)(s_w3d_tri_count * 3));
    }
    cb.EndPass();

    // OIT pass: transparent geometry (water, leaves, particles) over opaque scene.
    // Must run BEFORE CAS apply so scene is still in scene_target (cas.SceneTex())
    // and we can pass it as scene_tex to OIT Composite (scene_tex != output=swap).
    const bool use_oit = use_oit_check;
    if (use_oit) {
        s_oit.Resize(vp_w, vp_h);

        // Demo: 8 semi-transparent quads at fixed world positions (simulate leaves).
        // Non-indexed triangle list: 6 verts per quad (0-1-2, 0-2-3).
        struct OitVert { float x,y,z; float r,g,b,a; };
        static OitVert s_oit_verts[6 * 8];  // 48 verts max
        int nv = 0;

        auto push_quad = [&](float wx, float wz, float wy,
                              float r, float g, float b, float a) {
            const float hs = 0.6f;
            OitVert v0 = {wx-hs, wy, wz-hs, r,g,b,a};
            OitVert v1 = {wx+hs, wy, wz-hs, r,g,b,a};
            OitVert v2 = {wx+hs, wy, wz+hs, r,g,b,a};
            OitVert v3 = {wx-hs, wy, wz+hs, r,g,b,a};
            s_oit_verts[nv++] = v0; s_oit_verts[nv++] = v1; s_oit_verts[nv++] = v2;
            s_oit_verts[nv++] = v0; s_oit_verts[nv++] = v2; s_oit_verts[nv++] = v3;
        };

        // Quads placed on verified dry ground tiles east of the camp clearing.
        // Water spans wx=-14..18.5, wz=4.5..46 — these positions are confirmed
        // non-water (collision==0) with 3-tile buffer from any water tile.
        push_quad(10.0f, 20.0f, 1.0f, 0.2f, 0.8f, 0.2f, 0.5f);  // col=30,row=10
        push_quad(12.0f, 22.0f, 0.8f, 0.3f, 0.7f, 0.1f, 0.4f);  // col=34,row=10
        push_quad( 9.0f, 20.0f, 1.2f, 0.2f, 0.6f, 0.3f, 0.6f);  // col=29,row=11
        push_quad(12.5f, 23.5f, 0.9f, 0.1f, 0.9f, 0.2f, 0.35f); // col=36,row=11
        push_quad(11.5f, 22.5f, 1.1f, 0.3f, 0.7f, 0.15f, 0.45f);// col=34,row=11
        push_quad( 8.5f, 20.5f, 0.7f, 0.15f,0.8f, 0.25f, 0.55f);// col=29,row=12
        push_quad(11.0f, 21.0f, 1.3f, 0.2f, 0.75f,0.3f, 0.5f);  // col=32,row=10
        push_quad(10.5f, 20.5f, 1.0f, 0.25f,0.7f, 0.2f, 0.4f);  // col=30,row=11

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

        // OIT accumulation pass — pass scene depth so quads are clipped by walls.
        SDL_GPURenderPass* oit_rp = s_oit.BeginAccum(cmd, scene_depth);
        if (oit_rp && s_oit.AccumPipeline()) {
            SDL_BindGPUGraphicsPipeline(oit_rp, s_oit.AccumPipeline());

            // Push MVP + View uniforms (slot 0 = set=1, binding=0).
            struct OitUBO { float mvp[16]; float view[16]; } ubo;
            memcpy(ubo.mvp,  mat4_ptr(mvp),  64);
            memcpy(ubo.view, mat4_ptr(view), 64);
            SDL_PushGPUVertexUniformData(cmd, 0, &ubo, sizeof(ubo));

            SDL_GPUBufferBinding vb_bind{ oit_vbuf, 0 };
            SDL_BindGPUVertexBuffers(oit_rp, 0, &vb_bind, 1);
            SDL_DrawGPUPrimitives(oit_rp, (uint32_t)nv, 1, 0, 0);
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

void World3DShutdown() {
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

