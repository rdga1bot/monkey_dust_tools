#include "lua_editor_automation_api.h"
#include <monkey_dust/scripting/lua_system.h>
#include <monkey_dust/editor/cmd_registry.h>
#include <monkey_dust/editor/cmd_path_validate.h>
#include <monkey_dust/ecs/component_reflect.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/nav/nav_system.h>
#include "editor_core.h"
#include "editor_std_commands.h"
#include "editor_reflect_bridge.h"
#include "editor_screenshot.h"
#ifdef MONKEY_DUST_EDITOR_HOT_RELOAD
#include <monkey_dust/hot/editor_module.h>
#endif
#include <flecs.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

extern "C" {
#include <lua5.4/lua.h>
#include <lua5.4/lauxlib.h>
}

// Defined in editor_panels_entry.cpp (extern "C", called by the host across
// the dlopen boundary for F9's bug-capture path) — reused here for
// md.editor.dump so panel-specific state isn't captured twice by two
// independent implementations.
extern "C" void editor_panels_dump_state(void* file_ptr);

namespace {

// ── md.editor.exec/list/help/undo/redo ──────────────────────────────────────

const char* FieldTypeName(CmdArgType t) {
    switch (t) {
        case CmdArgType::I64:    return "i64";
        case CmdArgType::F64:    return "f64";
        case CmdArgType::Entity: return "entity";
        case CmdArgType::Str:    return "str";
    }
    return "?";
}

// Reads schema->args[i] out of a Lua args table (arg2 of exec): named field
// first (args_table[def.name]), positional fallback (args_table[arg_i_1based]).
// Missing/wrong-type values fall back to a zero-ish default via luaL_opt* —
// schema validation inside Dispatch() catches a genuinely missing required
// value by type-mismatch (I64 default 0 always matches CmdArgType::I64, so
// this can't accidentally slip an unintended value past validation; it can
// only silently pass 0/""/entity-0 for an arg the caller forgot, same as
// any dynamically-typed scripting binding).
void ReadArg(lua_State* L, int table_idx, int arg_i_1based, const CmdArgDef& def, CmdArgValue& out) {
    out.type = def.type;
    bool have_table = lua_istable(L, table_idx);
    if (have_table) {
        lua_getfield(L, table_idx, def.name);
        if (lua_isnil(L, -1)) {
            lua_pop(L, 1);
            lua_geti(L, table_idx, arg_i_1based);
        }
    } else {
        lua_pushnil(L);
    }
    switch (def.type) {
        case CmdArgType::I64:
            out.i64 = (int64_t)luaL_optinteger(L, -1, 0);
            break;
        case CmdArgType::F64:
            out.f64 = (double)luaL_optnumber(L, -1, 0.0);
            break;
        case CmdArgType::Entity:
            out.entity_id = (uint64_t)luaL_optinteger(L, -1, 0);
            break;
        case CmdArgType::Str: {
            const char* s = luaL_optstring(L, -1, "");
            strncpy(out.str, s, sizeof(out.str) - 1);
            out.str[sizeof(out.str) - 1] = '\0';
            break;
        }
    }
    lua_pop(L, 1);
}

int l_md_editor_exec(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const CmdSchema* schema = EditorCmdRegistry::Get().Help(name);
    if (!schema) {
        lua_pushboolean(L, false);
        lua_pushfstring(L, "Unknown command: %s", name);
        return 2;
    }
    CmdArgs args;
    args.count = schema->count;
    for (int i = 0; i < schema->count; ++i) {
        ReadArg(L, 2, i + 1, schema->args[i], args.values[i]);
    }
    CmdResult r = DispatchEditorCmd(name, args);
    lua_pushboolean(L, r.ok);
    lua_pushstring(L, r.msg);
    return 2;
}

int l_md_editor_list(lua_State* L) {
    auto& reg = EditorCmdRegistry::Get();
    lua_newtable(L);
    int n = reg.Count();
    for (int i = 0; i < n; ++i) {
        lua_pushstring(L, reg.NameAt(i));
        lua_seti(L, -2, i + 1);
    }
    return 1;
}

int l_md_editor_help(lua_State* L) {
    const char* name = luaL_checkstring(L, 1);
    const CmdSchema* schema = EditorCmdRegistry::Get().Help(name);
    if (!schema) { lua_pushnil(L); return 1; }
    lua_newtable(L);
    lua_pushinteger(L, schema->count); lua_setfield(L, -2, "count");
    lua_pushboolean(L, schema->no_undo); lua_setfield(L, -2, "no_undo");
    lua_newtable(L);
    for (int i = 0; i < schema->count; ++i) {
        lua_newtable(L);
        lua_pushstring(L, schema->args[i].name); lua_setfield(L, -2, "name");
        lua_pushstring(L, FieldTypeName(schema->args[i].type)); lua_setfield(L, -2, "type");
        lua_seti(L, -2, i + 1);
    }
    lua_setfield(L, -2, "args");
    return 1;
}

int l_md_editor_undo(lua_State* L) { (void)L; EditorCore::Get().Undo(); return 0; }
int l_md_editor_redo(lua_State* L) { (void)L; EditorCore::Get().Redo(); return 0; }

// Phase 4 (regression suite): real hot-reload (dlclose/dlopen
// libeditor_panels.so, re-running editor_panels_init/shutdown) is
// otherwise only reachable via an actual F5 keypress (main.cpp's
// input_key_pressed(SDL_SCANCODE_F5) check) — no existing Lua path
// triggers it, which makes "Phase 0 hot-reload survival" impossible to
// re-run as a live --exec regression scenario. This calls the exact same
// EditorModule::Get().Reload() the F5 handler does, just from Lua instead
// of a physical keypress, so the permanent regression suite can exercise
// the real dlopen/dlclose cycle deterministically.
int l_md_editor_trigger_hot_reload(lua_State* L) {
#ifdef MONKEY_DUST_EDITOR_HOT_RELOAD
    EditorModule::Get().Reload();
    // Synchronous from the script's point of view (unlike the F5 hotkey
    // path, which stays async for UI responsiveness) — see
    // WaitReloadReady()'s doc comment for the crash this avoids.
    EditorModule::Get().WaitReloadReady();
    return 0;
#else
    (void)L;
    return luaL_error(L, "trigger_hot_reload: this binary was not built with MONKEY_DUST_EDITOR_HOT_RELOAD");
#endif
}

// ── md.editor.selection/camera/status ───────────────────────────────────────

int l_md_editor_selection(lua_State* L) {
    auto& ec = EditorCore::Get();
    lua_newtable(L);
    for (int i = 0; i < ec.selected_count; ++i) {
        lua_pushinteger(L, (lua_Integer)ec.selected[i].ToIntegral());
        lua_seti(L, -2, i + 1);
    }
    return 1;
}

int l_md_editor_camera(lua_State* L) {
    auto& ec = EditorCore::Get();
    lua_newtable(L);
    lua_pushnumber(L, ec.cam_target.x); lua_setfield(L, -2, "x");
    lua_pushnumber(L, ec.cam_target.y); lua_setfield(L, -2, "y");
    lua_pushnumber(L, ec.cam_target.z); lua_setfield(L, -2, "z");
    lua_pushnumber(L, ec.cam_yaw);      lua_setfield(L, -2, "yaw");
    lua_pushnumber(L, ec.cam_pitch);    lua_setfield(L, -2, "pitch");
    return 1;
}

int l_md_editor_status(lua_State* L) {
    auto& reg = MdRegistry::Get();
    int entity_count = 0;
    static auto q = reg.Raw().query<MdManagedTag>();
    q.each([&](flecs::entity, MdManagedTag) { ++entity_count; });
    lua_newtable(L);
    lua_pushinteger(L, entity_count); lua_setfield(L, -2, "entity_count");
    lua_pushboolean(L, NavSystem::Get().IsReady()); lua_setfield(L, -2, "nav_ready");
    lua_pushnumber(L, EditorCore::Get().frame_fps); lua_setfield(L, -2, "fps");
    return 1;
}

// ── md.editor.capture/dump ───────────────────────────────────────────────────

int l_md_editor_capture(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!CmdPathValidate(path)) return luaL_error(L, "capture: path rejected (must be under data/, game/data/, or automation_out/): %s", path);
#ifdef MD_SDL_GPU
    EditorScreenshot_RequestPending(path);
#endif
    return 0;
}

