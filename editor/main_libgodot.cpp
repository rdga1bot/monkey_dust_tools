// LibGodot migration -- editor entry point (task #537, Крок 3a).
// Mirrors game/src/main_libgodot.cpp's own pattern: own target
// (monkey_dust_libgodot_editor, USE_LIBGODOT-gated, tools/CMakeLists.txt),
// own single source file, does NOT touch tools/editor/main.cpp (SDL3
// path, target monkey_dust_editor) at all -- no dual-render in one
// process, same precedent as the game side.
//
// Scope of THIS step (3a + input follow-up): prove the CMake target
// links, the RenderingServer-backed ImGui adapter (imgui_impl_
// renderingserver.h, Фаза E.1, already live-verified by the game HUD)
// draws real editor panel content on screen, AND real mouse + keyboard
// input (position + left/right/middle buttons via input.h's renderer-
// agnostic input_mouse_x/y/down(); the curated KEY_* set via
// input_key_down(), see FeedKeyboardToImGui() below) reaches
// ImGui::GetIO() so panels are genuinely clickable/navigable, not just
// drawn. Deliberately NOT in scope yet (tracked as remaining Крок 3
// work in CLAUDE_STATE.md):
//   - full UTF8 text typing (io.AddInputCharacterUTF8 + a much larger
//     key set than input.h's curated game-hotkey subset -- needed for
//     renaming assets, search boxes)
//   - mouse wheel (input.h's own comment: Godot represents it as
//     discrete WHEEL_UP/DOWN button events, needs a SceneTree _input()
//     hookup, unspiked)
//   - the 68 editor_*.h/.cpp panels themselves (this file draws ONE
//     generic smoke-test panel; the rest follow incrementally, excluding
//     any panel that turns out to reach render/scene_render.h
//     transitively, mirroring game/CMakeLists.txt's own
//     LIBGODOT_GAME_ECS_SOURCES filter comment: "exact remaining gaps
//     surface as real compile/link errors, not guesswork -- fix forward
//     from there")
//   - hot-reload (libeditor_panels.so) compatibility with this backend
//
// F3 terrain brush/sculpt + entity/NPC selection (parity gate blocker #2,
// docs/LIBGODOT_MIGRATION_PARITY_CHECKLIST.md section E/F) are now real
// here (see MakeCameraBasis/ScreenToRay/WorldToScreen, SpawnTestMarkers,
// and the per-frame block below LibgodotTerrain_Update's call) -- one
// raise/lower brush mode writing straight into TerrainAtlas via the
// already-existing TerrainAtlas_SetHeight()/SaveEdits() API, and ray+
// sphere entity picking against real WorldTransform-bearing flecs
// entities. Not yet ported: multiple brush modes (only raise/lower, no
// smooth/flatten), brush undo (SDL3's 32-step ring), real NPCs in this
// viewport (SpawnTestMarkers stands in for the real ECS-render bridge
// game/src/main_libgodot.cpp already has, not wired into this binary).
#include <monkey_dust/platform/window.h>
#include <monkey_dust/platform/input.h>
#include <monkey_dust/render/libgodot_bridge.h>
#include <monkey_dust/render/imgui_impl_renderingserver.h>
// F10 screenshot hotkey (below) -- this backend's real equivalent of the
// SDL3 editor's editor_screenshot.cpp (SDL_GPU swapchain readback, entirely
// #ifdef MD_SDL_GPU-gated, not portable: no swapchain/command-buffer
// concept on this backend). RenderingServer::viewport_get_texture()->
// texture_2d_get()->save_png() is the technique this whole session's own
// verification probes already use (e.g. probes/libgodot_imgui_test.cpp's
// SaveShot()) -- wiring it to a real hotkey here makes it available for
// live/automated verification of this binary too, not just ad-hoc temp
// code added and reverted per debugging session.
#include "servers/rendering/rendering_server.h"
#include <monkey_dust/render/libgodot_terrain_renderer.h>
#include <monkey_dust/render/char_preview_libgodot.h>
#include <monkey_dust/world/terrain_gen.h>
#include <monkey_dust/world/chunk_def.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/hot/editor_module.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/platform/md_log.h>
#include "imgui.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <string>
#include <unistd.h>

// Keyboard follow-up (task #537): input.h's KEY_* set is a deliberately
// small, curated subset (game hotkeys, not full text entry -- no full
// alphabet/punctuation, see input.h's own scancode_table() comment).
// Feeds what's there into ImGui::GetIO() via AddKeyEvent -- enough for
// navigation (arrows/Tab/Escape/Enter/Space/Backspace) and shortcuts
// (Ctrl/Shift + F-keys/digits/letters), not yet full UTF8 text typing
// (needs io.AddInputCharacterUTF8 + a much larger key set, separate
// follow-up). Local to this TU (not a shared header) -- small, and the
// project's own convention favors this over a premature abstraction.
// ── Clothing/hair item tables (parity gate blocker #5 follow-up) ────────
// Real assets only — verbatim (paths + names + colours) from tools/
// editor/editor_char_preview_sdlgpu_internal.h's s_cloth_items[] (SDL3
// reference), male paths only: this preview has no body-sex toggle (it
// always loads game/data/props/md_human.glb), so the _f female variants
// in that table are out of scope here, not fabricated substitutes.
struct LibgodotClothItem { const char* name; const char* path; float color[3]; int slot; };
static const LibgodotClothItem kClothItems[] = {
    // slot 0 -- top
    {"None",         nullptr,                                    {0.f,0.f,0.f},          0},
    {"Slave Shirt",  "game/data/clothes/slave_shirt.clothbin",   {0.62f,0.59f,0.53f}, 0},
    {"Drifter Coat", "game/data/clothes/drifter_coat.clothbin",  {0.25f,0.20f,0.14f}, 0},
    {"Jacket",       "game/data/clothes/jacket.clothbin",        {0.30f,0.22f,0.15f}, 0},
    {"Male Coat",    "game/data/clothes/male_coat.clothbin",     {0.20f,0.16f,0.10f}, 0},
    {"Monk Coat",    "game/data/clothes/monk_coat.clothbin",     {0.18f,0.14f,0.09f}, 0},
    {"Samurai Top",  "game/data/clothes/samurai_top.clothbin",   {0.15f,0.12f,0.08f}, 0},
    // slot 1 -- bottom
    {"None",         nullptr,                                    {0.f,0.f,0.f},          1},
    {"Drifter Pants","game/data/clothes/drifter_pants.clothbin", {0.22f,0.18f,0.12f}, 1},
    {"Cargo Pants",  "game/data/clothes/cargopants.clothbin",    {0.28f,0.22f,0.14f}, 1},
    {"Shorts",       "game/data/clothes/shorts.clothbin",        {0.35f,0.28f,0.18f}, 1},
    {"Half Pants",   "game/data/clothes/trousers.clothbin",      {0.20f,0.16f,0.10f}, 1},
    {"Monk Pants",   "game/data/clothes/monk_pants.clothbin",    {0.18f,0.14f,0.09f}, 1},
    {"Samurai Bot",  "game/data/clothes/samurai_bot.clothbin",   {0.15f,0.12f,0.08f}, 1},
    {"Slave Dress",  "game/data/clothes/slave_dress.clothbin",   {0.62f,0.59f,0.53f}, 1},
};
static constexpr int kClothItemCount = (int)(sizeof(kClothItems) / sizeof(kClothItems[0]));

