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
#include <monkey_dust/flare/billboard_renderer.h>
#include <monkey_dust/flare/sprite_resolver.h>
#include <monkey_dust/render/md_camera.h>
#include "raylib.h"
#include <cstdio>
#include <cstring>
#include <unistd.h>

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

static MdCamera MakeCamera(float cx, float cz, float ortho_size) {
    MdCamera cam;
    // Isometric angle: 30° elevation from the horizontal plane.
    // For ortho projection the camera distance only determines direction,
    // so we use ortho_size * 4 to place it well outside the visible volume.
    constexpr float ELEV = 30.0f * 3.14159265f / 180.0f;
    const float dist = ortho_size * 4.0f;
    cam.pos    = { cx, dist * sinf(ELEV), cz + dist * cosf(ELEV) };
    cam.target = { cx, 0.0f, cz };
    cam.up     = { 0.0f, 1.0f, 0.0f };
    cam.fovy   = 45.0f;   // unused by ortho path; kept for ToRaylib() compatibility
    return cam;
}

// ── main ──────────────────────────────────────────────────────────────────────

// Resolve the repo root from the binary path and chdir to it so that
// relative paths ("shaders/", "third_party/") work regardless of CWD.
static void ChdirToRepoRoot() {
    char exe[512] = {};
    if (readlink("/proc/self/exe", exe, sizeof(exe) - 1) <= 0) return;
    // Binary is at <repo>/build/tools/md_flare_demo — strip 3 components.
    for (int i = 0; i < 3; ++i) {
        char* p = strrchr(exe, '/');
        if (!p) return;
        *p = '\0';
    }
    if (exe[0] && chdir(exe) == 0)
        fprintf(stdout, "[demo] repo root: %s\n", exe);
}

