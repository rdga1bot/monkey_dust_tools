#pragma once
#include "editor_ui.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ─────────────────────────────────────────────────────────
// SettingsEditor — вкладка налаштувань редактора.
//
// Зберігає/читає data/editor_config.json.
// "Apply"  — перезавантажує шрифти без перезапуску.
// "Save"   — записує поточні налаштування у файл.
// ─────────────────────────────────────────────────────────

namespace SettingsEditor {

struct Config {
    char ui_font_path  [256];
    int  ui_font_size;
    char mono_font_path[256];
    int  mono_font_size;
};

static Config g_cfg = {
    "data/fonts/Arimo-Regular.ttf", 10,
    "data/fonts/UbuntuMono-R.ttf",  11
};

// Буфери для TextBox-ів
static char g_buf_ui_path  [256] = {};
static char g_buf_ui_size  [8]   = {};
static char g_buf_mono_path[256] = {};
static char g_buf_mono_size[8]   = {};

static void ConfigToBuffers() {
    strncpy(g_buf_ui_path,   g_cfg.ui_font_path,   255);
    snprintf(g_buf_ui_size,  sizeof(g_buf_ui_size),  "%d", g_cfg.ui_font_size);
    strncpy(g_buf_mono_path, g_cfg.mono_font_path, 255);
    snprintf(g_buf_mono_size,sizeof(g_buf_mono_size),"%d", g_cfg.mono_font_size);
}
static void BuffersToConfig() {
    strncpy(g_cfg.ui_font_path,   g_buf_ui_path,   255);
    g_cfg.ui_font_size  = (int)strtol(g_buf_ui_size,   nullptr, 10);
    strncpy(g_cfg.mono_font_path, g_buf_mono_path, 255);
    g_cfg.mono_font_size = (int)strtol(g_buf_mono_size, nullptr, 10);
}

// ── Завантаження/збереження config ───────────────────────

inline bool Load(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) { ConfigToBuffers(); return false; }
    fseek(f,0,SEEK_END); long sz=ftell(f); fseek(f,0,SEEK_SET);
    static char buf[2048];
    if (sz >= (long)sizeof(buf)) { fclose(f); ConfigToBuffers(); return false; }
    (void)fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);

    auto read_str = [&](const char* key, char* out, int maxlen) {
        const char* p = strstr(buf, key);
        if (!p) return;
        p = strchr(p, ':'); if (!p) return;
        p = strchr(p, '"'); if (!p) return; ++p;
        int i = 0;
        while (*p && *p != '"' && i < maxlen-1) out[i++] = *p++;
        out[i] = '\0';
    };
    auto read_int = [&](const char* key) -> int {
        const char* p = strstr(buf, key);
        if (!p) return 0;
        p = strchr(p, ':'); if (!p) return 0;
        return (int)strtol(p+1, nullptr, 10);
    };

    read_str("\"ui_font_path\"",   g_cfg.ui_font_path,   256);
    read_str("\"mono_font_path\"", g_cfg.mono_font_path, 256);
    int us = read_int("\"ui_font_size\"");   if (us > 0) g_cfg.ui_font_size   = us;
    int ms = read_int("\"mono_font_size\""); if (ms > 0) g_cfg.mono_font_size = ms;

    ConfigToBuffers();
    return true;
}

inline bool Save(const char* path) {
    FILE* f = fopen(path, "w");
    if (!f) return false;
    fprintf(f,
        "{\n"
        "  \"ui_font_path\":   \"%s\",\n"
        "  \"ui_font_size\":   %d,\n"
        "  \"mono_font_path\": \"%s\",\n"
        "  \"mono_font_size\": %d\n"
        "}\n",
        g_cfg.ui_font_path, g_cfg.ui_font_size,
        g_cfg.mono_font_path, g_cfg.mono_font_size);
    fclose(f);
    return true;
}

// ── Перезавантаження шрифтів ──────────────────────────────
// Викликається після "Apply".
inline void ApplyFonts() {
    using namespace EditorUI;
    BuffersToConfig();
    if (g_cfg.ui_font_size   < 6)  g_cfg.ui_font_size   = 6;
    if (g_cfg.mono_font_size < 6)  g_cfg.mono_font_size = 6;

    auto reload = [](Font& font, const char* path, int sz) {
        UnloadFont(font);
        int atlas = (int)(sz * 3 * EditorUI::g_ui_scale);
        if (atlas < 20) atlas = 20;
        font = LoadFontEx(path, atlas, NULL, 0);
        SetTextureFilter(font.texture, TEXTURE_FILTER_BILINEAR);
    };
    reload(g_ui_font,      g_cfg.ui_font_path,   g_cfg.ui_font_size);
    reload(g_ui_font_mono, g_cfg.mono_font_path, g_cfg.mono_font_size);
    ConfigToBuffers(); // оновити буфери (розмір міг бути скоригований)
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
    int fw = (int)area.width - S(48);    // ширина поля вводу

    // ── UI Font ───────────────────────────────────────────
    Label(x, y, "UI Font  (proportional)", 10, {170, 195, 255, 255});
    y += S(20);

    Label(x, y, "Path :", 10);
    TextBox(3001, {(float)(x + S(54)), (float)y, (float)(fw - S(54)), (float)S(24)},
            g_buf_ui_path, 255);
    y += S(30);

    Label(x, y, "Size :", 10);
    TextBox(3002, {(float)(x + S(54)), (float)y, (float)S(48), (float)S(24)},
            g_buf_ui_size, 8);
    y += S(38);

    HSep(x, y, fw); y += S(16);

    // ── Mono Font ─────────────────────────────────────────
    Label(x, y, "Mono Font  (TextBox / numbers)", 10, {170, 195, 255, 255});
    y += S(20);

    Label(x, y, "Path :", 10);
    TextBox(3003, {(float)(x + S(54)), (float)y, (float)(fw - S(54)), (float)S(24)},
            g_buf_mono_path, 255);
    y += S(30);

    Label(x, y, "Size :", 10);
    TextBox(3004, {(float)(x + S(54)), (float)y, (float)S(48), (float)S(24)},
            g_buf_mono_size, 8);
    y += S(42);

    HSep(x, y, fw); y += S(16);

    // ── Кнопки ────────────────────────────────────────────
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
