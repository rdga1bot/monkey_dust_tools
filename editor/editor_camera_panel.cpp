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
    DrawContent();
    ImGui::End();
}

void EditorCameraPanel::DrawContent() {

    auto& ec  = EditorCore::Get();
    auto& cam = ec.editor_cam;

    // ── Editor Camera Settings ────────────────────────────────────────────
    if (ImGui::CollapsingHeader("Editor Camera", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SliderFloat("FOV##cam",          &cam.fovy,      20.f, 120.f);
        ImGui::SliderFloat("Orbit Dist##cam",   &ec.cam_dist,   2.f,  500.f);
        ImGui::SliderFloat("Move Speed##cam",   &ec.cam_speed,  5.f,  5000.f, "%.0f m/s", ImGuiSliderFlags_Logarithmic);

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
        bool orbit = !ec.cam_flying && !ec.cam_game_mode;
        bool fly   =  ec.cam_flying && !ec.cam_game_mode;
        bool game  =  ec.cam_game_mode;
        if (ImGui::RadioButton("Orbit##cam",      orbit)) {
            if (ec.cam_game_mode) ec.cam_target = ec.last_game_cam_target;
            ec.cam_flying=false; ec.cam_game_mode=false;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Flythrough##cam", fly)) {
            if (ec.cam_game_mode) ec.cam_target = ec.last_game_cam_target;
            if (!fly) {
                const float DEG2R = 3.14159265f / 180.f;
                float yr = ec.cam_yaw   * DEG2R;
                float pr = ec.cam_pitch * DEG2R;
                // Move eye to where orbit camera was
                ec.cam_target.x += ec.cam_dist * cosf(pr) * sinf(yr);
                ec.cam_target.y += ec.cam_dist * sinf(pr);
                ec.cam_target.z += ec.cam_dist * cosf(pr) * cosf(yr);
                // Init fly_yaw/fly_pitch (radians) to look toward old orbit target.
                // fly fwd.y = -sin(fly_pitch); orbit looks at center below when pitch>0.
                // So fly_pitch = pr (same sign) to look in the same downward direction.
                ec.fly_yaw   = yr + 3.14159265f;
                ec.fly_pitch = pr;
                if (ec.fly_pitch < -0.3f) ec.fly_pitch = -0.3f;
                if (ec.fly_pitch >  1.3f) ec.fly_pitch =  1.3f;
            }
            ec.cam_flying=true; ec.cam_game_mode=false;
        }
        ImGui::SameLine();
        if (ImGui::RadioButton("Game##cam",       game))
            { ec.cam_game_mode=true; ec.cam_flying=false; }
        ImGui::Spacing();
        if      (orbit) ImGui::TextDisabled("RMB=rotate  MMB=pan  Scroll=zoom");
        else if (fly)   ImGui::TextDisabled("WASD=fly  Q/E=up/down  RMB=look  Shift=turbo×5");
        else            ImGui::TextDisabled("WASD=рух гравця  RMB=камера");
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
    // Raylib's TakeScreenshot() fallback removed 2026-08-09 (USE_SDL3=ON is
    // the only buildable configuration); real SDL_GPU screenshot capture is
    // md.screenshot() (Lua/automation), not this panel.
    if (ImGui::CollapsingHeader("Screenshot")) {
        ImGui::TextDisabled("Screenshot not available in SDL3 build");
    }

}
#endif
