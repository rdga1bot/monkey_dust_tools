#ifdef MONKEY_DUST_EDITOR
#include "editor_std_commands.h"
#include "editor_core.h"
#include "editor_command_palette.h"
#include "editor_reflect_bridge.h"
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/components/renderable.h>
#include <monkey_dust/world/faction_system.h>
#include <monkey_dust/nav/nav_system.h>
#include <monkey_dust/save/save_system.h>
#include <monkey_dust/platform/md_log.h>
#include "scene_serializer.h"
#include <monkey_dust/editor/cmd_path_validate.h>
#include <flecs.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

CmdResult DuplicateSelectedCmd(const CmdArgs&, MdRegistry& reg, uint8_t[64]) {
    auto& ec = EditorCore::Get();
    MdEntity src = ec.GetPrimary();
    if (!reg.Valid(src) || !reg.Handle(src).has<WorldTransform>()) {
        CmdResult r; r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "No valid primary selection to duplicate");
        return r;
    }
    MdEntity dst = reg.Create();
    reg.Handle(dst).emplace<WorldTransform>(reg.Handle(src).get_mut<WorldTransform>());
    auto& tr = reg.Handle(dst).get_mut<WorldTransform>();
    tr.x += 1.f; tr.slot = 0xFFFFFFFFu;
    uint32_t fid = reg.Handle(src).has<AIAgent>() ? reg.Handle(src).get_mut<AIAgent>().faction_id : 0u;
    tr.slot = TransformSoA::Get().Alloc(dst, tr.x, tr.z, (uint8_t)fid);
    if (reg.Handle(src).has<Health>())     reg.Handle(dst).emplace<Health>(reg.Handle(src).get_mut<Health>());
    if (reg.Handle(src).has<AIAgent>())    reg.Handle(dst).emplace<AIAgent>(reg.Handle(src).get_mut<AIAgent>());
    if (reg.Handle(src).has<Renderable>()) reg.Handle(dst).emplace<Renderable>(reg.Handle(src).get_mut<Renderable>());
    ec.Select(dst);
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "Duplicated entity %u -> %u", src.ToIntegral(), dst.ToIntegral());
    return r;
}

CmdResult SelectAllCmd(const CmdArgs&, MdRegistry& reg, uint8_t[64]) {
    auto& ec = EditorCore::Get();
    ec.DeselectAll();
    static auto q_all = reg.Raw().query<MdManagedTag>();
    int n = 0;
    q_all.each([&](flecs::entity fe, MdManagedTag) {
        if (ec.selected_count >= EditorCore::MAX_SELECTED) return;
        ec.selected[ec.selected_count++] = MdEntity(fe.id());
        ++n;
    });
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "Selected %d entities", n);
    return r;
}

CmdResult NewSceneCmd(const CmdArgs&, MdRegistry& reg, uint8_t[64]) {
    EditorCore::Get().DeselectAll();
    reg.Clear();
    TransformSoA::Get().Init();
    MD_LOG(MD_LOG_INFO, "[Editor] New scene");
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "New scene");
    return r;
}

// Phase 2.4 retrofit: Import/Export Scene took an unvalidated path
// straight from Phase 1.3 — found while implementing CmdPathValidate
// (Phase 2's own path-allowlist requirement), not fixed at the time to
// keep that migration commit scoped. Closed here, not silently left open.
CmdResult ImportSceneCmd(const CmdArgs& args, MdRegistry&, uint8_t[64]) {
    const char* path = args.values[0].str;
    CmdResult r;
    if (!CmdPathValidate(path)) {
        r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "Path rejected (outside data/, game/data/, automation_out/): %s", path);
        return r;
    }
    bool ok = SceneSerializer::Import(path);
    r.ok = ok;
    snprintf(r.msg, sizeof(r.msg), ok ? "Imported %s" : "Import failed: %s", path);
    return r;
}

CmdResult ExportSceneCmd(const CmdArgs& args, MdRegistry&, uint8_t[64]) {
    const char* path = args.values[0].str;
    CmdResult r;
    if (!CmdPathValidate(path)) {
        r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "Path rejected (outside data/, game/data/, automation_out/): %s", path);
        return r;
    }
    bool ok = SceneSerializer::Export(path);
    r.ok = ok;
    snprintf(r.msg, sizeof(r.msg), ok ? "Exported %s" : "Export failed: %s", path);
    return r;
}

CmdResult SaveGameCmd(const CmdArgs&, MdRegistry&, uint8_t[64]) {
    SaveSystem::Get().SaveAsync(SaveSystem::DefaultPath());
    MD_LOG(MD_LOG_INFO, "[Editor] Async save");
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "Save started");
    return r;
}

