#pragma once
#include "raylib.h"
#include <cstring>
#include <cstdio>

// ─────────────────────────────────────────────────────────
// EditorUI — мінімальний immediate-mode UI поверх raylib.
//
// Всі функції — inline, без зовнішніх залежностей.
// ID-based focus: g_active_id = який TextBox зараз активний.
// Font: Arimo-Regular, завантажується в main.cpp після InitWindow.
// Scale: g_ui_scale = screen_height / 720.0f (авто-detect монітора).
// ─────────────────────────────────────────────────────────

namespace EditorUI {

static int   g_active_id = -1;
static Font  g_ui_font   = {0};
static float g_ui_scale  = 1.0f;

// ── Scale helper: масштабує пікселі до поточного DPI ─────
inline int S(int px) { return (int)(px * g_ui_scale); }

// ── Text helpers (замінюють DrawText/MeasureText) ─────────
inline void UiText(const char* t, int x, int y, int sz, Color c) {
    if (g_ui_font.texture.id != 0)
        DrawTextEx(g_ui_font, t, {(float)x, (float)y}, sz * g_ui_scale, 0, c);
    else
        DrawText(t, x, y, sz, c);
}
inline int UiMeasure(const char* t, int sz) {
    if (g_ui_font.texture.id != 0)
        return (int)MeasureTextEx(g_ui_font, t, sz * g_ui_scale, 0).x;
    return MeasureText(t, sz);
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
    int tw = UiMeasure(label, 14);
    UiText(label,
           (int)(r.x + (r.width  - tw) * 0.5f),
           (int)(r.y + (r.height - 14 * g_ui_scale) * 0.5f),
           14, WHITE);
    return clk;
}

// ── TextBox ───────────────────────────────────────────────
inline bool TextBox(int id, Rectangle r, char* buf, int maxlen) {
    bool focused = (g_active_id == id);
    bool hov     = MouseIn(r);

    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        g_active_id = id;
    if (!hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && focused)
        g_active_id = -1;

    bool changed = false;
    if (focused) {
        int len = (int)strlen(buf);
        int ch;
        while ((ch = GetCharPressed()) > 0) {
            if (len < maxlen - 1 && ch >= 32) {
                buf[len++] = (char)ch;
                buf[len]   = '\0';
                changed    = true;
            }
        }
        if (IsKeyPressed(KEY_BACKSPACE) && len > 0) {
            buf[--len] = '\0';
            changed    = true;
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_TAB))
            g_active_id = -1;
    }

    Color bg  = focused ? Color{38, 48, 72, 255} : Color{24, 28, 44, 255};
    Color brd = focused ? Color{80, 160, 255, 255} : Color{65, 72, 98, 255};
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, brd);

    char disp[260];
    snprintf(disp, sizeof(disp), "%s", buf);
    if (focused && (int)(GetTime() * 2) % 2 == 0) {
        int dlen = (int)strlen(disp);
        if (dlen < (int)sizeof(disp) - 1) {
            disp[dlen]   = '|';
            disp[dlen+1] = '\0';
        }
    }
    UiText(disp,
           (int)r.x + S(5),
           (int)(r.y + (r.height - 14 * g_ui_scale) * 0.5f),
           14, WHITE);
    return changed;
}

// ── Label ─────────────────────────────────────────────────
inline void Label(int x, int y, const char* text,
                  int sz = 13, Color c = {195, 200, 215, 255}) {
    UiText(text, x, y, sz, c);
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
        int tw = UiMeasure(labels[i], 14);
        Color tc = is_act ? Color{255, 255, 255, 255} : Color{140, 148, 172, 255};
        UiText(labels[i],
               (int)(r.x + (r.width  - tw) * 0.5f),
               (int)(r.y + (r.height - 14 * g_ui_scale) * 0.5f),
               14, tc);
        if (MouseIn(r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            clicked = i;
    }
    return clicked;
}

} // namespace EditorUI
