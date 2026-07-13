#include "lua_editor_scenario_api.h"
#include <monkey_dust/scripting/lua_system.h>
#include <monkey_dust/world/terrain_gen.h>
#include "editor_world_3d_sdlgpu.h"
#include "editor_screenshot.h"
#include <cstring>
#include <cstdio>
#include <cmath>

extern "C" {
#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
}

// ── md.editor_open_panel(name) ────────────────────────────────────────────────
// Etap 4 scope: only "3D World" is wired to actually force-select the tab
// (that's the one VERIFY needs — teleport+screenshot happens there). Other
// tab names are accepted (stored, no error) but do nothing yet; wire them
// the same way if a future scenario needs them — don't pre-wrap every panel.
static char s_forced_tab[64] = {};

static int l_md_editor_open_panel(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    strncpy(s_forced_tab, name, sizeof(s_forced_tab) - 1);
    return 0;
}

const char* EditorPanels_ConsumeForcedTab() {
    if (s_forced_tab[0] == '\0') return nullptr;
    static char out[64];
    strncpy(out, s_forced_tab, sizeof(out) - 1);
    s_forced_tab[0] = '\0';  // one-shot — BeginTabItem only needs SetSelected once
    return out;
}

// ── md.editor_select_zone(x, y) — jump 3D World camera to a zone centre ──────
static int l_md_editor_select_zone(lua_State* L) {
    int zx = (int)luaL_checkinteger(L, 1);
    int zz = (int)luaL_checkinteger(L, 2);
#ifdef MD_SDL_GPU
    WorldEditor3D_SDLGPU::TeleportToZone(zx, zz);
#else
    (void)zx; (void)zz;
#endif
    return 0;
}

// ── md.editor_camera_pos(x, y, z, yaw, pitch) — full camera pose control ─────
// Visual-diagnostic tool (not part of the original Etap 4 "3-5 operations"
// scope) — added specifically to inspect the POM/forward shader-pass
// crossfade boundary near ground level; TeleportToZone's fixed aerial
// altitude can't get close enough to see it.
static int l_md_editor_camera_pos(lua_State* L) {
    float x     = (float)luaL_checknumber(L, 1);
    float y     = (float)luaL_checknumber(L, 2);
    float z     = (float)luaL_checknumber(L, 3);
    float yaw   = (float)luaL_optnumber(L, 4, 0.0);
    float pitch = (float)luaL_optnumber(L, 5, 0.38);
#ifdef MD_SDL_GPU
    WorldEditor3D_SDLGPU::SetCameraPos(x, y, z, yaw, pitch);
#else
    (void)x; (void)y; (void)z; (void)yaw; (void)pitch;
#endif
    return 0;
}

// ── md.editor_terrain_op(op, x, z) -> result ─────────────────────────────────
// Etap 4 scope: "height" is the one operation actually needed for terrain
// verification (matches game-side md.terrain_height's TerrainQuery source of
// truth, but via the editor's own already-loaded TerrainMaster atlas — same
// underlying md_master_hmap.r32, no second implementation of the sampling
// transform). Not a general brush-edit API — see plan's own "3-5 operations
// really needed, don't wrap every panel" scope note.
static int l_md_editor_terrain_op(lua_State* L) {
    const char* op = luaL_checkstring(L, 1);
    if (strcmp(op, "height") == 0) {
        float x = (float)luaL_checknumber(L, 2);
        float z = (float)luaL_checknumber(L, 3);
        float h = TerrainMaster_SampleWorld(x, z);
        lua_pushnumber(L, (lua_Number)h);
        return 1;
    }
    if (strcmp(op, "chunks_loaded") == 0) {
        // World3D's full 64x64=4096-chunk background build (editor_world_3d_
        // sdlgpu.cpp) — designed for ~0.5s at 60fps, observed much slower in
        // this session's environment; scenarios must wait_until this reaches
        // chunks_total before a screenshot reflects real terrain, not the
        // partial-load placeholder.
#ifdef MD_SDL_GPU
        lua_pushinteger(L, WorldEditor3D_SDLGPU::GetChunksLoaded());
        lua_pushinteger(L, WorldEditor3D_SDLGPU::GetChunksTotal());
        return 2;
#else
        lua_pushinteger(L, 0); lua_pushinteger(L, 0);
        return 2;
#endif
    }
    return luaL_error(L, "md.editor_terrain_op: unsupported op '%s' (Etap 4 supports \"height\", \"chunks_loaded\")", op);
}

// ── md.log(msg) — same convention as game's lua_scenario_api.cpp: bare
// stdout, [SCRIPT] prefix, not MD_LOG (defaults to stderr, md_log.h). Not
// part of the shared engine bootstrap (only wait_ticks/wait_until/quit are)
// — every consumer registers its own, matching the game side exactly.
static int l_md_log(lua_State* L) {
    const char* msg = lua_tostring(L, 1);
    if (msg) { printf("[SCRIPT] %s\n", msg); fflush(stdout); }
    return 0;
}

// ── md.assert_eq/assert_true — same [ASSERT FAIL] convention as game's
// lua_scenario_api.cpp, needed for VERIFY's RMSE threshold check.
static int l_md_assert_eq(lua_State* L) {
    double a   = luaL_checknumber(L, 1);
    double b   = luaL_checknumber(L, 2);
    double eps = luaL_optnumber(L, 3, 0.0001);
    const char* label = luaL_optstring(L, 4, "");
    if (fabs(a - b) > eps) {
        printf("[ASSERT FAIL] %s: ОЧІКУВАНЕ=%.6f ФАКТ=%.6f (eps=%.6f)\n", label, a, b, eps);
        return luaL_error(L, "assert_eq failed: %s", label);
    }
    return 0;
}

static int l_md_assert_true(lua_State* L) {
    bool cond = lua_toboolean(L, 1);
    const char* label = luaL_optstring(L, 2, "");
    if (!cond) {
        printf("[ASSERT FAIL] %s: ОЧІКУВАНЕ=true ФАКТ=false\n", label);
        return luaL_error(L, "assert_true failed: %s", label);
    }
    return 0;
}

// ── md.editor_screenshot(path) ────────────────────────────────────────────────
// Real SDL_GPU swapchain readback (editor_screenshot.h/.cpp) — the actual
// capture happens later THIS frame in main.cpp's render block (where the
// swapchain texture is available), not here. This just records the request.
static int l_md_editor_screenshot(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
#ifdef MD_SDL_GPU
    EditorScreenshot_RequestPending(path);
#else
    (void)path;
#endif
    return 0;
}

void RegisterLuaEditorScenarioAPI(LuaSystem& sys) {
    sys.RegisterNamespaceFunction("editor_open_panel",  l_md_editor_open_panel);
    sys.RegisterNamespaceFunction("editor_select_zone", l_md_editor_select_zone);
    sys.RegisterNamespaceFunction("editor_camera_pos",  l_md_editor_camera_pos);
    sys.RegisterNamespaceFunction("editor_terrain_op",  l_md_editor_terrain_op);
    sys.RegisterNamespaceFunction("editor_screenshot",  l_md_editor_screenshot);
    sys.RegisterNamespaceFunction("log",                l_md_log);
    sys.RegisterNamespaceFunction("assert_eq",          l_md_assert_eq);
    sys.RegisterNamespaceFunction("assert_true",        l_md_assert_true);
}
