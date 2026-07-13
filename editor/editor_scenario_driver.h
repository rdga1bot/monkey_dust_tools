#pragma once
// editor_scenario_driver.h — CLI parsing for editor --exec script mode
// (Etap 4, AUTONOMY_SYSTEM_PROMPT_v2.md).
//
// Unlike game/src/autonomy/scenario_driver.h (which owns the whole loop),
// this only parses args — the per-frame ResumeScenario() drive lives
// directly inside tools/editor/main.cpp's existing render loop, since
// duplicating that loop's window/ImGui/hot-reload plumbing in a second
// function would be pure risk for zero benefit (same render pipeline,
// same frame either way).

struct EditorScenarioConfig {
    bool     active      = false;   // --exec was passed
    char     script_path[256] = {};
    unsigned seed        = 42u;
    long     max_frames  = 0;       // 0 = unset
    double   max_seconds = 0.0;     // 0 = unset
    bool     fast        = false;   // skip the 60fps frame-cap sleep
    bool     headless    = false;   // documented scope note (main.cpp) — does NOT skip GPU/window init
};

// Parses argv into cfg. Returns false + writes to stderr on a malformed
// flag (e.g. --exec with no path). Unrecognized args are ignored.
bool ParseEditorScenarioArgs(int argc, char** argv, EditorScenarioConfig& cfg);
