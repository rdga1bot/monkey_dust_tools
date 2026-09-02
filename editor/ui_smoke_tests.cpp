// EDITOR_AUTOMATION_PLAN_v1.md Phase 5: ImGui Test Engine panel smoke
// tests. See ui_smoke_tests.h's doc comment for scope/rationale.
#include "ui_smoke_tests.h"

#include "imgui.h"
#include "backends/imgui_impl_sdl3.h"
#include "backends/imgui_impl_sdlgpu3.h"
#include "imgui_test_engine/imgui_te_engine.h"
#include "imgui_test_engine/imgui_te_context.h"
#include "imgui_test_engine/imgui_te_utils.h"

#include <monkey_dust/platform/window.h>
#include <monkey_dust/platform/input.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/scripting/lua_system.h>

#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>

// Same C ABI editor_module.cpp dlsym's — declared directly here since
// MD_UI_TESTS always links these symbols straight into monkey_dust_editor
// (editor_panels_entry.cpp is part of EDITOR_PANEL_SOURCES unconditionally),
// so no dlopen indirection is needed for this in-process test driver.
extern "C" {
void editor_panels_init(void* ctx, void* ecs_world, void* gpu, void* window,
                         float overlay_top, const char* layout_path, void* lua_system);
void editor_panels_shutdown(const char* layout_path);
uint32_t editor_panels_build_ui(float dt, float toolbar_h,
                                 char* status_msg, float* status_timer);
void editor_panels_render(void* cmd, float dt, uint32_t active_flags);
}

namespace {

// Mirrors editor_panels_entry.cpp's tab bar + Detach/Dock button names
// exactly — there is no generic way to discover "which panels exist and
// what their buttons are called" from outside, so this table is the
// thing to update if a panel's tab label or button name changes.
struct PanelCase {
    const char* test_name;     // ImGuiTestEngine test name (no spaces)
    const char* tab_label;     // BeginTabItem() label under "##editor/##tabs"
    const char* detach_btn;    // nullptr if the panel has no Detach button
    const char* float_window;  // e.g. "Items##float" (only used if detach_btn set)
    const char* dock_btn;      // button inside float_window
};

const PanelCase kPanels[] = {
    {"items",      "Items",      "Detach##items", "Items##float",           "Dock##items"},
    {"factions",   "Factions",   "Detach##fac",   "Factions##float",        "Dock##fac"},
    {"map",        "Map",        "Detach##map",   "Map##float",             "Dock##map"},
    {"world",      "World",      "Detach##world", "World##float",           "Dock##world"},
    {"world3d",    "3D World",   nullptr,         nullptr,                  nullptr},
    {"npcs",       "NPCs",       "Detach##npcs",  "NPC Archetypes##float",  "Dock##npcs"},
    {"characters", "Characters", "Detach##chars", "Characters##float",     "Dock##chars"},
    {"inspector",  "Inspector",  nullptr,         nullptr,                  nullptr},
    {"settings",   "Settings",   "Detach##set",   "Settings##float",        "Dock##set"},
};
constexpr int kPanelCount = sizeof(kPanels) / sizeof(kPanels[0]);

// open tab -> 2 frames -> detach (if supported) -> 2 frames -> dock -> 2
// frames -> no crash/assert. Deliberately does NOT assert anything about
// panel CONTENT (values, layout) -- ImGuiTestEngine already fails the test
// on its own if an ImGui ID/assert/crash happens during any of these
// steps; that failure IS the check.
void PanelSmokeTestFunc(ImGuiTestContext* ctx) {
    const PanelCase& p = *static_cast<const PanelCase*>(ctx->Test->UserData);

    char tab_ref[80];
    snprintf(tab_ref, sizeof(tab_ref), "##tabs/%s", p.tab_label);

    ctx->SetRef("##editor");
    ctx->ItemClick(tab_ref);
    ctx->Yield(2);

    if (p.detach_btn) {
        // BeginTabItem() pushes the tab's own ID scope over its content
        // (imgui_widgets.cpp: PushOverrideID(tab->ID)) -- Detach##items
        // etc. are NOT direct children of "##editor", they're nested
        // under "##tabs/<Label>" too. Found by running this and getting
        // "Unable to locate item: '##editor/Detach##items'" every time.
        char detach_ref[96];
        snprintf(detach_ref, sizeof(detach_ref), "##tabs/%s/%s", p.tab_label, p.detach_btn);
        ctx->SetRef("##editor");
        ctx->ItemClick(detach_ref);
        ctx->Yield(2);
        ctx->SetRef(p.float_window);
        ctx->ItemClick(p.dock_btn);
        ctx->Yield(2);
    }
}

void RegisterPanelSmokeTests(ImGuiTestEngine* engine) {
    // ImGuiTest::Name/Category store the RAW pointer, not a copy (see its
    // own doc comment: "Literal, not owned") -- registering with a
    // snprintf'd char[64] LOCAL to this loop was a real bug caught while
    // building this: every test ended up with a dangling Name pointer
    // aliasing the same reused stack slot, so all 9 tests displayed the
    // LAST-formatted name and (via the test engine's name-based lookup)
    // only the first-registered test's UserData ever actually ran, 9
    // times over. kPanels[i].test_name is a string literal (static
    // storage) -- use it directly, no formatting needed.
    for (int i = 0; i < kPanelCount; ++i) {
        ImGuiTest* t = ImGuiTestEngine_RegisterTest(engine, "panels", kPanels[i].test_name, __FILE__, __LINE__);
        t->TestFunc = PanelSmokeTestFunc;
        t->UserData = const_cast<PanelCase*>(&kPanels[i]);
    }
}

} // namespace

