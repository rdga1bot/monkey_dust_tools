#pragma once
#include "raylib.h"
#include <cstring>
#include <cstdio>

// ─────────────────────────────────────────────────────────
// EditorUI — immediate-mode UI поверх raylib.
//
// Три ролі шрифтів:
//   FONT_LABEL  — Arimo Regular 10   (мітки, кнопки, вкладки)
//   FONT_HEADER — Arimo Bold   12   (заголовки панелей, секцій)
//   FONT_MONO   — Ubuntu Mono  11   (TextBox, числові значення)
//
// Налаштовуються у вкладці Settings редактора.
// Конфіг зберігається у data/editor_config.json.
// ─────────────────────────────────────────────────────────

namespace EditorUI {

enum FontRole { FONT_LABEL = 0, FONT_HEADER = 1, FONT_MONO = 2, FONT_COUNT = 3 };

static Font  g_font    [FONT_COUNT] = {};
static int   g_font_sz [FONT_COUNT] = {10, 12, 11};   // базові розміри
static float g_ui_scale             = 1.0f;

static int   g_active_id    = -1;
static int   g_select_all_id = -1;   // TextBox із "select all" після кліку

// ── Scale helper ─────────────────────────────────────────
inline int S(int px) { return (int)(px * g_ui_scale); }

// ── Text helpers ─────────────────────────────────────────
inline void UiText(const char* t, int x, int y, int sz, Color c,
                   FontRole role = FONT_LABEL) {
    Font& f = g_font[role];
    if (f.texture.id != 0)
        DrawTextEx(f, t, {(float)x, (float)y}, sz * g_ui_scale, 0, c);
    else
        DrawText(t, x, y, sz, c);
}
inline int UiMeasure(const char* t, int sz, FontRole role = FONT_LABEL) {
    Font& f = g_font[role];
    if (f.texture.id != 0)
        return (int)MeasureTextEx(f, t, sz * g_ui_scale, 0).x;
    return MeasureText(t, sz);
}

inline void UiTextHeader(const char* t, int x, int y, int sz, Color c) {
    UiText(t, x, y, sz, c, FONT_HEADER);
}
inline void UiTextMono(const char* t, int x, int y, int sz, Color c) {
    UiText(t, x, y, sz, c, FONT_MONO);
}
inline int UiMeasureMono(const char* t, int sz) {
    return UiMeasure(t, sz, FONT_MONO);
}

// ── Helpers ───────────────────────────────────────────────
inline bool MouseIn(Rectangle r) {
    Vector2 m = GetMousePosition();
    return m.x >= r.x && m.x < r.x + r.width
        && m.y >= r.y && m.y < r.y + r.height;
}

// ── Button ────────────────────────────────────────────────
inline bool Button(Rectangle r, const char* label,
                   Color bg = {55, 100, 180, 255})
{
    bool hov = MouseIn(r);
    bool clk = hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    Color c  = hov ? Color{85, 140, 230, 255} : bg;
    DrawRectangleRec(r, c);
    DrawRectangleLinesEx(r, 1, {110, 150, 210, 255});
    int sz = g_font_sz[FONT_LABEL];
    int tw = UiMeasure(label, sz);
    UiText(label,
           (int)(r.x + (r.width  - tw) * 0.5f),
           (int)(r.y + (r.height - sz * g_ui_scale) * 0.5f),
           sz, WHITE);
    return clk;
}

// ── TextBox ───────────────────────────────────────────────
// Клік → виділяє весь вміст (підсвічення).
// Перший символ або Backspace → замінює вміст.
inline bool TextBox(int id, Rectangle r, char* buf, int maxlen) {
    bool focused = (g_active_id == id);
    bool hov     = MouseIn(r);

    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        g_active_id     = id;
        g_select_all_id = id;   // select-all при новому фокусі
    }
    if (!hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && focused) {
        g_active_id     = -1;
        g_select_all_id = -1;
    }

    bool changed  = false;
    bool sel_all  = (g_select_all_id == id);