int main(int argc, char** argv) {
    ChdirToRepoRoot();

    const char* mods_root = (argc > 1) ? argv[1]
                                       : "third_party/flare-game/mods";
    const char* mod_name  = (argc > 2) ? argv[2] : "empyrean_campaign";
    const char* map_name  = (argc > 3) ? argv[3] : "maps/goblin_camp.txt";

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
    // Prefer the per-tile atlas from tilesetdef (exact per-tile UV coordinates).
    // Fall back to the grid-based tiled/tilesheets atlas if not found.
    if (rt.GetMap().tileset_atlas[0]) {
        snprintf(atlas_path, sizeof(atlas_path), "%s", rt.GetMap().tileset_atlas);
        fprintf(stdout, "[demo] Atlas (per-tile): %s\n", atlas_path);
    } else {
        FindAtlas(mods_root, rt.GetMap(), atlas_path, sizeof(atlas_path));
        if (atlas_path[0])
            fprintf(stdout, "[demo] Atlas (grid): %s\n", atlas_path);
        else
            fprintf(stderr, "[demo] Warning: atlas not found — tiles will be white\n");
    }
    if (atlas_path[0]) tmr.SetAtlas(atlas_path);

    // ── billboard renderer + sprite atlases ──────────────────────────────────
    auto& br  = md::flare::BillboardRenderer::Get();
    auto& sr  = md::flare::SpriteResolver::Get();
    br.Init();
    sr.Clear();
    {
        const auto& spawn_map = rt.GetMap();
        for (int i = 0; i < spawn_map.spawn_count; ++i) {
            const md::flare::SpriteCategoryEntry* e =
                sr.Resolve(spawn_map.spawns[i].category);
            if (e) br.LoadSpriteAtlas(e->atlas_full_path);
        }
        fprintf(stdout, "[demo] %d spawn blocks, last atlas loaded\n",
                spawn_map.spawn_count);
    }

    // ── camera setup ─────────────────────────────────────────────────────────
    // Map center in isometric world coords
    const auto& map = rt.GetMap();
    float map_cx = 0.0f;  // isometric maps are symmetric around X=0
    float map_cz = (map.width + map.height) * 0.5f * 0.5f;
    // ortho_size = half the isometric diamond width so the map fills ~60% of screen.
    float ortho_size = (float)(map.width + map.height) * 0.5f * 0.4f;

    MdCamera cam  = MakeCamera(map_cx, map_cz, ortho_size);
    const float PAN_SPEED  = 0.1f;
    const float ZOOM_STEP  = 1.5f;
    bool show_debug = false;

    // ── game loop ─────────────────────────────────────────────────────────────
    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        rt.Tick(dt);

        // Camera pan
        if (IsKeyDown(KEY_A) || IsKeyDown(KEY_LEFT))  { cam.pos.x -= PAN_SPEED * ortho_size; cam.target.x -= PAN_SPEED * ortho_size; }
        if (IsKeyDown(KEY_D) || IsKeyDown(KEY_RIGHT)) { cam.pos.x += PAN_SPEED * ortho_size; cam.target.x += PAN_SPEED * ortho_size; }
        if (IsKeyDown(KEY_W) || IsKeyDown(KEY_UP))    { cam.pos.z -= PAN_SPEED * ortho_size; cam.target.z -= PAN_SPEED * ortho_size; }
        if (IsKeyDown(KEY_S) || IsKeyDown(KEY_DOWN))  { cam.pos.z += PAN_SPEED * ortho_size; cam.target.z += PAN_SPEED * ortho_size; }

        // Zoom: change ortho_size and reposition camera at the same isometric angle
        float scroll = GetMouseWheelMove();
        if (IsKeyPressed(KEY_Q) || scroll < 0) ortho_size = fminf(ortho_size + ZOOM_STEP, 200.0f);
        if (IsKeyPressed(KEY_E) || scroll > 0) ortho_size = fmaxf(ortho_size - ZOOM_STEP,  10.0f);
        {
            constexpr float ELEV = 30.0f * 3.14159265f / 180.0f;
            const float dist = ortho_size * 4.0f;
            cam.pos.y = dist * sinf(ELEV);
            cam.pos.z = cam.target.z + dist * cosf(ELEV);
        }

        // Reset / debug toggle
        if (IsKeyPressed(KEY_R))  cam = MakeCamera(map_cx, map_cz, ortho_size);
        if (IsKeyPressed(KEY_F3)) show_debug = !show_debug;

        float aspect = (float)GetScreenWidth() / (float)GetScreenHeight();

        BeginDrawing();
        ClearBackground({ 20, 20, 30, 255 });

        tmr.Render(rt.GetMap(), cam, aspect, rt.TileWorldSize(), ortho_size);

        // Billboard NPC spawns
        const float AW = (float)(br.AtlasWidth()  > 0 ? br.AtlasWidth()  : 2048);
        const float AH = (float)(br.AtlasHeight() > 0 ? br.AtlasHeight() : 2048);
        br.BeginFrame();
        for (int i = 0; i < map.spawn_count; ++i) {
            const md::flare::FlareSpawn& sp = map.spawns[i];
            md::flare::SpriteFrame frame;
            if (!sr.GetStanceFrame0(sp.category, frame)) continue;

            float wx = (sp.center_x - sp.center_y) * rt.TileWorldSize() * 0.5f;
            float wz = (sp.center_x + sp.center_y) * rt.TileWorldSize() * 0.5f;

            md::flare::BillboardInstance inst;
            inst.x      = wx;
            inst.y      = 0.0f;
            inst.z      = wz;
            inst.width  = (float)frame.w / 96.0f;
            inst.height = (float)frame.h / 96.0f;
            inst.u0     = (float)frame.x / AW;
            inst.v0     = (float)frame.y / AH;
            inst.u1     = (float)(frame.x + frame.w) / AW;
            inst.v1     = (float)(frame.y + frame.h) / AH;
            inst.r = 255; inst.g = 255; inst.b = 255; inst.a = 255;

            md::flare::BillboardRenderer::Get().Submit(inst);
        }
        br.Render(cam, aspect);

        // HUD
        DrawFPS(8, 8);
        DrawText(TextFormat("Map: %s  %dx%d  tiles",
                 map.title, map.width, map.height), 8, 32, 18, WHITE);
        DrawText(TextFormat("Enemies: %d  Items: %d  Powers: %d  Factions: %d",
                 rt.GetEnemies().count, rt.GetItems().count,
                 rt.GetPowers().count, rt.GetFactions().count),
                 8, 54, 16, LIGHTGRAY);
        DrawText("WASD=pan  Q/E or scroll=zoom  R=reset  F3=debug", 8, GetScreenHeight() - 24, 14, DARKGRAY);

        // F3 debug overlay
        if (show_debug) {
            const int PW = 340;
            const int SW = GetScreenWidth();
            DrawRectangle(SW - PW - 4, 0, PW + 4, 310, { 0, 0, 0, 180 });
            int dy = 8;
            const int LX = SW - PW;
            DrawText("── DEBUG ──────────────────────", LX, dy, 14, YELLOW); dy += 20;
            DrawText(TextFormat("Cam pos:    (%.1f, %.1f, %.1f)", cam.pos.x, cam.pos.y, cam.pos.z),    LX, dy, 13, WHITE); dy += 16;
            DrawText(TextFormat("Cam target: (%.1f, %.1f, %.1f)", cam.target.x, cam.target.y, cam.target.z), LX, dy, 13, WHITE); dy += 16;
            DrawText(TextFormat("OrthoSize:  %.1f", ortho_size),                                       LX, dy, 13, WHITE); dy += 20;
            DrawText(TextFormat("Map:  %dx%d  tilesets: %d", map.width, map.height, map.tileset_count), LX, dy, 13, WHITE); dy += 16;
            DrawText(TextFormat("Atlas: %s", atlas_path[0] ? atlas_path : "(none)"),                   LX, dy, 13, WHITE); dy += 16;
            DrawText(TextFormat("Sprite atlas: %dx%d", br.AtlasWidth(), br.AtlasHeight()),             LX, dy, 13, WHITE); dy += 20;
            DrawText(TextFormat("Spawns: %d   Billboards: %d", map.spawn_count, br.SubmittedCount()),  LX, dy, 13, YELLOW); dy += 16;
            for (int i = 0; i < map.spawn_count && i < 8; ++i) {
                DrawText(TextFormat("  [%d] %s @ (%.0f,%.0f)", i,
                         map.spawns[i].category, map.spawns[i].center_x, map.spawns[i].center_y),
                         LX, dy, 12, LIGHTGRAY); dy += 14;
            }
            if (map.spawn_count > 8)
                DrawText(TextFormat("  ... +%d more", map.spawn_count - 8), LX, dy, 12, DARKGRAY);
        }

        EndDrawing();
    }

    tmr.Shutdown();
    br.Shutdown();
    sr.Clear();
    CloseWindow();
    return 0;
}