CmdResult LoadGameCmd(const CmdArgs&, MdRegistry&, uint8_t[64]) {
    SaveSystem::Get().Load(SaveSystem::DefaultPath());
    MD_LOG(MD_LOG_INFO, "[Editor] Load");
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "Load complete");
    return r;
}

CmdResult ReloadShadersCmd(const CmdArgs&, MdRegistry&, uint8_t[64]) {
    int ret = system("bash scripts/compile_shaders.sh 2>&1 | tail -3");
    CmdResult r;
    if (ret == 0) {
        extern void EditorReloadAllShaderPipelines();
        EditorReloadAllShaderPipelines();
        r.ok = true;
        snprintf(r.msg, sizeof(r.msg), "Shaders reloaded OK");
    } else {
        r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "Shader compilation FAILED");
    }
    return r;
}

CmdResult RebuildNavMeshCmd(const CmdArgs&, MdRegistry&, uint8_t[64]) {
    Vec3 t = EditorCore::Get().cam_target;
    NavSystem::Get().EnqueueRebuild(t.x, t.z, nullptr, 0, nullptr, 0);
    MD_LOG(MD_LOG_INFO, "[Editor] NavMesh rebuild enqueued");
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "NavMesh rebuild enqueued at (%.0f,%.0f)", t.x, t.z);
    return r;
}

// args: [0]=faction(i64), [1]=x(f64), [2]=z(f64)
CmdResult SpawnEntityCmd(const CmdArgs& args, MdRegistry& reg, uint8_t[64]) {
    int64_t faction = args.values[0].i64;
    float cx = (float)args.values[1].f64;
    float cz = (float)args.values[2].f64;
    auto e = reg.Create();
    reg.Handle(e).emplace<WorldTransform>();
    auto& tr = reg.Handle(e).get_mut<WorldTransform>();
    tr.x = cx; tr.y = 0.f; tr.z = cz; tr.rot_y = 0.f;
    reg.Handle(e).emplace<AIAgent>();
    auto& ai = reg.Handle(e).get_mut<AIAgent>();
    ai.faction_id = (uint32_t)faction;
    reg.Handle(e).emplace<Health>(Health{100.f, 100.f});
    reg.Handle(e).emplace<Combat>(Combat::MakeBandit());
    // Phase 3 (EDITOR_AUTOMATION_PLAN_v1.md agent authoring layer): select
    // the new entity so md.editor.selection()[1] gives its id right after
    // exec — the authoring library (scripts/authoring/) needs the spawned
    // id to Add Component onto it (e.g. nav_agent), and there was no other
    // way to get it back through CmdResult's plain msg string. Matches
    // DuplicateSelectedCmd's existing ec.Select(dst) precedent — spawning
    // commands selecting their result is already this codebase's norm, not
    // a new convention invented for this.
    EditorCore::Get().Select(e);
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "Spawned entity faction=%lld at (%.1f,%.1f)", (long long)faction, cx, cz);
    return r;
}

// args: [0]=entity_id(entity)
CmdResult KillEntityCmd(const CmdArgs& args, MdRegistry& reg, uint8_t[64]) {
    uint32_t eid = (uint32_t)args.values[0].entity_id;
    MdEntity e = reg.FromIndex(eid);
    bool found = reg.Valid(e) && e.ToIntegral() == eid;
    if (found) {
        if (reg.Handle(e).has<Combat>()) reg.Handle(e).get_mut<Combat>().is_dead = true;
        if (reg.Handle(e).has<Health>()) {
            auto& h = reg.Handle(e).get_mut<Health>();
            for (int i = 0; i < LIMB_COUNT; ++i) h.hp[i] = 0.f;
        }
    }
    CmdResult r; r.ok = found;
    snprintf(r.msg, sizeof(r.msg), found ? "Killed entity %u" : "Entity %u not found", eid);
    return r;
}

// The registry's first genuinely undo-capable command (Phase 2): small,
// safe payload (2 uint32 ids + 2 int8 values, well under 64 bytes) — proves
// the mechanism end to end (registry-mediated capture -> tools/'s
// CommandStack push -> real Ctrl+Z) without the much bigger scope of
// capturing full entity+component state (Delete/Duplicate/Spawn/Kill stay
// NO_UNDO; see editor_std_commands.h's file header for why).
struct FactionRelationUndoData {
    uint32_t a;
    uint32_t b;
    int8_t   prev_v;
    int8_t   new_v;
};

// args: [0]=a(i64), [1]=b(i64), [2]=v(i64)
CmdResult SetFactionRelationCmd(const CmdArgs& args, MdRegistry&, uint8_t undo_data[64]) {
    uint32_t a = (uint32_t)args.values[0].i64;
    uint32_t b = (uint32_t)args.values[1].i64;
    int8_t   v = (int8_t)args.values[2].i64;

    FactionRelationUndoData ud;
    ud.a = a; ud.b = b;
    ud.prev_v = FactionSystem::Get().GetRelation(a, b);
    ud.new_v  = v;
    memcpy(undo_data, &ud, sizeof(ud));

    FactionSystem::Get().SetRelation(a, b, v);
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "Faction %u->%u = %d", a, b, (int)v);
    return r;
}