int l_md_editor_dump(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!CmdPathValidate(path)) return luaL_error(L, "dump: path rejected (must be under data/, game/data/, or automation_out/): %s", path);
    FILE* f = fopen(path, "w");
    if (!f) return luaL_error(L, "dump: cannot open %s", path);

    auto& ec = EditorCore::Get();
    int entity_count = 0;
    static auto q = MdRegistry::Get().Raw().query<MdManagedTag>();
    q.each([&](flecs::entity, MdManagedTag) { ++entity_count; });

    fprintf(f, "[Status]\n");
    fprintf(f, "  entity_count=%d\n", entity_count);
    fprintf(f, "  selection_count=%d\n", ec.selected_count);
    fprintf(f, "  camera=(%.2f,%.2f,%.2f) yaw=%.2f pitch=%.2f\n",
            ec.cam_target.x, ec.cam_target.y, ec.cam_target.z, ec.cam_yaw, ec.cam_pitch);
    fprintf(f, "  nav_ready=%d\n", NavSystem::Get().IsReady() ? 1 : 0);
    fprintf(f, "  fps=%.1f\n\n", ec.frame_fps);

    editor_panels_dump_state(f); // reuse the F9 path's panel-specific dump — not duplicated

    fclose(f);
    return 0;
}

// ── md.editor.load_script ────────────────────────────────────────────────────
// Phase 3 (agent authoring layer): the sandboxed alternative to Lua's own
// dofile/loadfile (blocked — see lua_system.h's doc comment). Found while
// wiring scripts/authoring/authoring.lua as a reusable module: a plain
// `dofile("scripts/authoring/authoring.lua")` call fails with "attempt to
// call a nil value (global 'dofile')" — the sandbox blocks it exactly as
// documented, including for --exec scenario scripts, since they run in the
// SAME Lua state as everything else. This reads the (CmdPathValidate'd)
// file in C++ — Lua's io/os restrictions don't apply to the host's own
// file access — then loads and calls it in the CURRENT Lua state via
// luaL_loadbuffer + lua_call, so a module written as `return M` (the
// authoring library's own convention) comes back as this function's
// single return value, same shape dofile() would have produced.
int l_md_editor_load_script(lua_State* L) {
    const char* path = luaL_checkstring(L, 1);
    if (!CmdPathValidate(path)) {
        return luaL_error(L, "load_script: path rejected (must be under data/, game/data/, automation_out/, or scripts/): %s", path);
    }
    FILE* f = fopen(path, "rb");
    if (!f) return luaL_error(L, "load_script: cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 0) {
        fclose(f);
        return luaL_error(L, "load_script: ftell failed for %s", path);
    }
    char* buf = static_cast<char*>(malloc(static_cast<size_t>(size) + 1));
    size_t n = fread(buf, 1, static_cast<size_t>(size), f);
    fclose(f);
    buf[n] = '\0';

    int load_err = luaL_loadbuffer(L, buf, n, path);
    free(buf);
    if (load_err != LUA_OK) {
        const char* msg = lua_tostring(L, -1);
        return luaL_error(L, "load_script: parse error in %s: %s", path, msg ? msg : "(no message)");
    }
    lua_call(L, 0, 1); // module is expected to `return M`
    return 1;
}