    if (focused) {
        int len = (int)strlen(buf);
        int ch;
        while ((ch = GetCharPressed()) > 0) {
            if (sel_all) {
                buf[0] = '\0'; len = 0;
                g_select_all_id = -1; sel_all = false;
            }
            if (len < maxlen - 1 && ch >= 32) {
                buf[len++] = (char)ch; buf[len] = '\0';
                changed = true;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            if (sel_all) {
                buf[0] = '\0';
                g_select_all_id = -1; sel_all = false;
            } else if (len > 0) {
                buf[--len] = '\0';
            }
            changed = true;
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_TAB)) {
            g_active_id = -1; g_select_all_id = -1;
        }
    }

    // Відрисовка
    Color bg  = focused ? Color{38, 48, 72, 255} : Color{24, 28, 44, 255};
    Color brd = focused ? Color{80, 160, 255, 255} : Color{65, 72, 98, 255};
    DrawRectangleRec(r, bg);
    // Select-all підсвічення
    if (sel_all && focused)
        DrawRectangleRec({r.x + 1, r.y + 1, r.width - 2, r.height - 2},
                         {60, 120, 200, 80});
    DrawRectangleLinesEx(r, 1, brd);

    int sz = g_font_sz[FONT_MONO];
    char disp[260];
    snprintf(disp, sizeof(disp), "%s", buf);
    if (focused && !sel_all && (int)(GetTime() * 2) % 2 == 0) {
        int dlen = (int)strlen(disp);
        if (dlen < (int)sizeof(disp) - 1) { disp[dlen] = '|'; disp[dlen+1] = '\0'; }
    }
    UiTextMono(disp,
               (int)r.x + S(5),
               (int)(r.y + (r.height - sz * g_ui_scale) * 0.5f),
               sz, WHITE);
    return changed;
}

// ── Label (Regular) ───────────────────────────────────────
inline void Label(int x, int y, const char* text,
                  int sz = -1, Color c = {195, 200, 215, 255}) {
    if (sz < 0) sz = g_font_sz[FONT_LABEL];
    UiText(text, x, y, sz, c, FONT_LABEL);
}

// ── LabelHeader (Bold) ────────────────────────────────────
inline void LabelHeader(int x, int y, const char* text,
                        int sz = -1, Color c = {170, 195, 255, 255}) {
    if (sz < 0) sz = g_font_sz[FONT_HEADER];
    UiTextHeader(text, x, y, sz, c);
}

// ── Horizontal separator ──────────────────────────────────
inline void HSep(int x, int y, int w) {
    DrawLine(x, y, x + w, y, {55, 60, 82, 255});
}

// ── Panel background ──────────────────────────────────────
inline void Panel(Rectangle r, Color bg = {18, 22, 36, 255}) {
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, {48, 54, 76, 255});
}

// ── Tab bar ───────────────────────────────────────────────
inline int TabBar(int x, int y, int tab_w, int tab_h,
                  const char** labels, int count, int active)
{
    int clicked = active;
    int sz = g_font_sz[FONT_LABEL];
    for (int i = 0; i < count; ++i) {
        Rectangle r = { (float)(x + i * tab_w), (float)y,
                        (float)tab_w, (float)tab_h };
        bool is_act = (i == active);
        Color bg    = is_act ? Color{45, 55, 88, 255}  : Color{28, 33, 52, 255};
        Color brd   = is_act ? Color{80, 160, 255, 255} : Color{50, 56, 78, 255};
        DrawRectangleRec(r, bg);
        DrawRectangleLinesEx(r, 1, brd);
        if (is_act)
            DrawLine((int)r.x+1, (int)(r.y+r.height-2),
                     (int)(r.x+r.width-2), (int)(r.y+r.height-2),
                     {80, 160, 255, 255});
        int tw = UiMeasure(labels[i], sz);
        Color tc = is_act ? WHITE : Color{140, 148, 172, 255};
        UiText(labels[i],
               (int)(r.x + (r.width  - tw) * 0.5f),
               (int)(r.y + (r.height - sz * g_ui_scale) * 0.5f),
               sz, tc);
        if (MouseIn(r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            clicked = i;
    }
    return clicked;
}

} // namespace EditorUI