int RunUiSmokeTests(md::GpuDeviceHandle gpu, SDL_GPUTextureFormat sc_fmt,
                     float overlay_top, const char* /*layout_path*/) {
    // Deliberately ignore the caller's real layout_path (data/editor_layout.json)
    // and pass nullptr instead -- editor_panels_init/shutdown both skip
    // loading/saving entirely when layout_path is null (see
    // editor_panels_entry.cpp's "if (layout_path && layout_path[0])"
    // guards), so every panel starts from its DEFAULT (docked/not
    // detached) state every run, regardless of whatever a developer's
    // real layout file currently has persisted (found the hard way: the
    // real file in this repo has items/factions/map/settings already
    // detached from earlier manual sessions, which made every
    // Detach-button test fail non-deterministically depending on when it
    // was run). This also means the smoke test run never touches/
    // overwrites the developer's real layout file.
    editor_panels_init(ImGui::GetCurrentContext(), Registry::Get().c_ptr(),
                        gpu, _wnd::ptr(), overlay_top, nullptr, &LuaSystem::Get());

    ImGuiTestEngine* engine = ImGuiTestEngine_CreateContext();
    ImGuiTestEngineIO& test_io = ImGuiTestEngine_GetIO(engine);
    test_io.ConfigVerboseLevel = ImGuiTestVerboseLevel_Info;
    test_io.ConfigVerboseLevelOnError = ImGuiTestVerboseLevel_Debug;
    ImGuiTestEngine_Start(engine, ImGui::GetCurrentContext());
    ImGuiTestEngine_InstallDefaultCrashHandler();

    RegisterPanelSmokeTests(engine);
    ImGuiTestEngine_QueueTests(engine, ImGuiTestGroup_Tests, nullptr);

    char status_msg[64] = {};
    float status_timer = 0.f;
    uint64_t last_ticks = SDL_GetTicks();
    uint32_t prev_flags = 0;

    // Watchdog: 9 panels x a handful of yields each is well under 200
    // frames even at Fast speed; 3000 is a generous ceiling so a genuinely
    // stuck test times out instead of hanging CI forever.
    constexpr int kMaxFrames = 3000;
    int frame = 0;
    for (; frame < kMaxFrames; ++frame) {
        if (ImGuiTestEngine_IsTestQueueEmpty(engine)) break;

        {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                ImGui_ImplSDL3_ProcessEvent(&ev);
            }
            input_begin_frame();
        }

        uint64_t now = SDL_GetTicks();
        float dt = (float)(now - last_ticks) / 1000.f;
        if (dt <= 0.f) dt = 1.f / 60.f;
        last_ticks = now;
        if (status_timer > 0.f) status_timer -= dt;

        window_begin_frame();
        ImGui_ImplSDLGPU3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();

        uint32_t new_flags = editor_panels_build_ui(dt, ImGui::GetFrameHeight() + 30.f,
                                                      status_msg, &status_timer);

        ImGui::Render();
        md::GpuCommandBufferHandle cmd = md::GpuDevice::Get().AcquireCommandBuffer();
        if (cmd) {
            editor_panels_render(cmd, dt, prev_flags);
            uint32_t sw = 0, sh = 0;
            SDL_GPUTexture* sc = md::GpuDevice::Get().AcquireSwapchainTexture(cmd, &sw, &sh);
            if (sc) {
                GpuCommandBuffer clear_cb;
                GpuCommandBuffer::ColorPassDesc clear_cpd;
                clear_cpd.cmd = cmd;
                clear_cpd.color_tex[0] = sc;
                clear_cpd.clear_color[0] = 0.10f; clear_cpd.clear_color[1] = 0.10f;
                clear_cpd.clear_color[2] = 0.13f; clear_cpd.clear_color[3] = 1.f;
                clear_cb.BeginColorPass(clear_cpd);
                clear_cb.EndPass();
                ImDrawData* dd = ImGui::GetDrawData();
                if (dd && dd->CmdListsCount > 0) {
                    ImGui_ImplSDLGPU3_PrepareDrawData(dd, cmd);
                    GpuCommandBuffer imgui_cb;
                    GpuCommandBuffer::ColorPassDesc imgui_cpd;
                    imgui_cpd.cmd = cmd;
                    imgui_cpd.color_tex[0] = sc;
                    imgui_cpd.load_color = true;
                    imgui_cb.BeginColorPass(imgui_cpd);
                    if (imgui_cb.SDLPass()) ImGui_ImplSDLGPU3_RenderDrawData(dd, cmd, imgui_cb.SDLPass());
                    imgui_cb.EndPass();
                }
            }
            md::GpuDevice::Get().Submit(cmd);
        }
        window_end_frame();
        prev_flags = new_flags;

        ImGuiTestEngine_PostSwap(engine);
    }

    bool timed_out = (frame >= kMaxFrames);

    ImGuiTestEngineResultSummary summary;
    ImGuiTestEngine_GetResultSummary(engine, &summary);

    // Per-test detail — ImGuiTestEngine only writes to test->Output.Log (an
    // in-memory buffer meant for its own on-screen window, which this
    // headless driver never shows), so without this loop a failure would
    // only ever show up as an aggregate "success=N/9" with zero indication
    // of WHICH panel or WHY.
    for (int i = 0; i < kPanelCount; ++i) {
        const char* name = kPanels[i].test_name;
        ImGuiTest* t = ImGuiTestEngine_FindTestByName(engine, "panels", name);
        if (!t) { fprintf(stderr, "[UiSmokeTests] %s: NOT FOUND\n", name); continue; }
        const char* status = (t->Output.Status == ImGuiTestStatus_Success) ? "OK" : "FAIL";
        fprintf(stdout, "[UiSmokeTests] %-16s %s\n", name, status);
        if (t->Output.Status != ImGuiTestStatus_Success && !t->Output.Log.IsEmpty()) {
            fprintf(stderr, "%s\n", t->Output.Log.GetText());
        }
    }

    ImGuiTestEngine_Stop(engine);
    editor_panels_shutdown(nullptr);
    ImGuiTestEngine_DestroyContext(engine);

    fprintf(stdout, "[UiSmokeTests] tested=%d success=%d in_queue=%d%s\n",
            summary.CountTested, summary.CountSuccess, summary.CountInQueue,
            timed_out ? " (TIMED OUT)" : "");

    if (timed_out || summary.CountInQueue > 0 || summary.CountSuccess != summary.CountTested
        || summary.CountTested != kPanelCount) {
        fprintf(stderr, "[UiSmokeTests] FAILED\n");
        return 1;
    }
    fprintf(stdout, "[UiSmokeTests] PASS (%d/%d panels)\n", summary.CountSuccess, kPanelCount);
    return 0;
}
