#pragma once

class LuaSystem;

// Registers md.editor_* functions into the given LuaSystem's md.* table.
// Called from editor_panels_init() (dlopen path, re-registered every F5
// reload — see EditorModule::Config::lua_system doc comment) and from
// tools/editor/main.cpp's non-hot-reload init block (single-binary path,
// no dlopen boundary, safe to call directly on the same instance).
void RegisterLuaEditorScenarioAPI(LuaSystem& sys);

// Set by md.editor_open_panel(name); read+cleared once by
// editor_panels_build_ui()'s tab loop to force-select the named tab via
// ImGuiTabItemFlags_SetSelected. Plain C++ linkage (no dlopen boundary
// concern — both sides of this call live inside libeditor_panels.so).
const char* EditorPanels_ConsumeForcedTab();
