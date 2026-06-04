#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>

// editor_world_3d_sdlgpu — 3D world viewport for the standalone SDL_GPU editor.
// Renders Kenshi terrain + props to an RTT; display via ImGui::Image().
// Implementation in editor_world_3d_sdlgpu.cpp.

namespace WorldEditor3D_SDLGPU {

bool Init(const char* overlay_path, int zone_ox = 28, int zone_oz = 28);
void RenderFrame(SDL_GPUCommandBuffer* cmd, float dt, bool tab_active = false);
void DrawImGui(float W, float H, float dt);
void UploadTerrainHeightmap(const float* hmap, int W, int H,
                            float world_size_m, int chunk_x, int chunk_z);

// Runtime camera config — called from Settings panel after load/save.
void  ApplyCameraConfig(float wasd_speed, float scroll_step, float zoom_in, float zoom_out);
float GetWasdSpeed();
float GetScrollStep();
float GetZoomIn();
float GetZoomOut();

} // namespace WorldEditor3D_SDLGPU
#endif // MD_SDL_GPU