// ── md.editor.begin_batch/end_batch ─────────────────────────────────────────
// Phase 2.6: commands inside collapse into ONE undo entry. Side-table of
// captured sub-commands (a Command's 64-byte data blob can't hold N full
// Commands, only an index into this) — bounded (MAX_BATCHES slots, never
// freed once used) rather than dynamically allocated, matching this
// project's no-heap convention. A known, documented limitation: after
// MAX_BATCHES distinct batches have been created in one process lifetime,
// further end_batch() calls silently degrade to leaving the individual
// undo entries un-collapsed (still correct, just not grouped) rather than
// crashing or leaking past the fixed array.

constexpr int MAX_BATCHES = 8;
constexpr int MAX_BATCH_COMMANDS = 16;

struct BatchRecord {
    Command cmds[MAX_BATCH_COMMANDS];
    int count = 0;
    bool used = false;
};

BatchRecord s_batches[MAX_BATCHES];
int s_batch_start_top = -1; // -1 = no batch in progress

void BatchUndo(void* data, MdRegistry& reg) {
    int idx; memcpy(&idx, data, sizeof(idx));
    BatchRecord& b = s_batches[idx];
    for (int i = b.count - 1; i >= 0; --i) {
        if (b.cmds[i].undo) b.cmds[i].undo(b.cmds[i].data, reg);
    }
}

