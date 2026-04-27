#include "raylib.h"
#include "editor_ui.h"
#include "item_editor.h"
#include "faction_editor.h"
#include <cstdio>

// ─────────────────────────────────────────────────────────
// monkey_dust EDITOR — окремий бінарник для редагування JSON.
//
// Вкладки:  [Items]  [Factions]
// Запуск:   ./build/tools/monkey_dust_editor
// Читає/пише data/ відносно CWD (запускати з кореня репо).
// Розмір вікна: авто-підбір під поточний монітор (85% висоти).
// Шрифт:   data/fonts/Arimo-Regular.ttf
// ─────────────────────────────────────────────────────────

int main(void)
{
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(1280, 720, "monkey_dust EDITOR v0.1");

    // ── Авто-розмір під монітор ───────────────────────────
    int mon = GetCurrentMonitor();
    int mw  = GetMonitorWidth(mon);
    int mh  = GetMonitorHeight(mon);

    // 85% висоти монітора, 16:9, не більше 90% ширини
    int wh = (mh * 85) / 100;
    int ww = (wh * 16) / 9;
    if (ww > (mw * 90) / 100) {
        ww = (mw * 90) / 100;
        wh = (ww * 9) / 16;
    }
    // Мінімальний розмір
    if (wh < 480) { wh = 480; ww = 854; }

    SetWindowSize(ww, wh);
    SetWindowPosition((mw - ww) / 2, (mh - wh) / 2);

    // Масштаб — основа 720p, все масштабується пропорційно
    EditorUI::g_ui_scale = wh / 720.0f;

    // ── Шрифт Arimo ───────────────────────────────────────
    // Завантажуємо після SetWindowSize щоб scale вже відомий.
    // Розмір атласу = 36px * scale → 1:1 пікселів на екрані.
    int font_atlas_px = (int)(36 * EditorUI::g_ui_scale);
    if (font_atlas_px < 20) font_atlas_px = 20;
    EditorUI::g_ui_font = LoadFontEx(
        "data/fonts/Arimo-Regular.ttf", font_atlas_px, NULL, 0);
    SetTextureFilter(EditorUI::g_ui_font.texture, TEXTURE_FILTER_BILINEAR);

    SetTargetFPS(60);
    SetExitKey(KEY_ESCAPE);

    // ── Завантаження даних ────────────────────────────────
    ItemEditor::Load("data/items/items.json");
    FactionEditor::Load("data/factions/factions.json");

    int  active_tab  = 0;
    char status_msg  [64] = "";
    float status_timer    = 0.0f;

    const char* tab_names[] = { "Items", "Factions" };

    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();
        if (status_timer > 0.0f) status_timer -= dt;

        // Поточний розмір вікна (користувач міг переміститись між моніторами)
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();

        using EditorUI::S;

        int top_h  = S(40);
        int list_w = S(270);
        int tab_w  = S(110);

        BeginDrawing();
        ClearBackground({14, 18, 30, 255});

        // ── Top bar ───────────────────────────────────────
        DrawRectangle(0, 0, sw, top_h, {22, 27, 44, 255});
        DrawLine(0, top_h - 1, sw, top_h - 1, {48, 54, 78, 255});

        EditorUI::UiText("monkey_dust EDITOR", S(10), S(13), 14,
                         {150, 170, 220, 255});

        // Tabs
        active_tab = EditorUI::TabBar(
            S(220), 0, tab_w, top_h,
            tab_names, 2, active_tab);

        // Статус-повідомлення (праворуч)
        if (status_timer > 0.0f && status_msg[0] != '\0') {
            int smw = EditorUI::UiMeasure(status_msg, 14);
            DrawRectangle(sw - smw - S(20), S(8), smw + S(14), S(24),
                          {30, 80, 40, 200});
            EditorUI::UiText(status_msg, sw - smw - S(13), S(13), 14,
                             {100, 230, 120, 255});
        }

        // Підказка ESC
        EditorUI::UiText("ESC: exit", sw - S(90), S(14), 12,
                         {75, 80, 105, 255});

        // ── Панелі ────────────────────────────────────────
        Rectangle list_r = { 0,          (float)top_h,
                              (float)list_w, (float)(sh - top_h) };
        Rectangle edit_r = { (float)list_w, (float)top_h,
                              (float)(sw - list_w), (float)(sh - top_h) };

        bool saved = false;
        if (active_tab == 0)
            saved = ItemEditor::Draw(list_r, edit_r, "data/items/items.json");
        else
            saved = FactionEditor::Draw(list_r, edit_r,
                                        "data/factions/factions.json");

        if (saved) {
            snprintf(status_msg, sizeof(status_msg), "Saved!");
            status_timer = 3.0f;
        }

        EndDrawing();
    }

    UnloadFont(EditorUI::g_ui_font);
    CloseWindow();
    return 0;
}
