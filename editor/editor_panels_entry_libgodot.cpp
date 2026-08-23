// editor_panels_entry_libgodot.cpp — extern "C" entry points for
// libeditor_panels_libgodot.so (LibGodot editor hot-reload, task/BORG
// LIBGODOT parity gate blocker-6). Mirrors tools/editor/editor_panels_
// entry.cpp's SDL3 pattern as closely as possible; see that file's own
// doc comment for the general shape (dlopen'd by EditorModule, symbols
// resolved from the HOST executable at load time since this .so does
// NOT link monkey_dust::engine statically — see tools/CMakeLists.txt's
// monkey_dust_libgodot_editor_panels target comment for why).
//
// v1->v2 (this pass): wires up the ~18 panel .cpp files that were already
// statically linked into monkey_dust_libgodot_editor (tools/CMakeLists.txt's
// panel-porting batch) but never called from anywhere — pure dead weight
// until now. Every panel below is called the SAME way EditorConsole already
// was: DrawContent() only, inside our own Begin/End (see tools/editor/
// CLAUDE.md's DrawContent() invariant — Draw() bundles a visibility-flag
// gate that defaults false for MONKEY_DUST_STANDALONE_EDITOR and this v1/v2
// host has no toolbar/menu to flip those flags interactively, so calling
// Draw() directly would just no-op like it did before this pass).
//
// Exceptions, using Draw() as-is instead of DrawContent():
//  - EditorCommandPalette: self-contained modal (Ctrl+P), no DrawContent().
//  - EditorAssetBrowser / EditorGraphicsPanel / EditorFlareBrowser: only
//    expose Draw() (older panels_visible[]-gated design, predates the
//    Draw()/DrawContent() split). Their gating index is forced true once
//    in editor_panels_init() below so Draw() actually shows something.
//
// Skipped (see report, not this file): EditorTranslator (needs a live
// selected entity + active 3D viewport gizmo-drag context this minimal
// host doesn't wire up), lua_editor_automation_api.cpp's registration
// (lua_system is nullptr on this backend — main_libgodot.cpp's own
// explicit "v1: lua editor API not wired on this backend yet" comment,
// out of scope for a panel-wiring pass), editor_scenario_driver.cpp
// (CLI/main-loop --exec feature, not a drawable panel).
//
// No editor_panels_render() is exported at all (LibGodot panels only ever
// issue ImGui widget calls during BuildUI — RenderingServer's own
// ImGui_ImplRenderingServer_RenderDrawData draws everything host-side,
// there is no separate RTT render step the way WorldEditor3D_SDLGPU/
// CharPreviewSDLGPU need on the SDL3 side). EditorModule::Render()'s dlsym
// lookup simply finds nothing and no-ops — see engine/src/hot/
// editor_module.cpp, already handles a missing symbol gracefully, no
// change needed there.
//
// gpu/window params are unused (SDL_GPU/SDL_Window* on the SDL3 side;
// this backend has neither) — EditorModule::Config still declares them
// as SDL types (forward-declared pointers only, never dereferenced),
// so passing nullptr from main_libgodot.cpp is enough; no engine_
// module.h change needed. Reused verbatim, not duplicated — see that
// file's own header comment for the reasoning this call chose reuse
// over a parallel class.
#include "imgui.h"
#include <monkey_dust/platform/md_log.h>
#include "editor_core.h"
#include "editor_console.h"
#include "editor_hierarchy.h"
#include "editor_director_panel.h"
#include "editor_viewcone_panel.h"
#include "editor_animation_panel.h"
#include "editor_sequencer_panel.h"
#include "editor_flowgraph_panel.h"
#include "editor_gpu_profiler_panel.h"
#include "editor_camera_panel.h"
#include "editor_node_graph.h"
#include "editor_reflect_inspector.h"
#include "editor_command_palette.h"
#include "editor_asset_browser.h"
#include "editor_graphics_panel.h"
#include "editor_flare_browser.h"
#include "editor_reflect_bridge.h"
#include "editor_std_commands.h"
#include <monkey_dust/editor/cmd_registry.h>
#include <monkey_dust/ecs/registry.h>
#include <cstdio>
#include <cstring>

