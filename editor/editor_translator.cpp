#ifdef MONKEY_DUST_EDITOR
#include "editor_translator.h"
#include "editor_core.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/world/world_transform.h>
#include "raymath.h"
#include <cmath>
#ifdef MD_OPENGL43_ENABLED
#include <monkey_dust/world/transform_soa.h>
#endif

// ── Ray helpers ───────────────────────────────────────────────────────────────
static float RaySphereIntersect(Ray ray, Vector3 center, float radius) {
    Vector3 oc = Vector3Subtract(ray.position, center);
    float b = Vector3DotProduct(oc, ray.direction);
    float c = Vector3DotProduct(oc, oc) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.f) return -1.f;
    float t = -b - sqrtf(disc);
    return (t > 0.f) ? t : -1.f;
}

static float RayPlaneIntersect(Ray ray, Vector3 normal, Vector3 point) {
    float denom = Vector3DotProduct(normal, ray.direction);
    if (fabsf(denom) < 1e-6f) return -1.f;
    float t = Vector3DotProduct(Vector3Subtract(point, ray.position), normal) / denom;
    return (t > 0.f) ? t : -1.f;
}

static Vector3 RayAt(Ray ray, float t) {
    return Vector3Add(ray.position, Vector3Scale(ray.direction, t));
}

static float SnapF(float v, float snap) {
    return roundf(v / snap) * snap;
}

// ── Plane selection per axis ──────────────────────────────────────────────────
Vector3 EditorTranslator::ComputePlaneHit(Ray ray, EditorGizmoOp op,
                                           int axis, Vector3 ep, Camera3D cam) {
    Vector3 normal;

    if (op == EditorGizmoOp::ROTATE) {
        normal = {0.f, 1.f, 0.f}; // XZ plane for Y ring
    } else {
        if (axis == 1) {
            // Y drag: vertical plane facing camera (contains Y axis)
            float dx = cam.position.x - ep.x;
            float dz = cam.position.z - ep.z;
            float len = sqrtf(dx * dx + dz * dz);
            if (len < 0.001f) len = 0.001f;
            normal = {dx / len, 0.f, dz / len};
        } else {
            // X or Z drag: XZ plane (horizontal)
            normal = {0.f, 1.f, 0.f};
        }
    }

    float t = RayPlaneIntersect(ray, normal, ep);
    return (t > 0.f) ? RayAt(ray, t) : ep;
}

// ── Draw ──────────────────────────────────────────────────────────────────────
void EditorTranslator::Draw(Camera3D cam, entt::entity sel, EditorGizmoOp op) {
    (void)cam;
    if (sel == entt::null) return;
    auto& reg = Registry::Get();
    if (!reg.valid(sel) || !reg.all_of<WorldTransform>(sel)) return;
    const auto& tr = reg.get<WorldTransform>(sel);
    Vector3 pos = {tr.x, tr.y, tr.z};

    Color cx = (active_axis_ == 0) ? YELLOW : RED;
    Color cy = (active_axis_ == 1) ? YELLOW : GREEN;
    Color cz = (active_axis_ == 2) ? YELLOW : BLUE;

    if (op == EditorGizmoOp::TRANSLATE) {
        DrawLine3D(pos, Vector3Add(pos, {3.5f, 0.f, 0.f}), cx);
        DrawLine3D(pos, Vector3Add(pos, {0.f,  3.5f, 0.f}), cy);
        DrawLine3D(pos, Vector3Add(pos, {0.f,  0.f, 3.5f}), cz);
        DrawSphere(Vector3Add(pos, {3.5f, 0.f, 0.f}), 0.22f, cx);
        DrawSphere(Vector3Add(pos, {0.f,  3.5f, 0.f}), 0.22f, cy);
        DrawSphere(Vector3Add(pos, {0.f,  0.f, 3.5f}), 0.22f, cz);
        DrawCube(pos, 0.2f, 0.2f, 0.2f, WHITE);
    } else if (op == EditorGizmoOp::SCALE) {
        DrawLine3D(pos, Vector3Add(pos, {3.5f, 0.f, 0.f}), cx);
        DrawLine3D(pos, Vector3Add(pos, {0.f,  3.5f, 0.f}), cy);
        DrawLine3D(pos, Vector3Add(pos, {0.f,  0.f, 3.5f}), cz);
        DrawCube(Vector3Add(pos, {3.5f, 0.f, 0.f}), 0.3f, 0.3f, 0.3f, cx);
        DrawCube(Vector3Add(pos, {0.f,  3.5f, 0.f}), 0.3f, 0.3f, 0.3f, cy);
        DrawCube(Vector3Add(pos, {0.f,  0.f, 3.5f}), 0.3f, 0.3f, 0.3f, cz);
        DrawCube(pos, 0.2f, 0.2f, 0.2f, WHITE);
    } else if (op == EditorGizmoOp::ROTATE) {
        DrawCircle3D(pos, 3.5f, {1.f, 0.f, 0.f}, 90.f, cx);
        DrawCircle3D(pos, 3.5f, {0.f, 1.f, 0.f}, 90.f, cy);
        DrawCircle3D(pos, 3.5f, {0.f, 0.f, 1.f},  0.f, cz);
    }
}

