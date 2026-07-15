#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>

// Autonomy system (Etap 4, md.editor_screenshot) — real SDL_GPU swapchain
// readback + PNG encode. No existing precedent in this codebase (grepped:
// zero SDL_DownloadFromGPUTexture call sites anywhere before this) — new
// GPU code, tested incrementally per CLAUDE.md's GPU debug discipline.
//
// Usage (called from tools/editor/main.cpp's render block, once per frame,
// right before the existing SDL_SubmitGPUCommandBuffer(cmd) call):
//   char path[256];
//   if (EditorScreenshot_ConsumePending(path, sizeof(path))) {
//       EditorScreenshot_CaptureAndSubmit(dev, cmd, swapchain_tex, sw, sh, path);
//       // CaptureAndSubmit submits cmd itself (via a fence-waited submit) —
//       // do NOT also call SDL_SubmitGPUCommandBuffer(cmd) this frame.
//   } else {
//       SDL_SubmitGPUCommandBuffer(cmd);
//   }

// Called by md.editor_screenshot(label) — stores the request, returns
// immediately (actual capture happens later in the SAME frame, in main.cpp,
// where the swapchain texture is available). One-shot: consuming clears it.
void EditorScreenshot_RequestPending(const char* path);

// Returns true and fills out_path if a screenshot was requested this frame
// (and clears the pending flag — one-shot).
bool EditorScreenshot_ConsumePending(char* out_path, int out_path_size);

// Peek without consuming — task #158f: game's RunScenarioMode (--exec) runs
// a pure logic-tick loop with NO rendering by design (speed, see
// scenario_driver.cpp's "--fast: no sleep, no real-time pacing, no
// rendering" comment), so md.screenshot's pending request has nothing to
// trigger it. The driver checks this flag each tick to decide whether to
// pay for one real render+capture frame, without disturbing every other
// scenario's fast no-render ticks.
bool EditorScreenshot_HasPending();

// Records a copy-to-transfer-buffer pass on cmd (source: swapchain_tex),
// then submits cmd via SDL_SubmitGPUCommandBufferAndAcquireFence + waits on
// the fence (synchronous — acceptable here, this is an on-demand scripted
// action, not a per-frame operation), maps the transfer buffer, encodes PNG
// via stb_image_write, and cleans up all GPU resources it created. Returns
// false (logs the SDL_GetError()) on any failure — never silently drops a
// requested screenshot without saying why.
bool EditorScreenshot_CaptureAndSubmit(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd,
                                       SDL_GPUTexture* swapchain_tex,
                                       uint32_t w, uint32_t h,
                                       SDL_GPUTextureFormat fmt,
                                       const char* out_path);
#endif // MD_SDL_GPU
