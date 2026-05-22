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

// ── Font file scanner (data/fonts/*.ttf, lazy, one-shot) ──
static char s_fnames[32][64] = {};
static int  s_fcount = 0;

inline void ScanFonts() {
    if (s_fcount > 0) return;
    DIR* d = opendir("data/fonts");
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && s_fcount < 32) {
        const char* n = e->d_name;
        int len = (int)strlen(n);
        if (len > 4 && strcmp(n + len - 4, ".ttf") == 0)
            strncpy(s_fnames[s_fcount++], n, 63);
    }
    closedir(d);
}

// ── Draw ──────────────────────────────────────────────────
inline void Draw(const char* config_path,
                 char* status_msg, float* status_timer)
{
    ScanFonts();
    const float MARGIN = 12.0f;

    ImGui::Spacing();
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    ImGui::SeparatorText("Fonts");
    ImGui::Spacing();

    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, {8.0f, 2.0f});

    constexpr ImGuiTableFlags tflags =
        ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_SizingFixedFit;

    // indent both sides
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + MARGIN);
    float tbl_width = ImGui::GetContentRegionAvail().x - MARGIN * 2.0f;

    if (ImGui::BeginTable("##font_tbl", 3, tflags, {tbl_width, 0})) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,   90.0f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,   58.0f);

        struct Row { const char* label; char* path; int* size; const char* id_path; const char* id_size; };
        Row rows[3] = {
            { "Label",  g_cfg.label.path,  &g_cfg.label.size,  "##lp", "##ls" },
            { "Header", g_cfg.header.path, &g_cfg.header.size, "##hp", "##hs" },
            { "Mono",   g_cfg.mono.path,   &g_cfg.mono.size,   "##mp", "##ms" },
        };

        for (auto& r : rows) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            float lw = ImGui::CalcTextSize(r.label).x;
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                 ImGui::GetContentRegionAvail().x - lw);
            ImGui::TextUnformatted(r.label);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1.0f);
            {
                const char* preview = strrchr(r.path, '/');
                preview = preview ? preview + 1 : r.path;
                if (ImGui::BeginCombo(r.id_path, preview)) {
                    for (int fi = 0; fi < s_fcount; fi++) {
                        bool sel = strcmp(s_fnames[fi], preview) == 0;
                        if (ImGui::Selectable(s_fnames[fi], sel))
                            snprintf(r.path, 256, "data/fonts/%s", s_fnames[fi]);
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
            }

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1.0f);
            ImGui::InputInt(r.id_size, r.size, 0);
            if (*r.size < 6) *r.size = 6;
        }

        ImGui::EndTable();
    }

    ImGui::PopStyleVar(); // CellPadding

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
}

} // namespace SettingsEditor