void BatchRedo(void* data, MdRegistry& reg) {
    int idx; memcpy(&idx, data, sizeof(idx));
    BatchRecord& b = s_batches[idx];
    for (int i = 0; i < b.count; ++i) {
        if (b.cmds[i].execute) b.cmds[i].execute(b.cmds[i].data, reg);
    }
}

int l_md_editor_begin_batch(lua_State* L) {
    (void)L;
    s_batch_start_top = EditorCore::Get().history.top;
    return 0;
}

int l_md_editor_end_batch(lua_State* L) {
    (void)L;
    if (s_batch_start_top < 0) return 0;
    auto& history = EditorCore::Get().history;
    int start = s_batch_start_top;
    int end = history.top;
    s_batch_start_top = -1;
    int n = end - start;
    if (n <= 1) return 0; // 0 or 1 real entries — nothing to collapse

    int slot = -1;
    for (int i = 0; i < MAX_BATCHES; ++i) {
        if (!s_batches[i].used) { slot = i; break; }
    }
    if (slot < 0) return 0; // out of batch slots — leave entries un-collapsed

    BatchRecord& b = s_batches[slot];
    b.used = true;
    b.count = 0;
    for (int i = start; i < end && b.count < MAX_BATCH_COMMANDS; ++i) {
        b.cmds[b.count++] = history.stack[i % CommandStack::MAX_UNDO];
    }

    Command batched;
    batched.execute = BatchRedo;
    batched.undo = BatchUndo;
    memcpy(batched.data, &slot, sizeof(slot));
    history.stack[start % CommandStack::MAX_UNDO] = batched;
    history.top = start + 1;
    history.redo_top = history.top;
    return 0;
}

// ── md.ecs.get/set/has/components ───────────────────────────────────────────
// Via EcsReflectBridge (Id(i)=flecs component id, Desc(i)=field metadata) +
// raw flecs C API — same pattern as editor_reflect_inspector.cpp's Inspector
// panel, which already proves both READ (DrawReflectedFields) and WRITE
// (ecs_get_mut_id + ecs_modified_id) work generically across every
// reflected component. field.offset/size make byte-level get/set possible
// without per-component glue code.

const md::ComponentDesc* FindComponentDesc(const char* name, ecs_entity_t* out_cid) {
    auto& bridge = EcsReflectBridge::Get();
    for (int i = 0; i < bridge.Count(); ++i) {
        if (strncmp(bridge.Desc(i).name, name, sizeof(bridge.Desc(i).name)) == 0) {
            if (out_cid) *out_cid = bridge.Id(i);
            return &bridge.Desc(i);
        }
    }
    return nullptr;
}

const md::FieldDesc* FindField(const md::ComponentDesc& desc, const char* name) {
    for (int i = 0; i < desc.field_count; ++i) {
        if (strcmp(desc.fields[i].name, name) == 0) return &desc.fields[i];
    }
    return nullptr;
}

void PushFieldValue(lua_State* L, const void* comp, const md::FieldDesc& f) {
    const uint8_t* p = static_cast<const uint8_t*>(comp) + f.offset;
    switch (f.type) {
        case md::FieldType::F32:  lua_pushnumber(L, *reinterpret_cast<const float*>(p)); break;
        case md::FieldType::F64:  lua_pushnumber(L, *reinterpret_cast<const double*>(p)); break;
        case md::FieldType::I8:   lua_pushinteger(L, *reinterpret_cast<const int8_t*>(p)); break;
        case md::FieldType::I16:  lua_pushinteger(L, *reinterpret_cast<const int16_t*>(p)); break;
        case md::FieldType::I32:  lua_pushinteger(L, *reinterpret_cast<const int32_t*>(p)); break;
        case md::FieldType::I64:  lua_pushinteger(L, (lua_Integer)*reinterpret_cast<const int64_t*>(p)); break;
        case md::FieldType::U8:   lua_pushinteger(L, *reinterpret_cast<const uint8_t*>(p)); break;
        case md::FieldType::U16:  lua_pushinteger(L, *reinterpret_cast<const uint16_t*>(p)); break;
        case md::FieldType::U32:  lua_pushinteger(L, (lua_Integer)*reinterpret_cast<const uint32_t*>(p)); break;
        case md::FieldType::U64:  lua_pushinteger(L, (lua_Integer)*reinterpret_cast<const uint64_t*>(p)); break;
        case md::FieldType::Bool: lua_pushboolean(L, *reinterpret_cast<const bool*>(p)); break;
        case md::FieldType::Vec3: {
            const float* v = reinterpret_cast<const float*>(p);
            lua_newtable(L);
            lua_pushnumber(L, v[0]); lua_setfield(L, -2, "x");
            lua_pushnumber(L, v[1]); lua_setfield(L, -2, "y");
            lua_pushnumber(L, v[2]); lua_setfield(L, -2, "z");
            break;
        }
        case md::FieldType::Enum8:  lua_pushinteger(L, *reinterpret_cast<const int8_t*>(p)); break;
        case md::FieldType::Enum16: lua_pushinteger(L, *reinterpret_cast<const int16_t*>(p)); break;
        case md::FieldType::Enum32: lua_pushinteger(L, *reinterpret_cast<const int32_t*>(p)); break;
    }
}

