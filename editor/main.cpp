#include "raylib.h"
#include "editor_ui.h"
#include "item_editor.h"
#include "faction_editor.h"
#include <cstdio>

// ─────────────────────────────────────────────────────────
// monkey_dust EDITOR — окремий бінарник для редагування JSON.
//
// Вкладки:  [Items]  [Factions]
// Запуск:   ./build/monkey_dust_editor
// Читає/пише data/ відносно CWD (запускати з кореня репо).
// ─────────────────────────────────────────────────────────

static constexpr int  W      = 1280;
static constexpr int  H      = 720;
static constexpr int  TOP_H  = 40;
static constexpr int  LIST_W = 270;

int main(void)
{
    SetTraceLogLevel(LOG_WARNING);
    InitWindow(W, H, "monkey_dust EDITOR v0.1");
    SetTargetFPS(60);
    SetExitKey(KEY_ESCAPE);

    // ── Завантаження даних ────────────────────────────────
    ItemEditor::Load("data/items/items.json");
    FactionEditor::Load("data/factions/factions.json");

    int  active_tab  = 0;
    char status_msg  [64] = "";
    float status_timer    = 0.0f;

    const char* tab_names[] = { "Items", "Factions" };
    static constexpr int TAB_W = 110;

    while (!WindowShouldClose())
    {
        float dt = GetFrameTime();
        if (status_timer > 0.0f) status_timer -= dt;

        BeginDrawing();
        ClearBackground({14, 18, 30, 255});

        // ── Top bar ───────────────────────────────────────
        DrawRectangle(0, 0, W, TOP_H, {22, 27, 44, 255});
        DrawLine(0, TOP_H - 1, W, TOP_H - 1, {48, 54, 78, 255});

        DrawText("monkey_dust EDITOR", 10, 13, 14, {150, 170, 220, 255});

        // Tabs
        active_tab = EditorUI::TabBar(
            220, 0, TAB_W, TOP_H,
            tab_names, 2, active_tab);

        // Статус-повідомлення (праворуч)
        if (status_timer > 0.0f && status_msg[0] != '\0') {
            int sw = MeasureText(status_msg, 14);
            DrawRectangle(W - sw - 20, 8, sw + 14, 24, {30, 80, 40, 200});
            DrawText(status_msg, W - sw - 13, 13, 14, {100, 230, 120, 255});
        }

        // Підказка ESC
        DrawText("ESC: exit", W - 90, 14, 12, {75, 80, 105, 255});

        // ── Панелі ────────────────────────────────────────
        Rectangle list_r = { 0,          (float)TOP_H,
                              (float)LIST_W, (float)(H - TOP_H) };
        Rectangle edit_r = { (float)LIST_W, (float)TOP_H,
                              (float)(W - LIST_W), (float)(H - TOP_H) };

        bool saved = false;
        if (active_tab == 0)
            saved = ItemEditor::Draw(list_r, edit_r, "data/items/items.json");
        else
            saved = FactionEditor::Draw(list_r, edit_r, "data/factions/factions.json");

        if (saved) {
            snprintf(status_msg, sizeof(status_msg), "Saved!");
            status_timer = 3.0f;
        }

        EndDrawing();
    }

    CloseWindow();
    return 0;
}
