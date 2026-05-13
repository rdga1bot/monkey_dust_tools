#ifdef MONKEY_DUST_EDITOR
#include "editor_command_palette.h"
#include "editor_toolbar.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/components/renderable.h>
#include <monkey_dust/nav/nav_system.h>
#include <monkey_dust/save/save_system.h>
#include <monkey_dust/tools/debug_system.h>
#include <monkey_dust/platform/md_log.h>
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
namespace {

struct PaletteCmd {
    const char* name;
    const char* hint;
    void(*action)();
};

static const PaletteCmd kCommands[] = {
    // Gizmo
    { "Gizmo: Translate",        "W",        []{ EditorCore::Get().gizmo_op = EditorGizmoOp::TRANSLATE; } },
    { "Gizmo: Rotate",           "E",        []{ EditorCore::Get().gizmo_op = EditorGizmoOp::ROTATE;    } },
    { "Gizmo: Scale",            "R",        []{ EditorCore::Get().gizmo_op = EditorGizmoOp::SCALE;     } },
    { "Gizmo: Toggle World/Local","G",        []{ auto& ec = EditorCore::Get();
                                                    ec.gizmo_space = (ec.gizmo_space == EditorGizmoSpace::WORLD)
                                                        ? EditorGizmoSpace::LOCAL : EditorGizmoSpace::WORLD; } },
    // Camera
    { "Camera: Focus on Selection","F",       []{ EditorCore::Get().FocusOnSelected(); } },
    // Selection
    { "Select All",              "Ctrl+A",    []{
        auto& ec = EditorCore::Get(); auto& reg = Registry::Get();
        ec.DeselectAll();
        for (auto e : reg.storage<entt::entity>())
            if (ec.selected_count < EditorCore::MAX_SELECTED)
                ec.selected[ec.selected_count++] = e;
    }},
    { "Deselect All",            "",          []{ EditorCore::Get().DeselectAll(); } },
    { "Delete Selected",         "Del",       []{
        auto& ec = EditorCore::Get(); auto& reg = Registry::Get();
        for (int i = ec.selected_count - 1; i >= 0; --i) {
            entt::entity e = ec.selected[i];
            if (!reg.valid(e)) continue;
            if (reg.all_of<WorldTransform>(e)) TransformSoA::Get().Free(e);
            reg.destroy(e);
        }
        ec.DeselectAll();
    }},
    // Edit
    { "Edit: Undo",              "Ctrl+Z",    []{ EditorCore::Get().Undo(); } },
    { "Edit: Redo",              "Ctrl+Y",    []{ EditorCore::Get().Redo(); } },
    // Spawn
    { "Spawn: Empty Transform",  "",          []{ EditorToolbar::Get().SpawnEntity("Transform");  } },
    { "Spawn: NPC Bandit",       "",          []{ EditorToolbar::Get().SpawnEntity("NPC Bandit"); } },
    { "Spawn: NPC Trader",       "",          []{ EditorToolbar::Get().SpawnEntity("NPC Trader"); } },
    { "Spawn: NPC Holy",         "",          []{ EditorToolbar::Get().SpawnEntity("NPC Holy");   } },
    // Scene
    { "Scene: Rebuild NavMesh",  "",          []{
        Vec3 t = EditorCore::Get().cam_target;
        NavSystem::Get().EnqueueRebuild(t.x, t.z, nullptr, 0, nullptr, 0);
        MD_LOG(MD_LOG_INFO, "[Editor] NavMesh rebuild enqueued");
    }},
    { "Scene: Save Game",        "F5",        []{
        SaveSystem::Get().SaveAsync(SaveSystem::DefaultPath());
        MD_LOG(MD_LOG_INFO, "[Editor] Async save");
    }},
    { "Scene: Load Game",        "F9",        []{
        SaveSystem::Get().Load(SaveSystem::DefaultPath());
        MD_LOG(MD_LOG_INFO, "[Editor] Load");
    }},
    // Physics
    { "Physics: Toggle Pause",   "",          []{ EditorCore::Get().physics_paused ^= true; } },
    // Debug
    { "Debug: Toggle Overlay",   "F1",        []{ DebugSystem::Get().overlay_on ^= true; } },
    { "Debug: Toggle Grid",      "F2",        []{ DebugSystem::Get().grid_on ^= true;     } },
    { "Debug: Toggle NavMesh",   "",          []{ DebugSystem::Get().navmesh_on ^= true;  } },
    // Panels
    { "Panel: Hierarchy",        "",          []{ EditorCore::Get().panels_visible[0] ^= true; } },
    { "Panel: Inspector",        "",          []{ EditorCore::Get().panels_visible[1] ^= true; } },
    { "Panel: Assets",           "",          []{ EditorCore::Get().panels_visible[2] ^= true; } },
    { "Panel: Console",          "",          []{ EditorCore::Get().panels_visible[3] ^= true; } },
    { "Panel: Graphics",         "",          []{ EditorCore::Get().panels_visible[4] ^= true; } },
    { "Panel: Camera",           "",          []{ EditorCore::Get().panels_visible[5] ^= true; } },
    { "Panel: Animation",        "",          []{ EditorCore::Get().panels_visible[6] ^= true; } },
    { "Panel: Reset Layout",     "",          []{
        auto& pv = EditorCore::Get().panels_visible;
        for (int i = 0; i < 8; ++i) pv[i] = (i < 6);
    }},
    // Exit
    { "Exit Editor",             "",          []{ EditorCore::Get().editor_open = false; } },
};

static constexpr int kCommandCount = (int)(sizeof(kCommands) / sizeof(kCommands[0]));

} // namespace

