#pragma once
#ifdef MONKEY_DUST_EDITOR
#include "editor_core.h"

// Phase 0 falsification probe (EDITOR_AUTOMATION_PLAN_v1.md) — the ONE
// command body migrated out of the palette's inline lambda so it can be
// registered with EditorCmdRegistry from editor_panels_init() and called
// by name (palette, console `exec`, or future Lua binding) instead of only
// via the palette's own array. Defined in editor_command_palette.cpp.
void DeleteSelectedCmd();

class EditorCommandPalette {
public:
    static EditorCommandPalette& Get() { static EditorCommandPalette inst; return inst; }

    void Open();
    void Draw();          // call each frame from toolbar

private:
    EditorCommandPalette() = default;

    bool  open_        = false;
    bool  just_opened_ = false;
    char  filter_[128] = {};
    int   selected_    = 0;
};
#endif
