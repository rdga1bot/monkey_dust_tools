#pragma once
#ifdef MONKEY_DUST_EDITOR
#include <monkey_dust/editor/cmd_registry.h>
#include <monkey_dust/ecs/md_registry.h>

// EDITOR_AUTOMATION_PLAN_v1.md Phase 1.3: command bodies for actions that
// previously existed as duplicated inline lambdas across 2+ of
// palette/toolbar/console (violating the plan's "one execution path"
// invariant — found while doing the console inventory this phase required:
// Select All, Save Game, Load Game, and Rebuild NavMesh each had a second,
// independent copy of the same logic), or that are real engine-state
// mutations worth making scriptable/discoverable via the registry.
// Registered from editor_panels_init() (editor_panels_entry.cpp);
// dispatched by name from the palette/toolbar/console call sites that used
// to each carry their own copy of this logic.
//
// All NO_UNDO for now — real undo would need to capture enough state to
// reverse each action (full component data for AddComponent, spawned entity
// ids for Spawn, prior faction relation value, etc.) into the 64-byte
// undo_data blob. Deferred to a follow-up now that the registry's
// undo-mediation mechanism itself is proven (Phase 1.1-1.2); tracked as a
// finding, not silently done partially here.

CmdResult DuplicateSelectedCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult SelectAllCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult NewSceneCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult ImportSceneCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult ExportSceneCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult SaveGameCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult LoadGameCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult ReloadShadersCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult RebuildNavMeshCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult SpawnEntityCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult KillEntityCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult SetFactionRelationCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult FpsQueryCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);
CmdResult AddComponentCmd(const CmdArgs& args, MdRegistry& reg, uint8_t undo_data[64]);

// Registers every command above (plus DeleteSelectedCmd, editor_command_
// palette.cpp) under owner_module_id. Factored out so the two independent
// places that bring up an EditorCore instance — the hot-reloadable panels
// .so (editor_panels_entry.cpp's editor_panels_init(), used by
// monkey_dust_editor) and the game's own statically-linked F3 editor
// (game/src/main.cpp, used by monkey_dust -DMONKEY_DUST_EDITOR=ON, which
// never dlopens the panels .so at all) — both register into the SAME
// EditorCmdRegistry singleton the same way, instead of one of them being a
// silent gap where every palette/toolbar/console Dispatch() call would
// return "Unknown command". Call with a distinct owner_module_id per
// caller so UnregisterModule() only purges that caller's own entries.
void RegisterStdEditorCommands(uint32_t owner_module_id);

// EDITOR_AUTOMATION_PLAN_v1.md Phase 2: the ONE place that actually pushes
// onto EditorCore::Get().history (CommandStack) when a dispatch produces
// undo/redo data. Found missing while wiring md.editor.undo(): every
// existing call site (palette/toolbar/console/inspector, Phase 1.3) called
// EditorCmdRegistry::Get().Dispatch(...) directly and discarded the
// DispatchOutcome's undo/redo fields entirely — Phase 1.1-1.2's undo-
// mediation mechanism was proven only against a FAKE stack in the unit
// test, never wired into the REAL one. Every dispatch call site (including
// this file's own commands' future callers, and the Lua md.editor.exec
// binding) should go through this helper instead of calling
// EditorCmdRegistry::Get().Dispatch(...) directly, so Ctrl+Z/Ctrl+Y and
// md.editor.undo()/redo() actually see every undo-capable command's
// effect, not just the ones some call site happened to wire by hand.
CmdResult DispatchEditorCmd(const char* name, const CmdArgs& args);

#endif
