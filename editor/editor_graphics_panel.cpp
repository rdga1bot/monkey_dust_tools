#ifdef MONKEY_DUST_EDITOR
#include "editor_graphics_panel.h"
#include "editor_core.h"
#include "imgui.h"
#include <monkey_dust/tools/graphics_settings.h>
#include <monkey_dust/tools/debug_system.h>
#include "raylib.h"

void EditorGraphicsPanel::Draw() {
    if (!EditorCore::Get().panels_visible[4]) return;

    ImGui::SetNextWindowSize(ImVec2(300, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(1280 - 305, 620), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Graphics##GraphicsPanel", &EditorCore::Get().panels_visible[4])) {
        ImGui::End();
        return;
    }

    auto& gs  = GraphicsSettings::Get();
    auto& dbg = DebugSystem::Get();

    // ── Rendering ─────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Checkbox("V-Sync##gfx", &gs.vsync))
            SetTargetFPS(gs.vsync ? 60 : 0);

        static const char* render_modes[] = { "Forward+Shadow", "Forward+PBR", "Debug" };
        static int render_mode = 1;
        ImGui::Combo("Render Path##gfx", &render_mode, render_modes, 3);
    }

    // ── Fog ───────────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Fog Enabled##gfx", &gs.fog_enabled);
        if (gs.fog_enabled) {
            ImGui::SliderFloat("Near##fog", &gs.fog_near, 10.f, 300.f);
            ImGui::SliderFloat("Far##fog",  &gs.fog_far,  50.f, 800.f);
            if (gs.fog_near > gs.fog_far - 1.f) gs.fog_near = gs.fog_far - 1.f;
            ImGui::ColorEdit3("Color##fog", gs.fog_color);
        }
    }

    // ── Shadows ───────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Shadows")) {
#ifdef MD_OPENGL43_ENABLED
        ImGui::Checkbox("Shadows Enabled##gfx",  &gs.shadows_enabled);
        ImGui::Checkbox("Soft Shadows (PCF)##gfx",&gs.soft_shadows);
        static const char* quality[] = { "Low (1 cascade)", "Medium (2 cascades)", "High (3 cascades)" };
        int q = gs.shadow_cascades - 1;
        if (ImGui::Combo("Quality##shadow", &q, quality, 3))
            gs.shadow_cascades = q + 1;
        ImGui::SliderFloat("Distance##shadow", &gs.shadow_distance, 50.f, 500.f);
#else
        ImGui::TextDisabled("Requires MD_OPENGL43_ENABLED");
#endif
    }

    // ── IBL / Global Illumination ─────────────────────────────────────────
    if (ImGui::CollapsingHeader("Global Illumination")) {
#ifdef MD_OPENGL43_ENABLED
        ImGui::Checkbox("IBL Enabled##gfx", &gs.ibl_enabled);
        if (gs.ibl_enabled) {
            ImGui::SliderFloat("IBL Intensity##gfx", &gs.ibl_intensity, 0.f, 5.f);
        }
#else
        ImGui::TextDisabled("Requires MD_OPENGL43_ENABLED");
#endif
    }

    // ── Debug Rendering ───────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Debug Rendering", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Checkbox("Debug Overlay (F1)##gfx",  &dbg.overlay_on);
        ImGui::Checkbox("SpatialGrid (F2)##gfx",    &dbg.grid_on);
        ImGui::Checkbox("NavMesh Wireframe##gfx",   &dbg.navmesh_on);
        ImGui::Checkbox("Clean Mode##gfx",          &dbg.clean_mode);

        ImGui::Separator();
        bool phys = EditorCore::Get().physics_paused;
        if (ImGui::Checkbox("Physics Paused##gfx", &phys))
            EditorCore::Get().physics_paused = phys;
    }

    // ── Reset ─────────────────────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::Button("Reset to Defaults##gfx")) {
        gs.fog_near       = 80.f;
        gs.fog_far        = 150.f;
        gs.fog_color[0]   = 0.18f;
        gs.fog_color[1]   = 0.20f;
        gs.fog_color[2]   = 0.25f;
        gs.fog_enabled    = true;
        gs.shadows_enabled= true;
        gs.soft_shadows   = true;
        gs.shadow_cascades= 3;
        gs.shadow_distance= 150.f;
        gs.ibl_enabled    = true;
        gs.ibl_intensity  = 1.f;
        SetTargetFPS(60);
    }

    ImGui::End();
}
#endif
