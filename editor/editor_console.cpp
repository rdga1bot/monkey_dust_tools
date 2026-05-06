#ifdef MONKEY_DUST_EDITOR
#include "editor_console.h"
#include "editor_core.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/scripting/lua_system.h>
#include <monkey_dust/save/save_system.h>
#include <monkey_dust/nav/nav_system.h>
#include "world/faction_system.h"
#include <monkey_dust/platform/md_log.h>
#include <cstdio>
#include <cstring>
#ifdef MD_OPENGL43_ENABLED
#include <monkey_dust/world/transform_soa.h>
#endif

void EditorConsole::Init() {
    MdLogSetHook([](int level, const char* msg) {
        EditorConsole::Get().Log(level, msg);
    });
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorConsole::Log(int level, const char* text) {
    int idx = log_head_ % MAX_LINES;
    snprintf(log_lines_[idx], LINE_LEN, "%s", text);
    log_level_[idx] = level;
    log_head_++;
    if (log_count_ < MAX_LINES) log_count_++;
    scroll_bottom_ = true;
}

void EditorConsole::PushHistory(const char* cmd) {
    if (cmd[0] == '\0') return;
    // Avoid duplicate of last entry
    if (history_count_ > 0 &&
        strcmp(history_[history_count_ - 1], cmd) == 0) return;
    int idx = history_count_ % MAX_HISTORY;
    snprintf(history_[idx], MAX_CMD, "%s", cmd);
    if (history_count_ < MAX_HISTORY) history_count_++;
    history_idx_ = -1;
}

// ── Command executor ──────────────────────────────────────────────────────────
void EditorConsole::ExecCommand(const char* raw) {
    Log(MD_LOG_INFO, raw);
    PushHistory(raw);

    if (lua_mode_) {
        LuaSystem::Get().CallAction(raw, entt::null);
        return;
    }

    // Strip leading '/'
    const char* cmd = (raw[0] == '/') ? raw + 1 : raw;

    if (strncmp(cmd, "help", 4) == 0) {
        Log(MD_LOG_INFO, "/reload  /spawn N x z  /kill N  /save  /load");
        Log(MD_LOG_INFO, "/navmesh  /ai N cmd  /faction a b v  /quest N  /fps  /help");
        return;
    }

    if (strncmp(cmd, "reload", 6) == 0) {
        Log(MD_LOG_INFO, "[Console] Hot-reload: use scene load to refresh data");
        return;
    }

    if (strncmp(cmd, "spawn", 5) == 0) {
        int faction = 1; float cx = 0.f, cz = 0.f;
        sscanf(cmd + 5, "%d %f %f", &faction, &cx, &cz);
        auto& reg = Registry::Get();
        auto e = reg.create();
        auto& tr = reg.emplace<WorldTransform>(e);
        tr.x = cx; tr.y = 0.f; tr.z = cz; tr.rot_y = 0.f;
#ifdef MD_OPENGL43_ENABLED
        tr.slot = TransformSoA::Get().Alloc(e, cx, cz, (uint8_t)faction);
#endif
        auto& ai = reg.emplace<AIAgent>(e);
        ai.faction_id = (uint32_t)faction;
        reg.emplace<Health>(e, Health{100.f, 100.f});
        reg.emplace<Combat>(e, Combat::MakeBandit());
        char msg[64];
        snprintf(msg, sizeof(msg), "[Console] Spawned entity faction=%d at (%.1f,%.1f)", faction, cx, cz);
        Log(MD_LOG_INFO, msg);
        return;
    }

    if (strncmp(cmd, "kill", 4) == 0) {
        uint32_t eid = 0;
        sscanf(cmd + 4, "%u", &eid);
        auto& reg = Registry::Get();
        // find entity by integral id
        for (auto e : reg.storage<entt::entity>()) {
            if ((uint32_t)entt::to_integral(e) == eid) {
                if (reg.all_of<Combat>(e))  reg.get<Combat>(e).is_dead = true;
                if (reg.all_of<Health>(e))  reg.get<Health>(e).current = 0.f;
                char msg[48];
                snprintf(msg, sizeof(msg), "[Console] Killed entity %u", eid);
                Log(MD_LOG_INFO, msg);
                return;
            }
        }
        Log(MD_LOG_WARNING, "[Console] Entity not found");
        return;
    }

    if (strncmp(cmd, "save", 4) == 0) {
        SaveSystem::Get().SaveAsync(SaveSystem::DefaultPath());
        Log(MD_LOG_INFO, "[Console] Save started");
        return;
    }

    if (strncmp(cmd, "load", 4) == 0) {
        SaveSystem::Get().Load(SaveSystem::DefaultPath());
        Log(MD_LOG_INFO, "[Console] Load complete");
        return;
    }

    if (strncmp(cmd, "navmesh", 7) == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "[Console] NavSystem: %s",
                 NavSystem::Get().IsReady() ? "READY" : "NOT READY");
        Log(MD_LOG_INFO, msg);
        return;
    }

    if (strncmp(cmd, "faction", 7) == 0) {
        int a = 0, b = 0, v = 0;
        sscanf(cmd + 7, "%d %d %d", &a, &b, &v);
        FactionSystem::Get().SetRelation((uint32_t)a, (uint32_t)b, (int8_t)v);
        char msg[64];
        snprintf(msg, sizeof(msg), "[Console] Faction %d→%d = %d", a, b, v);
        Log(MD_LOG_INFO, msg);
        return;
    }

    if (strncmp(cmd, "fps", 3) == 0) {
        char msg[64];
        snprintf(msg, sizeof(msg), "[Console] FPS=%.0f  dt=%.2fms",
                 frame_fps_, frame_dt_ms_);
        Log(MD_LOG_INFO, msg);
        return;
    }

    Log(MD_LOG_WARNING, "[Console] Unknown command. Type /help");
}

