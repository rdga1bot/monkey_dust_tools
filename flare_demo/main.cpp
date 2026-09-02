#include "flare_demo_internal.h"

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
    md::GpuDeviceHandle  sdl_dev = md::GpuDevice::Get().SDLDevice();
    GpuColorTexture cam_idle_tex;
    GpuColorTexture cam_rec_tex;
    {
        uint8_t px[BTN_SZ * BTN_SZ * 4] = {};
        GenCamIcon(px, false); MakeCamTex(sdl_dev, px, cam_idle_tex);
        GenCamIcon(px, true);  MakeCamTex(sdl_dev, px, cam_rec_tex);
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
    BTSystem::ConnectRegistry(MdRegistry::Get().Raw());
    SpawnDemoEntities(rt);

    // ── Component reflection ──────────────────────────────────────────────────
    md::RegisterCoreComponents();
    md::WarmUpEngineComponents();  // flecs type registration before any JobGraph batch (task #248)

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
        rpg.LoadFromJSON("data/render_settings.json");

        // ── Resource dependency declarations (Step 7 — FrameGraph) ────────────
        // Declares data-flow between passes.  SDL_GPU handles VkImageLayout
        // barriers internally; this layer adds documentation + ordering validation.
        //
        // 3D pipeline:  world_3d → [scene_color] → cas_sharpening → swapchain
        rpg.DeclareWrite("evsm_shadows",  "evsm_moment_tex");  // shadow pass
        rpg.DeclareRead ("world_3d",      "evsm_moment_tex");  // main pass reads shadow
        rpg.DeclareWrite("world_3d",      "scene_color");
        rpg.DeclareRead ("cas_sharpening","scene_color");
        rpg.DeclareWrite("cas_sharpening","swapchain");
        rpg.DeclareWrite("tiles_2d",      "swapchain");
        rpg.DeclareWrite("overlay",       "swapchain");
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
        if (s_player == MdEntity::Null()) return;
        auto* pwt = MdRegistry::Get().Handle(s_player).try_get_mut<WorldTransform>();
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
                        if (s_player != MdEntity::Null()) {
                            auto* pwt = MdRegistry::Get().Handle(s_player).try_get_mut<WorldTransform>();
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
                        md::flare::ExportWorldOBJ(map, 1.0f, "tools/qa/world.obj");
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
            } else if (lmb && !over_btn && !s_player_dead && s_player != MdEntity::Null()) {
                // Mouse → tile coords (inverse isometric formula).
                float xmz = (mx_f - origin_x) / (96.f * scale);
                float xpz = (my_f - origin_y) / (48.f * scale);
                float mtx = (xmz + xpz) * 0.5f;
                float mtz = (xpz - xmz) * 0.5f;

                if (lmb_click) {
                    // ── Rising edge: check NPC targeting first ────────────────
                    float best = 1.5f;
                    int   hit  = -1;
                    auto& reg  = MdRegistry::Get();
                    for (int i = 0; i < s_npc_count; ++i) {
                        if (s_npc_hp[i] <= 0) continue;
                        if (!reg.Valid(s_npcs[i])) continue;
                        auto* nwt = reg.Handle(s_npcs[i]).try_get_mut<WorldTransform>();
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
        if (!s_player_dead && s_player != MdEntity::Null()) {
            auto* pwt = MdRegistry::Get().Handle(s_player).try_get_mut<WorldTransform>();
            if (pwt) {
                float tx = pwt->x, tz = pwt->z;  // default: stay
                bool  should_move = false;
                auto& reg = MdRegistry::Get();

                if (s_player_atk_tgt >= 0) {
                    // Validate attack target.
                    if (s_npc_hp[s_player_atk_tgt] <= 0 ||
                        !reg.Valid(s_npcs[s_player_atk_tgt])) {
                        s_player_atk_tgt = -1;  // target died
                    } else {
                        auto* nwt = reg.Handle(s_npcs[s_player_atk_tgt]).try_get_mut<WorldTransform>();
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
                                        auto* nas = reg.Handle(s_npcs[s_player_atk_tgt]).try_get_mut<AgentState>();
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
            auto& creg = MdRegistry::Get();
            for (int i = 0; i < s_npc_count && cull_n < 64; ++i) {
                if (s_npc_hp[i] <= 0 || !creg.Valid(s_npcs[i])) continue;
                auto* wt = creg.Handle(s_npcs[i]).try_get_mut<WorldTransform>();
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
            auto& reg = MdRegistry::Get();

            // Player sprite.
            if (!s_player_dead && s_player != MdEntity::Null()) {
                auto* pwt = reg.Handle(s_player).try_get_mut<WorldTransform>();
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
                if (!reg.Valid(s_npcs[i])) continue;
                auto* wt = reg.Handle(s_npcs[i]).try_get_mut<WorldTransform>();
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
                    void* btn_tex = (void*)cam_idle_tex.SDLTexture();
                    if (s_recording)
                        btn_tex = ((now_ms / 500) & 1)
                                  ? (void*)cam_idle_tex.SDLTexture() : (void*)cam_rec_tex.SDLTexture();
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
    StopRecording();        // non-blocking SIGTERM (no-op if already stopped)
    WaitRecordingChild();   // block until demo_capture.py finishes; banner prints here
    World3DShutdown();
    tmr2d.ClearOverlayBlit(0);
    md::GpuDevice::Get().WaitForIdle();
    cam_idle_tex.Shutdown();
    cam_rec_tex.Shutdown();
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
