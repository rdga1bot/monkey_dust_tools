#pragma once
#ifdef DEBUG
#ifdef USE_SDL3
#  include "backends/imgui_impl_sdl3.h"
#  include "backends/imgui_impl_opengl3.h"
#else
#  include "raylib.h"
#  include "rlImGui.h"
#endif
#include "imgui.h"
#include "editor_history.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/render/md_camera.h>
#include <entt/entt.hpp>

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
    EditorGizmoOp    gizmo_op    = EditorGizmoOp::TRANSLATE;
    EditorGizmoSpace gizmo_space = EditorGizmoSpace::WORLD;

    // ── Selection ─────────────────────────────────────────
    static constexpr int MAX_SELECTED = 64;
    entt::entity selected[MAX_SELECTED];
    int          selected_count = 0;

    entt::entity GetPrimary() const;
    void         Select(entt::entity e, bool add = false);
    void         Deselect(entt::entity e);
    void         DeselectAll();
    bool         IsSelected(entt::entity e) const;

    // ── Editor Camera ─────────────────────────────────────
    MdCamera editor_cam;          // was Camera3D — now platform-neutral
    float    cam_yaw     = -45.0f;
    float    cam_pitch   = 25.0f;
    float    cam_dist    = 35.0f;
    Vec3     cam_target  = { 0.f, 2.f, 0.f };
    bool     cam_flying  = false;
    float    cam_speed   = 30.0f;

    void UpdateEditorCamera(float dt, bool viewport_hovered);
    void FocusOnSelected();

    // ── History ───────────────────────────────────────────
    CommandStack history;
    void Undo();
    void Redo();

    // ── Panel visibility ──────────────────────────────────
    // [0]=Hierarchy [1]=Inspector [2]=Assets [3]=Console
    // [4]=Graphics  [5]=Camera   [6]=Animation [7]=Paint
    bool panels_visible[8] = { true, true, true, true, true, true, false, false };

    // Set by EditorToolbar::Draw each frame; read by camera/animation panels.
    float frame_fps   = 0.f;
    float frame_dt_ms = 0.f;

private:
    EditorCore() = default;
};
#endif