// Real game/data/hair/*.glb list, verbatim from tools/editor/editor_char_
// preview_assets.cpp's s_hair_styles[30] (all 30 files confirmed present
// on disk).
static const char* const kHairStyles[] = {
    "hair01","hair02","hair03","hair04","hair05","hair06","hair07","hair08",
    "haircut_male01","haircut_male02","haircut_male03","haircut_male04","haircut_male05",
    "haircut_male06","haircut_male07","haircut_male08","haircut_male09","haircut_male10",
    "haircut_female01","haircut_female02","haircut_female03","haircut_female04",
    "haircut_female05","haircut_female06","haircut_female07","haircut_female08",
    "hairlongbald","frizzhair","nutfro","afrohair",
};
static constexpr int kHairStyleCount = (int)(sizeof(kHairStyles) / sizeof(kHairStyles[0]));

static void FeedKeyboardToImGui(ImGuiIO& io) {
    struct KeyMap { int md_key; ImGuiKey imgui_key; };
    static const KeyMap kKeys[] = {
        {KEY_A, ImGuiKey_A}, {KEY_B, ImGuiKey_B}, {KEY_D, ImGuiKey_D},
        {KEY_E, ImGuiKey_E}, {KEY_F, ImGuiKey_F}, {KEY_M, ImGuiKey_M},
        {KEY_R, ImGuiKey_R}, {KEY_S, ImGuiKey_S}, {KEY_T, ImGuiKey_T},
        {KEY_W, ImGuiKey_W}, {KEY_X, ImGuiKey_X}, {KEY_Y, ImGuiKey_Y},
        {KEY_Z, ImGuiKey_Z},
        {KEY_ONE, ImGuiKey_1}, {KEY_TWO, ImGuiKey_2},
        {KEY_THREE, ImGuiKey_3}, {KEY_FOUR, ImGuiKey_4},
        {KEY_ESCAPE, ImGuiKey_Escape}, {KEY_TAB, ImGuiKey_Tab},
        {KEY_F1, ImGuiKey_F1}, {KEY_F2, ImGuiKey_F2}, {KEY_F3, ImGuiKey_F3},
        {KEY_F4, ImGuiKey_F4}, {KEY_F5, ImGuiKey_F5}, {KEY_F6, ImGuiKey_F6},
        {KEY_F7, ImGuiKey_F7}, {KEY_F8, ImGuiKey_F8}, {KEY_F9, ImGuiKey_F9},
        {KEY_F10, ImGuiKey_F10},
        {KEY_LEFT_CONTROL, ImGuiKey_LeftCtrl}, {KEY_RIGHT_CONTROL, ImGuiKey_RightCtrl},
        {KEY_LEFT_SHIFT, ImGuiKey_LeftShift}, {KEY_RIGHT_SHIFT, ImGuiKey_RightShift},
        {KEY_LEFT, ImGuiKey_LeftArrow}, {KEY_RIGHT, ImGuiKey_RightArrow},
        {KEY_UP, ImGuiKey_UpArrow}, {KEY_DOWN, ImGuiKey_DownArrow},
        {KEY_ENTER, ImGuiKey_Enter}, {KEY_SPACE, ImGuiKey_Space},
        {KEY_BACKSPACE, ImGuiKey_Backspace},
    };
    for (const auto& k : kKeys) {
        io.AddKeyEvent(k.imgui_key, input_key_down(k.md_key));
    }
    io.AddKeyEvent(ImGuiMod_Ctrl, input_key_down(KEY_LEFT_CONTROL) || input_key_down(KEY_RIGHT_CONTROL));
    io.AddKeyEvent(ImGuiMod_Shift, input_key_down(KEY_LEFT_SHIFT) || input_key_down(KEY_RIGHT_SHIFT));
}

// Minimal static-terrain-viewport orbit camera (task #537 remainder --
// first real 3D-viewport step, no sculpt/select). Same orbit math as
// EditorCore::UpdateEditorCameraFromInput's orbit branch
// (editor_core.cpp:532-561), duplicated locally rather than shared --
// same "avoid premature abstraction" call already made for
// FeedKeyboardToImGui() above (constructing a full EditorCore here would
// pull in MdRegistry/undo-history wiring this minimal step doesn't need).
//
// Mouse delta is a plain frame-to-frame position diff (input.h has no
// unlimited relative-mouse mode for this backend, unlike the SDL3 editor's
// editor_camera_input_sdl3.cpp) -- clamps at window edges, a known,
// documented limitation, acceptable for this first step.
//
// Zoom uses W/S instead of scroll: input_get_scroll_y() is hardcoded to
// return 0.f for the LibGodot backend (input.h:187, unspiked SceneTree
// _input() wiring, separate follow-up).
struct OrbitCamera {
    float yaw = 0.f, pitch = 25.f, dist = 400.f;
    float target_x = 0.f, target_y = 0.f, target_z = 0.f;
    float prev_mouse_x = 0.f, prev_mouse_y = 0.f;
    bool  have_prev = false;

    void Update(float dt, float& eye_x, float& eye_y, float& eye_z) {
        float mx = input_mouse_x(), my = input_mouse_y();
        bool rmb = input_mouse_down(MOUSE_BUTTON_RIGHT);
        float rdx = 0.f, rdy = 0.f;
        if (rmb && have_prev) { rdx = mx - prev_mouse_x; rdy = my - prev_mouse_y; }
        prev_mouse_x = mx; prev_mouse_y = my; have_prev = true;

        if (rmb) {
            yaw   -= rdx * 0.4f;
            pitch += rdy * 0.4f;
            if (pitch >  89.f) pitch =  89.f;
            if (pitch < -89.f) pitch = -89.f;
        }
        if (input_key_down(KEY_W)) dist -= 200.f * dt;
        if (input_key_down(KEY_S)) dist += 200.f * dt;
        if (dist < 10.f)   dist = 10.f;
        if (dist > 2000.f) dist = 2000.f;

        constexpr float kDeg2Rad = 3.14159265f / 180.f;
        float yaw_r = yaw * kDeg2Rad, pitch_r = pitch * kDeg2Rad;
        eye_x = target_x + dist * cosf(pitch_r) * sinf(yaw_r);
        eye_y = target_y + dist * sinf(pitch_r);
        eye_z = target_z + dist * cosf(pitch_r) * cosf(yaw_r);
    }
};

// World is centre-origin, +/-14745.6m -- see kFeaturesToAtlasShift's own
// doc comment in engine/src/render/libgodot_terrain_renderer.cpp
// (TerrainNodeHeightSample). Hoisted to file scope (was local-only inside
// main()'s TerrainAtlas_Loaded() block) so the sculpt/marker code below can
// share the exact same conversion.
constexpr float kFeaturesToAtlasShift = 29491.2f * 0.5f;

// TerrainAtlas layout constants (parity gate blocker #2 / F3 sculpt).
// Not exposed by engine/include/monkey_dust/world/terrain_gen.h as named
// constants (only via its own doc comment: "64x64 zones... col,row in
// 0..64") -- terrain_gen_internal.h's ATLAS_ZONES/ATLAS_VERTS are a
// private header not meant for external includes, same reason
// libgodot_terrain_renderer.cpp keeps its own local kTerrainChunkSize
// copy instead of including that internal header.
constexpr int kAtlasZones        = 64;
// PRE-EXISTING BUG FOUND + FIXED (task #534 remainder, this step): this was
// 65, not 129 -- terrain_gen_internal.h's REAL ATLAS_VERTS (private header,
// not includable here, see comment below) is 129, confirmed both by
// TerrainAtlas_SmoothBoundaries's own source ("col=128 of A... ATLAS_VERTS-1")
// and by TerrainAtlas_SampleWorld's GRID_N = ATLAS_ZONES*(ATLAS_VERTS-1) =
// 8192 (64*128), NOT 4096 (64*64). With the old wrong 65, this file's own
// vspacing = CHUNK_SIZE/(kAtlasVertsPerZone-1) came out to 7.2m instead of
// the real 3.6m, AND the brush loop's col/row range [0,65) only ever
// touched real atlas indices [0,64] out of the true valid [0,128] range --
// i.e. every brush stroke (raise/lower included, already merged before this
// step) silently wrote into (and only into) the near quarter of each
// touched zone's real world footprint, at double the intended world-space
// stride, invisible to a quick before/after screenshot glance but caught by
// this step's SampleWorld-vs-GetHeight cross-check (see the smooth/flatten/
// undo verification notes for the isolated repro that found this: writing
// (32,32,10,10) via SetHeight and reading back the SAME real-world position
// via SampleWorld returned a stale, unrelated value -- proof the two were
// indexing the atlas with different, inconsistent per-zone vertex counts).
constexpr int kAtlasVertsPerZone = 129; // col/row in [0, kAtlasVertsPerZone)

