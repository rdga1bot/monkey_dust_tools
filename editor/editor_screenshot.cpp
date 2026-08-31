#include "editor_screenshot.h"
#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
#include <SDL3/SDL.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include "stb_image_write.h"

static char s_pending_path[256] = {};

void EditorScreenshot_RequestPending(const char* path) {
    strncpy(s_pending_path, path, sizeof(s_pending_path) - 1);
}

bool EditorScreenshot_HasPending() {
    return s_pending_path[0] != '\0';
}

bool EditorScreenshot_ConsumePending(char* out_path, int out_path_size) {
    if (s_pending_path[0] == '\0') return false;
    strncpy(out_path, s_pending_path, out_path_size - 1);
    out_path[out_path_size - 1] = '\0';
    s_pending_path[0] = '\0';  // one-shot
    return true;
}

bool EditorScreenshot_CaptureAndSubmit(SDL_GPUDevice* dev, SDL_GPUCommandBuffer* cmd,
                                       SDL_GPUTexture* swapchain_tex,
                                       uint32_t w, uint32_t h,
                                       SDL_GPUTextureFormat fmt,
                                       const char* out_path) {
    if (!dev || !cmd || !swapchain_tex || !w || !h) {
        fprintf(stderr, "[EditorScreenshot] invalid args (dev/cmd/tex null or zero size)\n");
        return false;
    }

    // Row pitch must match the destination buffer's tight-packed RGBA8 layout
    // stb_image_write expects — SDL_GPU may pad rows internally for some
    // backends, but pixels_per_row below tells it our transfer buffer layout,
    // not the source texture's, so this stays tightly packed regardless.
    uint32_t download_size = w * h * 4;
    SDL_GPUTransferBufferCreateInfo tbi{};
    tbi.usage = SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD;
    tbi.size  = download_size;
    SDL_GPUTransferBuffer* tb = SDL_CreateGPUTransferBuffer(dev, &tbi);
    if (!tb) {
        fprintf(stderr, "[EditorScreenshot] SDL_CreateGPUTransferBuffer failed: %s\n", SDL_GetError());
        return false;
    }

    GpuCopyPass cp;
    cp.Begin(cmd);
    if (!cp.SDLPass()) {
        fprintf(stderr, "[EditorScreenshot] SDL_BeginGPUCopyPass failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        return false;
    }
    SDL_GPUTextureRegion src{};
    src.texture = swapchain_tex;
    src.w = w; src.h = h; src.d = 1;
    SDL_GPUTextureTransferInfo dst{};
    dst.transfer_buffer = tb;
    dst.pixels_per_row  = w;
    dst.rows_per_layer  = h;
    cp.DownloadTexture(src, dst);
    cp.End();

    // Fence-waited submit — SDL docs' own documented pattern for
    // DownloadFromGPUTexture specifically (data not guaranteed copied until
    // the fence signals). Synchronous stall is fine: this is an on-demand
    // scripted action (md.editor_screenshot), not a per-frame operation.
    // GEOCLIPMAP Phase 7 (2026-08-16): this fence-wait is ALREADY the exact
    // "submit + synchronous wait" pattern GpuDevice::Submit's SetSyncTiming
    // path uses -- report it there too, so md.get_gpu_ms() stays meaningful
    // on frames that only rendered because a screenshot was pending
    // (RunScenarioMode/--exec never renders otherwise, see this function's
    // only caller, npc_render_deferred.cpp). Without this, GpuDevice::
    // Submit() -- and therefore LastGpuMs() -- is simply never reached on
    // those frames (this function owns the whole submit, deliberately: it
    // must append its own copy-pass to `cmd` before submitting).
    bool sync_timing = md::GpuDevice::Get().SyncTiming();
    uint64_t t0 = sync_timing ? SDL_GetPerformanceCounter() : 0;
    SDL_GPUFence* fence = md::GpuDevice::Get().SubmitAndAcquireFence(cmd);
    // This path submits `cmd` itself instead of going through GpuDevice::
    // Submit() -- report completion so HasActiveCommandBuffer()'s frame-
    // resize tripwire (gpu_device.h) doesn't stay stuck true afterward.
    md::GpuDevice::Get().NotifyCommandBufferSubmitted();
    if (!fence) {
        fprintf(stderr, "[EditorScreenshot] SDL_SubmitGPUCommandBufferAndAcquireFence failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        return false;
    }
    bool waited = md::GpuDevice::Get().WaitForFence(fence);
    if (sync_timing) {
        uint64_t t1 = SDL_GetPerformanceCounter();
        uint64_t freq = SDL_GetPerformanceFrequency();
        md::GpuDevice::Get().RecordExternalGpuMs(
            freq ? (float)((double)(t1 - t0) * 1000.0 / (double)freq) : 0.f);
    }
    md::GpuDevice::Get().ReleaseFence(fence);
    if (!waited) {
        fprintf(stderr, "[EditorScreenshot] SDL_WaitForGPUFences failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        return false;
    }

    void* mapped = SDL_MapGPUTransferBuffer(dev, tb, false);
    if (!mapped) {
        fprintf(stderr, "[EditorScreenshot] SDL_MapGPUTransferBuffer failed: %s\n", SDL_GetError());
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        return false;
    }

    // Swapchain format varies by backend/driver — SDL_GPU_TEXTUREFORMAT_
    // B8G8R8A8_UNORM (common on Vulkan/ANV) needs an R/B swap for stb_image_
    // write's expected RGBA order; R8G8B8A8_UNORM needs none. Checked against
    // the actual format (SDL_GetGPUSwapchainTextureFormat, passed in by the
    // caller), not assumed — a wrong guess here silently swaps every
    // screenshot's colors, which would corrupt every baseline it's compared
    // against without ever producing an error.
    bool need_bgr_swap = (fmt == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
    uint8_t* src_px = (uint8_t*)mapped;
    uint8_t* rgba = (uint8_t*)malloc(download_size);
    if (!rgba) {
        fprintf(stderr, "[EditorScreenshot] malloc failed for %u bytes\n", download_size);
        SDL_UnmapGPUTransferBuffer(dev, tb);
        SDL_ReleaseGPUTransferBuffer(dev, tb);
        return false;
    }
    if (need_bgr_swap) {
        for (uint32_t i = 0; i < w * h; ++i) {
            rgba[i*4+0] = src_px[i*4+2];  // R <- B
            rgba[i*4+1] = src_px[i*4+1];  // G
            rgba[i*4+2] = src_px[i*4+0];  // B <- R
            rgba[i*4+3] = src_px[i*4+3];  // A
        }
    } else {
        memcpy(rgba, src_px, download_size);
    }
    SDL_UnmapGPUTransferBuffer(dev, tb);
    SDL_ReleaseGPUTransferBuffer(dev, tb);

    int ok = stbi_write_png(out_path, (int)w, (int)h, 4, rgba, (int)(w * 4));
    free(rgba);
    if (!ok) {
        fprintf(stderr, "[EditorScreenshot] stbi_write_png failed: %s\n", out_path);
        return false;
    }
    fprintf(stdout, "[EditorScreenshot] saved %ux%u -> %s\n", w, h, out_path);
    return true;
}
#endif // MD_SDL_GPU
