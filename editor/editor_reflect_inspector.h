#pragma once
#ifdef MONKEY_DUST_EDITOR
// EditorReflectInspector — generalized reflect-driven Entity Inspector tab
// for the standalone tools/editor. New, separate from game/src/editor/
// editor_inspector.h (which is game-coupled and out of scope here).
//
// Component access goes through each backend's untyped/runtime-id API
// exclusively (EcsBridgeHas/EcsBridgeGetMut/EcsBridgeModified/
// EcsBridgeAddDefault/EcsBridgeRemove, editor_reflect_bridge.h) — see that
// file for why. Structural ops (add/remove component) are collected during
// the draw pass and applied after it: both backends can relocate an
// entity's whole archetype/table row on a structural change, which would
// invalidate an EcsBridgeGetMut() pointer held by fields drawn later in the
// same pass (same class of bug as md_registry.h's B3.4 notes).
#include "editor_reflect_bridge.h"
class EditorReflectInspector {
public:
    static void DrawContent(EcsBridgeWorldT* world);
};
#endif