// Smooth/flatten/undo follow-up (task #534 remainder, this step): brush_radius
// slider maxes out at 200m (below) and CHUNK_SIZE=460.8m, so the zone-aligned
// bbox (zx0/zx1 = floor((hit +/- radius)/CHUNK_SIZE), same formula the paint
// loop already uses) spans at most 2 zones per axis -- diameter 400m < 460.8m
// means floor(x+d)-floor(x) in {0,1} for d=2*200/460.8=0.868<1. Sizes the
// fixed undo snapshot buffer (no heap alloc, this project's hot-path
// convention) to that worst case; a runtime guard below still checks it and
// logs+skips rather than overrunning if the slider bound ever changes.
constexpr int kMaxBrushZonesPerAxis = 2;
constexpr int kUndoSnapDim = kMaxBrushZonesPerAxis * kAtlasVertsPerZone; // 130

// Brush mode (task #534 remainder): plain ints, not enum class, so
// ImGui::RadioButton can bind directly to the state variable below.
constexpr int kSculptRaiseLower = 0;
constexpr int kSculptSmooth     = 1;
constexpr int kSculptFlatten    = 2;

// ── Minimal ray/projection math (parity gate blocker #2) ───────────────────
// OrbitCamera only ever exposes eye/target as plain floats (no Mat4/
// MdCamera dependency -- same "avoid premature abstraction" call already
// made for FeedKeyboardToImGui/OrbitCamera above: pulling in game_camera.
// cpp's MdCamera/Mat4 machinery here would drag along SDL3-editor-only
// state this file doesn't have). Basis is derived directly from eye->target
// with world-up=+Y, matching what LibGodotBridge::SetCameraTransform's own
// Transform3D::set_look_at(eye, target) builds for the RS camera this
// viewport actually renders through (Godot's own default up vector) --
// so a ray built from this basis lines up with what's on screen.
struct Vec3F { float x = 0.f, y = 0.f, z = 0.f; };
static inline Vec3F V3Sub(Vec3F a, Vec3F b)   { return {a.x-b.x, a.y-b.y, a.z-b.z}; }
static inline Vec3F V3Add(Vec3F a, Vec3F b)   { return {a.x+b.x, a.y+b.y, a.z+b.z}; }
static inline Vec3F V3Scale(Vec3F a, float s) { return {a.x*s, a.y*s, a.z*s}; }
static inline float V3Dot(Vec3F a, Vec3F b)   { return a.x*b.x + a.y*b.y + a.z*b.z; }
static inline Vec3F V3Cross(Vec3F a, Vec3F b) {
    return {a.y*b.z - a.z*b.y, a.z*b.x - a.x*b.z, a.x*b.y - a.y*b.x};
}
static inline Vec3F V3Normalize(Vec3F a) {
    float len = sqrtf(V3Dot(a, a));
    if (len < 1e-6f) return {0.f, 0.f, -1.f};
    return V3Scale(a, 1.f / len);
}

constexpr float kDeg2Rad      = 3.14159265f / 180.f;
constexpr float kViewportFovY = 60.0f; // matches LibGodotBridge::Init's camera_set_perspective(60,...)

struct CameraBasis { Vec3F eye, forward, right, up; };
static CameraBasis MakeCameraBasis(Vec3F eye, Vec3F target) {
    CameraBasis b;
    b.eye = eye;
    b.forward = V3Normalize(V3Sub(target, eye));
    Vec3F world_up{0.f, 1.f, 0.f};
    b.right = V3Normalize(V3Cross(b.forward, world_up));
    if (V3Dot(b.right, b.right) < 1e-6f) b.right = Vec3F{1.f, 0.f, 0.f}; // looking straight up/down
    b.up = V3Cross(b.right, b.forward);
    return b;
}

// Screen pixel (px,py) -> world-space ray. sw/sh = viewport size in pixels.
static void ScreenToRay(const CameraBasis& cam, float px, float py, int sw, int sh,
                         Vec3F& out_origin, Vec3F& out_dir) {
    out_origin = cam.eye;
    if (sw <= 0 || sh <= 0) { out_dir = cam.forward; return; }
    float aspect = (float)sw / (float)sh;
    float thf = tanf(kViewportFovY * 0.5f * kDeg2Rad);
    float ndx = (2.f * px / (float)sw) - 1.f;
    float ndy = 1.f - (2.f * py / (float)sh);
    Vec3F dir = V3Add(cam.forward,
                V3Add(V3Scale(cam.right, ndx * thf * aspect),
                      V3Scale(cam.up,    ndy * thf)));
    out_dir = V3Normalize(dir);
}

// World-space point -> screen pixel. Returns false if behind the camera
// (or degenerate viewport size) -- callers must skip drawing in that case.
static bool WorldToScreen(const CameraBasis& cam, Vec3F world, int sw, int sh,
                           float& out_x, float& out_y) {
    if (sw <= 0 || sh <= 0) return false;
    Vec3F d = V3Sub(world, cam.eye);
    float cz = V3Dot(d, cam.forward);
    if (cz < 0.1f) return false;
    float cx = V3Dot(d, cam.right);
    float cy = V3Dot(d, cam.up);
    float aspect = (float)sw / (float)sh;
    float thf = tanf(kViewportFovY * 0.5f * kDeg2Rad);
    float ndx = (cx / cz) / (thf * aspect);
    float ndy = (cy / cz) / thf;
    out_x = (ndx * 0.5f + 0.5f) * (float)sw;
    out_y = (1.f - (ndy * 0.5f + 0.5f)) * (float)sh;
    return true;
}

// Smooth brush (task #534 remainder): reads a neighbour vertex crossing zone
// boundaries, via the same global-vertex parameterization TerrainAtlas_
// SampleWorld's own sample_grid lambda uses internally (terrain_gen_atlas_
// io.cpp) -- zones SHARE their border vertices (col kAtlasVertsPerZone-1 of
// zone zx == col 0 of zone zx+1, kept in sync by TerrainAtlas_SmoothBoundaries
// at load time), so a global grid has (kAtlasVertsPerZone-1) DISTINCT columns/
// rows per zone, not kAtlasVertsPerZone (that would double-count the shared
// vertex). Clamps at both the low and high edge of the whole atlas so brush
// strokes near the world border don't read out of range.
static float TerrainAtlas_GetHeightGlobal(int gcol, int grow) {
    int zx = gcol / (kAtlasVertsPerZone - 1), col = gcol % (kAtlasVertsPerZone - 1);
    int zy = grow / (kAtlasVertsPerZone - 1), row = grow % (kAtlasVertsPerZone - 1);
    if (gcol < 0) { zx = 0; col = 0; }
    if (grow < 0) { zy = 0; row = 0; }
    if (zx >= kAtlasZones) { zx = kAtlasZones - 1; col = kAtlasVertsPerZone - 1; }
    if (zy >= kAtlasZones) { zy = kAtlasZones - 1; row = kAtlasVertsPerZone - 1; }
    return TerrainAtlas_GetHeight(zx, zy, col, row);
}

