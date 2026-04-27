// Flare tile-map viewer — standalone demo for M7 Flare-runtime.
//
// Controls:
//   WASD / arrow keys — pan camera
//   Q / E             — zoom out / in
//   R                 — reset camera
//
// Usage:
//   md_flare_demo [mods_root] [mod_name] [map_name]
//
// Defaults:
//   mods_root = third_party/flare-game/mods   (relative to CWD)
//   mod_name  = empyrean_campaign
//   map_name  = maps/perdition_harbor.txt

#include <monkey_dust/flare/flare_runtime.h>
#include <monkey_dust/flare/tile_map_renderer.h>
#include <monkey_dust/render/md_camera.h>
#include "raylib.h"
#include <cstdio>
#include <cstring>

// ── Atlas path resolution ─────────────────────────────────────────────────────
// TileMapRenderer needs the grassland atlas that comes with flare-game.
// Path is relative to the repo root (where the binary is typically run).

static void FindAtlas(const char* mods_root, const md::flare::FlareMap& map,
                      char* out, int out_sz) {
    // The perdition_harbor.txt [tilesets] line 2 points to grassland.png
    // via a path like "../../../tiled/tilesheets/grassland.png" relative to mods/.
    // We look for the second tileset entry (index 1, skip collision).
    for (int i = 1; i < map.tileset_count; ++i) {
        const char* img = map.tilesets[i].image_path;
        if (!img[0]) continue;

        // Try absolute/relative path as-is
        if (FileExists(img)) { snprintf(out, (size_t)out_sz, "%s", img); return; }

        // Try relative to mods_root
        char full[512];
        snprintf(full, sizeof(full), "%s/%s", mods_root, img);
        if (FileExists(full)) { snprintf(out, (size_t)out_sz, "%s", full); return; }

        // Tileset paths are relative to the map file's dir (mods_root/mod/maps/).
        // Strip leading "../" tokens and anchor to mods_root parent, adjusted
        // for the 2 levels of depth from mods_root to maps/.
        const char* p = img;
        int ups = 0;
        while (strncmp(p, "../", 3) == 0) { p += 3; ++ups; }
        if (ups > 0) {
            // Map dir is 2 levels below mods_root, so effective ups from mods_root
            // is (ups - 2). Strip that many levels from mods_root.
            char base[512];
            snprintf(base, sizeof(base), "%s", mods_root);
            int strip = ups - 2;  // number of levels to go above mods_root
            for (int u = 0; u < strip; ++u) {
                char* last = nullptr;
                for (char* c = base; *c; ++c) if (*c == '/') last = c;
                if (last) *last = '\0';
            }
            snprintf(full, sizeof(full), "%s/%s", base, p);
            if (FileExists(full)) { snprintf(out, (size_t)out_sz, "%s", full); return; }
        }
    }
    out[0] = '\0';
}

// ── Camera ────────────────────────────────────────────────────────────────────

static MdCamera MakeCamera(float cx, float cz, float dist) {
    MdCamera cam;
    cam.pos    = { cx,          dist * 0.8f, cz - dist * 0.6f };
    cam.target = { cx,          0.0f,        cz               };
    cam.up     = { 0.0f,        1.0f,        0.0f             };
    cam.fovy   = 45.0f;
    return cam;
}

