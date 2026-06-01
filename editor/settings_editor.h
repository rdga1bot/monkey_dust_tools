#pragma once
#include "editor_ui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>

// ─────────────────────────────────────────────────────────
// SettingsEditor — вкладка налаштувань (ImGui).
// Зберігає/читає data/editor_config.json.
// Зміни шрифтів застосовуються після перезапуску.
// ─────────────────────────────────────────────────────────

namespace SettingsEditor {

// Fonts are embedded in the binary (MdFonts::Load) — only sizes are configurable.
struct Config {
    int ui_size   = 14;  // Arimo Regular/Bold pixel size
    int mono_size = 13;  // UbuntuMono pixel size
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
    int us = rint("\"ui_size\"");   if (us > 0) g_cfg.ui_size   = us;
    int ms = rint("\"mono_size\""); if (ms > 0) g_cfg.mono_size = ms;
    return true;
}

inline bool Save(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f, "{\n  \"ui_size\": %d,\n  \"mono_size\": %d\n}\n",
            g_cfg.ui_size, g_cfg.mono_size);
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
    ImGui::Spacing();

    // ── Save ──────────────────────────────────────────────
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::PushStyleColor(ImGuiCol_Button,        {0.14f, 0.43f, 0.22f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, {0.20f, 0.58f, 0.30f, 1.0f});
    ImGui::PushStyleColor(ImGuiCol_ButtonActive,  {0.10f, 0.32f, 0.16f, 1.0f});
    if (ImGui::Button("Save config", {120, 0})) {
        if (Save(config_path)) {
            snprintf(status_msg, 64, "Config saved — restart to apply");
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
