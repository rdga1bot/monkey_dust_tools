// md_flare_demo — Flare tile-map viewer via SDL_GPU.
//
// Demonstrates the monkey_dust SDL_GPU API end-to-end:
//   plain SDL3 window → GpuDevice::Init → SDL_GPU Vulkan/Metal/D3D12 backend
//   → SPIR-V shaders → TileMap2DRenderer renders the tile map via SDL_GPU swapchain.
//
// Controls:
//   WASD / arrow keys — pan
//   Q / E or scroll   — zoom out / in
//   R                 — reset camera
//   Escape            — quit
//
// Usage:
//   md_flare_demo [mods_root] [mod_name] [map_name]

#include <monkey_dust/flare/flare_runtime.h>
#include <monkey_dust/flare/tile_map_2d_renderer.h>
#include <monkey_dust/render/gpu_device.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <unistd.h>

// ── repo root detection ───────────────────────────────────────────────────────

static void ChdirToRepoRoot() {
    char exe[512] = {};
    if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) <= 0) return;
    // Binary is at <repo>/build/tools/md_flare_demo — strip 3 path components.
    for (int i = 0; i < 3; ++i) {
        char* p = strrchr(exe, '/');
        if (!p) return;
        *p = '\0';
    }
    if (exe[0] && chdir(exe) == 0)
        fprintf(stdout, "[demo] repo root: %s\n", exe);
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    ChdirToRepoRoot();

    const char* mods_root = (argc > 1) ? argv[1] : "third_party/flare-game/mods";
    const char* mod_name  = (argc > 2) ? argv[2] : "empyrean_campaign";
    const char* map_name  = (argc > 3) ? argv[3] : "maps/goblin_camp.txt";

    // ── SDL3 init (plain window — no SDL_WINDOW_OPENGL) ───────────────────────
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "[demo] SDL_Init failed: %s\n", SDL_GetError());
        return 1;
    }

    const int WIN_W = 1280, WIN_H = 720;
    SDL_Window* window = SDL_CreateWindow("md_flare_demo — SDL_GPU Tile Map",
                                          WIN_W, WIN_H,
                                          SDL_WINDOW_RESIZABLE);
    if (!window) {
        fprintf(stderr, "[demo] SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    // ── GPU device ────────────────────────────────────────────────────────────
    if (!md::GpuDevice::Get().Init(window)) {
        fprintf(stderr, "[demo] GpuDevice::Init failed — no Vulkan/Metal/D3D12 driver?\n");
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    fprintf(stderr, "[demo] SDL_GPU driver: %s\n", md::GpuDevice::Get().DriverName());

    // ── Load Flare mod ────────────────────────────────────────────────────────
    fprintf(stderr, "[demo] LoadMod '%s' from '%s' ...\n", mod_name, mods_root);
    auto& rt = md::flare::FlareRuntime::Get();
    if (!rt.LoadMod(mod_name, mods_root, map_name, 1.0f)) {
        fprintf(stderr, "[demo] LoadMod FAILED\n");
        md::GpuDevice::Get().Shutdown();
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    const auto& map = rt.GetMap();
    fprintf(stderr, "[demo] Map loaded: %dx%d '%s'\n", map.width, map.height, map.title);
    fprintf(stderr, "[demo] Map: %dx%d '%s'  enemies:%d  items:%d  powers:%d\n",
            map.width, map.height, map.title,
            rt.GetEnemies().count, rt.GetItems().count, rt.GetPowers().count);

    // ── 2D renderer (SDL_GPU path selected automatically by Init) ─────────────
    auto& tmr2d = md::flare::TileMap2DRenderer::Get();
    tmr2d.Init();
    if (map.tileset_atlas_count > 0) {
        tmr2d.SetAtlases(map);
        fprintf(stderr, "[demo] %d atlas(es) loaded via SDL_GPU\n",
                map.tileset_atlas_count);
    } else {
        fprintf(stderr, "[demo] Warning: no atlas paths — tiles invisible\n");
    }

    // ── 2D camera state ───────────────────────────────────────────────────────
    const float MAP_SCR_W = (float)(map.width + map.height) * 96.f;
    const float MAP_SCR_H = (float)(map.width + map.height) * 48.f;

    int vp_w = WIN_W, vp_h = WIN_H;
    float scale = fminf((float)vp_w / MAP_SCR_W,
                        (float)vp_h / MAP_SCR_H) * 0.82f;

    float origin_x = 0.f, origin_y = 0.f;
    auto ResetOrigin = [&]() {
        SDL_GetWindowSize(window, &vp_w, &vp_h);
        float cx_tile = (float)(map.width - map.height) * 48.f;
        float cy_tile = (float)(map.width + map.height) * 24.f;
        scale    = fminf((float)vp_w / MAP_SCR_W, (float)vp_h / MAP_SCR_H) * 0.82f;
        origin_x = (float)vp_w * 0.5f - cx_tile * scale;
        origin_y = (float)vp_h * 0.5f - cy_tile * scale;
    };
    ResetOrigin();

    const float PAN_SPEED = 400.f;  // screen pixels / second
    uint64_t prev_ms = SDL_GetTicks();
    bool quit = false;

    // ── Game loop ─────────────────────────────────────────────────────────────
    while (!quit) {
        uint64_t now_ms = SDL_GetTicks();
        float dt = (float)(now_ms - prev_ms) * 0.001f;
        if (dt > 0.1f) dt = 0.1f;
        prev_ms = now_ms;

        float scroll_y = 0.f;
        bool  do_zoom_out = false, do_zoom_in = false, do_reset = false;

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) { quit = true; break; }
            if (ev.type == SDL_EVENT_MOUSE_WHEEL) scroll_y = ev.wheel.y;
            if (ev.type == SDL_EVENT_WINDOW_RESIZED)
                SDL_GetWindowSize(window, &vp_w, &vp_h);
            if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                switch (ev.key.scancode) {
                case SDL_SCANCODE_ESCAPE: quit = true;         break;
                case SDL_SCANCODE_Q:      do_zoom_out = true;  break;
                case SDL_SCANCODE_E:      do_zoom_in  = true;  break;
                case SDL_SCANCODE_R:      do_reset    = true;  break;
                default: break;
                }
            }
        }
        if (quit) break;

        // Continuous pan
        const bool* kb   = SDL_GetKeyboardState(nullptr);
        float step = PAN_SPEED * dt;
        if (kb[SDL_SCANCODE_A] || kb[SDL_SCANCODE_LEFT])  origin_x += step;
        if (kb[SDL_SCANCODE_D] || kb[SDL_SCANCODE_RIGHT]) origin_x -= step;
        if (kb[SDL_SCANCODE_W] || kb[SDL_SCANCODE_UP])    origin_y += step;
        if (kb[SDL_SCANCODE_S] || kb[SDL_SCANCODE_DOWN])  origin_y -= step;

        // Zoom toward screen center
        float old_scale = scale;
        if (do_zoom_out) scale = fmaxf(scale * 0.92f, 0.02f);
        if (do_zoom_in)  scale = fminf(scale * 1.08f, 4.0f);
        if (scroll_y != 0.f) {
            float factor = powf(1.05f, scroll_y);  // 5 % per scroll unit
            scale = fmaxf(fminf(scale * factor, 4.0f), 0.02f);
        }
        if (scale != old_scale) {
            float cx = (float)vp_w * 0.5f, cy = (float)vp_h * 0.5f;
            origin_x = cx - (cx - origin_x) * (scale / old_scale);
            origin_y = cy - (cy - origin_y) * (scale / old_scale);
        }
        if (do_reset) ResetOrigin();

        rt.Tick(dt);

        // Single call: acquires cmd buffer + swapchain, renders, submits.
        tmr2d.Render(map, (float)now_ms * 0.001f,
                     origin_x, origin_y, scale,
                     vp_w, vp_h);
    }

    // ── Cleanup ───────────────────────────────────────────────────────────────
    tmr2d.Shutdown();
    md::GpuDevice::Get().Shutdown();
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