void WriteFieldValue(lua_State* L, int idx, void* comp, const md::FieldDesc& f) {
    uint8_t* p = static_cast<uint8_t*>(comp) + f.offset;
    switch (f.type) {
        case md::FieldType::F32:  *reinterpret_cast<float*>(p) = (float)lua_tonumber(L, idx); break;
        case md::FieldType::F64:  *reinterpret_cast<double*>(p) = (double)lua_tonumber(L, idx); break;
        case md::FieldType::I8:   *reinterpret_cast<int8_t*>(p) = (int8_t)lua_tointeger(L, idx); break;
        case md::FieldType::I16:  *reinterpret_cast<int16_t*>(p) = (int16_t)lua_tointeger(L, idx); break;
        case md::FieldType::I32:  *reinterpret_cast<int32_t*>(p) = (int32_t)lua_tointeger(L, idx); break;
        case md::FieldType::I64:  *reinterpret_cast<int64_t*>(p) = (int64_t)lua_tointeger(L, idx); break;
        case md::FieldType::U8:   *reinterpret_cast<uint8_t*>(p) = (uint8_t)lua_tointeger(L, idx); break;
        case md::FieldType::U16:  *reinterpret_cast<uint16_t*>(p) = (uint16_t)lua_tointeger(L, idx); break;
        case md::FieldType::U32:  *reinterpret_cast<uint32_t*>(p) = (uint32_t)lua_tointeger(L, idx); break;
        case md::FieldType::U64:  *reinterpret_cast<uint64_t*>(p) = (uint64_t)lua_tointeger(L, idx); break;
        case md::FieldType::Bool: *reinterpret_cast<bool*>(p) = lua_toboolean(L, idx); break;
        case md::FieldType::Vec3: {
            float* v = reinterpret_cast<float*>(p);
            lua_getfield(L, idx, "x"); v[0] = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, idx, "y"); v[1] = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            lua_getfield(L, idx, "z"); v[2] = (float)lua_tonumber(L, -1); lua_pop(L, 1);
            break;
        }
        case md::FieldType::Enum8:  *reinterpret_cast<int8_t*>(p) = (int8_t)lua_tointeger(L, idx); break;
        case md::FieldType::Enum16: *reinterpret_cast<int16_t*>(p) = (int16_t)lua_tointeger(L, idx); break;
        case md::FieldType::Enum32: *reinterpret_cast<int32_t*>(p) = (int32_t)lua_tointeger(L, idx); break;
    }
}

int l_md_ecs_get(lua_State* L) {
    lua_Integer eid = luaL_checkinteger(L, 1);
    const char* comp_name = luaL_checkstring(L, 2);
    const char* field_name = luaL_checkstring(L, 3);
    ecs_entity_t cid = 0;
    const md::ComponentDesc* desc = FindComponentDesc(comp_name, &cid);
    if (!desc || !cid) { lua_pushnil(L); return 1; }
    ecs_world_t* world = MdRegistry::Get().Raw().c_ptr();
    if (!ecs_is_alive(world, (ecs_entity_t)eid)) { lua_pushnil(L); return 1; }
    const void* comp = ecs_get_id(world, (ecs_entity_t)eid, cid);
    if (!comp) { lua_pushnil(L); return 1; }
    const md::FieldDesc* f = FindField(*desc, field_name);
    if (!f) { lua_pushnil(L); return 1; }
    PushFieldValue(L, comp, *f);
    return 1;
}