// Returns 0 = no match; >0 = score (higher = better match).
// Case-insensitive subsequence match with consecutive and word-boundary bonuses.
static int FuzzyScore(const char* pattern, const char* str) {
    if (!pattern[0]) return 1;
    int score = 0, pi = 0, prev_si = -2;
    for (int si = 0; str[si] && pattern[pi]; ++si) {
        if ((str[si] | 0x20) == (pattern[pi] | 0x20)) {
            score += 1;
            if (si == prev_si + 1)                              score += 4;  // consecutive
            if (si == 0 || str[si-1] == ' ' || str[si-1] == ':') score += 2; // word start
            prev_si = si;
            ++pi;
        }
    }
    return (pattern[pi] == '\0') ? score : 0;
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorCommandPalette::Open() {
    open_        = true;
    just_opened_ = true;
    selected_    = 0;
    filter_[0]   = '\0';
}

// ─────────────────────────────────────────────────────────────────────────────
void EditorCommandPalette::Draw() {
    // Ctrl+P opens palette regardless of keyboard capture state.
    if ((ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) &&
        ImGui::IsKeyPressed(ImGuiKey_P, false))
    {
        if (!open_) Open(); else open_ = false;
    }

    if (!open_) return;

    // Center modal.
    ImVec2 display = ImGui::GetIO().DisplaySize;
    ImGui::SetNextWindowPos(ImVec2(display.x * 0.5f, display.y * 0.25f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSize(ImVec2(480, 0), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.96f);

    ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoDecoration  | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;

    if (!ImGui::Begin("##CmdPalette", nullptr, flags)) { ImGui::End(); return; }

    // Auto-focus input on first open.
    if (just_opened_) {
        ImGui::SetKeyboardFocusHere();
        just_opened_ = false;
    }

    ImGui::SetNextItemWidth(-1);
    bool changed = ImGui::InputText("##CmdFilter", filter_, sizeof(filter_));
    (void)changed;

    // Escape closes.
    if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        open_ = false;
        ImGui::End();
        return;
    }

    // Build filtered list with fuzzy scoring, sorted best-first (no allocation).
    struct VEntry { const PaletteCmd* cmd; int score; };
    static VEntry visible[kCommandCount];
    int vcount = 0;
    for (int i = 0; i < kCommandCount; ++i) {
        int s = FuzzyScore(filter_, kCommands[i].name);
        if (s > 0) visible[vcount++] = { &kCommands[i], s };
    }
    // Insertion sort by score descending (≤30 items, no std::sort needed).
    for (int i = 1; i < vcount; ++i) {
        VEntry key = visible[i];
        int j = i - 1;
        while (j >= 0 && visible[j].score < key.score) { visible[j+1] = visible[j]; --j; }
        visible[j+1] = key;
    }

    // Clamp selection.
    if (selected_ >= vcount) selected_ = (vcount > 0) ? vcount - 1 : 0;

    // Keyboard navigation.
    if (ImGui::IsKeyPressed(ImGuiKey_DownArrow, true))  { if (selected_ < vcount - 1) ++selected_; }
    if (ImGui::IsKeyPressed(ImGuiKey_UpArrow,   true))  { if (selected_ > 0)           --selected_; }

    bool execute = ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                   ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);

    ImGui::Separator();
    ImGui::BeginChild("##CmdList", ImVec2(0, (float)(vcount < 12 ? vcount : 12) * 22.f + 4.f), false);

    for (int i = 0; i < vcount; ++i) {
        const PaletteCmd* c = visible[i].cmd;
        bool sel = (i == selected_);
        if (ImGui::Selectable(c->name, sel, ImGuiSelectableFlags_AllowDoubleClick)) {
            selected_ = i;
            if (ImGui::IsMouseDoubleClicked(0)) execute = true;
        }
        if (ImGui::IsItemHovered()) selected_ = i;
        if (c->hint[0] != '\0') {
            ImGui::SameLine(ImGui::GetWindowWidth() - 80.f);
            ImGui::TextDisabled("%s", c->hint);
        }
        if (sel) ImGui::SetScrollHereY(0.5f);
    }

    ImGui::EndChild();

    if (execute && vcount > 0) {
        visible[selected_].cmd->action();
        open_ = false;
    }

    ImGui::End();
}
#endif