// ── Update (hit detection + drag) ────────────────────────────────────────────
void EditorTranslator::Update(Camera3D cam, entt::entity sel,
                               EditorGizmoOp op, EditorGizmoSpace space) {
    (void)space;
    if (sel == entt::null) return;
    auto& reg = Registry::Get();
    if (!reg.valid(sel) || !reg.all_of<WorldTransform>(sel)) return;

    auto& ec  = EditorCore::Get();
    bool  snap = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    Ray   ray  = GetMouseRay(GetMousePosition(), cam);

    auto& tr  = reg.get<WorldTransform>(sel);
    Vector3 pos = {tr.x, tr.y, tr.z};

    // ── Press: detect which axis was clicked ──────────────────────────────
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !dragging_) {
        active_axis_ = -1;

        if (op == EditorGizmoOp::TRANSLATE || op == EditorGizmoOp::SCALE) {
            constexpr float HIT_R = 0.45f;
            float tx = RaySphereIntersect(ray, Vector3Add(pos, {3.5f, 0.f, 0.f}), HIT_R);
            float ty = RaySphereIntersect(ray, Vector3Add(pos, {0.f,  3.5f, 0.f}), HIT_R);
            float tz = RaySphereIntersect(ray, Vector3Add(pos, {0.f,  0.f, 3.5f}), HIT_R);
            float best = 1e9f;
            if (tx > 0.f && tx < best) { best = tx; active_axis_ = 0; }
            if (ty > 0.f && ty < best) { best = ty; active_axis_ = 1; }
            if (tz > 0.f && tz < best) { best = tz; active_axis_ = 2; }
        } else if (op == EditorGizmoOp::ROTATE) {
            // Y ring: project onto XZ plane, check ring radius 3.5±0.55
            float t = RayPlaneIntersect(ray, {0.f, 1.f, 0.f}, pos);
            if (t > 0.f) {
                Vector3 hit = RayAt(ray, t);
                float dx = hit.x - pos.x, dz = hit.z - pos.z;
                float dist = sqrtf(dx * dx + dz * dz);
                if (dist >= 2.95f && dist <= 4.05f) active_axis_ = 1;
            }
        }

        if (active_axis_ >= 0) {
            dragging_           = true;
            entity_start_       = pos;
            entity_start_rot_y_ = tr.rot_y;
            drag_start_         = ComputePlaneHit(ray, op, active_axis_, pos, cam);
        }
    }

    // ── Held: apply delta each frame ──────────────────────────────────────
    if (dragging_ && IsMouseButtonDown(MOUSE_BUTTON_LEFT)) {
        Vector3 cur = ComputePlaneHit(ray, op, active_axis_, entity_start_, cam);

        if (op == EditorGizmoOp::TRANSLATE) {
            Vector3 raw = entity_start_;
            if (active_axis_ == 0) raw.x += cur.x - drag_start_.x;
            if (active_axis_ == 1) raw.y += cur.y - drag_start_.y;
            if (active_axis_ == 2) raw.z += cur.z - drag_start_.z;

            if (snap) {
                raw.x = SnapF(raw.x, translate_snap_);
                raw.y = SnapF(raw.y, translate_snap_);
                raw.z = SnapF(raw.z, translate_snap_);
            }

            Vector3 delta = Vector3Subtract(raw, pos);

            for (int i = 0; i < ec.selected_count; ++i) {
                entt::entity e = ec.selected[i];
                if (!reg.valid(e) || !reg.all_of<WorldTransform>(e)) continue;
                auto& etr = reg.get<WorldTransform>(e);
                etr.x += delta.x;
                etr.y += delta.y;
                etr.z += delta.z;
            }

#ifdef MD_OPENGL43_ENABLED
            TransformSoA::Get().FlushAoStoSoA(reg);
            TransformSoA::Get().UploadToGPU();
#endif

        } else if (op == EditorGizmoOp::ROTATE && active_axis_ == 1) {
            float a0 = atan2f(drag_start_.z - entity_start_.z,
                              drag_start_.x - entity_start_.x);
            float a1 = atan2f(cur.z - entity_start_.z,
                              cur.x - entity_start_.x);
            float delta_deg = (a1 - a0) * RAD2DEG;
            if (snap) delta_deg = SnapF(delta_deg, rotate_snap_);

            float new_rot = entity_start_rot_y_ + delta_deg;
            for (int i = 0; i < ec.selected_count; ++i) {
                entt::entity e = ec.selected[i];
                if (!reg.valid(e) || !reg.all_of<WorldTransform>(e)) continue;
                reg.get<WorldTransform>(e).rot_y = new_rot;
            }
        }
        // SCALE: WorldTransform has no scale field — no-op
    }

    // ── Release: end drag ─────────────────────────────────────────────────
    if (dragging_ && IsMouseButtonReleased(MOUSE_BUTTON_LEFT)) {
        dragging_    = false;
        active_axis_ = -1;
    }
}
#endif
