#pragma once
#ifdef MONKEY_DUST_EDITOR
// editor_characters_panel.h — F3 "Characters" tab.
// Embeds the full character creator directly in F3, plus "Apply to player" button.
// No separate binary needed — design character in-game and apply instantly.

#include "imgui.h"
#include "character_editor.h"          // full 3-panel creator (static state)
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/components/char_body_state.h>
#include <monkey_dust/components/player_controller.h>
#include "world/char_def.h"
#include "render/npc_render.h"
#include <cstdio>
#include <cstring>

class EditorCharactersPanel {
public:
    static EditorCharactersPanel& Get() { static EditorCharactersPanel s; return s; }

    void DrawContent() {
        // ── Top bar: Apply + clothing quick-test ──────────────────────────────
        bool do_apply = ImGui::Button("Apply to player", {160.f, 0});
        ImGui::SameLine();
        if (ImGui::Button("Clear clothing", {120.f, 0})) {
            NpcRender::SetPlayerClothSlot(0, nullptr, nullptr);
            NpcRender::SetPlayerClothSlot(1, nullptr, nullptr);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("| RMB=камера  WASD=рух");

        if (do_apply) apply_to_player();

        ImGui::Separator();

        // ── Full character creator ────────────────────────────────────────────
        CharacterEditor::Draw();
    }

private:
    void apply_to_player() {
        // Save chardef from live editor state, then load and apply.
        // SaveJSON writes to game/data/chars/player.chardef (same path CharDef::LoadFromFile reads).
        CharacterEditor::SaveJSON(CharacterEditor::s_path);

        CharDef cd;
        if (!CharDef::LoadFromFile(CharacterEditor::s_path, cd)) return;

        auto& reg = Registry::Get();
        entt::entity pe = entt::null;
        reg.view<PlayerController>().each([&](entt::entity e, const PlayerController&){ pe=e; });
        if (pe == entt::null) { fprintf(stderr,"[CharPanel] no player entity\n"); return; }

        auto& bs = reg.emplace_or_replace<CharBodyState>(pe);
        CharBodyState_InitFromDef(bs, cd);
        fprintf(stdout,"[CharPanel] Applied '%s' to player\n", cd.name);
    }
};

#endif // MONKEY_DUST_EDITOR
