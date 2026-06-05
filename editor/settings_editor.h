#pragma once
#include "editor_world_3d_sdlgpu.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ─────────────────────────────────────────────────────────
// SettingsEditor — вкладка налаштувань (ImGui).
// Window management (Detach/Dock, floating) is handled by
// editor_panels_entry.cpp, mirroring the F3 editor pattern.
// ─────────────────────────────────────────────────────────

namespace SettingsEditor {

struct Config {
    int   ui_size         = 14;
    int   mono_size       = 13;
    float w3d_wasd_speed  = 1000.f;
    float w3d_scroll_step = 0.03f;
    float w3d_zoom_in     = 0.94f;
    float w3d_zoom_out    = 1.06f;
};
static Config g_cfg;

// Detach state + window geometry — managed by editor_panels_entry.cpp.
static bool   g_detached = false;
static ImVec2 g_win_pos  = {620.f, 110.f};
static ImVec2 g_win_size = {560.f, 440.f};

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
#ifdef MD_SDL_GPU
    WorldEditor3D_SDLGPU::ApplyCameraConfig(
        g_cfg.w3d_wasd_speed, g_cfg.w3d_scroll_step,
        g_cfg.w3d_zoom_in, g_cfg.w3d_zoom_out);
#endif
    return true;
}

inline bool Save(const char* path) {
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
        "  \"w3d_zoom_out\": %.4f\n"
        "}\n",
        g_cfg.ui_size, g_cfg.mono_size,
        g_cfg.w3d_wasd_speed, g_cfg.w3d_scroll_step,
        g_cfg.w3d_zoom_in, g_cfg.w3d_zoom_out);
    fclose(f);
    return true;
}

// ── DrawContent — pure content, no window management ──────
// Called either embedded in ##editor or inside "Settings##float".
inline void DrawContent(const char* config_path,
                        char* status_msg, float* status_timer)
{
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
#ifdef MD_SDL_GPU
    WorldEditor3D_SDLGPU::ApplyCameraConfig(
        g_cfg.w3d_wasd_speed, g_cfg.w3d_scroll_step,
        g_cfg.w3d_zoom_in, g_cfg.w3d_zoom_out);
#endif

    ImGui::Spacing();
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
}

} // namespace SettingsEditor
