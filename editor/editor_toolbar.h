#pragma once
#ifdef DEBUG
#include "imgui.h"
#include "editor_core.h"

class EditorToolbar {
public:
    static EditorToolbar& Get() { static EditorToolbar inst; return inst; }

    void Draw(float dt);

private:
    EditorToolbar() = default;

    void DrawMenuBar();
    void DrawButtonBar();
    void SpawnEntity(const char* type);

    char scene_path_[256] = "saves/scene.json";
    bool show_path_popup_ = false;
};
#endif
