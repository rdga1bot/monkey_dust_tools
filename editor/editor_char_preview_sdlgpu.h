#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>
#include <cstdio>

// editor_char_preview_sdlgpu — SDL_GPU character preview RTT for the standalone editor.
// T-pose md_human.glb rendered off-screen; displayed via ImGui::Image().
// RenderFrame() must be called once per frame before ImGui render (from main.cpp).
// Implementation in editor_char_preview_sdlgpu.cpp.

namespace CharPreviewSDLGPU {

bool Init(const char* glb_path, const char* tex_path);
void RenderFrame(SDL_GPUCommandBuffer* cmd);
void DrawInImGui(float W, float H,
                 float height_scale, float bulk_scale,
                 const float skin_rgb[3], float skin_str,
                 float sat, float bri,
                 float muscle = 0.f,
                 const float hair_rgb[3] = nullptr);
void ReloadPipelines();
void SetBoneScalesFromDef(const float body[18], const float face[24]);
void SetBodyMorphWeights(const float body[18], const float face[24]);
void SetMorphWeightsFromFace(const float face[], const float def[],
                              const float lo[], const float hi[]);
void SetCameraForTab(int tab);
bool LoadHairStyle(int style_idx);
void SetClothingItem(int item_idx);
void DumpState(FILE* f);
void ResetAnimPhase();

// ── Clothing constants ────────────────────────────────────────────────────────
constexpr int CLOTH_MAX_SLOTS  = 3;
constexpr int CLOTH_ITEM_COUNT = 15;

// ── Hair constants ────────────────────────────────────────────────────────────
constexpr int s_hair_style_count = 30;

// ── Public state (accessed directly by character_editor.h) ───────────────────
extern bool  s_clothes_visible;
extern int   s_cloth_sel[CLOTH_MAX_SLOTS];
extern float s_cloth_color[CLOTH_MAX_SLOTS][3];
extern int   s_hair_style;
extern const char* const s_hair_styles[30];

} // namespace CharPreviewSDLGPU
#endif // MD_SDL_GPU
