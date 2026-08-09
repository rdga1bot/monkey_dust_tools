#ifdef MONKEY_DUST_EDITOR
#include "editor_translator.h"
#include "editor_core.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/platform/window.h>
#include <monkey_dust/platform/input.h>
#include <cmath>
#include <monkey_dust/world/transform_soa.h>

// ── Ray helpers ───────────────────────────────────────────────────────────────
static float RaySphereIntersect(MdRay ray, Vec3 center, float radius) {
    Vec3  oc   = vec3_sub(ray.pos, center);
    float b    = vec3_dot(oc, ray.dir);
    float c    = vec3_dot(oc, oc) - radius * radius;
    float disc = b * b - c;
    if (disc < 0.f) return -1.f;
    float t = -b - sqrtf(disc);
    return (t > 0.f) ? t : -1.f;
}

static float RayPlaneIntersect(MdRay ray, Vec3 normal, Vec3 point) {
    float denom = vec3_dot(normal, ray.dir);
    if (fabsf(denom) < 1e-6f) return -1.f;
    float t = vec3_dot(vec3_sub(point, ray.pos), normal) / denom;
    return (t > 0.f) ? t : -1.f;
}

static Vec3 RayAt(MdRay ray, float t) {
    return vec3_add(ray.pos, vec3_scale(ray.dir, t));
}

static float SnapF(float v, float snap) {
    return roundf(v / snap) * snap;
}

static MdRay CameraRay(float mx, float my, const MdCamera& cam) {
    int sw = window_get_width(), sh = window_get_height();
    float aspect = (sw > 0 && sh > 0) ? (float)sw / sh : 1.f;
    Vec3 fwd = vec3_norm(vec3_sub(cam.target, cam.pos));
    Vec3 rgt = vec3_norm(vec3_cross(fwd, cam.up));
    Vec3 up  = vec3_cross(rgt, fwd);
    float tan_h = tanf(cam.fovy * 0.5f * (3.14159265f / 180.f));
    float nx    = (2.f * mx / sw) - 1.f;
    float ny    = 1.f - (2.f * my / sh);
    Vec3 dir    = vec3_norm(vec3_add(vec3_add(fwd,
                      vec3_scale(rgt, nx * tan_h * aspect)),
                      vec3_scale(up,  ny * tan_h)));
    return { cam.pos, dir };
}

// ── Plane selection per axis ──────────────────────────────────────────────────
Vec3 EditorTranslator::ComputePlaneHit(MdRay ray, EditorGizmoOp op,
                                        int axis, Vec3 ep, const MdCamera& cam) {
    Vec3 normal;

    if (op == EditorGizmoOp::ROTATE) {
        normal = {0.f, 1.f, 0.f};
    } else {
        if (axis == 1) {
            float dx  = cam.pos.x - ep.x;
            float dz  = cam.pos.z - ep.z;
            float len = sqrtf(dx * dx + dz * dz);
            if (len < 0.001f) len = 0.001f;
            normal = {dx / len, 0.f, dz / len};
        } else {
            normal = {0.f, 1.f, 0.f};
        }
    }

    float t = RayPlaneIntersect(ray, normal, ep);
    return (t > 0.f) ? RayAt(ray, t) : ep;
}

// ── Draw ──────────────────────────────────────────────────────────────────────
// Gizmo rendering used Raylib 3D draw calls (DrawLine3D/DrawSphere/DrawCube/
// DrawCircle3D) -- removed 2026-08-09 along with the rest of the Raylib
// fallback skeleton. Was already a no-op under SDL3 (never ported); still a
// no-op now, just without the dead alternate body.
void EditorTranslator::Draw(const MdCamera& /*cam*/, MdEntity sel, EditorGizmoOp /*op*/) {
    if (sel == MdEntity::Null()) return;
}

// ── Update (hit detection + drag) ────────────────────────────────────────────
// Gizmo interaction (Raylib 3D draw + ImGui input) removed 2026-08-09 along
// with the rest of the Raylib fallback skeleton. Was already a no-op under
// SDL3 (never ported); still a no-op now, just without the dead alternate
// body.
void EditorTranslator::Update(const MdCamera& /*cam*/, MdEntity sel,
                               EditorGizmoOp /*op*/, EditorGizmoSpace /*space*/) {
    if (sel == MdEntity::Null()) return;
}
#endif // MONKEY_DUST_EDITOR
