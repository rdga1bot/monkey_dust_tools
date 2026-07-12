#ifdef MONKEY_DUST_EDITOR
#include "editor_console.h"
#include "editor_core.h"
#include <string>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/scripting/lua_system.h>
#include <monkey_dust/save/save_system.h>
#include <monkey_dust/nav/nav_system.h>
#include <monkey_dust/world/faction_system.h>
#include <monkey_dust/platform/md_log.h>
#include <monkey_dust/platform/cvar_registry.h>
#include <monkey_dust/tools/graphics_settings.h>
#include <monkey_dust/core/subsystem_registry.h>
#include <cstdio>
#include <cstring>

// ARCHITECTURE_IDEAS.md #1 — CVar registry init. Binds console-settable names
// to the LIVE GraphicsSettings fields already read every frame by the
// renderer, so `set fog_far 5000` takes effect with no extra glue code.
// Registration is idempotent (CVarRegistry::Register overwrites by name),
// so calling this more than once (e.g. hot-reload) is harmless.
static void s_register_cvars() {
    auto& gs = GraphicsSettings::Get();
    CVarRegistry::Get().RegisterFloat("fog_near", &gs.fog_near);
    CVarRegistry::Get().RegisterFloat("fog_far",  &gs.fog_far);
    CVarRegistry::Get().RegisterFloat("shadow_distance", &gs.shadow_distance);
    CVarRegistry::Get().RegisterFloat("resolution_scale", &gs.resolution_scale);
    CVarRegistry::Get().RegisterFloat("ibl_intensity", &gs.ibl_intensity);
    CVarRegistry::Get().RegisterBool("ssao_enabled", &gs.ssao_enabled);
    CVarRegistry::Get().RegisterBool("smaa_enabled", &gs.smaa_enabled);
}

