#pragma once
#include <monkey_dust/ecs/md_entity.h>
#ifdef MONKEY_DUST_EDITOR
// Raylib fallback (rlImGui) removed 2026-08-09 -- USE_SDL3=ON is the only
// buildable configuration (engine/CMakeLists.txt's FATAL_ERROR gate).
#  include "backends/imgui_impl_sdl3.h"
#  include "backends/imgui_impl_opengl3.h"
#include "imgui.h"
#include "editor_history.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/render/md_camera.h>

enum class EditorGizmoOp    { TRANSLATE = 0, ROTATE = 1, SCALE = 2 };
enum class EditorGizmoSpace { LOCAL = 0, WORLD = 1 };

class EditorCore {
public:
    static EditorCore& Get() { static EditorCore inst; return inst; }

    void Init();
    void Update(float dt);
    void Shutdown();

    // ── State ─────────────────────────────────────────────
    bool editor_open    = false;
    bool physics_paused = true;
    int  active_tab     = 0;  // mirrors s_f3_active_tab (0=Scene…5=Camera,6=Settings)
    // Set to true by main.cpp when a full-screen viewport tab (e.g. 3D World) is
    // active so ##f3editor gets NoMouseInputs and stops blocking the viewport.
    bool f3_passthrough = false;

    // F3 panel detach states + positions (persisted to data/editor_f3_layout.json)
    bool f3_det_scene    = false, f3_det_ai    = false, f3_det_anim     = false;
    bool f3_det_flow     = false, f3_det_debug = false, f3_det_cam      = false;
    bool f3_det_settings = false;
    // Default positions spread across 1280×720; y >= 85 so title bars never overlap the tab bar.
    ImVec2 f3_pos_scene    = { 10.f,  85.f},  f3_size_scene    = {420.f, 545.f};
    ImVec2 f3_pos_ai       = {440.f,  85.f},  f3_size_ai       = {410.f, 430.f};
    ImVec2 f3_pos_anim     = { 10.f, 435.f},  f3_size_anim     = {870.f, 255.f};
    ImVec2 f3_pos_flow     = {230.f,  85.f},  f3_size_flow     = {820.f, 570.f};
    ImVec2 f3_pos_debug    = {870.f, 255.f},  f3_size_debug    = {390.f, 410.f};
    ImVec2 f3_pos_cam      = {960.f,  85.f},  f3_size_cam      = {300.f, 225.f};
    ImVec2 f3_pos_settings = {400.f, 200.f},  f3_size_settings = {360.f, 200.f};
    EditorGizmoOp    gizmo_op    = EditorGizmoOp::TRANSLATE;
    EditorGizmoSpace gizmo_space = EditorGizmoSpace::WORLD;

    // ── Selection ─────────────────────────────────────────
    static constexpr int MAX_SELECTED = 64;
    MdEntity selected[MAX_SELECTED];
    int          selected_count = 0;

    MdEntity GetPrimary() const;
    void         Select(MdEntity e, bool add = false);
    void         Deselect(MdEntity e);
    void         DeselectAll();
    bool         IsSelected(MdEntity e) const;

    // ── Editor Camera ─────────────────────────────────────
    MdCamera editor_cam;          // was Camera3D — now platform-neutral
    float    cam_yaw     = -45.0f;
    float    cam_pitch   = 25.0f;
    float    cam_dist    = 35.0f;
    Vec3     cam_target  = { 0.f, 2.f, 0.f };
    bool     cam_flying   = false;
    bool     cam_game_mode = false;  // true = use normal game camera (player moves, RMB rotates)
    float    cam_speed    = 100.0f; // m/s; Shift+Scroll to adjust
    Vec3     last_game_cam_target = { 0.f, 2.f, 0.f }; // updated each frame in game mode
    // Absolute-Kenshi-world-space shift (metres) for the current session's
    // floating local origin: abs = local + (scene_world_dx, scene_world_dz).
    // Written from game/ each frame (game_render_frame.cpp, EditorCore::Update()
    // call site) -- SceneRender::GraniteAbsCam (scene_render.cpp:565-571)
    // proves this shift is independent of the point being converted, so only
    // the constant itself crosses the tools/<->game boundary; the actual
    // per-point add + zone lookup happen tools/-side (editor_camera_panel.cpp).
    float    scene_world_dx = 0.f, scene_world_dz = 0.f;
    // Free-fly state (mirrors editor_world_3d_sdlgpu — radians, direct eye pos)
    float    fly_yaw     = 0.f;     // radians
    float    fly_pitch   = 0.f;     // radians
    bool     fly_rel_active = false;

    void UpdateEditorCamera(float dt, bool viewport_hovered);
    void FocusOnSelected();

    // ── History ───────────────────────────────────────────
    CommandStack history;
    void Undo();
    void Redo();

    // ── Panel visibility ──────────────────────────────────
    // [0]=Hierarchy [1]=Inspector  [2]=—removed— [3]=Console
    // [4]=—removed— [5]=Camera     [6]=Animation [7]=Paint
    // [8]=ViewCone  [9]=—removed— [10]=Director [11]=GPU Profiler
    // [12]=—removed— [13]=—removed— [14]=—removed—
    bool panels_visible[15] = { true,  true,  false, true,
                                false, true,  true,  false,
                                false, false, false, false,
                                false, false, false };

    // Set by EditorToolbar::Draw each frame; read by camera/animation panels.
    float frame_fps   = 0.f;
    float frame_dt_ms = 0.f;

private:
    EditorCore() = default;
};
#endif
