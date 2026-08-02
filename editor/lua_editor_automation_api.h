#pragma once

class LuaSystem;

// EDITOR_AUTOMATION_PLAN_v1.md Phase 2: generic Lua bindings —
// md.editor.exec/list/help/undo/redo/selection/camera/status/capture/dump/
// begin_batch/end_batch, md.ecs.get/set/has/components. Real nested tables
// (LuaSystem::RegisterSubNamespaceFunction), not the flat md.editor_foo()
// convention lua_editor_scenario_api.cpp uses — see that method's own doc
// comment for why this is a deliberate, separate registration path.
//
// Called from the same 2 places as RegisterLuaEditorScenarioAPI
// (tools/editor/main.cpp, editor_panels_entry.cpp) — this API is
// standalone-editor-only, same as the scenario API it sits alongside; the
// game's F3 editor registers neither (pre-existing scope, not something
// this phase changes — see editor_std_commands.h's RegisterStdEditorCommands
// doc comment for the DIFFERENT, already-fixed gap that command dispatch
// itself had).
void RegisterLuaEditorAutomationAPI(LuaSystem& sys);
