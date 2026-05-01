#include "raylib.h"
#include "rlImGui.h"
#include "imgui.h"
#include "editor_ui.h"
#include "item_editor.h"
#include "faction_editor.h"
#include "settings_editor.h"
#include "editor_map_view.h"
#include <cstdio>
#include <cstring>

// ─────────────────────────────────────────────────────────
// monkey_dust EDITOR — Dear ImGui via rlImGui.
//
// Вкладки:  [Items]  [Factions]  [Settings]
// Запуск:   ./build/tools/monkey_dust_editor  (з кореня репо)
// Конфіг:   data/editor_config.json
// ─────────────────────────────────────────────────────────

static constexpr const char* CFG_PATH = "data/editor_config.json";

int main(void) {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT);
    InitWindow(1280, 720, "monkey_dust EDITOR v0.1");

    // ── Авто-розмір під монітор ───────────────────────────
    int mon = GetCurrentMonitor();
    int mw  = GetMonitorWidth(mon);
    int mh  = GetMonitorHeight(mon);
    int wh  = (mh * 85) / 100;
    int ww  = (wh * 16) / 9;
    if (ww > (mw * 90) / 100) { ww = (mw * 90) / 100; wh = (ww * 9) / 16; }
    if (wh < 480) { wh = 480; ww = 854; }
    SetWindowSize(ww, wh);
    SetWindowPosition((mw - ww) / 2, (mh - wh) / 2);
    EditorUI::ui_scale = wh / 720.0f;

    // ── Конфіг шрифтів ────────────────────────────────────
    SettingsEditor::Load(CFG_PATH);
    SettingsEditor::Config& cfg = SettingsEditor::g_cfg;

    // ── ImGui init ────────────────────────────────────────
    rlImGuiBeginInitImGui();

    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;   // не зберігати imgui.ini

    // Очищаємо дефолтний шрифт доданий rlImGuiBeginInitImGui
    io.Fonts->Clear();

    // Glyph ranges: Basic Latin + Cyrillic
    static const ImWchar ranges[] = { 0x0020, 0x00FF, 0x0400, 0x04FF, 0 };

    float sc = EditorUI::ui_scale;
    auto load_font = [&](const char* path, float base_sz) -> ImFont* {
        float sz = base_sz * sc;
        if (sz < 8.0f) sz = 8.0f;
        ImFont* f = io.Fonts->AddFontFromFileTTF(path, sz, nullptr, ranges);
        return f ? f : io.Fonts->AddFontDefault();
    };

    EditorUI::font_regular = load_font(cfg.label.path,  (float)cfg.label.size);
    EditorUI::font_bold    = load_font(cfg.header.path, (float)cfg.header.size);
    EditorUI::font_mono    = load_font(cfg.mono.path,   (float)cfg.mono.size);

    rlImGuiEndInitImGui();
    EditorUI::SetupTheme();

    // ── Дані ─────────────────────────────────────────────
    ItemEditor::Load("data/items/items.json");
    FactionEditor::Load("data/factions/factions.json");
    MapViewPanel::Get().Init();

    SetTargetFPS(60);
    SetExitKey(KEY_ESCAPE);

    char  status_msg  [64] = "";
    float status_timer     = 0.0f;

    while (!WindowShouldClose()) {
        float dt = GetFrameTime();
        if (status_timer > 0.0f) status_timer -= dt;

        BeginDrawing();
        ClearBackground({14, 18, 30, 255});
        rlImGuiBegin();

        // Повноекранне вікно ImGui
        ImGuiIO& fio = ImGui::GetIO();
        ImGui::SetNextWindowPos({0, 0});
        ImGui::SetNextWindowSize(fio.DisplaySize);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0, 0});
        ImGui::Begin("##editor", nullptr,
            ImGuiWindowFlags_NoTitleBar  | ImGuiWindowFlags_NoResize  |
            ImGuiWindowFlags_NoMove      | ImGuiWindowFlags_NoScrollbar|
            ImGuiWindowFlags_NoBringToFrontOnFocus |
            ImGuiWindowFlags_MenuBar);
        ImGui::PopStyleVar();

        // ── Menu bar ──────────────────────────────────────
        static char s_open_path[256]   = "";
        static char s_saveas_path[256] = "";
        static bool s_open_modal       = false;
        static bool s_saveas_modal     = false;

        if (ImGui::BeginMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Map...")) {
                    // Pre-fill with current path if available.
                    snprintf(s_open_path, sizeof(s_open_path), "%s",
                             MapViewPanel::Get().GetLoadPath());
                    s_open_modal = true;
                }
                ImGui::Separator();
                bool has_map = MapViewPanel::Get().IsLoaded();
                if (!has_map) ImGui::BeginDisabled();
                if (ImGui::MenuItem("Save Map", "Ctrl+S")) {
                    if (MapViewPanel::Get().SaveCurrent()) {
                        snprintf(status_msg, sizeof(status_msg), "Map saved.");
                        status_timer = 3.0f;
                    } else {
                        snprintf(status_msg, sizeof(status_msg), "Save FAILED.");
                        status_timer = 3.0f;
                    }
                }
                if (ImGui::MenuItem("Save Map As...")) {
                    snprintf(s_saveas_path, sizeof(s_saveas_path), "%s",
                             MapViewPanel::Get().GetSavePath());
                    s_saveas_modal = true;
                }
                if (!has_map) ImGui::EndDisabled();
                ImGui::EndMenu();
            }
            ImGui::EndMenuBar();
        }

        // Ctrl+S shortcut
        if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) && ImGui::IsKeyPressed(ImGuiKey_S)) {
            if (MapViewPanel::Get().IsLoaded()) {
                if (MapViewPanel::Get().SaveCurrent()) {
                    snprintf(status_msg, sizeof(status_msg), "Map saved.");
                    status_timer = 3.0f;
                }
            }
        }

        // ── Open Map modal ────────────────────────────────
        if (s_open_modal) { ImGui::OpenPopup("Open Map"); s_open_modal = false; }
        ImGui::SetNextWindowSize({520, 110}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Open Map", nullptr, ImGuiWindowFlags_NoResize)) {
            ImGui::Text("Map path (relative to repo root):");
            ImGui::SetNextItemWidth(-70);
            ImGui::InputText("##opath", s_open_path, sizeof(s_open_path));
            ImGui::SameLine();
            if (ImGui::Button("Open", {60, 0})) {
                if (MapViewPanel::Get().LoadMap(s_open_path)) {
                    snprintf(status_msg, sizeof(status_msg), "Loaded: %s", s_open_path);
                    status_timer = 3.0f;
                } else {
                    snprintf(status_msg, sizeof(status_msg), "Failed to load map.");
                    status_timer = 3.0f;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ── Save Map As modal ─────────────────────────────
        if (s_saveas_modal) { ImGui::OpenPopup("Save Map As"); s_saveas_modal = false; }
        ImGui::SetNextWindowSize({520, 110}, ImGuiCond_Always);
        if (ImGui::BeginPopupModal("Save Map As", nullptr, ImGuiWindowFlags_NoResize)) {
            ImGui::Text("Save path (relative to repo root):");
            ImGui::SetNextItemWidth(-70);
            ImGui::InputText("##spath", s_saveas_path, sizeof(s_saveas_path));
            ImGui::SameLine();
            if (ImGui::Button("Save", {60, 0})) {
                if (MapViewPanel::Get().SaveTo(s_saveas_path)) {
                    snprintf(status_msg, sizeof(status_msg), "Saved: %s", s_saveas_path);
                    status_timer = 3.0f;
                } else {
                    snprintf(status_msg, sizeof(status_msg), "Save FAILED.");
                    status_timer = 3.0f;
                }
                ImGui::CloseCurrentPopup();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // ── Top bar ───────────────────────────────────────
        ImGui::SetCursorPos({10, 8});
        if (EditorUI::font_bold) ImGui::PushFont(EditorUI::font_bold);
        ImGui::Text("monkey_dust EDITOR");
        if (EditorUI::font_bold) ImGui::PopFont();

        // Status (праворуч у top bar)
        if (status_timer > 0.0f && status_msg[0] != '\0') {
            float alpha = (status_timer > 1.0f) ? 1.0f : status_timer;
            ImGui::SameLine(fio.DisplaySize.x - 280);
            ImGui::TextColored({0.40f, 0.90f, 0.50f, alpha}, "%s", status_msg);
        }

        ImGui::SetCursorPosX(0);
        ImGui::Separator();

        // ── Tab bar ───────────────────────────────────────
        ImGui::SetCursorPosX(4);
        if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_None)) {

            if (ImGui::BeginTabItem("Items")) {
                ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                if (ItemEditor::Draw("data/items/items.json")) {
                    snprintf(status_msg, sizeof(status_msg), "Items saved!");
                    status_timer = 3.0f;
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Factions")) {
                ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                if (FactionEditor::Draw("data/factions/factions.json")) {
                    snprintf(status_msg, sizeof(status_msg), "Factions saved!");
                    status_timer = 3.0f;
                }
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Map")) {
                ImGui::SetCursorPos({8, ImGui::GetCursorPosY() + 4});
                MapViewPanel::Get().Draw(dt);
                ImGui::EndTabItem();
            }

            if (ImGui::BeginTabItem("Settings")) {
                ImGui::SetCursorPos({12, ImGui::GetCursorPosY() + 6});
                SettingsEditor::Draw(CFG_PATH, status_msg, &status_timer);
                ImGui::EndTabItem();
            }

            ImGui::EndTabBar();
        }

        ImGui::End();

        rlImGuiEnd();
        EndDrawing();
    }

    MapViewPanel::Get().Shutdown();
    rlImGuiShutdown();
    CloseWindow();
    return 0;
}
