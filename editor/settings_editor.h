#pragma once
#include "editor_ui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ─────────────────────────────────────────────────────────
// SettingsEditor — вкладка налаштувань (ImGui).
// Зберігає/читає data/editor_config.json.
// Зміни шрифтів застосовуються після перезапуску.
// ─────────────────────────────────────────────────────────

namespace SettingsEditor {

struct FontSlot { char path[256]; int size; };
struct Config {
    FontSlot label;   // Arimo Regular
    FontSlot header;  // Arimo Bold
    FontSlot mono;    // Ubuntu Mono
};

static Config g_cfg = {
    {"data/fonts/Arimo-Regular.ttf", 15},
    {"data/fonts/Arimo-Bold.ttf",    16},
    {"data/fonts/UbuntuMono-R.ttf",  14},
};

// ── Load / Save ───────────────────────────────────────────

inline bool Load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    static char buf[4096];
    if (sz >= (long)sizeof(buf)) { fclose(f); return false; }
    (void)fread(buf, 1, (size_t)sz, f); buf[sz]='\0'; fclose(f);

    auto rstr = [&](const char* key, char* out) {
        const char* p = strstr(buf, key); if (!p) return;
        p = strchr(p, ':');  if (!p) return;
        p = strchr(p, '"');  if (!p) return; ++p;
        int i = 0; while (*p && *p != '"' && i < 255) out[i++] = *p++;
        out[i] = '\0';
    };
    auto rint = [&](const char* key) -> int {
        const char* p = strstr(buf, key); if (!p) return 0;
        p = strchr(p, ':'); if (!p) return 0;
        return (int)strtol(p+1, nullptr, 10);
    };

    rstr("\"label_path\"",  g_cfg.label.path);
    rstr("\"header_path\"", g_cfg.header.path);
    rstr("\"mono_path\"",   g_cfg.mono.path);
    int ls = rint("\"label_size\"");  if (ls > 0) g_cfg.label.size  = ls;
    int hs = rint("\"header_size\""); if (hs > 0) g_cfg.header.size = hs;
    int ms = rint("\"mono_size\"");   if (ms > 0) g_cfg.mono.size   = ms;
    return true;
}

inline bool Save(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f,
        "{\n"
        "  \"label_path\":  \"%s\",\n"
        "  \"label_size\":  %d,\n"
        "  \"header_path\": \"%s\",\n"
        "  \"header_size\": %d,\n"
        "  \"mono_path\":   \"%s\",\n"
        "  \"mono_size\":   %d\n"
        "}\n",
        g_cfg.label.path,  g_cfg.label.size,
        g_cfg.header.path, g_cfg.header.size,
        g_cfg.mono.path,   g_cfg.mono.size);
    fclose(f);
    return true;
}

// ── Draw ──────────────────────────────────────────────────
inline void Draw(const char* config_path,
                 char* status_msg, float* status_timer)
{
    ImGui::Spacing();

    // ── Label font ────────────────────────────────────────
    if (EditorUI::font_bold) ImGui::PushFont(EditorUI::font_bold);
    ImGui::SeparatorText("Label font  (мітки, кнопки)");
    if (EditorUI::font_bold) ImGui::PopFont();

    ImGui::SetNextItemWidth(-120);
    ImGui::InputText("##label_path", g_cfg.label.path, 255);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("px##ls", &g_cfg.label.size, 0);
    if (g_cfg.label.size < 6) g_cfg.label.size = 6;

    ImGui::Spacing();

    // ── Header font ───────────────────────────────────────
    if (EditorUI::font_bold) ImGui::PushFont(EditorUI::font_bold);
    ImGui::SeparatorText("Header font  (заголовки — Bold)");
    if (EditorUI::font_bold) ImGui::PopFont();

    ImGui::SetNextItemWidth(-120);
    ImGui::InputText("##header_path", g_cfg.header.path, 255);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("px##hs", &g_cfg.header.size, 0);
    if (g_cfg.header.size < 6) g_cfg.header.size = 6;

    ImGui::Spacing();

    // ── Mono font ─────────────────────────────────────────
    if (EditorUI::font_bold) ImGui::PushFont(EditorUI::font_bold);
    ImGui::SeparatorText("Mono font  (InputText, числа)");
    if (EditorUI::font_bold) ImGui::PopFont();

    ImGui::SetNextItemWidth(-120);
    ImGui::InputText("##mono_path", g_cfg.mono.path, 255);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60);
    ImGui::InputInt("px##ms", &g_cfg.mono.size, 0);
    if (g_cfg.mono.size < 6) g_cfg.mono.size = 6;

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    // ── Save ──────────────────────────────────────────────
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

    // ── Current fonts info ────────────────────────────────
    ImGui::Spacing();
    ImGui::Spacing();
    if (EditorUI::font_bold) ImGui::PushFont(EditorUI::font_bold);
    ImGui::SeparatorText("Активні шрифти");
    if (EditorUI::font_bold) ImGui::PopFont();

    ImGui::TextDisabled("Label:  %s  %dpx", g_cfg.label.path,  g_cfg.label.size);
    ImGui::TextDisabled("Header: %s  %dpx", g_cfg.header.path, g_cfg.header.size);
    ImGui::TextDisabled("Mono:   %s  %dpx", g_cfg.mono.path,   g_cfg.mono.size);
}

} // namespace SettingsEditor