void UndoSetFactionRelation(void* data, MdRegistry&) {
    FactionRelationUndoData ud;
    memcpy(&ud, data, sizeof(ud));
    FactionSystem::Get().SetRelation(ud.a, ud.b, ud.prev_v);
}

void RedoSetFactionRelation(void* data, MdRegistry&) {
    FactionRelationUndoData ud;
    memcpy(&ud, data, sizeof(ud));
    FactionSystem::Get().SetRelation(ud.a, ud.b, ud.new_v);
}

CmdResult FpsQueryCmd(const CmdArgs&, MdRegistry&, uint8_t[64]) {
    auto& ec = EditorCore::Get();
    CmdResult r;
    snprintf(r.msg, sizeof(r.msg), "FPS=%.0f dt=%.2fms", ec.frame_fps, ec.frame_dt_ms);
    return r;
}

// Phase 3 (EDITOR_AUTOMATION_PLAN_v1.md agent authoring layer): named
// camera positions so an authoring script can jump to a consistent framing
// before md.editor.capture() — referenced directly in the plan's own text
// ("registered camera bookmarks (md.editor.exec('Camera Bookmark', ...)")
// but that single-command dual-purpose (save-or-goto) design doesn't fit
// CmdSchema's fixed-required-args shape cleanly, so this is two commands
// instead: Save/Goto. Fixed, no-heap slot table (MAX_BOOKMARKS), matching
// this project's array-not-container convention — oldest-name-wins is not
// implemented (a full slot table is a real, if unlikely, limitation for a
// script bookmarking more than MAX_BOOKMARKS distinct names).
namespace {
constexpr int MAX_BOOKMARKS = 16;
struct CameraBookmark {
    char name[32] = {};
    float x = 0.f, y = 0.f, z = 0.f, yaw = 0.f, pitch = 0.f;
    bool used = false;
};
CameraBookmark s_bookmarks[MAX_BOOKMARKS];

CameraBookmark* FindBookmark(const char* name) {
    for (int i = 0; i < MAX_BOOKMARKS; ++i) {
        if (s_bookmarks[i].used && strncmp(s_bookmarks[i].name, name, sizeof(s_bookmarks[i].name)) == 0) {
            return &s_bookmarks[i];
        }
    }
    return nullptr;
}
} // namespace

// args: [0]=name(str)
CmdResult SaveCameraBookmarkCmd(const CmdArgs& args, MdRegistry&, uint8_t[64]) {
    const char* name = args.values[0].str;
    CameraBookmark* bm = FindBookmark(name);
    if (!bm) {
        for (int i = 0; i < MAX_BOOKMARKS; ++i) {
            if (!s_bookmarks[i].used) { bm = &s_bookmarks[i]; break; }
        }
    }
    CmdResult r;
    if (!bm) {
        r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "No free bookmark slots (max %d)", MAX_BOOKMARKS);
        return r;
    }
    auto& ec = EditorCore::Get();
    strncpy(bm->name, name, sizeof(bm->name) - 1);
    bm->name[sizeof(bm->name) - 1] = '\0';
    bm->x = ec.cam_target.x; bm->y = ec.cam_target.y; bm->z = ec.cam_target.z;
    bm->yaw = ec.cam_yaw; bm->pitch = ec.cam_pitch;
    bm->used = true;
    snprintf(r.msg, sizeof(r.msg), "Saved camera bookmark '%s'", name);
    return r;
}

// args: [0]=name(str)
CmdResult GotoCameraBookmarkCmd(const CmdArgs& args, MdRegistry&, uint8_t[64]) {
    const char* name = args.values[0].str;
    CameraBookmark* bm = FindBookmark(name);
    CmdResult r;
    if (!bm) {
        r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "Unknown camera bookmark: %s", name);
        return r;
    }
    auto& ec = EditorCore::Get();
    ec.cam_target.x = bm->x;
    ec.cam_target.y = bm->y;
    ec.cam_target.z = bm->z;
    ec.cam_yaw = bm->yaw;
    ec.cam_pitch = bm->pitch;
    snprintf(r.msg, sizeof(r.msg), "Moved camera to bookmark '%s'", name);
    return r;
}

