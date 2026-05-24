#pragma once
#ifdef MONKEY_DUST_EDITOR
// Thin bridge so other editor panels can trigger a 3D viewport heightmap update
// without #including the heavy editor_world_3d_sdlgpu.h (static-namespace header).
void EditorW3D_UploadTerrainHeightmap(const float* hmap, int W, int H,
                                       float world_size_m, int chunk_x, int chunk_z);
#endif
