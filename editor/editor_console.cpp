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
#include <monkey_dust/editor/cmd_registry.h>
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

    // EDITOR_AUTOMATION_PLAN_v1.md Phase 0/1.1-1.2: temporary passthrough to
    // EditorCmdRegistry with an empty CmdArgs (no arg-string parsing yet) —
    // remove once Phase 1.3.3's real schema-driven `exec <name> [args...]`
    // parser replaces this.
    if (strncmp(cmd, "exec ", 5) == 0) {
        const char* name = cmd + 5;
        while (*name == ' ') ++name;
        CmdArgs args;
        DispatchOutcome outcome = EditorCmdRegistry::Get().Dispatch(name, args, MdRegistry::Get());
        if (!outcome.result.ok) {
            char out[160]; snprintf(out, sizeof(out), "[Console] %s", outcome.result.msg);
            Log(MD_LOG_WARNING, out);
        }
        return;
    }

    // EDITOR_AUTOMATION_PLAN_v1.md Phase 1.3.3 console inventory (required
    // before writing this file's generic `exec` parser above): real
    // ExecCommand branch count is 14 (help/set/get/list/subsystem/
    // reload-shaders/reload/spawn/kill/save/load/navmesh/faction/fps) — the
    // plan's own "current-state facts" undercounted this at 6. Per-command
    // decision (registered vs console-only), recorded here rather than only
    // in a PR description so it stays next to the code it describes:
    //   REGISTERED (now dispatch via EditorCmdRegistry, below):
    //     reload-shaders, spawn, kill, save, load, faction, fps
    //   CONSOLE-ONLY (stay a direct passthrough — decision, not a default):
    //     help       — pure text dump, nothing to script/undo
    //     set/get/list — CVarRegistry is already an adequate mini-registry
    //                    (name+value, generic across all cvars); folding it
    //                    into EditorCmdRegistry would just duplicate it
    //     subsystem  — same reasoning, SubsystemRegistry
    //     reload     — vestigial: prints a hint pointing at reload-shaders,
    //                    not a distinct action (found while doing this
    //                    inventory; not fixed here — findings, not fixes,
    //                    inside a migration commit)
    //     navmesh    — read-only status query; better home is Phase 2.3's
    //                    md.editor.status() extension than a "command" that
    //                    doesn't command anything
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
        CmdArgs args;
        DispatchOutcome out = EditorCmdRegistry::Get().Dispatch("Reload Shaders", args, MdRegistry::Get());
        Log(out.result.ok ? MD_LOG_INFO : MD_LOG_WARNING, out.result.msg);
        return;
    }
    if (strncmp(cmd, "reload", 6) == 0) {
        Log(MD_LOG_INFO, "[Console] /reload-shaders — hot-reload SPIR-V without restart");
        return;
    }

    if (strncmp(cmd, "spawn", 5) == 0) {
        int faction = 1; float cx = 0.f, cz = 0.f;
        sscanf(cmd + 5, "%d %f %f", &faction, &cx, &cz);
        CmdArgs args; args.count = 3;
        args.values[0].type = CmdArgType::I64; args.values[0].i64 = faction;
        args.values[1].type = CmdArgType::F64; args.values[1].f64 = cx;
        args.values[2].type = CmdArgType::F64; args.values[2].f64 = cz;
        DispatchOutcome out = EditorCmdRegistry::Get().Dispatch("Spawn Entity", args, MdRegistry::Get());
        Log(out.result.ok ? MD_LOG_INFO : MD_LOG_WARNING, out.result.msg);
        return;
    }

    if (strncmp(cmd, "kill", 4) == 0) {
        uint32_t eid = 0;
        sscanf(cmd + 4, "%u", &eid);
        CmdArgs args; args.count = 1;
        args.values[0].type = CmdArgType::Entity; args.values[0].entity_id = eid;
        DispatchOutcome out = EditorCmdRegistry::Get().Dispatch("Kill Entity", args, MdRegistry::Get());
        Log(out.result.ok ? MD_LOG_INFO : MD_LOG_WARNING, out.result.msg);
        return;
    }

    if (strncmp(cmd, "save", 4) == 0) {
        CmdArgs args;
        DispatchOutcome out = EditorCmdRegistry::Get().Dispatch("Save Game", args, MdRegistry::Get());
        Log(out.result.ok ? MD_LOG_INFO : MD_LOG_WARNING, out.result.msg);
        return;
    }

    if (strncmp(cmd, "load", 4) == 0) {
        CmdArgs args;
        DispatchOutcome out = EditorCmdRegistry::Get().Dispatch("Load Game", args, MdRegistry::Get());
        Log(out.result.ok ? MD_LOG_INFO : MD_LOG_WARNING, out.result.msg);
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
        CmdArgs args; args.count = 3;
        args.values[0].type = CmdArgType::I64; args.values[0].i64 = a;
        args.values[1].type = CmdArgType::I64; args.values[1].i64 = b;
        args.values[2].type = CmdArgType::I64; args.values[2].i64 = v;
        DispatchOutcome out = EditorCmdRegistry::Get().Dispatch("Set Faction Relation", args, MdRegistry::Get());
        Log(out.result.ok ? MD_LOG_INFO : MD_LOG_WARNING, out.result.msg);
        return;
    }

    if (strncmp(cmd, "fps", 3) == 0) {
        CmdArgs args;
        DispatchOutcome out = EditorCmdRegistry::Get().Dispatch("FPS Query", args, MdRegistry::Get());
        Log(out.result.ok ? MD_LOG_INFO : MD_LOG_WARNING, out.result.msg);
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