// args: [0]=entity_id(entity), [1]=component_name(str) — matches
// EcsReflectBridge::Desc(i).name (snake_case reflect name, not the
// PascalCase flecs type name used internally for ecs_lookup()).
CmdResult AddComponentCmd(const CmdArgs& args, MdRegistry& reg, uint8_t[64]) {
    ecs_world_t* world = reg.Raw().c_ptr();
    ecs_entity_t eid = (ecs_entity_t)args.values[0].entity_id;
    CmdResult r;
    if (!eid || !ecs_is_alive(world, eid)) {
        r.ok = false;
        snprintf(r.msg, sizeof(r.msg), "Invalid or dead entity");
        return r;
    }
    const char* name = args.values[1].str;
    auto& bridge = EcsReflectBridge::Get();
    for (int i = 0; i < bridge.Count(); ++i) {
        if (strncmp(bridge.Desc(i).name, name, sizeof(bridge.Desc(i).name)) != 0) continue;
        ecs_entity_t cid = bridge.Id(i);
        if (!cid) {
            r.ok = false;
            snprintf(r.msg, sizeof(r.msg), "Component '%s' has no resolved id", name);
            return r;
        }
        ecs_add_id(world, eid, cid);
        r.ok = true;
        snprintf(r.msg, sizeof(r.msg), "Added %s to entity", name);
        return r;
    }
    r.ok = false;
    snprintf(r.msg, sizeof(r.msg), "Unknown component: %s", name);
    return r;
}

void RegisterStdEditorCommands(uint32_t owner_module_id) {
    auto& cmds = EditorCmdRegistry::Get();
    static CmdSchema kNoArgsNoUndo{ {}, 0, true };
    cmds.Register("Delete Selected",    DeleteSelectedCmd,    nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("Duplicate Selected", DuplicateSelectedCmd, nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("Select All",         SelectAllCmd,         nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("New Scene",          NewSceneCmd,          nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("Save Game",          SaveGameCmd,          nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("Load Game",          LoadGameCmd,          nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("Reload Shaders",     ReloadShadersCmd,     nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("Rebuild NavMesh",    RebuildNavMeshCmd,    nullptr, nullptr, kNoArgsNoUndo, owner_module_id);
    cmds.Register("FPS Query",          FpsQueryCmd,          nullptr, nullptr, kNoArgsNoUndo, owner_module_id);

    static CmdSchema kImportExportSchema{ { {"path", CmdArgType::Str} }, 1, true };
    cmds.Register("Import Scene", ImportSceneCmd, nullptr, nullptr, kImportExportSchema, owner_module_id);
    cmds.Register("Export Scene", ExportSceneCmd, nullptr, nullptr, kImportExportSchema, owner_module_id);

    static CmdSchema kSpawnEntitySchema{
        { {"faction", CmdArgType::I64}, {"x", CmdArgType::F64}, {"z", CmdArgType::F64} }, 3, true };
    cmds.Register("Spawn Entity", SpawnEntityCmd, nullptr, nullptr, kSpawnEntitySchema, owner_module_id);

    static CmdSchema kKillEntitySchema{ { {"entity_id", CmdArgType::Entity} }, 1, true };
    cmds.Register("Kill Entity", KillEntityCmd, nullptr, nullptr, kKillEntitySchema, owner_module_id);

    // no_undo=false: the registry's first real undo-capable command — see
    // FactionRelationUndoData's doc comment above.
    static CmdSchema kSetFactionRelationSchema{
        { {"a", CmdArgType::I64}, {"b", CmdArgType::I64}, {"v", CmdArgType::I64} }, 3, false };
    cmds.Register("Set Faction Relation", SetFactionRelationCmd,
                  UndoSetFactionRelation, RedoSetFactionRelation,
                  kSetFactionRelationSchema, owner_module_id);

    static CmdSchema kAddComponentSchema{
        { {"entity_id", CmdArgType::Entity}, {"component_name", CmdArgType::Str} }, 2, true };
    cmds.Register("Add Component", AddComponentCmd, nullptr, nullptr, kAddComponentSchema, owner_module_id);

    static CmdSchema kCameraBookmarkSchema{ { {"name", CmdArgType::Str} }, 1, true };
    cmds.Register("Save Camera Bookmark", SaveCameraBookmarkCmd, nullptr, nullptr, kCameraBookmarkSchema, owner_module_id);
    cmds.Register("Goto Camera Bookmark", GotoCameraBookmarkCmd, nullptr, nullptr, kCameraBookmarkSchema, owner_module_id);
}

CmdResult DispatchEditorCmd(const char* name, const CmdArgs& args) {
    DispatchOutcome out = EditorCmdRegistry::Get().Dispatch(name, args, MdRegistry::Get());
    if (out.has_undo) {
        Command c;
        c.execute = out.redo;
        c.undo = out.undo;
        memcpy(c.data, out.undo_data, sizeof(c.data));
        EditorCore::Get().history.Push(c);
    }
    return out.result;
}
#endif
