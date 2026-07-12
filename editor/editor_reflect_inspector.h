#pragma once
#ifdef MONKEY_DUST_EDITOR
// EditorReflectInspector — generalized reflect-driven Entity Inspector tab
// for the standalone tools/editor. New, separate from game/src/editor/
// editor_inspector.h (which is game-coupled and out of scope here).
//
// Component access goes through the untyped flecs C API exclusively
// (ecs_has_id/ecs_get_mut_id/ecs_modified_id/ecs_add_id/ecs_remove_id) —
// see editor_reflect_bridge.h for why. Structural ops (add/remove component)
// are collected during the draw pass and applied after it: flecs relocates
// an entity's whole archetype-table row on any structural change, which
// would invalidate ecs_get_mut_id() pointers held by fields drawn later in
// the same pass (same class of bug as md_registry.h's B3.4 notes).
class EditorReflectInspector {
public:
    static void DrawContent(struct ecs_world_t* world);
};
#endif
