#pragma once
#ifdef MD_SDL_GPU
#include <SDL3/SDL_gpu.h>

// editor_world_3d_sdlgpu — 3D world viewport for the standalone SDL_GPU editor.
// Renders Kenshi terrain + props to an RTT; display via ImGui::Image().
// Implementation in editor_world_3d_sdlgpu.cpp.

namespace WorldEditor3D_SDLGPU {

bool Init(const char* overlay_path, int zone_ox = 28, int zone_oz = 28);
// Joins the background loader thread spawned by Init(). MUST be called
// before GpuDevice::Shutdown() — see this header's Init() doc and the
// definition site in editor_world_3d_sdlgpu.cpp for the race this fixes.
void Shutdown();
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

// Autonomy system (md.editor_select_zone) — jumps the free-fly camera to the
// centre of the given zone (500m grid, same convention as the 'T' hotkey's
// hardcoded Hub position: zone (23,29) == world (11750,14250)).
void TeleportToZone(int zone_x, int zone_z);

// Autonomy system (md.editor_camera_pos) — full camera pose control for
// visual diagnostics (e.g. inspecting the POM/forward shader-pass
// crossfade boundary, which TeleportToZone's fixed altitude/look-down
// pitch can't get close enough to see). pitch in radians, same convention
// as s_pitch (0.38 = default look-down angle, clamped [-0.3, 1.3]
// elsewhere in this file — passing out of range here is NOT clamped,
// caller's responsibility, this is a diagnostic tool not player input).
void SetCameraPos(float x, float y, float z, float yaw, float pitch);

// Autonomy system (md.editor_terrain_op("chunks_loaded")) — progress counter
// for the full 64x64=4096-chunk world build (background thread, see
// s_chunks_built in editor_world_3d_sdlgpu.cpp). Designed for ~0.5s at 60fps;
// observed much slower in this session's environment (~6fps) — scenarios
// waiting on this should use a generous timeout, not assume the design target.
int GetChunksLoaded();
int GetChunksTotal();

// terrain-vt Phase 1 verification only (md.editor_vt_debug("fill"/"dump")) --
// NOT part of the shipping render path yet (Phase 4 wires real shading
// consumption, Phase 2 wires real visibility-driven requests). Lazily
// inits a TerrainVtPageCache on first call. VtDebugFill requests a small
// neighborhood of pages around the current camera position at tier 0 and
// flushes them via its own acquired+submitted command buffer, returning
// the cache's total resident count (or -1 if terrain/cache isn't ready).
// VtDebugDump blocks (fence-waited readback) writing the whole physical
// atlas to a PNG for visual inspection.
int  VtDebugFill();
bool VtDebugDump(const char* out_png_path);
// terrain-vt Phase 2 verification: total resident page count in the SAME
// cache the real per-frame visibility loop (s_update_granite_terrain)
// now drives via RequestPage/FlushFillQueue every frame -- lets a --exec
// scenario confirm the live wiring actually fills pages while flying the
// camera around, without needing VtDebugFill's manual neighborhood probe.
int VtResidentCount();
int VtEvictionCount();

} // namespace WorldEditor3D_SDLGPU
#endif // MD_SDL_GPU
