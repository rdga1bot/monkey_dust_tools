#ifdef MONKEY_DUST_EDITOR
#include "editor_animation_panel.h"
#include "editor_core.h"
#include "imgui.h"

#ifdef MD_OPENGL43_ENABLED
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/render/animation_soa.h>
#include "raylib.h"
#include <cstdio>
#endif

void EditorAnimationPanel::Draw() {
    if (!EditorCore::Get().panels_visible[6]) return;

    ImGui::SetNextWindowSize(ImVec2(300, 340), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(570, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Animation##AnimPanel", &EditorCore::Get().panels_visible[6])) {
        ImGui::End();
        return;
    }

#ifndef MD_OPENGL43_ENABLED
    ImGui::TextDisabled("Animation requires Phase 31 (MD_OPENGL43_ENABLED).");
    ImGui::TextDisabled("Rebuild with -DMD_OPENGL43=ON to enable GPU skinning.");
    ImGui::End();
    return;
#else
    auto& anim = AnimationSoA::Get();
    auto& reg  = Registry::Get();

    // ── Entity picker ─────────────────────────────────────────────────────
    // Build label list of entities that have WorldTransform (= have a SoA slot)
    static uint32_t entity_slots[MAX_ANIMATED_NPC];
    static char     entity_labels[MAX_ANIMATED_NPC][32];
    static int      entity_pick_count = 0;
    static int      selected_pick     = 0;

    if (ImGui::CollapsingHeader("Entity", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::SmallButton("Refresh##anim")) {
            entity_pick_count = 0;
            for (auto e : reg.storage<entt::entity>()) {
                if (!reg.valid(e) || !reg.all_of<WorldTransform>(e)) continue;
                int slot = (int)reg.get<WorldTransform>(e).slot;
                if (slot < 0 || slot >= MAX_ANIMATED_NPC) continue;
                if (entity_pick_count >= MAX_ANIMATED_NPC) break;
                entity_slots[entity_pick_count] = (uint32_t)slot;
                snprintf(entity_labels[entity_pick_count], 32,
                         "Entity_%u [slot %d]",
                         (uint32_t)entt::to_integral(e), slot);
                entity_pick_count++;
            }
        }

        if (entity_pick_count == 0) {
            ImGui::TextDisabled("Click Refresh to list entities");
        } else {
            // Build pointer array for Combo
            static const char* labels_ptr[MAX_ANIMATED_NPC];
            for (int i = 0; i < entity_pick_count; ++i)
                labels_ptr[i] = entity_labels[i];
            if (selected_pick >= entity_pick_count) selected_pick = 0;
            ImGui::Combo("##anim_entity", &selected_pick, labels_ptr, entity_pick_count);
        }
    }

    if (entity_pick_count == 0 || selected_pick >= entity_pick_count) {
        ImGui::End();
        return;
    }

    int slot = (int)entity_slots[selected_pick];
    AnimNpcState& state = anim.GetState(slot);

    // ── Clip selector ─────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Clip", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (anim.ClipCount() == 0) {
            ImGui::TextDisabled("No clips loaded");
        } else {
            static const char* clip_ptr[MAX_ANIM_CLIPS];
            for (int i = 0; i < anim.ClipCount(); ++i)
                clip_ptr[i] = anim.GetClip(i).name;
            int cur = (int)state.clip_id;
            if (cur >= anim.ClipCount()) cur = 0;
            if (ImGui::Combo("Clip##anim", &cur, clip_ptr, anim.ClipCount()))
                anim.SetClip(slot, (uint8_t)cur);
        }
    }

    // ── Playback controls ─────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Playback", ImGuiTreeNodeFlags_DefaultOpen)) {
        int cid = (int)state.clip_id;
        float dur = (cid < anim.ClipCount()) ? anim.GetClip(cid).duration_s : 1.f;

        // Time slider
        float t = state.time_s;
        if (ImGui::SliderFloat("Time##anim", &t, 0.f, dur))
            state.time_s = t;

        // Progress bar
        ImGui::ProgressBar(dur > 0.f ? t / dur : 0.f, ImVec2(-1, 0));

        // Buttons
        static bool playing = true;
        if (ImGui::Button(playing ? "[||]##anim" : "[>]##anim")) playing = !playing;
        ImGui::SameLine();
        if (ImGui::Button("[<<]##anim")) state.time_s = 0.f;
        ImGui::SameLine();
        ImGui::Text("%.2f / %.2fs", state.time_s, dur);

        if (playing) {
            state.time_s += GetFrameTime();
            if (state.time_s >= dur) state.time_s -= dur;
        }
    }

    // ── Keyframe stub ─────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Keyframes (stub)")) {
        ImGui::TextDisabled("Record keyframes — Phase 35+");
        if (ImGui::Button("Record Keyframe##anim"))
            TraceLog(LOG_INFO, "[AnimPanel] Keyframe record stub (Phase 35)");
    }

    ImGui::End();
#endif // MD_OPENGL43_ENABLED
}
#endif // MONKEY_DUST_EDITOR
