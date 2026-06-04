#pragma once
#include "editor_ui.h"
#include "editor_world_3d_sdlgpu.h"
#include "editor_core.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>

// ─────────────────────────────────────────────────────────
// SettingsEditor — вкладка налаштувань (ImGui).
// Зберігає/читає data/editor_config.json.
// Зміни шрифтів застосовуються після перезапуску.
// Зміни камери застосовуються негайно.
// ─────────────────────────────────────────────────────────

namespace SettingsEditor {

struct Config {
    // Fonts
    int   ui_size         = 14;     // Arimo Regular/Bold pixel size
    int   mono_size       = 13;     // UbuntuMono pixel size
    // 3D World camera
    float w3d_wasd_speed  = 1000.f; // m/s WASD
    float w3d_scroll_step = 0.03f;  // step = s_cy * step * wheel
    float w3d_zoom_in     = 0.94f;  // s_cy multiplier on scroll up
    float w3d_zoom_out    = 1.06f;  // s_cy multiplier on scroll down
    // F3 Flythrough camera
    float fly_speed       = 100.f;  // m/s WASD
};
static Config g_cfg;
static bool   g_detached = false;
static ImVec2 g_win_pos  = {180.f, 100.f};
static ImVec2 g_win_size = {560.f, 380.f};

// ── Load / Save ───────────────────────────────────────────

inline bool Load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    static char buf[4096];
    if (sz >= (long)sizeof(buf)) { fclose(f); return false; }
    (void)fread(buf, 1, (size_t)sz, f); buf[sz]='\0'; fclose(f);

    auto rint = [&](const char* key) -> int {
        const char* p = strstr(buf, key); if (!p) return 0;
        p = strchr(p, ':'); if (!p) return 0;
        return (int)strtol(p+1, nullptr, 10);
    };
    auto rfloat = [&](const char* key, float def) -> float {
        const char* p = strstr(buf, key); if (!p) return def;
        p = strchr(p, ':'); if (!p) return def;
        float v = (float)strtod(p+1, nullptr);
        return (v > 0.f) ? v : def;
    };
    int us = rint("\"ui_size\"");   if (us > 0) g_cfg.ui_size   = us;
    int ms = rint("\"mono_size\""); if (ms > 0) g_cfg.mono_size = ms;
    g_cfg.w3d_wasd_speed  = rfloat("\"w3d_wasd_speed\"",  g_cfg.w3d_wasd_speed);
    g_cfg.w3d_scroll_step = rfloat("\"w3d_scroll_step\"", g_cfg.w3d_scroll_step);
    g_cfg.w3d_zoom_in     = rfloat("\"w3d_zoom_in\"",     g_cfg.w3d_zoom_in);
    g_cfg.w3d_zoom_out    = rfloat("\"w3d_zoom_out\"",    g_cfg.w3d_zoom_out);
    g_cfg.fly_speed       = rfloat("\"fly_speed\"",       g_cfg.fly_speed);
    // Apply camera values immediately after load
#ifdef MD_SDL_GPU
    WorldEditor3D_SDLGPU::ApplyCameraConfig(
        g_cfg.w3d_wasd_speed, g_cfg.w3d_scroll_step,
        g_cfg.w3d_zoom_in, g_cfg.w3d_zoom_out);
#endif
    EditorCore::Get().cam_speed = g_cfg.fly_speed;
    return true;
}

inline bool Save(const char* path) {
    // Sync live values before writing
    g_cfg.fly_speed = EditorCore::Get().cam_speed;
#ifdef MD_SDL_GPU
    g_cfg.w3d_wasd_speed  = WorldEditor3D_SDLGPU::GetWasdSpeed();
    g_cfg.w3d_scroll_step = WorldEditor3D_SDLGPU::GetScrollStep();
    g_cfg.w3d_zoom_in     = WorldEditor3D_SDLGPU::GetZoomIn();
    g_cfg.w3d_zoom_out    = WorldEditor3D_SDLGPU::GetZoomOut();
#endif
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f,
        "{\n"
        "  \"ui_size\": %d,\n"
        "  \"mono_size\": %d,\n"
        "  \"w3d_wasd_speed\": %.2f,\n"
        "  \"w3d_scroll_step\": %.4f,\n"
        "  \"w3d_zoom_in\": %.4f,\n"
        "  \"w3d_zoom_out\": %.4f,\n"
        "  \"fly_speed\": %.1f\n"
        "}\n",
        g_cfg.ui_size, g_cfg.mono_size,
        g_cfg.w3d_wasd_speed, g_cfg.w3d_scroll_step,
        g_cfg.w3d_zoom_in, g_cfg.w3d_zoom_out,
        g_cfg.fly_speed);
    fclose(f);
    return true;
}

