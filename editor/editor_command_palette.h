#pragma once
#ifdef MONKEY_DUST_EDITOR
#include "editor_core.h"

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
