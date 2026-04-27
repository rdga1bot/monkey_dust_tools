#ifdef MONKEY_DUST_EDITOR
#include "editor_core.h"
#include "editor_toolbar.h"
#include "editor_hierarchy.h"
#include "editor_inspector.h"
#include "editor_asset_browser.h"
#include "editor_graphics_panel.h"
#include "editor_camera_panel.h"
#include "editor_console.h"
#include "editor_animation_panel.h"
#include "editor_flare_browser.h"
#include <monkey_dust/world/world_transform.h>
#include "raymath.h"
#include <cmath>
#include <cstring>

void EditorCore::Init() {
    EditorConsole::Get().Init();
    for (int i = 0; i < MAX_SELECTED; ++i)
        selected[i] = entt::null;

    editor_cam.position   = { 0.f, 35.f, 35.f };
    editor_cam.target     = cam_target;
    editor_cam.up         = { 0.f, 1.f, 0.f };
    editor_cam.fovy       = 60.0f;
    editor_cam.projection = CAMERA_PERSPECTIVE;
}

void EditorCore::Update(float dt) {
    EditorToolbar::Get().Draw(dt);
    EditorHierarchy::Get().Draw();
    EditorInspector::Get().Draw();
    EditorAssetBrowser::Get().Draw();
    EditorGraphicsPanel::Get().Draw();
    EditorCameraPanel::Get().Draw();
    EditorConsole::Get().Draw();
    EditorAnimationPanel::Get().Draw();
    EditorFlareBrowser::Get().Draw();
}

void EditorCore::Shutdown() {
    DeselectAll();
}

// ── Selection ─────────────────────────────────────────────────
entt::entity EditorCore::GetPrimary() const {
    return (selected_count > 0) ? selected[0] : entt::null;
}

void EditorCore::Select(entt::entity e, bool add) {
    if (!add) DeselectAll();
    if (IsSelected(e)) return;
    if (selected_count >= MAX_SELECTED) return;
    selected[selected_count++] = e;
}

void EditorCore::Deselect(entt::entity e) {
    for (int i = 0; i < selected_count; ++i) {
        if (selected[i] == e) {
            selected[i] = selected[--selected_count];
            selected[selected_count] = entt::null;
            return;
        }
    }
}

void EditorCore::DeselectAll() {
    for (int i = 0; i < selected_count; ++i) selected[i] = entt::null;
    selected_count = 0;
}

bool EditorCore::IsSelected(entt::entity e) const {
    for (int i = 0; i < selected_count; ++i)
        if (selected[i] == e) return true;
    return false;
}

// ── Editor Camera ─────────────────────────────────────────────
void EditorCore::UpdateEditorCamera(float dt, bool viewport_hovered) {
    (void)viewport_hovered;

    if (cam_flying) {
        // WASD fly mode (RMB held)
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            Vector2 md = GetMouseDelta();
            cam_yaw   -= md.x * 0.3f;
            cam_pitch  += md.y * 0.3f;
            if (cam_pitch >  89.f) cam_pitch =  89.f;
            if (cam_pitch < -89.f) cam_pitch = -89.f;

            float yaw_r   = cam_yaw   * DEG2RAD;
            float pitch_r = cam_pitch * DEG2RAD;
            Vector3 fwd = {
                cosf(pitch_r) * sinf(yaw_r),
                sinf(pitch_r),
                cosf(pitch_r) * cosf(yaw_r)
            };
            Vector3 right = Vector3Normalize(
                Vector3CrossProduct(fwd, {0.f, 1.f, 0.f}));

            float speed = cam_speed * dt;
            if (IsKeyDown(KEY_W)) cam_target = Vector3Add(cam_target, Vector3Scale(fwd,  speed));
            if (IsKeyDown(KEY_S)) cam_target = Vector3Add(cam_target, Vector3Scale(fwd, -speed));
            if (IsKeyDown(KEY_A)) cam_target = Vector3Add(cam_target, Vector3Scale(right,-speed));
            if (IsKeyDown(KEY_D)) cam_target = Vector3Add(cam_target, Vector3Scale(right, speed));
        }
    } else {
        // Orbit mode
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) {
            Vector2 md = GetMouseDelta();
            cam_yaw   -= md.x * 0.4f;
            cam_pitch  += md.y * 0.4f;
            if (cam_pitch >  89.f) cam_pitch =  89.f;
            if (cam_pitch < -89.f) cam_pitch = -89.f;
        }
        // MMB pan
        if (IsMouseButtonDown(MOUSE_BUTTON_MIDDLE)) {
            Vector2 md = GetMouseDelta();
            float yaw_r = cam_yaw * DEG2RAD;
            Vector3 right = { cosf(yaw_r), 0.f, -sinf(yaw_r) };
            Vector3 up    = { 0.f, 1.f, 0.f };
            float pan = cam_dist * 0.002f;
            cam_target = Vector3Subtract(cam_target, Vector3Scale(right, md.x * pan));
            cam_target = Vector3Add(cam_target,      Vector3Scale(up,    md.y * pan));
        }
        // Scroll zoom
        float wheel = GetMouseWheelMove();
        cam_dist -= wheel * cam_dist * 0.1f;
        if (cam_dist < 1.f)   cam_dist = 1.f;
        if (cam_dist > 500.f) cam_dist = 500.f;
    }

    float yaw_r   = cam_yaw   * DEG2RAD;
    float pitch_r = cam_pitch * DEG2RAD;
    editor_cam.position = {
        cam_target.x + cam_dist * cosf(pitch_r) * sinf(yaw_r),
        cam_target.y + cam_dist * sinf(pitch_r),
        cam_target.z + cam_dist * cosf(pitch_r) * cosf(yaw_r)
    };
    editor_cam.target = cam_target;
    editor_cam.up     = { 0.f, 1.f, 0.f };
}

void EditorCore::FocusOnSelected() {
    if (selected_count == 0) return;
    auto& reg = Registry::Get();
    entt::entity e = GetPrimary();
    if (!reg.valid(e)) return;
    if (!reg.all_of<WorldTransform>(e)) return;
    const auto& tr = reg.get<WorldTransform>(e);
    cam_target = { tr.x, 0.f, tr.z };
    cam_dist   = 15.f;
}

// ── History ───────────────────────────────────────────────────
void EditorCore::Undo() {
    history.Undo(Registry::Get());
}

void EditorCore::Redo() {
    history.Redo(Registry::Get());
}
#endif
