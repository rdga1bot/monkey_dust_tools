#pragma once

// RENDER-BACKEND-STAGE-6 (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.3/
// §2.4). Permanent form of probes/granite_m3_imgui_dynamic_rendering.cpp's
// proven VK_KHR_dynamic_rendering ImGui bridge -- live-verified 0
// validation errors there.
//
// Крок 4 (§2.4) change from Крок 3: operates on the real editor's OWN
// shared ImGui context (tools/editor/main.cpp's ImGui::CreateContext(),
// same one imgui_impl_sdlgpu3 normally renders) instead of a dedicated
// one -- MD_RENDER_BACKEND=GRANITE switches WHICH backend submits the
// SAME already-built draw data (main.cpp's EditorModule::Get().BuildUI()
// + ImGui::Render() are completely unchanged either way), it doesn't fork
// the UI into a second, parallel context. This is what lets the real
// editor UI chrome (toolbar/panels/menus) actually render through Granite
// -- unlike the 3D viewports (editor_world_3d_sdlgpu.cpp etc), which stay
// on their own SdlGpuBackend instances regardless (§5.4 "власний
// екземпляр") since their scene-content draw calls aren't ported.
//
// Called directly from main.cpp, NOT through md::render_backend::
// GraniteBackend::SetEditorOverlayCallback()/RenderEditorOverlay() --
// main.cpp doesn't route its own chrome through any IRenderBackend
// instance today (imgui_impl_sdlgpu3 is called directly too), so this
// matches the existing pattern rather than introducing a new one.
//
// No-ops (return false / do nothing) when compiled with MD_USE_GRANITE
// undefined (USE_GRANITE=OFF, the default) -- zero cost.
struct SDL_Window;

namespace md::editor {

// Call once after md::GraniteBackend::Get().Init(window) AND
// ImGui::CreateContext() (the real editor's own) have already succeeded,
// with that context current. Sets up the Vulkan ImGui backend
// (UseDynamicRendering=true) against the CURRENT context -- does not
// create or switch contexts itself.
bool GraniteImGuiBridge_Init(SDL_Window* window);
void GraniteImGuiBridge_Shutdown();

// Call once per frame at the SAME position ImGui_ImplSDLGPU3_NewFrame()
// normally would (before ImGui_ImplSDL3_NewFrame()/ImGui::NewFrame()) --
// mutually exclusive with that call, not in addition to it. That
// backend's own per-frame bookkeeping (descriptor pool cycling etc).
void GraniteImGuiBridge_NewFrame();

// Call once per frame, AFTER ImGui::Render() (real editor UI draw data
// already built against the current context, main.cpp's own
// EditorModule::Get().BuildUI() unchanged) -- submits ImGui::GetDrawData()
// through md::GraniteBackend::Get().RenderFrameWithOverlay(). Replaces the
// SDL_GPU path's cmd-acquire + clear-pass + ImGui_ImplSDLGPU3_PrepareDrawData/
// RenderDrawData block.
void GraniteImGuiBridge_RenderCurrentDrawData();

}  // namespace md::editor