void EditorConsole::Init() {
    MdLogSetHook([](int level, const char* msg) {
        EditorConsole::Get().Log(level, msg);
    });
    s_register_cvars();
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorConsole::Log(int level, const char* text) {
    int idx = log_head_ % MAX_LINES;
    snprintf(log_lines_[idx], LINE_LEN, "%s", text);
    log_level_[idx] = level;
    log_head_++;
    if (log_count_ < MAX_LINES) log_count_++;
    scroll_bottom_ = true;
    if (strstr(text, "[Lua]")) lua_editor_dirty_ = true;
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
        LuaSystem::Get().CallAction(raw, MdEntity::Null());
        return;
    }

    // Strip leading '/'
    const char* cmd = (raw[0] == '/') ? raw + 1 : raw;

    if (strncmp(cmd, "help", 4) == 0) {
        Log(MD_LOG_INFO, "/reload-shaders — hot-reload SPIR-V without restart");
        Log(MD_LOG_INFO, "/spawn N x z  /kill N  /save  /load  /navmesh  /fps");
        Log(MD_LOG_INFO, "/ai N cmd  /faction a b v  /quest N  /help");
        Log(MD_LOG_INFO, "/set <cvar> <value>  /get <cvar>  /list — live-tunable params (ARCHITECTURE_IDEAS.md #1)");
        Log(MD_LOG_INFO, "/subsystem <name> on|off  (no args = list) — toggle a logic_tick.cpp Tick* (ARCH #5)");
        return;
    }

    // ARCHITECTURE_IDEAS.md #1 — CVar registry commands.
    if (strncmp(cmd, "set", 3) == 0) {
        char name[CVarRegistry::NAME_LEN] = {}, value[32] = {};
        if (sscanf(cmd + 3, "%47s %31s", name, value) == 2) {
            if (CVarRegistry::Get().Set(name, value)) {
                char msg[96];
                CVarRegistry::Get().Format(name, msg, sizeof(msg));
                char out[128]; snprintf(out, sizeof(out), "[Console] %s", msg);
                Log(MD_LOG_INFO, out);
            } else {
                Log(MD_LOG_WARNING, "[Console] Unknown cvar. /list to see all");
            }
        } else {
            Log(MD_LOG_WARNING, "[Console] Usage: /set <cvar> <value>");
        }
        return;
    }

    if (strncmp(cmd, "get", 3) == 0) {
        char name[CVarRegistry::NAME_LEN] = {};
        if (sscanf(cmd + 3, "%47s", name) == 1) {
            char msg[96];
            if (CVarRegistry::Get().Format(name, msg, sizeof(msg))) {
                char out[128]; snprintf(out, sizeof(out), "[Console] %s", msg);
                Log(MD_LOG_INFO, out);
            } else {
                Log(MD_LOG_WARNING, "[Console] Unknown cvar. /list to see all");
            }
        } else {
            Log(MD_LOG_WARNING, "[Console] Usage: /get <cvar>");
        }
        return;
    }

    if (strncmp(cmd, "list", 4) == 0) {
        char msg[96];
        for (int i = 0; i < CVarRegistry::Get().Count(); ++i) {
            if (CVarRegistry::Get().FormatAt(i, msg, sizeof(msg))) Log(MD_LOG_INFO, msg);
        }
        return;
    }

    // ARCHITECTURE_IDEAS.md #5 — toggle a logic_tick.cpp subsystem on/off.
    if (strncmp(cmd, "subsystem", 9) == 0) {
        char name[SubsystemRegistry::NAME_LEN] = {}, state[8] = {};
        if (sscanf(cmd + 9, "%31s %7s", name, state) == 2) {
            bool on = (strcmp(state, "on") == 0 || strcmp(state, "1") == 0);
            SubsystemRegistry::Get().SetEnabled(name, on);
            char out[80]; snprintf(out, sizeof(out), "[Console] %s = %s", name, on ? "on" : "off");
            Log(MD_LOG_INFO, out);
        } else {
            char msg[64];
            Log(MD_LOG_INFO, "[Console] Registered subsystems:");
            for (int i = 0; i < SubsystemRegistry::Get().Count(); ++i) {
                snprintf(msg, sizeof(msg), "  %s = %s", SubsystemRegistry::Get().NameAt(i),
                         SubsystemRegistry::Get().EnabledAt(i) ? "on" : "off");
                Log(MD_LOG_INFO, msg);
            }
            Log(MD_LOG_WARNING, "[Console] Usage: /subsystem <name> on|off");
        }
        return;
    }

    if (strncmp(cmd, "reload-shaders", 14) == 0) {
        Log(MD_LOG_INFO, "[Shaders] Compiling SPIR-V (blocking ~3s)...");
        int ret = system("bash scripts/compile_shaders.sh 2>&1 | tail -3");
        if (ret == 0) {
            extern void EditorReloadAllShaderPipelines();
            EditorReloadAllShaderPipelines();
            Log(MD_LOG_INFO, "[Shaders] Reloaded OK — restart not required");
        } else {
            Log(MD_LOG_WARNING, "[Shaders] Compilation FAILED — check compile_shaders.sh output");
        }
        return;
    }
    if (strncmp(cmd, "reload", 6) == 0) {
        Log(MD_LOG_INFO, "[Console] /reload-shaders — hot-reload SPIR-V without restart");
        return;
    }

    if (strncmp(cmd, "spawn", 5) == 0) {
        int faction = 1; float cx = 0.f, cz = 0.f;
        sscanf(cmd + 5, "%d %f %f", &faction, &cx, &cz);
        auto& reg = MdRegistry::Get();
        auto e = reg.Create();
        reg.Handle(e).emplace<WorldTransform>();
        auto& tr = reg.Get<WorldTransform>(e);
        tr.x = cx; tr.y = 0.f; tr.z = cz; tr.rot_y = 0.f;
        reg.Handle(e).emplace<AIAgent>();
        auto& ai = reg.Get<AIAgent>(e);
        ai.faction_id = (uint32_t)faction;
        reg.Handle(e).emplace<Health>(Health{100.f, 100.f});
        reg.Handle(e).emplace<Combat>(Combat::MakeBandit());
        char msg[64];
        snprintf(msg, sizeof(msg), "[Console] Spawned entity faction=%d at (%.1f,%.1f)", faction, cx, cz);
        Log(MD_LOG_INFO, msg);
        return;
    }

    if (strncmp(cmd, "kill", 4) == 0) {
        uint32_t eid = 0;
        sscanf(cmd + 4, "%u", &eid);
        auto& reg = MdRegistry::Get();
        // find entity by integral id
        bool found = false;
        reg.Each([&](MdEntity e) -> bool {
            if (e.ToIntegral() != eid) return true;
            found = true;
            if (reg.AllOf<Combat>(e))  reg.Get<Combat>(e).is_dead = true;
            if (reg.AllOf<Health>(e)) { auto& h = reg.Get<Health>(e); for (int _i=0;_i<LIMB_COUNT;++_i) h.hp[_i]=0.f; }
            return false;  // found — stop
        });
        if (found) {
            char msg[48];
            snprintf(msg, sizeof(msg), "[Console] Killed entity %u", eid);
            Log(MD_LOG_INFO, msg);
        } else {
            Log(MD_LOG_WARNING, "[Console] Entity not found");
        }
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
    DrawContent();
    ImGui::End();
}

void EditorConsole::DrawContent() {

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

    float footer_h = ImGui::GetFrameHeightWithSpacing() + 4.f;

    if (lua_mode_) {
        // ── Lua mode: syntax-highlighted view of [Lua] log lines ──────────
        if (!lua_editor_init_) {
            lua_editor_.SetLanguageDefinition(TextEditor::LanguageDefinition::Lua());
            lua_editor_.SetReadOnly(true);
            lua_editor_init_ = true;
            lua_editor_dirty_ = true;
        }
        if (lua_editor_dirty_) {
            std::string text;
            int start = (log_count_ < MAX_LINES) ? 0 : log_head_ % MAX_LINES;
            for (int i = 0; i < log_count_; ++i) {
                int idx = (start + i) % MAX_LINES;
                if (strstr(log_lines_[idx], "[Lua]")) {
                    text += log_lines_[idx];
                    text += '\n';
                }
            }
            lua_editor_.SetText(text);
            lua_editor_dirty_ = false;
        }
        lua_editor_.Render("##luaed", ImVec2(0, -footer_h), false);
    } else {
        // ── Normal log scroll region ───────────────────────────────────────
        ImGui::BeginChild("##LogScroll", ImVec2(0, -footer_h), false,
                          ImGuiWindowFlags_HorizontalScrollbar);

        int start = (log_count_ < MAX_LINES) ? 0 : log_head_ % MAX_LINES;
        for (int i = 0; i < log_count_; ++i) {
            int idx = (start + i) % MAX_LINES;
            const char* line = log_lines_[idx];
            if (filter_[0] != '\0' && strstr(line, filter_) == nullptr) continue;

            ImVec4 col;
            int lv = log_level_[idx];
            if      (lv == MD_LOG_ERROR)           col = {1.f, 0.3f, 0.3f, 1.f};
            else if (lv == MD_LOG_WARNING)         col = {1.f, 0.8f, 0.2f, 1.f};
            else if (strstr(line, "[Lua]"))        col = {0.4f, 0.8f, 1.f, 1.f};
            else if (strstr(line, "[NavMesh]") ||
                     strstr(line, "NavSystem"))    col = {0.4f, 1.f, 0.4f, 1.f};
            else if (strstr(line, "[Console]"))    col = {0.8f, 0.8f, 1.f, 1.f};
            else                                   col = {0.7f, 0.7f, 0.7f, 1.f};

            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::TextUnformatted(line);
            ImGui::PopStyleColor();
        }

        if (scroll_bottom_ && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.f) {
            ImGui::SetScrollHereY(1.f);
            scroll_bottom_ = false;
        }
        ImGui::EndChild();
    }

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

}
#endif