// EDITOR_AUTOMATION_PLAN_v1.md Phase 0 pattern (see editor_panels_entry.cpp):
// every command registered from editor_panels_init() is tagged with this
// module id so editor_panels_shutdown() purges exactly this module's
// commands before dlclose. Separate PROCESS from the SDL3 editor's own
// kPanelsModuleId=1 (different binary entirely) — no collision risk,
// but kept distinct in spirit (see editor_panels_entry.cpp:43).
static constexpr uint32_t kLibgodotPanelsModuleId = 1;

static ecs_world_t* s_ecs_world = nullptr;

extern "C" {

// ── Init — called after dlopen ────────────────────────────────────────────
// ctx:       ImGuiContext* from host (must share for ImGui calls to work)
// ecs_world: ecs_world_t* from host (Registry::Get().c_ptr()) — untyped
//            flecs C API only past this point, same rule as the SDL3 side.
// gpu/window: unused on this backend (see file doc comment above).
void editor_panels_init(void* ctx, void* ecs_world, void* /*gpu*/, void* /*window*/,
                         float /*overlay_top*/, const char* /*layout_path*/,
                         void* /*lua_system*/) {
    ImGui::SetCurrentContext(static_cast<ImGuiContext*>(ctx));

    s_ecs_world = static_cast<ecs_world_t*>(ecs_world);
    EcsReflectBridge::Get().Init(s_ecs_world);

    EditorCore::Get().Init();
    EditorConsole::Get().Init();   // installs MdLogSetHook + registers CVars

    // EditorCore::Init() resets panels_visible[] all-false for
    // MONKEY_DUST_STANDALONE_EDITOR (both the SDL3 and this LibGodot
    // target define it) — the SDL3 standalone editor's main.cpp never
    // flips these back on either (no toolbar/menu on this backend to do
    // it interactively), which is exactly why AssetBrowser/GraphicsPanel/
    // FlareBrowser were dead code there too. Force the 3 indices these
    // panels gate on so their Draw() calls below actually show something.
    // Indices per their own Draw() bodies (editor_core.h's own [n]=Foo
    // comment is stale from an earlier panel layout, these are the real,
    // current ones):
    EditorCore::Get().panels_visible[2] = true;  // EditorAssetBrowser
    EditorCore::Get().panels_visible[4] = true;  // EditorGraphicsPanel
    EditorCore::Get().panels_visible[7] = true;  // EditorFlareBrowser

    // Same idempotent-on-reload pattern as the SDL3 side (editor_panels_
    // entry.cpp:119-125) — safe to call every init/reload.
    RegisterStdEditorCommands(kLibgodotPanelsModuleId);

    MD_LOG(MD_LOG_INFO, "[EditorPanelsLibgodot] init complete");
}

// ── Shutdown — called before dlclose ───────────────────────────────────────
void editor_panels_shutdown(const char* /*layout_path*/) {
    EditorCore::Get().Shutdown();

    // Purge this module's EditorCmdRegistry entries before dlclose — a
    // dangling function pointer into an unmapped .so is the exact hazard
    // cmd_registry.h's own header comment warns about (see editor_panels_
    // entry.cpp:148-153 for the SDL3-side precedent this mirrors).
    EditorCmdRegistry::Get().UnregisterModule(kLibgodotPanelsModuleId);

    MD_LOG(MD_LOG_INFO, "[EditorPanelsLibgodot] shutdown complete");
}

// ── BuildUI — called inside ImGui frame ─────────────────────────────────────
// Every panel below follows the exact same shape: SetNextWindowSize/Pos
// (FirstUseEver only, so the user's own resize/move sticks across frames)
// + Begin + DrawContent() (or Draw() for the panels_visible[]-gated ones,
// see file doc comment) + End. Positions are staggered in a rough grid so
// nothing perfectly occludes anything else on first launch; ImGui itself
// remembers the user's own layout afterwards (io.IniFilename is left at
// its default here — unlike the SDL3 side's io.IniFilename=nullptr, so
// window positions DO persist via imgui.ini next to the binary, a
// reasonable default for this v2 host with no other layout-persistence
// mechanism of its own).
uint32_t editor_panels_build_ui(float dt, float /*toolbar_h*/,
                                 char* /*status_msg*/, float* /*status_timer*/) {
    EditorCore::Get().Update(dt);   // no-op toolbar draw on this backend (editor_toolbar_libgodot_stub.cpp)

    EditorConsole::Get().SetFrameStats(dt > 0.f ? 1.f / dt : 0.f, dt * 1000.f);

    ImGui::SetNextWindowSize(ImVec2(700, 320), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Console (hot-reload libgodot)")) {
        EditorConsole::Get().DrawContent();
    }
    ImGui::End();

    // ── Scene / Hierarchy ───────────────────────────────────────────────
    ImGui::SetNextWindowSize(ImVec2(320, 420), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(740, 20), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Hierarchy##libgodot")) {
        EditorHierarchy::Get().DrawContent();
    }
    ImGui::End();

    // ── AI: Director + ViewCone ──────────────────────────────────────────
    ImGui::SetNextWindowSize(ImVec2(320, 360), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(1080, 20), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Director##libgodot")) {
        EditorDirectorPanel::Get().DrawContent();
    }
    ImGui::End();

    ImGui::SetNextWindowSize(ImVec2(320, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(1080, 400), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("ViewCone##libgodot")) {
        EditorViewConePanel::Get().DrawContent();
    }
    ImGui::End();

    // ── Animation + Sequencer ────────────────────────────────────────────
    ImGui::SetNextWindowSize(ImVec2(700, 220), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, 360), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Animation##libgodot")) {
        EditorAnimationPanel::Get().DrawContent();
    }
    ImGui::End();

    ImGui::SetNextWindowSize(ImVec2(700, 220), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(20, 600), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Sequencer##libgodot")) {
        EditorSequencerPanel::Get().DrawContent();
    }
    ImGui::End();

    // ── FlowGraph (Lua event-node editor, imnodes) ──────────────────────
    ImGui::SetNextWindowSize(ImVec2(760, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(740, 460), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("FlowGraph##libgodot")) {
        EditorFlowGraphPanel::Get().DrawContent();
    }
    ImGui::End();

    // ── Terrain PCG Node Graph (imgui-node-editor) ──────────────────────
    ImGui::SetNextWindowSize(ImVec2(760, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(1520, 20), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Terrain Node Graph##libgodot")) {
        EditorNodeGraphPanel::Get().DrawContent();
    }
    ImGui::End();

    // ── GPU Profiler ─────────────────────────────────────────────────────
    ImGui::SetNextWindowSize(ImVec2(420, 380), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(1420, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("GPU Profiler##libgodot")) {
        EditorGpuProfilerPanel::Get().DrawContent();
    }
    ImGui::End();

    // ── Editor Camera settings ───────────────────────────────────────────
    ImGui::SetNextWindowSize(ImVec2(300, 300), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(1860, 520), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Camera##libgodot")) {
        EditorCameraPanel::Get().DrawContent();
    }
    ImGui::End();

    // ── Reflect-driven Entity Inspector ─────────────────────────────────
    // s_ecs_world is set every editor_panels_init()/reload cycle above —
    // exactly the same pointer the SDL3 entry.cpp passes to this same
    // DrawContent(ecs_world) call for its own "Inspector" tab.
    ImGui::SetNextWindowSize(ImVec2(380, 480), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(2280, 20), ImGuiCond_FirstUseEver);
    if (ImGui::Begin("Inspector##libgodot")) {
        EditorReflectInspector::DrawContent(s_ecs_world);
    }
    ImGui::End();

    // ── Panels that only expose Draw() (older panels_visible[]-gated
    // design, or a self-contained modal) — called as-is, no wrapper
    // Begin/End of our own (see file doc comment). ─────────────────────
    EditorAssetBrowser::Get().Draw();      // panels_visible[2], forced true in init
    EditorGraphicsPanel::Get().Draw();     // panels_visible[4], forced true in init
    EditorFlareBrowser::Get().Draw();      // panels_visible[7], forced true in init
    EditorCommandPalette::Get().Draw();    // self-contained modal, Ctrl+P to open

    return 0;  // no RTT viewport flags on this backend — see file doc comment
}

} // extern "C"