// ── Draw ──────────────────────────────────────────────────────────────────────
void EditorConsole::Draw() {
    if (!EditorCore::Get().panels_visible[3]) return;

    ImGui::SetNextWindowSize(ImVec2(600, 220), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(285, 500), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Console##EditorConsole", &EditorCore::Get().panels_visible[3])) {
        ImGui::End();
        return;
    }

    // Toolbar
    if (ImGui::SmallButton("Clear##con"))  { log_count_ = 0; log_head_ = 0; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Copy##con")) {
        // Build clipboard text from ring buffer
        char buf[MAX_LINES * 32]; buf[0] = '\0';
        int start = (log_count_ < MAX_LINES) ? 0 : log_head_ % MAX_LINES;
        for (int i = 0; i < log_count_; ++i) {
            int idx = (start + i) % MAX_LINES;
            strncat(buf, log_lines_[idx], sizeof(buf) - strlen(buf) - 2);
            strncat(buf, "\n", sizeof(buf) - strlen(buf) - 1);
        }
        ImGui::SetClipboardText(buf);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.f);
    ImGui::InputText("##con_filter", filter_, sizeof(filter_));
    ImGui::SameLine();
    ImGui::Checkbox("Lua##con", &lua_mode_);
    if (lua_mode_) { ImGui::SameLine(); ImGui::TextColored({0.4f,0.8f,1.f,1.f}, "LUA"); }

    ImGui::Separator();

    // Log region
    float footer_h = ImGui::GetFrameHeightWithSpacing() + 4.f;
    ImGui::BeginChild("##LogScroll", ImVec2(0, -footer_h), false,
                      ImGuiWindowFlags_HorizontalScrollbar);

    int start = (log_count_ < MAX_LINES) ? 0 : log_head_ % MAX_LINES;
    for (int i = 0; i < log_count_; ++i) {
        int idx = (start + i) % MAX_LINES;
        const char* line = log_lines_[idx];
        if (filter_[0] != '\0' && strstr(line, filter_) == nullptr) continue;

        ImVec4 col;
        int lv = log_level_[idx];
        if      (lv == MD_LOG_ERROR) col = {1.f, 0.3f, 0.3f, 1.f};
        else if (lv == MD_LOG_WARNING)                     col = {1.f, 0.8f, 0.2f, 1.f};
        else if (strstr(line, "[Lua]"))                 col = {0.4f, 0.8f, 1.f, 1.f};
        else if (strstr(line, "[NavMesh]") ||
                 strstr(line, "NavSystem"))             col = {0.4f, 1.f, 0.4f, 1.f};
        else if (strstr(line, "[Console]"))             col = {0.8f, 0.8f, 1.f, 1.f};
        else                                            col = {0.7f, 0.7f, 0.7f, 1.f};

        ImGui::PushStyleColor(ImGuiCol_Text, col);
        ImGui::TextUnformatted(line);
        ImGui::PopStyleColor();
    }

    if (scroll_bottom_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.f) {
        ImGui::SetScrollHereY(1.f);
        scroll_bottom_ = false;
    }
    ImGui::EndChild();

    // Input line
    ImGui::Separator();
    bool reclaim = false;
    ImGui::SetNextItemWidth(-60.f);
    ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue
                              | ImGuiInputTextFlags_CallbackHistory;

    struct HistCB {
        static int Callback(ImGuiInputTextCallbackData* data) {
            auto& con = EditorConsole::Get();
            if (data->EventFlag == ImGuiInputTextFlags_CallbackHistory) {
                int prev = con.history_idx_;
                if (data->EventKey == ImGuiKey_UpArrow) {
                    con.history_idx_ = (con.history_idx_ + 1 < con.history_count_)
                                       ? con.history_idx_ + 1 : con.history_idx_;
                } else if (data->EventKey == ImGuiKey_DownArrow) {
                    con.history_idx_ = (con.history_idx_ > 0) ? con.history_idx_ - 1 : -1;
                }
                if (prev != con.history_idx_) {
                    if (con.history_idx_ >= 0) {
                        int hi = (con.history_count_ - 1 - con.history_idx_) % MAX_HISTORY;
                        data->DeleteChars(0, data->BufTextLen);
                        data->InsertChars(0, con.history_[hi]);
                    } else {
                        data->DeleteChars(0, data->BufTextLen);
                    }
                }
            }
            return 0;
        }
    };

    if (ImGui::InputText("##con_input", input_buf_, sizeof(input_buf_),
                         flags, HistCB::Callback)) {
        if (input_buf_[0] != '\0') {
            ExecCommand(input_buf_);
            input_buf_[0] = '\0';
            scroll_bottom_ = true;
        }
        reclaim = true;
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(">>##con") && input_buf_[0] != '\0') {
        ExecCommand(input_buf_);
        input_buf_[0] = '\0';
        reclaim = true;
    }
    if (reclaim) ImGui::SetKeyboardFocusHere(-1);

    ImGui::End();
}
#endif
