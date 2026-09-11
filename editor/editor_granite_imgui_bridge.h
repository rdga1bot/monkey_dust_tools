#pragma once

// RENDER-BACKEND-STAGE-6 (docs/GRANITE_IRENDERBACKEND_INTEGRATION.md §2.3).
// Permanent form of probes/granite_m3_imgui_dynamic_rendering.cpp's proven
// VK_KHR_dynamic_rendering ImGui bridge -- live-verified 0 validation
// errors there. Wired through md::render_backend::GraniteBackend's
// RenderEditorOverlay() (SetEditorOverlayCallback), not called directly by
// the real editor's own render loop yet (that wiring, and
// MD_RENDER_BACKEND=GRANITE runtime selection, is Крок 4 -- out of scope
// here per §4 "ЩО НЕ РОБИТИ").
//
// Uses its OWN dedicated ImGui context (ImGui::CreateContext(), separate
// from the real editor's SDL_GPU-backed one) -- consistent with
// md::GraniteBackend's whole M2 dual-run design: a second, independent
// Vulkan device/WSI on the SAME shared SDL_Window as the SDL_GPU path, not
// a replacement for it. Two ImGui contexts drawing to two different GPU
// devices on one window is the reason for the separate context, not an
// oversight.
//
// No-ops (return false / do nothing) when compiled with MD_USE_GRANITE
// undefined (USE_GRANITE=OFF, the default) -- zero cost, matches
// md::GraniteBackend's own pattern.
#include <monkey_dust/render/backend/render_backend_types.h>

struct SDL_Window;

namespace md::editor {

// Call once after md::GraniteBackend::Get().Init(window) has already
// succeeded (same window). Sets up a dedicated ImGui context + the Vulkan
// backend (UseDynamicRendering=true).
bool GraniteImGuiBridge_Init(SDL_Window* window);
void GraniteImGuiBridge_Shutdown();

// BackendStageFn-compatible -- pass to md::render_backend::GraniteBackend::
// SetEditorOverlayCallback(&GraniteImGuiBridge_RenderEditorOverlay, nullptr).
// Draws a minimal proof-of-life overlay (matches the probe's own demo
// content -- no real editor UI ported here, that's a separate future
// phase); switches to the bridge's own ImGui context, runs a full
// NewFrame/content/Render cycle, then calls md::GraniteBackend::Get().
// RenderFrameWithOverlay() to submit it.
void GraniteImGuiBridge_RenderEditorOverlay(void* user, const md::render_backend::RenderFrameParams& params);

}  // namespace md::editor
