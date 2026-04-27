#pragma once
#ifdef MONKEY_DUST_EDITOR
#include "raylib.h"
#include <entt/entt.hpp>
#include "editor_core.h"

class EditorTranslator {
public:
    static EditorTranslator& Get() { static EditorTranslator inst; return inst; }

    void Update(Camera3D cam, entt::entity sel,
                EditorGizmoOp op, EditorGizmoSpace space);
    void Draw(Camera3D cam, entt::entity sel, EditorGizmoOp op);

private:
    EditorTranslator() = default;

    bool     dragging_          = false;
    int      active_axis_       = -1;     // 0=X, 1=Y, 2=Z
    Vector3  drag_start_        = {};
    Vector3  entity_start_      = {};
    float    entity_start_rot_y_= 0.f;

    float translate_snap_ = 1.0f;
    float rotate_snap_    = 15.0f;
    float scale_snap_     = 0.1f;

    Vector3 ComputePlaneHit(Ray ray, EditorGizmoOp op, int axis,
                            Vector3 entity_pos, Camera3D cam);
};
#endif