// ── Entity/NPC selection test data (parity gate blocker #2, section F) ─────
// tools/editor/main_libgodot.cpp does not spawn any NPCs yet -- the real
// ECS-render bridge (register_skin()/RegisterGroup() sync) lives in
// game/src/main_libgodot.cpp, a different binary entirely, not wired into
// this editor viewport. To exercise real selection logic against real
// WorldTransform-bearing entities here, spawn a small fixed ring of named
// marker entities on real sampled terrain height around the camera's
// starting target -- genuine flecs entities using the SAME WorldTransform
// component every real NPC in this ECS uses (not a parallel struct), so
// this selection code is not a special case that needs rewriting once real
// NPCs are wired into this backend later.
constexpr int kMarkerCount = 6;

static void SpawnTestMarkers(flecs::world& w, float center_x, float center_z) {
    constexpr float kRing = 80.f;
    for (int i = 0; i < kMarkerCount; ++i) {
        float ang = (float)i * (2.f * 3.14159265f / (float)kMarkerCount);
        float x = center_x + kRing * cosf(ang);
        float z = center_z + kRing * sinf(ang);
        float y = 0.f;
        if (TerrainAtlas_Loaded())
            y = TerrainAtlas_SampleWorld(x + kFeaturesToAtlasShift, z + kFeaturesToAtlasShift);
        char name[32];
        snprintf(name, sizeof(name), "TestMarker_%d", i);
        w.entity(name).set<WorldTransform>(WorldTransform{x, y, z, 0.f, 0xFFFFFFFFu});
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IOLBF, BUFSIZ);
    printf("=== tools/editor/main_libgodot.cpp -- LibGodot editor entry point (task #537, Крок 3a) ===\n");

    int max_frames = 120;
    // --screenshot-frame N: non-interactive equivalent of the F10 hotkey
    // below (task requires live visual proof of the new hair/clothing
    // without a human at the keyboard -- this backend has no scripted-
    // input-injection harness yet, so a CLI-triggered frame is the
    // smallest additive way to get a deterministic screenshot out of an
    // automated `--frames N` run). -1 (default) disables it entirely.
    int screenshot_frame = -1;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            max_frames = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--screenshot-frame") == 0 && i + 1 < argc) {
            screenshot_frame = atoi(argv[++i]);
        }
    }

    // launch_cwd MUST be captured BEFORE window_init() (Godot's own
    // chdir() for project detection) -- same bug class already fixed in
    // game/src/main_libgodot.cpp and audio_system.cpp.
    char cwd_buf[4096];
    std::string launch_cwd = getcwd(cwd_buf, sizeof(cwd_buf)) ? cwd_buf : ".";

    window_init(1280, 720, "monkey_dust_libgodot_editor");
    printf("OK: window_init(1280, 720)\n");

    LibGodotBridge bridge;
    bool bridge_ok = bridge.Init(1280, 720, /*attach_to_screen=*/true);
    printf("%s: LibGodotBridge::Init(attach_to_screen=true)\n", bridge_ok ? "OK" : "FAILED");
    if (!bridge_ok) { window_shutdown(); return 1; }

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(1280.0f, 720.0f);
    bool imgui_ok = ImGui_ImplRenderingServer_Init(bridge.ViewportRid());
    printf("%s: ImGui_ImplRenderingServer_Init\n", imgui_ok ? "OK" : "FAILED");

    // ── Hot-reload (parity gate blocker-6, docs/LIBGODOT_MIGRATION_
    // PARITY_CHECKLIST.md section E/F) ──────────────────────────────────
    // Reuses the SAME EditorModule singleton the SDL3 editor uses
    // (engine/src/hot/editor_module.cpp/.h) — its Config::gpu/window
    // fields are SDL types but only ever forward-declared pointers,
    // never dereferenced by EditorModule itself (they're passed straight
    // through to editor_panels_init() as void*), so nullptr here is
    // correct and safe: this backend has neither. Path is a literal
    // relative to repo-root cwd, same fragile-but-consistent convention
    // tools/editor/main.cpp uses for "build/hot/libeditor_panels.so".
    {
        EditorModule::Config ecfg;
        ecfg.imgui_ctx   = ImGui::GetCurrentContext();
        ecfg.ecs_world   = Registry::Get().c_ptr();
        ecfg.gpu         = nullptr;
        ecfg.window      = nullptr;
        ecfg.overlay_top = 0.f;
        ecfg.layout_path = nullptr;  // v1: EditorConsole has no persisted layout
        ecfg.lua_system  = nullptr;  // v1: lua editor API not wired on this backend yet
        EditorModule::Get().Init("build_libgodot/hot/libeditor_panels_libgodot.so", ecfg);
    }

    // First real 3D-viewport step (task #537 remainder, Крок 1e already
    // built and proven game-side) -- static terrain, no sculpt/select.
    LibgodotTerrain_Init(launch_cwd);
    printf("OK: LibgodotTerrain_Init\n");

    // Character preview panel (parity gate blocker #5, real scoped slice —
    // see char_preview_libgodot.h's own doc comment for what's ported vs
    // deferred). Own offscreen scenario/camera/viewport, independent of
    // LibGodotBridge's main on-screen one — non-fatal if the .glb is
    // missing (ok=false just means the panel below stays hidden).
    bool char_preview_ok = CharPreviewLibgodot_Init(launch_cwd);
    printf("%s: CharPreviewLibgodot_Init\n", char_preview_ok ? "OK" : "FAILED");
    float char_skin_rgb[3] = {0.82f, 0.65f, 0.52f};

    // Real default clothing/hair -- mirrors tools/editor/editor_char_
    // preview_runtime.cpp's own Init() defaults (SetClothingItem(1) ==
    // Slave Shirt, SetClothingItem(9) == Cargo Pants, LoadHairStyle(0) ==
    // hair01), same indices into kClothItems/kHairStyles above.
    int   char_cloth_sel[2] = {1, 9};
    int   char_hair_sel     = 0;
    float char_hair_rgb[3]  = {0.18f, 0.12f, 0.08f}; // character_editor_data.h's hair_rgb default
    if (char_preview_ok) {
        for (int slot = 0; slot < 2; ++slot) {
            const LibgodotClothItem& item = kClothItems[char_cloth_sel[slot]];
            std::string full_path = item.path ? (launch_cwd + "/" + item.path) : std::string();
            CharPreviewLibgodot_SetClothingSlot(slot, item.path ? full_path.c_str() : nullptr,
                                                 item.color[0], item.color[1], item.color[2]);
        }
        std::string hair_path = launch_cwd + "/game/data/hair/" + kHairStyles[char_hair_sel] + ".glb";
        CharPreviewLibgodot_SetHairStyle(hair_path.c_str(),
                                          char_hair_rgb[0], char_hair_rgb[1], char_hair_rgb[2]);
    }

    OrbitCamera cam;
    // World is centre-origin, +/-14745.6m (kFeaturesToAtlasShift's own
    // doc comment, engine/src/render/libgodot_terrain_renderer.cpp) --
    // (0,0) is a valid, central point of the loaded Kenshi world, not an
    // edge case. Sample real height there so the camera doesn't start
    // underground/floating absurdly far from the surface.
    if (TerrainAtlas_Loaded()) {
        cam.target_y = TerrainAtlas_SampleWorld(kFeaturesToAtlasShift, kFeaturesToAtlasShift);
    }

    // F3 sculpt + entity selection (parity gate blocker #2) -- test markers
    // + the shared query used every frame to iterate them for picking/
    // drawing. See SpawnTestMarkers's own doc comment for why these are
    // real ECS entities and not a parallel struct.
    SpawnTestMarkers(Registry::Get(), cam.target_x, cam.target_z);
    flecs::query<WorldTransform> marker_query = Registry::Get().query<WorldTransform>();

    bool  sculpt_active         = false;
    int   sculpt_mode           = kSculptRaiseLower;
    float brush_radius          = 30.f;
    float brush_strength        = 5.f;
    char  sculpt_status_msg[64] = {};
    float sculpt_status_timer   = 0.f;

    // Flatten brush: target height captured once at stroke-start (edge-
    // triggered by was_painting below), not re-sampled every frame -- "the
    // height at the brush's centre hit-point... at the moment the stroke
    // starts" per this task's own spec.
    float flatten_target_h = 0.f;
    bool  was_painting      = false;

    // One-level undo (Ctrl+Z): fixed-size snapshot of the exact zone/col/row
    // range the brush is about to touch, captured once per stroke (edge-
    // triggered, BEFORE any TerrainAtlas_SetHeight call this stroke). Static,
    // not stack-local -- kUndoSnapDim*kUndoSnapDim*4B ~= 66KB, too large for
    // a per-frame stack alloc even though it's fixed-size. Known limitation
    // (matches this task's own "sufficient" framing, not a bug): the snapshot
    // region is fixed at stroke-start, so if a single continuous drag moves
    // the hit point far enough to leave that zone range mid-stroke, cells in
    // the newly-entered zones won't be covered by this undo -- acceptable for
    // one-level undo, not attempting multi-region tracking.
    static float s_undo_h[kUndoSnapDim][kUndoSnapDim];
    bool  undo_valid = false;
    int   undo_zx0 = 0, undo_zy0 = 0, undo_zx1 = -1, undo_zy1 = -1;

    bool     have_selection  = false;
    uint64_t selected_id     = 0;

    char  hot_status_msg[64] = {};
    float hot_status_timer   = 0.f;

    // F10 screenshot state -- separate from hot_status_msg above (that one
    // is EditorModule::Reload()'s own status line, a different feature).
    char  shot_status_msg[96] = {};
    float shot_status_timer   = 0.f;
    int   shot_seq            = 0;

    for (int frame = 0; frame < max_frames; ++frame) {
        input_begin_frame();
        window_begin_frame();

        // F5: hot-reload editor panels (parity gate blocker-6). Mirrors
        // tools/editor/main.cpp's own F5 handler 1:1.
        if (input_key_pressed(KEY_F5)) {
            EditorModule::Get().Reload();
            snprintf(hot_status_msg, sizeof(hot_status_msg), "[F5] Editor panels reloaded");
            hot_status_timer = 3.f;
        }
        EditorModule::Get().Tick();  // mtime watcher — auto-reload on .so change

        // F10: on-demand screenshot -- this backend's equivalent of the
        // SDL3 editor's editor_screenshot.cpp (see file doc comment above
        // for why that file itself isn't portable). Captures the fully-
        // composited frame (3D viewport + every ImGui panel drawn this
        // frame) via the same RenderingServer readback technique this
        // session's own verification probes use, so this binary has a
        // real, permanent way to produce evidence without ad-hoc temp
        // code added-and-reverted per debugging session.
        if (input_key_pressed(KEY_F10)) {
            RenderingServer* rs = RenderingServer::get_singleton();
            RID vp = RID::from_uint64(bridge.ViewportRid());
            char path[128];
            snprintf(path, sizeof(path), "tmp_/libgodot_editor_screenshot_%03d.png", shot_seq++);
            bool ok = false;
            if (rs && vp.is_valid()) {
                RID tex = rs->viewport_get_texture(vp);
                Ref<Image> img = tex.is_valid() ? rs->texture_2d_get(tex) : Ref<Image>();
                ok = img.is_valid() && !img->is_empty() && (img->save_png(path) == OK);
            }
            snprintf(shot_status_msg, sizeof(shot_status_msg), "[F10] %s %s",
                     ok ? "Saved" : "FAILED", path);
            shot_status_timer = 4.f;
        }

        // --screenshot-frame N: non-interactive equivalent of the F10
        // handler above (see this flag's own parsing comment near argv
        // parsing) -- same capture technique, own filename so an
        // automated run's evidence never collides with an interactive
        // F10 press's output.
        if (screenshot_frame >= 0 && frame == screenshot_frame) {
            RenderingServer* rs = RenderingServer::get_singleton();
            RID vp = RID::from_uint64(bridge.ViewportRid());
            char path[128];
            snprintf(path, sizeof(path), "tmp_/libgodot_editor_screenshot_auto_%03d.png", frame);
            bool ok = false;
            if (rs && vp.is_valid()) {
                RID tex = rs->viewport_get_texture(vp);
                Ref<Image> img = tex.is_valid() ? rs->texture_2d_get(tex) : Ref<Image>();
                ok = img.is_valid() && !img->is_empty() && (img->save_png(path) == OK);
            }
            printf("[--screenshot-frame %d] %s %s\n", frame, ok ? "Saved" : "FAILED", path);

            // Also capture CharPreviewLibgodot's OWN offscreen viewport
            // texture directly (bypasses ImGui window z-order entirely --
            // the composited screenshot above can end up with the
            // Character Preview panel occluded by whichever debug panels
            // happen to stack on top of it at their ImGuiCond_FirstUseEver
            // default position in a fresh, never-rearranged-by-a-human
            // automated run). This is the more direct proof of what's
            // actually under test: the RS scenario/instance/material this
            // whole feature adds, independent of panel layout.
            if (char_preview_ok) {
                RID ptex = RID::from_uint64(CharPreviewLibgodot_TextureId());
                Ref<Image> pimg = ptex.is_valid() && rs ? rs->texture_2d_get(ptex) : Ref<Image>();
                char ppath[128];
                snprintf(ppath, sizeof(ppath), "tmp_/libgodot_char_preview_auto_%03d.png", frame);
                bool pok = pimg.is_valid() && !pimg->is_empty() && (pimg->save_png(ppath) == OK);
                printf("[--screenshot-frame %d] char-preview-viewport %s %s\n",
                       frame, pok ? "Saved" : "FAILED", ppath);
            }
        }

        io.DisplaySize = ImVec2((float)window_get_width(), (float)window_get_height());
        io.DeltaTime = 1.0f / 60.0f;
        io.AddMousePosEvent(input_mouse_x(), input_mouse_y());
        io.AddMouseButtonEvent(0, input_mouse_down(MOUSE_BUTTON_LEFT));
        io.AddMouseButtonEvent(1, input_mouse_down(MOUSE_BUTTON_RIGHT));
        io.AddMouseButtonEvent(2, input_mouse_down(MOUSE_BUTTON_MIDDLE));
        FeedKeyboardToImGui(io);
        ImGui_ImplRenderingServer_NewFrame();
        ImGui::NewFrame();

        if (hot_status_timer > 0.f) hot_status_timer -= io.DeltaTime;
        if (shot_status_timer > 0.f) shot_status_timer -= io.DeltaTime;
        EditorModule::Get().BuildUI(io.DeltaTime, 0.f, hot_status_msg, &hot_status_timer);

        float eye_x, eye_y, eye_z;
        cam.Update(io.DeltaTime, eye_x, eye_y, eye_z);
        bridge.SetCameraTransform(eye_x, eye_y, eye_z, cam.target_x, cam.target_y, cam.target_z);
        LibgodotTerrain_Update(bridge.ScenarioRid(), eye_x, eye_y, eye_z,
                               cam.target_x, cam.target_y, cam.target_z);

        // ── F3 sculpt + entity selection (parity gate blocker #2) ──────────
        CameraBasis cam_basis = MakeCameraBasis({eye_x, eye_y, eye_z},
                                                 {cam.target_x, cam.target_y, cam.target_z});
        int   vw = window_get_width(), vh = window_get_height();
        float mx = input_mouse_x(), my = input_mouse_y();
        Vec3F ray_o, ray_d;
        ScreenToRay(cam_basis, mx, my, vw, vh, ray_o, ray_d);
        bool mouse_over_ui = io.WantCaptureMouse;

        // Entity selection: left-click picks the nearest marker whose
        // world position lies within kPickRadius of the mouse ray (a
        // sphere test around each entity's known WorldTransform position,
        // not exact-mesh picking -- these markers have no real mesh to
        // test against anyway). Disabled while sculpting so LMB doesn't
        // fight the brush for the same button.
        if (!sculpt_active && !mouse_over_ui && input_mouse_pressed(MOUSE_BUTTON_LEFT)) {
            constexpr float kPickRadius = 6.f;
            float    best_t  = 1e9f;
            uint64_t best_id = 0;
            marker_query.each([&](flecs::entity e, WorldTransform& wt) {
                Vec3F p{wt.x, wt.y, wt.z};
                Vec3F v = V3Sub(p, ray_o);
                float t = V3Dot(v, ray_d);
                if (t < 0.f) return;
                Vec3F closest = V3Add(ray_o, V3Scale(ray_d, t));
                Vec3F diff = V3Sub(p, closest);
                float d2 = V3Dot(diff, diff);
                if (d2 <= kPickRadius * kPickRadius && t < best_t) {
                    best_t = t;
                    best_id = e.id();
                }
            });
            selected_id    = best_id;
            have_selection = (best_id != 0);
        }

        // Draw every marker (dim) + the selection ring (bright) via the
        // ImGui foreground draw list -- cheapest real indicator that
        // doesn't require authoring a new RS mesh/material for this step
        // (see this task's own scope note: an ImGui-drawn ring is an
        // accepted "visible indicator").
        if (ImGui::GetCurrentContext()) {
            ImDrawList* dl = ImGui::GetForegroundDrawList();
            marker_query.each([&](flecs::entity e, WorldTransform& wt) {
                float sx, sy;
                if (!WorldToScreen(cam_basis, {wt.x, wt.y, wt.z}, vw, vh, sx, sy)) return;
                bool sel = have_selection && e.id() == selected_id;
                ImU32 col = sel ? IM_COL32(255, 60, 60, 255) : IM_COL32(80, 220, 255, 200);
                float r   = sel ? 14.f : 8.f;
                dl->AddCircle(ImVec2(sx, sy), r, col, 24, sel ? 3.f : 1.5f);
                dl->AddCircleFilled(ImVec2(sx, sy), 3.f, col);
                dl->AddText(ImVec2(sx + r + 4, sy - 8), col, e.name().c_str());
            });
        }

        // Terrain brush: LMB raise / RMB lower within brush_radius of the
        // mouse ray's hit point against a horizontal plane at the real
        // sampled ground height near the camera (same single-plane
        // simplification game/src/game_camera.cpp's real SDL3 brush uses
        // -- see its own comment: accurate enough for a brush radius that
        // is a small fraction of the world, no GPU depth readback needed).
        float hit_x = eye_x, hit_z = eye_z, ref_y = 0.f;
        bool  have_hit = false;
        if (sculpt_active && TerrainAtlas_Loaded()) {
            ref_y = TerrainAtlas_SampleWorld(eye_x + kFeaturesToAtlasShift, eye_z + kFeaturesToAtlasShift);
            if (ray_d.y < -1e-6f) {
                float t = (ref_y - eye_y) / ray_d.y;
                if (t > 0.f) {
                    hit_x = eye_x + t * ray_d.x;
                    hit_z = eye_z + t * ray_d.z;
                    have_hit = true;
                }
            }

            bool lmb = input_mouse_down(MOUSE_BUTTON_LEFT);
            bool rmb = input_mouse_down(MOUSE_BUTTON_RIGHT);
            bool painting = have_hit && (lmb || rmb) && !mouse_over_ui;
            bool stroke_start = painting && !was_painting;
            was_painting = painting;

            if (painting) {
                float atlas_hit_x = hit_x + kFeaturesToAtlasShift;
                float atlas_hit_z = hit_z + kFeaturesToAtlasShift;
                float vspacing = CHUNK_SIZE / (float)(kAtlasVertsPerZone - 1);

                int zx0 = (int)floorf((atlas_hit_x - brush_radius) / CHUNK_SIZE);
                int zx1 = (int)floorf((atlas_hit_x + brush_radius) / CHUNK_SIZE);
                int zz0 = (int)floorf((atlas_hit_z - brush_radius) / CHUNK_SIZE);
                int zz1 = (int)floorf((atlas_hit_z + brush_radius) / CHUNK_SIZE);
                zx0 = zx0 < 0 ? 0 : (zx0 >= kAtlasZones ? kAtlasZones - 1 : zx0);
                zx1 = zx1 < 0 ? 0 : (zx1 >= kAtlasZones ? kAtlasZones - 1 : zx1);
                zz0 = zz0 < 0 ? 0 : (zz0 >= kAtlasZones ? kAtlasZones - 1 : zz0);
                zz1 = zz1 < 0 ? 0 : (zz1 >= kAtlasZones ? kAtlasZones - 1 : zz1);

                if (stroke_start) {
                    // Flatten target: sampled once, right here, before any
                    // write this stroke -- "the average height under the
                    // brush at the moment the stroke starts" via the same
                    // bilinear world-space sampler ground-snap call sites use.
                    if (sculpt_mode == kSculptFlatten) {
                        flatten_target_h = TerrainAtlas_SampleWorld(atlas_hit_x, atlas_hit_z);
                    }
                    // Undo snapshot: exact zone/col/row range about to be
                    // painted, captured BEFORE this stroke's first write.
                    int dimx = (zx1 - zx0 + 1) * kAtlasVertsPerZone;
                    int dimz = (zz1 - zz0 + 1) * kAtlasVertsPerZone;
                    if (dimx <= kUndoSnapDim && dimz <= kUndoSnapDim) {
                        for (int zy = zz0; zy <= zz1; ++zy) {
                            for (int zx = zx0; zx <= zx1; ++zx) {
                                for (int row = 0; row < kAtlasVertsPerZone; ++row) {
                                    for (int col = 0; col < kAtlasVertsPerZone; ++col) {
                                        int sy = (zy - zz0) * kAtlasVertsPerZone + row;
                                        int sx = (zx - zx0) * kAtlasVertsPerZone + col;
                                        s_undo_h[sy][sx] = TerrainAtlas_GetHeight(zx, zy, col, row);
                                    }
                                }
                            }
                        }
                        undo_zx0 = zx0; undo_zy0 = zz0; undo_zx1 = zx1; undo_zy1 = zz1;
                        undo_valid = true;
                    } else {
                        // Defensive only -- see kMaxBrushZonesPerAxis's own
                        // doc comment: unreachable at the current 200m slider
                        // max, but don't silently overrun s_undo_h if that
                        // ever changes.
                        MD_LOG(MD_LOG_WARNING,
                               "[F3 sculpt] brush bbox %dx%d zones exceeds undo buffer, skipping snapshot",
                               zx1 - zx0 + 1, zz1 - zz0 + 1);
                        undo_valid = false;
                    }
                }

                float delta = brush_strength * io.DeltaTime * (rmb ? -1.f : 1.f);
                // Smooth/flatten blend factor: exponential ease toward the
                // target so repeated frames never overshoot even at high
                // strength/low framerate -- brush_strength reused here as a
                // 0..15 intensity dial, not a literal metres/sec rate like
                // the raise/lower branch uses it for.
                float blend = 1.f - expf(-brush_strength * 0.2f * io.DeltaTime);

                // Fixed nested loop over zone x vertex ranges (no heap
                // alloc, bounded by kAtlasZones*kAtlasVertsPerZone) --
                // same order-of-magnitude per-frame cost as the reference
                // SDL3 brush (game/src/game_camera.cpp).
                for (int zy = zz0; zy <= zz1; ++zy) {
                    for (int zx = zx0; zx <= zx1; ++zx) {
                        float zone_ox = (float)zx * CHUNK_SIZE;
                        float zone_oz = (float)zy * CHUNK_SIZE;
                        for (int row = 0; row < kAtlasVertsPerZone; ++row) {
                            float vz  = zone_oz + (float)row * vspacing;
                            float ddz = vz - atlas_hit_z;
                            for (int col = 0; col < kAtlasVertsPerZone; ++col) {
                                float vx  = zone_ox + (float)col * vspacing;
                                float ddx = vx - atlas_hit_x;
                                float d2  = ddx * ddx + ddz * ddz;
                                if (d2 > brush_radius * brush_radius) continue;
                                float falloff = 1.f - sqrtf(d2) / brush_radius;
                                float h = TerrainAtlas_GetHeight(zx, zy, col, row);
                                float new_h = h;
                                if (sculpt_mode == kSculptRaiseLower) {
                                    new_h = h + delta * falloff;
                                } else if (sculpt_mode == kSculptSmooth) {
                                    // Box-blur: this vertex + its 4 grid
                                    // neighbours (global-vertex coords so
                                    // zone-boundary neighbours resolve
                                    // correctly, see TerrainAtlas_GetHeightGlobal).
                                    int gcol = zx * (kAtlasVertsPerZone - 1) + col;
                                    int grow = zy * (kAtlasVertsPerZone - 1) + row;
                                    float avg = (h
                                               + TerrainAtlas_GetHeightGlobal(gcol - 1, grow)
                                               + TerrainAtlas_GetHeightGlobal(gcol + 1, grow)
                                               + TerrainAtlas_GetHeightGlobal(gcol, grow - 1)
                                               + TerrainAtlas_GetHeightGlobal(gcol, grow + 1)) / 5.f;
                                    new_h = h + (avg - h) * blend * falloff;
                                } else { // kSculptFlatten
                                    new_h = h + (flatten_target_h - h) * blend * falloff;
                                }
                                TerrainAtlas_SetHeight(zx, zy, col, row, new_h);
                            }
                        }
                    }
                }
                // Force the currently-visible LOD node(s) covering this
                // rect to rebuild their height_tex this frame -- without
                // this, LibgodotTerrain_Update's own (depth,gx,gz) cache
                // means the edit above would be invisible until the
                // camera moved far enough to evict/reload the node.
                LibgodotTerrain_InvalidateRegion(hit_x - brush_radius, hit_z - brush_radius,
                                                  hit_x + brush_radius, hit_z + brush_radius);
            }

            // Ctrl+Z: restore the exact snapshot captured at the start of
            // the last brush stroke (one level only -- see s_undo_h's own
            // doc comment above). Gated on !io.WantCaptureKeyboard, not
            // !mouse_over_ui -- this is a keyboard shortcut, not a mouse
            // action, so an ImGui text field having keyboard focus is the
            // right thing to check, not viewport mouse hover.
            if (undo_valid && !io.WantCaptureKeyboard &&
                (input_key_down(KEY_LEFT_CONTROL) || input_key_down(KEY_RIGHT_CONTROL)) &&
                input_key_pressed(KEY_Z)) {
                for (int zy = undo_zy0; zy <= undo_zy1; ++zy) {
                    for (int zx = undo_zx0; zx <= undo_zx1; ++zx) {
                        for (int row = 0; row < kAtlasVertsPerZone; ++row) {
                            for (int col = 0; col < kAtlasVertsPerZone; ++col) {
                                int sy = (zy - undo_zy0) * kAtlasVertsPerZone + row;
                                int sx = (zx - undo_zx0) * kAtlasVertsPerZone + col;
                                TerrainAtlas_SetHeight(zx, zy, col, row, s_undo_h[sy][sx]);
                            }
                        }
                    }
                }
                float undo_wx0 = (float)undo_zx0 * CHUNK_SIZE - kFeaturesToAtlasShift;
                float undo_wz0 = (float)undo_zy0 * CHUNK_SIZE - kFeaturesToAtlasShift;
                float undo_wx1 = (float)(undo_zx1 + 1) * CHUNK_SIZE - kFeaturesToAtlasShift;
                float undo_wz1 = (float)(undo_zy1 + 1) * CHUNK_SIZE - kFeaturesToAtlasShift;
                LibgodotTerrain_InvalidateRegion(undo_wx0, undo_wz0, undo_wx1, undo_wz1);
                undo_valid = false; // one level only -- consumed
                snprintf(sculpt_status_msg, sizeof(sculpt_status_msg), "Undo OK");
                sculpt_status_timer = 3.f;
            }

            if (have_hit && ImGui::GetCurrentContext()) {
                float sx, sy;
                if (WorldToScreen(cam_basis, {hit_x, ref_y, hit_z}, vw, vh, sx, sy)) {
                    float screen_r = 40.f;
                    float sx2, sy2;
                    Vec3F edge = V3Add({hit_x, ref_y, hit_z}, V3Scale(cam_basis.right, brush_radius));
                    if (WorldToScreen(cam_basis, edge, vw, vh, sx2, sy2)) {
                        float ddx = sx2 - sx, ddy = sy2 - sy;
                        screen_r = sqrtf(ddx * ddx + ddy * ddy);
                    }
                    const char* mode_label =
                        sculpt_mode == kSculptSmooth  ? "[Smooth]"  :
                        sculpt_mode == kSculptFlatten ? "[Flatten]" :
                        (rmb ? "[Lower]" : "[Raise]");
                    ImDrawList* dl = ImGui::GetForegroundDrawList();
                    dl->AddCircle(ImVec2(sx, sy), screen_r, IM_COL32(255, 200, 0, 200), 48, 1.5f);
                    dl->AddText(ImVec2(sx + screen_r + 4, sy - 8), IM_COL32(255, 220, 0, 255),
                                mode_label);
                }
            }
        }

        ImGui::Begin("Selected Entity");
        if (have_selection) {
            flecs::entity e(Registry::Get(), selected_id);
            if (e.is_alive() && e.has<WorldTransform>()) {
                const WorldTransform& wt = e.get<WorldTransform>();
                ImGui::Text("name: %s", e.name().c_str());
                ImGui::Text("id:   %llu", (unsigned long long)selected_id);
                ImGui::Text("pos:  (%.1f, %.1f, %.1f)", wt.x, wt.y, wt.z);
            } else {
                ImGui::TextDisabled("(entity no longer alive)");
                have_selection = false;
            }
        } else {
            ImGui::TextDisabled("Left-click a marker in the viewport to select it.");
        }
        ImGui::End();

        ImGui::Begin("Terrain Sculpt (F3)");
        if (!TerrainAtlas_Loaded()) {
            ImGui::TextColored({1.f, 0.4f, 0.2f, 1.f}, "! Height atlas not loaded");
        } else {
            ImGui::Checkbox("Activate Sculpt", &sculpt_active);
            ImGui::RadioButton("Raise/Lower", &sculpt_mode, kSculptRaiseLower); ImGui::SameLine();
            ImGui::RadioButton("Smooth", &sculpt_mode, kSculptSmooth); ImGui::SameLine();
            ImGui::RadioButton("Flatten", &sculpt_mode, kSculptFlatten);
            ImGui::SliderFloat("Radius (m)", &brush_radius, 5.f, 200.f, "%.0f m");
            ImGui::SliderFloat("Strength", &brush_strength, 0.1f, 15.f, "%.1f");
            if (sculpt_mode == kSculptRaiseLower) {
                ImGui::TextDisabled("LMB Raise   RMB Lower");
            } else if (sculpt_mode == kSculptSmooth) {
                ImGui::TextDisabled("LMB/RMB: box-blur average toward neighbours");
            } else {
                ImGui::TextDisabled("LMB/RMB: blend toward height at stroke start");
            }
            ImGui::TextDisabled(undo_valid ? "Ctrl+Z: undo last stroke" : "Ctrl+Z: (no stroke to undo)");
            if (sculpt_active)
                ImGui::TextColored({0.2f, 1.f, 0.4f, 1.f}, "SCULPT ACTIVE");
            if (ImGui::Button("Save Height Map", ImVec2(-1, 0))) {
                bool ok = TerrainAtlas_SaveEdits("game/data/terrain/world_hmap");
                snprintf(sculpt_status_msg, sizeof(sculpt_status_msg), "%s", ok ? "Saved OK" : "Save FAILED");
                sculpt_status_timer = 3.f;
            }
            if (sculpt_status_timer > 0.f) {
                sculpt_status_timer -= io.DeltaTime;
                ImGui::TextUnformatted(sculpt_status_msg);
            }
        }
        ImGui::End();

        // ── Character preview panel (parity gate blocker #5) ────────────────
        // ImGui::Image() of CharPreviewLibgodot's own offscreen viewport
        // texture — the raw RID's get_id() IS the ImTextureID this
        // backend's canvas draws with directly (imgui_impl_renderingserver.
        // cpp's RenderDrawData() already does the same RID::from_uint64()
        // cast for every draw command's texture, no separate ImTextureData
        // registration needed for a texture we created ourselves).
        if (char_preview_ok) {
            ImGui::SetNextWindowSize(ImVec2(460, 700), ImGuiCond_FirstUseEver);
            ImGui::Begin("Character Preview");
            ImGui::TextUnformatted("Skin Colour");
            if (ImGui::ColorEdit3("##char_skin_tint", char_skin_rgb)) {
                CharPreviewLibgodot_SetSkinColor(char_skin_rgb[0], char_skin_rgb[1], char_skin_rgb[2]);
            }

            // ── Clothing (real .clothbin items, slot 0=top / 1=bottom) ──
            ImGui::Separator();
            ImGui::TextUnformatted("Clothing");
            static const char* kClothSlotLabel[2] = {"Top   ", "Bottom"};
            for (int slot = 0; slot < 2; ++slot) {
                ImGui::PushID(slot);
                ImGui::TextUnformatted(kClothSlotLabel[slot]);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::BeginCombo("##clothitem", kClothItems[char_cloth_sel[slot]].name)) {
                    for (int i = 0; i < kClothItemCount; ++i) {
                        if (kClothItems[i].slot != slot) continue;
                        bool sel = (i == char_cloth_sel[slot]);
                        if (ImGui::Selectable(kClothItems[i].name, sel)) {
                            char_cloth_sel[slot] = i;
                            const LibgodotClothItem& item = kClothItems[i];
                            std::string full_path = item.path ? (launch_cwd + "/" + item.path) : std::string();
                            CharPreviewLibgodot_SetClothingSlot(
                                slot, item.path ? full_path.c_str() : nullptr,
                                item.color[0], item.color[1], item.color[2]);
                        }
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                ImGui::PopID();
            }

            // ── Hair (real game/data/hair/*.glb, 30 real styles) ────────
            ImGui::Separator();
            ImGui::TextUnformatted("Hair Style");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::BeginCombo("##hairstyle", kHairStyles[char_hair_sel])) {
                for (int i = 0; i < kHairStyleCount; ++i) {
                    bool sel = (i == char_hair_sel);
                    if (ImGui::Selectable(kHairStyles[i], sel)) {
                        char_hair_sel = i;
                        std::string hair_path = launch_cwd + "/game/data/hair/" + kHairStyles[i] + ".glb";
                        CharPreviewLibgodot_SetHairStyle(hair_path.c_str(),
                                                          char_hair_rgb[0], char_hair_rgb[1], char_hair_rgb[2]);
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::TextUnformatted("Hair Colour");
            if (ImGui::ColorEdit3("##char_hair_tint", char_hair_rgb)) {
                CharPreviewLibgodot_SetHairTint(char_hair_rgb[0], char_hair_rgb[1], char_hair_rgb[2]);
            }
            ImGui::Separator();

            ImVec2 preview_size((float)CharPreviewLibgodot_ViewportW(), (float)CharPreviewLibgodot_ViewportH());
            ImGui::Image((ImTextureID)(uintptr_t)CharPreviewLibgodot_TextureId(), preview_size);
            // LMB-drag over the preview image orbits the camera; idle ->
            // slow auto-rotate (see CharPreviewLibgodot_Update's own doc
            // comment). Scoped to ImGui::IsItemHovered() on the Image
            // widget specifically, so it never fights the main viewport's
            // RMB-drag orbit (OrbitCamera above) or other panels' widgets.
            bool preview_dragging = ImGui::IsItemHovered() && input_mouse_down(MOUSE_BUTTON_LEFT);
            CharPreviewLibgodot_Update(io.DeltaTime, preview_dragging, io.MouseDelta.x, io.MouseDelta.y, 0.f);
            ImGui::TextDisabled("LMB-drag to orbit; auto-rotates when idle");
            ImGui::End();
        }

        ImGui::Begin("monkey_dust_libgodot_editor (3D viewport step)");
        ImGui::Text("frame %d / %d", frame, max_frames);
        ImGui::Text("RenderingServer-backed ImGui, no SDL3");
        ImGui::Text("eye    (%.1f, %.1f, %.1f)", eye_x, eye_y, eye_z);
        ImGui::Text("target (%.1f, %.1f, %.1f)", cam.target_x, cam.target_y, cam.target_z);
        ImGui::Text("RMB-drag orbit, W/S zoom (scroll unavailable on this backend)");
        ImGui::Text("F10: screenshot -> tmp_/libgodot_editor_screenshot_NNN.png");
        if (hot_status_timer > 0.f) ImGui::TextColored({0.4f, 1.f, 0.4f, 1.f}, "%s", hot_status_msg);
        if (shot_status_timer > 0.f) ImGui::TextColored({0.4f, 0.8f, 1.f, 1.f}, "%s", shot_status_msg);
        ImGui::End();

        ImGui::Render();
        ImGui_ImplRenderingServer_RenderDrawData(ImGui::GetDrawData());

        window_end_frame();
    }

    printf("OK: %d frames complete, no crash\n", max_frames);

    EditorModule::Get().Shutdown();  // calls editor_panels_shutdown() before dlclose
    ImGui_ImplRenderingServer_Shutdown();
    ImGui::DestroyContext();
    LibgodotTerrain_Shutdown();  // MUST run before bridge.Shutdown() -- see its own doc comment
    CharPreviewLibgodot_Shutdown();  // own scenario/viewport/doc-state -- same ordering rule
    bridge.Shutdown();
    window_shutdown();
    printf("OK: tools/editor/main_libgodot.cpp complete\n");
    return 0;
}
