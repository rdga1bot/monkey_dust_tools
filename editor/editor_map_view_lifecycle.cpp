#include "editor_map_view.h"
#ifndef MD_SDL_GPU
#  include "glad.h"
#else
#  include <monkey_dust/render/gpu_hal.h>
#endif
#include <monkey_dust/flare/tile_map.h>

// ── RTT management ────────────────────────────────────────────────────────────

void MapViewPanel::EnsureRT(int w, int h) {
    if (rt_ok_ && rt_w_ == w && rt_h_ == h) return;
#ifndef MD_SDL_GPU
    if (rt_fbo_) {
        glDeleteFramebuffers(1, &rt_fbo_);
        glDeleteTextures(1, &rt_tex_);
        glDeleteRenderbuffers(1, &rt_depth_);
        rt_fbo_ = rt_tex_ = rt_depth_ = 0;
    }
    glGenFramebuffers(1, &rt_fbo_);
    glGenTextures(1, &rt_tex_);
    glGenRenderbuffers(1, &rt_depth_);

    glBindTexture(GL_TEXTURE_2D, rt_tex_);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);

    glBindRenderbuffer(GL_RENDERBUFFER, rt_depth_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);

    glBindFramebuffer(GL_FRAMEBUFFER, rt_fbo_);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, rt_tex_, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, rt_depth_);
    rt_ok_ = (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#else
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (rt_color_) { GpuReleaseTexture(dev, rt_color_); rt_color_ = nullptr; }
    rt_depth_.Shutdown();

    SDL_GPUTextureCreateInfo ci = {};
    ci.type   = SDL_GPU_TEXTURETYPE_2D;
    ci.width  = (uint32_t)w;
    ci.height = (uint32_t)h;
    ci.layer_count_or_depth = 1;
    ci.num_levels = 1;
    ci.sample_count = SDL_GPU_SAMPLECOUNT_1;
    ci.format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.usage  = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    rt_color_ = GpuCreateTexture(dev, &ci);

    // GpuDepthTexture, not raw D24_UNORM: D24_UNORM caused a GPU hang on
    // Intel Gen9 (docs/HAL_CLOSURE_INVENTORY.md §5) -- this was the one
    // depth texture in the codebase missing that fix.
    rt_depth_.Init(w, h);

    rt_ok_ = (rt_color_ != nullptr && rt_depth_.SDLTexture() != nullptr);
#endif
    rt_w_ = w;
    rt_h_ = h;
}

// ── Init / Shutdown ───────────────────────────────────────────────────────────

void MapViewPanel::Init() {
    if (init_) return;
    md::flare::TileMap2DRenderer::Get().Init();
    LoadMap(path_buf_);
    init_ = true;
}

void MapViewPanel::Shutdown() {
    if (!init_) return;
#ifndef MD_SDL_GPU
    if (rt_fbo_) {
        glDeleteFramebuffers(1, &rt_fbo_);
        glDeleteTextures(1, &rt_tex_);
        glDeleteRenderbuffers(1, &rt_depth_);
        rt_fbo_ = rt_tex_ = rt_depth_ = 0;
    }
#else
    md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
    if (rt_color_) { GpuReleaseTexture(dev, rt_color_); rt_color_ = nullptr; }
    rt_depth_.Shutdown();
#endif
    rt_ok_ = false;
    md::flare::TileMap2DRenderer::Get().Shutdown();
    init_ = false;
}

// ── SDL_GPU: render tile map to RTT (called from main.cpp before ImGui) ───────

#ifdef MD_SDL_GPU
void MapViewPanel::RenderFrame(md::GpuCommandBufferHandle cmd) {
    if (!init_ || !loaded_ || !rt_ok_ || !rt_color_) return;
    GpuCommandBuffer clear_cb;
    GpuCommandBuffer::ColorPassDesc clear_cpd;
    clear_cpd.cmd = cmd;
    clear_cpd.color_tex[0] = rt_color_;
    clear_cpd.clear_color[0] = 20/255.f; clear_cpd.clear_color[1] = 20/255.f;
    clear_cpd.clear_color[2] = 30/255.f; clear_cpd.clear_color[3] = 1.f;
    clear_cb.BeginColorPass(clear_cpd);
    clear_cb.EndPass();
    md::flare::TileMap2DRenderer::Get().RenderToTarget(
        map_, now_s_,
        origin_x_, origin_y_, scale_,
        rt_w_, rt_h_, LayerMask(),
        cmd, rt_color_);
}
#endif

// ── Undo / Redo ───────────────────────────────────────────────────────────────

void MapViewPanel::ClearHistory() {
    undo_top_ = 0;
    redo_top_ = 0;
}

void MapViewPanel::PushUndo(const PaintOp& op) {
    if (undo_top_ == UNDO_MAX) {
        for (int i = 0; i < UNDO_MAX - 1; i++) undo_stack_[i] = undo_stack_[i + 1];
        undo_top_ = UNDO_MAX - 1;
    }
    undo_stack_[undo_top_++] = op;
    redo_top_ = 0;
}

void MapViewPanel::Undo() {
    if (undo_top_ == 0 || !loaded_) return;
    PaintOp op = undo_stack_[--undo_top_];
    if (op.type == OpType::FLOOD) {
        auto& s = snap_pool_[op.count];
        for (int i = 0; i < md::flare::MAX_MAP_WIDTH * md::flare::MAX_MAP_HEIGHT; i++)
            map_.layers[op.layer].tiles[i] = s.before[i];
    } else {
        for (int i = 0; i < op.count; i++)
            map_.layers[op.layer].tiles[op.cells[i].row * md::flare::MAX_MAP_WIDTH + op.cells[i].col] = op.cells[i].old_val;
    }
    if (redo_top_ < UNDO_MAX) redo_stack_[redo_top_++] = op;
}

void MapViewPanel::Redo() {
    if (redo_top_ == 0 || !loaded_) return;
    PaintOp op = redo_stack_[--redo_top_];
    if (op.type == OpType::FLOOD) {
        auto& s = snap_pool_[op.count];
        for (int i = 0; i < md::flare::MAX_MAP_WIDTH * md::flare::MAX_MAP_HEIGHT; i++)
            map_.layers[op.layer].tiles[i] = s.after[i];
    } else {
        for (int i = 0; i < op.count; i++)
            map_.layers[op.layer].tiles[op.cells[i].row * md::flare::MAX_MAP_WIDTH + op.cells[i].col] = op.cells[i].new_val;
    }
    if (undo_top_ < UNDO_MAX) undo_stack_[undo_top_++] = op;
}
