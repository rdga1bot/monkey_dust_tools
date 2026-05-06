#ifdef MONKEY_DUST_EDITOR
#include "editor_camera_panel.h"
#include "editor_core.h"
#include "imgui.h"

void EditorCameraPanel::Draw() {
    if (!EditorCore::Get().panels_visible[5]) return;

    ImGui::SetNextWindowSize(ImVec2(280, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(285, 60), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Camera##CameraPanel", &EditorCore::Get().panels_visible[5])) {
        ImGui::End();
        return;
    }

    auto& ec  = EditorCore::Get();
    auto& cam = ec.editor_cam;

    // ── Editor Camera Settings ────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Editor Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("FOV##cam",          &cam.fovy,      20.f, 120.f);
        ImGui::SliderFloat("Orbit Dist##cam",   &ec.cam_dist,   2.f,  500.f);
        ImGui::SliderFloat("Move Speed##cam",   &ec.cam_speed,  5.f,  200.f);

        if (ImGui::Button("Reset Camera##cam")) {
            ec.cam_yaw   = -45.f;
            ec.cam_pitch =  25.f;
            ec.cam_dist  =  35.f;
            ec.cam_target = {0.f, 2.f, 0.f};
            cam.fovy     =  60.f;
        }
    }

    // ── Camera Mode ───────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Camera Mode", ImGuiTreeNodeFlags_DefaultOpen)) {
        bool orbit = !ec.cam_flying;
        if (ImGui::RadioButton("Orbit##cam",      orbit))  ec.cam_flying = false;
        ImGui::SameLine();
        if (ImGui::RadioButton("Flythrough##cam", ec.cam_flying)) ec.cam_flying = true;
        ImGui::TextDisabled("Orbit:  RMB=rotate  MMB=pan  Scroll=zoom");
        ImGui::TextDisabled("Fly:    RMB+WASD");
    }

    // ── Viewport Info ─────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Viewport Info", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Position: (%.1f, %.1f, %.1f)",
                    cam.pos.x, cam.pos.y, cam.pos.z);
        ImGui::Text("Target:   (%.1f, %.1f, %.1f)",
                    ec.cam_target.x, ec.cam_target.y, ec.cam_target.z);
        ImGui::Text("Yaw: %.1f  Pitch: %.1f  Dist: %.1f",
                    ec.cam_yaw, ec.cam_pitch, ec.cam_dist);
        ImGui::Separator();
        ImGui::Text("FPS: %.0f  |  dt: %.2f ms",
                    EditorCore::Get().frame_fps, EditorCore::Get().frame_dt_ms);
    }

    // ── Screenshot ────────────────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Screenshot")) {
#ifndef USE_SDL3
        if (ImGui::Button("Take Screenshot##cam"))
            TakeScreenshot("editor_screenshot.png");
        ImGui::TextDisabled("Saves to editor_screenshot.png");
#else
        ImGui::TextDisabled("Screenshot not available in SDL3 build");
#endif
    }

    ImGui::End();
}
#endif
