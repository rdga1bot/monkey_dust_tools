#pragma once
#include "editor_ui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ─────────────────────────────────────────────────────────
// SettingsEditor — вкладка налаштувань редактора.
//
// Три шрифти:
//   Label  (мітки, кнопки, вкладки)
//   Header (заголовки панелей — Bold)
//   Mono   (TextBox, числові значення)
//
// Apply  — перезавантажує шрифти без перезапуску.
// Save   — записує data/editor_config.json.
// ─────────────────────────────────────────────────────────

namespace SettingsEditor {

struct FontSlot {
    char path[256];
    int  size;
};

struct Config {
    FontSlot label;
    FontSlot header;
    FontSlot mono;
};

static Config g_cfg = {
    {"data/fonts/Arimo-Regular.ttf", 10},
    {"data/fonts/Arimo-Bold.ttf",    12},
    {"data/fonts/UbuntuMono-R.ttf",  11},
};

// Буфери для TextBox-ів
static char g_buf[3][2][256] = {};  // [role][0=path/1=size]

static void ConfigToBuffers() {
    FontSlot* slots[3] = {&g_cfg.label, &g_cfg.header, &g_cfg.mono};
    for (int i = 0; i < 3; ++i) {
        strncpy(g_buf[i][0], slots[i]->path, 255);
        snprintf(g_buf[i][1], 8, "%d", slots[i]->size);
    }
}
static void BuffersToConfig() {
    FontSlot* slots[3] = {&g_cfg.label, &g_cfg.header, &g_cfg.mono};
    for (int i = 0; i < 3; ++i) {
        strncpy(slots[i]->path, g_buf[i][0], 255);
        int s = (int)strtol(g_buf[i][1], nullptr, 10);
        if (s > 0) slots[i]->size = s;
    }
}

// ── Load / Save ───────────────────────────────────────────

inline bool Load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { ConfigToBuffers(); return false; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    static char buf[4096];
    if (sz >= (long)sizeof(buf)) { fclose(f); ConfigToBuffers(); return false; }
    (void)fread(buf, 1, (size_t)sz, f); buf[sz]='\0'; fclose(f);

    auto rstr = [&](const char* key, char* out) {
        const char* p = strstr(buf, key); if (!p) return;
        p = strchr(p, ':');  if (!p) return;
        p = strchr(p, '"');  if (!p) return; ++p;
        int i = 0;
        while (*p && *p != '"' && i < 255) out[i++] = *p++;
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

    ConfigToBuffers();
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

// ── ApplyFonts ────────────────────────────────────────────
inline void ApplyFonts() {
    using namespace EditorUI;
    BuffersToConfig();

    FontSlot* slots[FONT_COUNT] = {&g_cfg.label, &g_cfg.header, &g_cfg.mono};
    for (int i = 0; i < FONT_COUNT; ++i) {
        if (slots[i]->size < 6) slots[i]->size = 6;
        UnloadFont(g_font[i]);
        int atlas = (int)(slots[i]->size * 3 * g_ui_scale);
        if (atlas < 20) atlas = 20;
        g_font[i]    = LoadFontEx(slots[i]->path, atlas, NULL, 0);
        g_font_sz[i] = slots[i]->size;
        SetTextureFilter(g_font[i].texture, TEXTURE_FILTER_BILINEAR);
    }
    ConfigToBuffers();
}

// ── Draw ──────────────────────────────────────────────────
// Повертає true якщо натиснуто Save.
inline bool Draw(Rectangle area, const char* config_path,
                 char* status_msg, float* status_timer)
{
    using namespace EditorUI;
    Panel(area);

    int x  = (int)area.x + S(24);
    int y  = (int)area.y + S(20);
    int pw = (int)area.width - S(200);   // ширина поля path
    int sw_num = S(48);                  // ширина поля size

    // Підпис колонок
    int lx = x + S(62);
    Label(lx,           y, "Path", g_font_sz[FONT_LABEL], {100,108,140,255});
    Label(lx + pw + S(8), y, "Size", g_font_sz[FONT_LABEL], {100,108,140,255});
    y += S(18);
    HSep(x, y, (int)area.width - S(48)); y += S(10);

    // Три рядки шрифтів
    struct Row { const char* title; int path_id; int size_id; int role_idx; };
    Row rows[3] = {
        {"Label  (мітки, кнопки)",       4001, 4002, FONT_LABEL},
        {"Header (заголовки — Bold)",     4003, 4004, FONT_HEADER},
        {"Mono   (TextBox, числа)",       4005, 4006, FONT_MONO},
    };

    for (int i = 0; i < 3; ++i) {
        LabelHeader(x, y, rows[i].title);
        y += S(20);

        TextBox(rows[i].path_id,
                {(float)(x + S(62)), (float)y, (float)pw, (float)S(24)},
                g_buf[rows[i].role_idx][0], 255);

        TextBox(rows[i].size_id,
                {(float)(x + S(62) + pw + S(8)), (float)y,
                 (float)sw_num, (float)S(24)},
                g_buf[rows[i].role_idx][1], 8);

        y += S(34);
    }

    HSep(x, y, (int)area.width - S(48)); y += S(16);

    bool saved = false;

    if (Button({(float)x, (float)y, (float)S(90), (float)S(28)},
               "Apply", {60, 100, 170, 255})) {
        ApplyFonts();
        snprintf(status_msg, 64, "Fonts reloaded");
        *status_timer = 3.0f;
    }

    if (Button({(float)(x + S(102)), (float)y, (float)S(110), (float)S(28)},
               "Save config", {36, 110, 55, 255})) {
        BuffersToConfig();
        saved = Save(config_path);
        snprintf(status_msg, 64, saved ? "Config saved" : "Save failed!");
        *status_timer = 3.0f;
    }

    return saved;
}

} // namespace SettingsEditor
