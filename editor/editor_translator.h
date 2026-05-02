#pragma once
#ifdef MONKEY_DUST_EDITOR
#include <monkey_dust/render/md_camera.h>
#include <entt/entt.hpp>
#include "editor_core.h"

class EditorTranslator {
public:
    static EditorTranslator& Get() { static EditorTranslator inst; return inst; }

    void Update(const MdCamera& cam, entt::entity sel,
                EditorGizmoOp op, EditorGizmoSpace space);
    void Draw(const MdCamera& cam, entt::entity sel, EditorGizmoOp op);

private:
    EditorTranslator() = default;

    bool  dragging_           = false;
    int   active_axis_        = -1;     // 0=X, 1=Y, 2=Z
    Vec3  drag_start_         = {};
    Vec3  entity_start_       = {};
    float entity_start_rot_y_ = 0.f;

    float translate_snap_ = 1.0f;
    float rotate_snap_    = 15.0f;
    float scale_snap_     = 0.1f;

    Vec3 ComputePlaneHit(MdRay ray, EditorGizmoOp op, int axis,
                         Vec3 entity_pos, const MdCamera& cam);
};
#endif