int l_md_ecs_set(lua_State* L) {
    lua_Integer eid = luaL_checkinteger(L, 1);
    const char* comp_name = luaL_checkstring(L, 2);
    const char* field_name = luaL_checkstring(L, 3);
    ecs_entity_t cid = 0;
    const md::ComponentDesc* desc = FindComponentDesc(comp_name, &cid);
    if (!desc || !cid) return luaL_error(L, "md.ecs.set: unknown component %s", comp_name);
    ecs_world_t* world = MdRegistry::Get().Raw().c_ptr();
    if (!ecs_is_alive(world, (ecs_entity_t)eid)) return luaL_error(L, "md.ecs.set: entity %lld not alive", (long long)eid);
    void* comp = ecs_get_mut_id(world, (ecs_entity_t)eid, cid);
    if (!comp) return luaL_error(L, "md.ecs.set: entity has no component %s", comp_name);
    const md::FieldDesc* f = FindField(*desc, field_name);
    if (!f) return luaL_error(L, "md.ecs.set: unknown field %s.%s", comp_name, field_name);
    WriteFieldValue(L, 4, comp, *f);
    ecs_modified_id(world, (ecs_entity_t)eid, cid);
    return 0;
}

int l_md_ecs_has(lua_State* L) {
    lua_Integer eid = luaL_checkinteger(L, 1);
    const char* comp_name = luaL_checkstring(L, 2);
    ecs_entity_t cid = 0;
    if (!FindComponentDesc(comp_name, &cid) || !cid) { lua_pushboolean(L, false); return 1; }
    ecs_world_t* world = MdRegistry::Get().Raw().c_ptr();
    lua_pushboolean(L, ecs_is_alive(world, (ecs_entity_t)eid) && ecs_has_id(world, (ecs_entity_t)eid, cid));
    return 1;
}

int l_md_ecs_components(lua_State* L) {
    lua_Integer eid = luaL_checkinteger(L, 1);
    ecs_world_t* world = MdRegistry::Get().Raw().c_ptr();
    lua_newtable(L);
    if (!ecs_is_alive(world, (ecs_entity_t)eid)) return 1;
    auto& bridge = EcsReflectBridge::Get();
    int n = 0;
    for (int i = 0; i < bridge.Count(); ++i) {
        ecs_entity_t cid = bridge.Id(i);
        if (cid && ecs_has_id(world, (ecs_entity_t)eid, cid)) {
            lua_pushstring(L, bridge.Desc(i).name);
            lua_seti(L, -2, ++n);
        }
    }
    return 1;
}

} // namespace

void RegisterLuaEditorAutomationAPI(LuaSystem& sys) {
    sys.RegisterSubNamespaceFunction("editor", "exec",        l_md_editor_exec);
    sys.RegisterSubNamespaceFunction("editor", "list",        l_md_editor_list);
    sys.RegisterSubNamespaceFunction("editor", "help",        l_md_editor_help);
    sys.RegisterSubNamespaceFunction("editor", "undo",        l_md_editor_undo);
    sys.RegisterSubNamespaceFunction("editor", "redo",        l_md_editor_redo);
    sys.RegisterSubNamespaceFunction("editor", "trigger_hot_reload", l_md_editor_trigger_hot_reload);
    sys.RegisterSubNamespaceFunction("editor", "selection",   l_md_editor_selection);
    sys.RegisterSubNamespaceFunction("editor", "camera",      l_md_editor_camera);
    sys.RegisterSubNamespaceFunction("editor", "status",      l_md_editor_status);
    sys.RegisterSubNamespaceFunction("editor", "capture",     l_md_editor_capture);
    sys.RegisterSubNamespaceFunction("editor", "dump",        l_md_editor_dump);
    sys.RegisterSubNamespaceFunction("editor", "load_script", l_md_editor_load_script);
    sys.RegisterSubNamespaceFunction("editor", "begin_batch", l_md_editor_begin_batch);
    sys.RegisterSubNamespaceFunction("editor", "end_batch",   l_md_editor_end_batch);

    sys.RegisterSubNamespaceFunction("ecs", "get",        l_md_ecs_get);
    sys.RegisterSubNamespaceFunction("ecs", "set",        l_md_ecs_set);
    sys.RegisterSubNamespaceFunction("ecs", "has",        l_md_ecs_has);
    sys.RegisterSubNamespaceFunction("ecs", "components", l_md_ecs_components);
}