// ── main ──────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    const char* mods_root = (argc > 1) ? argv[1]
                                       : "third_party/flare-game/mods";
    const char* mod_name  = (argc > 2) ? argv[2] : "empyrean_campaign";
    const char* map_name  = (argc > 3) ? argv[3] : "maps/perdition_harbor.txt";

    // ── window init ───────────────────────────────────────────────────────────
    const int W = 1280, H = 720;
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(W, H, "md_flare_demo — Flare Tile Map Viewer");
    SetTargetFPS(60);

    // ── load mod ──────────────────────────────────────────────────────────────
    fprintf(stdout, "[demo] LoadMod '%s' from '%s' ...\n", mod_name, mods_root);
    auto& rt = md::flare::FlareRuntime::Get();
    if (!rt.LoadMod(mod_name, mods_root, map_name, 1.0f)) {
        fprintf(stderr, "[demo] LoadMod FAILED\n");
        CloseWindow();
        return 1;
    }
    fprintf(stdout, "[demo] Map: %dx%d '%s', %d enemies, %d items, %d powers\n",
            rt.GetMap().width, rt.GetMap().height, rt.GetMap().title,
            rt.GetEnemies().count, rt.GetItems().count, rt.GetPowers().count);

    // ── renderer init ─────────────────────────────────────────────────────────
    auto& tmr = md::flare::TileMapRenderer::Get();
    tmr.Init();

    char atlas_path[512] = {};
    FindAtlas(mods_root, rt.GetMap(), atlas_path, sizeof(atlas_path));
    if (atlas_path[0]) {
        tmr.SetAtlas(atlas_path);
        fprintf(stdout, "[demo] Atlas: %s\n", atlas_path);
    } else {
        fprintf(stderr, "[demo] Warning: atlas not found — tiles will be white\n");
    }

    // ── camera setup ─────────────────────────────────────────────────────────
    // Map center in isometric world coords
    const auto& map = rt.GetMap();
    float map_cx = 0.0f;  // isometric maps are symmetric around X=0
    float map_cz = (map.width + map.height) * 0.5f * 0.5f;
    float cam_dist = (float)(map.width > map.height ? map.width : map.height) * 0.7f;

    MdCamera cam  = MakeCamera(map_cx, map_cz, cam_dist);
    const float PAN_SPEED  = 0.1f;
    const float ZOOM_STEP  = 1.5f;

    // ── game loop ─────────────────────────────────────────────────────────────
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        rt.Tick(dt);

        // Camera pan
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))  { cam.pos.x -= PAN_SPEED * cam_dist; cam.target.x -= PAN_SPEED * cam_dist; }
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) { cam.pos.x += PAN_SPEED * cam_dist; cam.target.x += PAN_SPEED * cam_dist; }
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))    { cam.pos.z -= PAN_SPEED * cam_dist; cam.target.z -= PAN_SPEED * cam_dist; }
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))  { cam.pos.z += PAN_SPEED * cam_dist; cam.target.z += PAN_SPEED * cam_dist; }

        // Zoom (change camera distance)
        float scroll = GetMouseWheelMove();
        if (IsKeyPressed(KEY_Q) || scroll < 0) cam_dist = fminf(cam_dist + ZOOM_STEP, 100.0f);
        if (IsKeyPressed(KEY_E) || scroll > 0) cam_dist = fmaxf(cam_dist - ZOOM_STEP,  5.0f);
        cam.pos.y   = cam_dist * 0.8f;
        cam.pos.z   = cam.target.z - cam_dist * 0.6f;

        // Reset
        if (IsKeyPressed(KEY_R)) {
            cam = MakeCamera(map_cx, map_cz, cam_dist);
        }

        float aspect = (float)GetScreenWidth() / (float)GetScreenHeight();

        BeginDrawing();
        ClearBackground({ 20, 20, 30, 255 });

        rt.Render(cam, aspect);

        // HUD
        DrawFPS(8, 8);
        DrawText(TextFormat("Map: %s  %dx%d  tiles",
                 map.title, map.width, map.height), 8, 32, 18, WHITE);
        DrawText(TextFormat("Enemies: %d  Items: %d  Powers: %d  Factions: %d",
                 rt.GetEnemies().count, rt.GetItems().count,
                 rt.GetPowers().count, rt.GetFactions().count),
                 8, 54, 16, LIGHTGRAY);
        DrawText("WASD=pan  Q/E or scroll=zoom  R=reset", 8, H - 24, 14, DARKGRAY);

        EndDrawing();
    }

    tmr.Shutdown();
    CloseWindow();
    return 0;
}