// ── Draw ──────────────────────────────────────────────────
inline void Draw(const char* config_path,
                 char* status_msg, float* status_timer)
{
    if (g_detached) {
        ImGui::SetNextWindowPos(g_win_pos,   ImGuiCond_Appearing);
        ImGui::SetNextWindowSize(g_win_size, ImGuiCond_Appearing);
        bool open = true;
        if (!ImGui::Begin("Settings##float", &open)) {
            ImGui::End();
            if (!open) g_detached = false;
            ImGui::Dummy({0,0});
            return;
        }
        g_win_pos  = ImGui::GetWindowPos();
        g_win_size = ImGui::GetWindowSize();
    }

    // Detach / Dock button (right-aligned)
    {
        const char* lbl = g_detached ? "Dock##set" : "Detach##set";
        float btn_w = ImGui::CalcTextSize(lbl).x + ImGui::GetStyle().FramePadding.x * 2.f;
        ImGui::SetCursorPosX(ImGui::GetContentRegionMax().x - btn_w);
        ImGui::PushStyleColor(ImGuiCol_Button,
            g_detached ? ImVec4(0.25f,0.45f,0.65f,1.f) : ImVec4(0.18f,0.18f,0.28f,1.f));
        if (ImGui::Button(lbl)) g_detached = !g_detached;
        ImGui::PopStyleColor();
    }

    const float MARGIN = 12.0f;
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::SeparatorText("Fonts (Arimo + UbuntuMono, embedded)");
    ImGui::Spacing();
    ImGui::TextDisabled("Fonts are embedded — no system dependency.");
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::SetNextItemWidth(120.f);
    ImGui::InputInt("UI size (px)",   &g_cfg.ui_size,   1);
    if (g_cfg.ui_size   < 6) g_cfg.ui_size   = 6;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::SetNextItemWidth(120.f);
    ImGui::InputInt("Mono size (px)", &g_cfg.mono_size, 1);
    if (g_cfg.mono_size < 6) g_cfg.mono_size = 6;
    ImGui::TextDisabled("Restart editor to apply size changes.");

    ImGui::Spacing();

    // ── Camera Settings ───────────────────────────────────
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::SeparatorText("3D World Camera");
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::SetNextItemWidth(160.f);
    ImGui::SliderFloat("WASD speed (m/s)##w3d",  &g_cfg.w3d_wasd_speed, 10.f, 5000.f, "%.0f", ImGuiSliderFlags_Logarithmic);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::SetNextItemWidth(160.f);
    ImGui::SliderFloat("Scroll step##w3d",        &g_cfg.w3d_scroll_step, 0.005f, 0.15f, "%.3f");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::SetNextItemWidth(160.f);
    ImGui::SliderFloat("Zoom in factor##w3d",     &g_cfg.w3d_zoom_in,  0.80f, 0.99f, "%.3f");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::SetNextItemWidth(160.f);
    ImGui::SliderFloat("Zoom out factor##w3d",    &g_cfg.w3d_zoom_out, 1.01f, 1.25f, "%.3f");
    // Apply immediately as user drags sliders
#ifdef MD_SDL_GPU
    WorldEditor3D_SDLGPU::ApplyCameraConfig(
        g_cfg.w3d_wasd_speed, g_cfg.w3d_scroll_step,
        g_cfg.w3d_zoom_in, g_cfg.w3d_zoom_out);
#endif

    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::SeparatorText("F3 Flythrough Camera");
    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::SetNextItemWidth(160.f);
    ImGui::SliderFloat("Fly speed (m/s)##fly",   &g_cfg.fly_speed, 5.f, 5000.f, "%.0f", ImGuiSliderFlags_Logarithmic);
    EditorCore::Get().cam_speed = g_cfg.fly_speed;
    ImGui::TextDisabled("  (same as Camera tab > Move Speed)");

    ImGui::Spacing();

    // ── Save ──────────────────────────────────────────────
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::PushStyleColor(ImGuiCol_Button,        {0.14f, 0.43f, 0.22f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.20f, 0.58f, 0.30f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.10f, 0.32f, 0.16f, 1.0f});
    if (ImGui::Button("Save config", {120, 0})) {
        if (Save(config_path)) {
            snprintf(status_msg, 64, "Config saved (camera applied instantly)");
            *status_timer = 4.0f;
        } else {
            snprintf(status_msg, 64, "Save failed!");
            *status_timer = 3.0f;
        }
    }
    ImGui::PopStyleColor(3);

    ImGui::SameLine();
    ImGui::TextDisabled("(перезапустити редактор для застосування шрифтів)");

    if (g_detached) { ImGui::End(); ImGui::Dummy({0,0}); }
}

} // namespace SettingsEditor
