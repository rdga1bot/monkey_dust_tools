#pragma once
#include <SDL3/SDL_gpu.h>

// EDITOR_AUTOMATION_PLAN_v1.md Phase 5: ImGui Test Engine panel smoke
// tests. MD_UI_TESTS builds only (tools/CMakeLists.txt) — this header/its
// .cpp compile to nothing in a normal editor build.
//
// Runs the real editor UI (editor_panels_init/build_ui/render/shutdown —
// the same functions EditorModule normally calls through dlopen, called
// directly here since MD_UI_TESTS always builds them into
// monkey_dust_editor regardless of the hot-reload flag) driven by
// ImGuiTestEngine instead of a human, then exits with a nonzero code if
// any test failed. Caller (main.cpp) must have already completed the
// normal GPU/window/ImGui setup and NOT called EditorModule::Get().Init()
// — this function owns editor_panels_init/shutdown itself.
int RunUiSmokeTests(SDL_GPUDevice* gpu, SDL_GPUTextureFormat sc_fmt,
                     float overlay_top, const char* layout_path);
