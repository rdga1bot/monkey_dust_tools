#pragma once
#include "raylib.h"
#include <cstring>
#include <cstdio>

// ─────────────────────────────────────────────────────────
// EditorUI — мінімальний immediate-mode UI поверх raylib.
//
// Всі функції — inline, без зовнішніх залежностей.
// ID-based focus: g_active_id = який TextBox зараз активний.
// ─────────────────────────────────────────────────────────

namespace EditorUI {

static int g_active_id = -1;

// ── Helpers ───────────────────────────────────────────────
inline bool MouseIn(Rectangle r) {
    Vector2 m = GetMousePosition();
    return m.x >= r.x && m.x < r.x + r.width
        && m.y >= r.y && m.y < r.y + r.height;
}

// ── Button ────────────────────────────────────────────────
// Повертає true при натисканні.
inline bool Button(Rectangle r, const char* label,
                   Color bg = {55, 100, 180, 255})
{
    bool hov = MouseIn(r);
    bool clk = hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    Color c  = hov ? Color{85, 140, 230, 255} : bg;
    DrawRectangleRec(r, c);
    DrawRectangleLinesEx(r, 1, {110, 150, 210, 255});
    int tw = MeasureText(label, 14);
    DrawText(label,
             (int)(r.x + (r.width  - tw) * 0.5f),
             (int)(r.y + (r.height - 14) * 0.5f),
             14, WHITE);
    return clk;
}

// ── TextBox ───────────────────────────────────────────────
// id — унікальний int на кожен поле.
// Повертає true якщо текст змінився в цьому кадрі.
inline bool TextBox(int id, Rectangle r, char* buf, int maxlen) {
    bool focused = (g_active_id == id);
    bool hov     = MouseIn(r);

    // Клік всередині — отримуємо фокус.
    if (hov && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
        g_active_id = id;
    // Клік поза — втрачаємо фокус.
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

    // Відрисовка
    Color bg  = focused ? Color{38, 48, 72, 255} : Color{24, 28, 44, 255};
    Color brd = focused ? Color{80, 160, 255, 255} : Color{65, 72, 98, 255};
    DrawRectangleRec(r, bg);
    DrawRectangleLinesEx(r, 1, brd);

    // Текст з курсором (мигає кожні 0.5с)
    char disp[260];
    snprintf(disp, sizeof(disp), "%s", buf);
    if (focused && (int)(GetTime() * 2) % 2 == 0) {
        int dlen = (int)strlen(disp);
        if (dlen < (int)sizeof(disp) - 1) {
            disp[dlen]   = '|';
            disp[dlen+1] = '\0';
        }
    }
    DrawText(disp,
             (int)r.x + 5,
             (int)(r.y + (r.height - 14) * 0.5f),
             14, WHITE);
    return changed;
}

// ── Label ─────────────────────────────────────────────────
inline void Label(int x, int y, const char* text,
                  int sz = 13, Color c = {195, 200, 215, 255}) {
    DrawText(text, x, y, sz, c);
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
// Повертає індекс активної вкладки.
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
        // Активна вкладка: яскрава лінія знизу
        if (is_act)
            DrawLine((int)r.x+1, (int)(r.y+r.height-2),
                     (int)(r.x+r.width-2), (int)(r.y+r.height-2),
                     {80, 160, 255, 255});
        int tw = MeasureText(labels[i], 14);
        Color tc = is_act ? Color{255, 255, 255, 255} : Color{140, 148, 172, 255};
        DrawText(labels[i],
                 (int)(r.x + (r.width  - tw) * 0.5f),
                 (int)(r.y + (r.height - 14) * 0.5f),
                 14, tc);
        if (MouseIn(r) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT))
            clicked = i;
    }
    return clicked;
}

} // namespace EditorUI
