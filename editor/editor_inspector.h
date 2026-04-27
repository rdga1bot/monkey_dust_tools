#pragma once
#ifdef MONKEY_DUST_EDITOR
#include "imgui.h"
#include <entt/entt.hpp>

class EditorInspector {
public:
    static EditorInspector& Get() { static EditorInspector inst; return inst; }

    void Draw();

private:
    EditorInspector() = default;

    void DrawTransform(entt::entity e);
    void DrawHealth(entt::entity e);
    void DrawCombat(entt::entity e);
    void DrawAIAgent(entt::entity e);
    void DrawNavAgent(entt::entity e);
    void DrawInventory(entt::entity e);
    void DrawBuilding(entt::entity e);
    void DrawPlayerController(entt::entity e);
    void DrawAIScript(entt::entity e);
    void DrawAddComponent(entt::entity e);

    bool CollapsingSection(const char* label, ImVec4 color);
};
#endif
