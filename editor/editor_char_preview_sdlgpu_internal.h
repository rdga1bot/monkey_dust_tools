#pragma once
// Shared state/declarations for editor_char_preview_sdlgpu.cpp's split
// translation units (editor_char_preview_anim.cpp, _assets.cpp, _runtime.cpp).
// Own #ifdef MD_SDL_GPU is self-closed (GCC rejects one left open across a
// header/includer boundary); each .cpp re-opens its own around its content.
// The `namespace CharPreviewSDLGPU {` below is deliberately left OPEN --
// unlike #ifdef this is plain C++ scoping, not a per-file preprocessor
// constraint, so each .cpp's own trailing `}` closes it correctly.

#include "editor_char_preview_sdlgpu.h"

#ifdef MD_SDL_GPU
// editor_char_preview_sdlgpu.h — SDL_GPU character preview RTT for the standalone editor.
// T-pose md_human.glb rendered to an off-screen texture, displayed via ImGui::Image.
// RenderFrame() must be called once per frame before ImGui render (from main.cpp).
// DrawInImGui() is called inside the Characters tab.

#include "imgui.h"
#include <monkey_dust/platform/window.h>   // _wnd::ptr() for SDL_WarpMouseInWindow
#include <monkey_dust/render/hair_shading.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
#include <monkey_dust/render/skin_mesh.h>
#include <monkey_dust/render/char_customization.h>
#include <monkey_dust/render/ozz_animator.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "cgltf.h"
#include "stb_image.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <initializer_list>

namespace CharPreviewSDLGPU {

// ── Vertex layout: pos(12)+norm(12)+uv(8)+joints_u8x4(4)+weights_f4(16) = stride 52 ──
struct Vtx { float px,py,pz, nx,ny,nz, u,v; uint8_t ji[4]; float wt[4]; };

// ── Uniform structs ───────────────────────────────────────────────────────────
struct VU { float mvp[16]; };  // 64 bytes, set=1 — normalMat removed (LBS handles normals)
struct FU {                                          // 48 bytes, set=3
    float skin[3]; float str;
    float sat; float bri; float muscle; float pad;
    float hair[3]; float hairpad;
};

// ── State ─────────────────────────────────────────────────────────────────────
extern GpuPipeline     s_bg_pipeline;
extern GpuTexture      s_bg_sand;   // desert_sand.jpg — ground texture
extern GpuTexture      s_bg_dune;   // desert_dune.jpg — large-scale dune pattern
extern GpuPipeline     s_scene_pipeline;  // anthropometer pole, flat-color
extern GpuStaticBuffer s_scene_vbo;
extern GpuStaticBuffer s_scene_ibo;
extern int             s_scene_ni;
extern GpuPipeline     s_pipeline;
extern GpuStaticBuffer s_vbo;
extern GpuStaticBuffer s_ibo;
extern GpuTexture      s_tex;
extern GpuTexture      s_tex_head;    // head/face diffuse (V<0 UV island)
extern GpuTexture      s_tex_muscle;  // 1×1 neutral muscle mask
extern GpuTexture      s_tex_blood;   // 1×1 clear blood overlay
// Bone scale texture: 30×1 RGBA32F — raw SDL_GPU (GpuTexture only supports RGBA8)
extern md::GpuTextureHandle s_bones_tex;
extern SDL_GPUSampler* s_bones_sampler;
extern int             s_ni;
extern bool            s_ok;

// ── Hair system ───────────────────────────────────────────────────────────────
struct HairFU { float hair[3]; float pad; };   // set=3, 16 bytes
extern GpuPipeline     s_hair_pipeline;
extern GpuStaticBuffer s_hair_vbo;
extern GpuStaticBuffer s_hair_ibo;
extern int             s_hair_ni;     // index count of current style
// s_hair_style / s_hair_styles already declared extern by editor_char_preview_sdlgpu.h
// (public API); real definitions live in editor_char_preview_assets.cpp.

// ── Clothing system ───────────────────────────────────────────────────────────
struct ClothFU   { float color[3]; float pad; };
struct ClothSlot { GpuStaticBuffer vbo, ibo; int ni = 0; bool loaded = false; };
struct ClothItemDef {
    const char* name; const char* path_m; const char* path_f;
    float color[3]; int slot;
};
static const ClothItemDef s_cloth_items[] = {
    // slot 0 — top
    {"None",         nullptr,                                         nullptr,                                      {0,0,0},             0},
    {"Slave Shirt",  "game/data/clothes/slave_shirt.clothbin",       "game/data/clothes/slave_shirt_f.clothbin",   {0.62f,0.59f,0.53f}, 0},
    {"Drifter Coat", "game/data/clothes/drifter_coat.clothbin",      "game/data/clothes/drifter_coat_f.clothbin",  {0.25f,0.20f,0.14f}, 0},
    {"Jacket",       "game/data/clothes/jacket.clothbin",            "game/data/clothes/jacket_f.clothbin",        {0.30f,0.22f,0.15f}, 0},
    {"Male Coat",    "game/data/clothes/male_coat.clothbin",         "game/data/clothes/drifter_coat2_f.clothbin", {0.20f,0.16f,0.10f}, 0},
    {"Monk Coat",    "game/data/clothes/monk_coat.clothbin",         "game/data/clothes/monk_coat_f.clothbin",     {0.18f,0.14f,0.09f}, 0},
    {"Samurai Top",  "game/data/clothes/samurai_top.clothbin",       "game/data/clothes/samurai_top_f.clothbin",   {0.15f,0.12f,0.08f}, 0},
    // slot 1 — bottom
    {"None",         nullptr,                                         nullptr,                                      {0,0,0},             1},
    {"Drifter Pants","game/data/clothes/drifter_pants.clothbin",     "game/data/clothes/drifter_pants_f.clothbin", {0.22f,0.18f,0.12f}, 1},
    {"Cargo Pants",  "game/data/clothes/cargopants.clothbin",        "game/data/clothes/cargopants_f.clothbin",    {0.28f,0.22f,0.14f}, 1},
    {"Shorts",       "game/data/clothes/shorts.clothbin",            "game/data/clothes/shorts_f.clothbin",        {0.35f,0.28f,0.18f}, 1},
    {"Half Pants",   "game/data/clothes/trousers.clothbin",          "game/data/clothes/halfpants_f.clothbin",     {0.20f,0.16f,0.10f}, 1},
    {"Monk Pants",   "game/data/clothes/monk_pants.clothbin",        "game/data/clothes/monk_pants_f.clothbin",    {0.18f,0.14f,0.09f}, 1},
    {"Samurai Bot",  "game/data/clothes/samurai_bot.clothbin",       "game/data/clothes/samurai_bot_f.clothbin",   {0.15f,0.12f,0.08f}, 1},
    {"Slave Dress",  "game/data/clothes/slave_dress.clothbin",       "game/data/clothes/slave_dress_f.clothbin",   {0.62f,0.59f,0.53f}, 1},
};
// s_clothes_visible / s_cloth_sel / s_cloth_color already declared extern by
// editor_char_preview_sdlgpu.h (public API); real definitions live in
// editor_char_preview_assets.cpp.
extern ClothSlot   s_cloth[3];
extern GpuPipeline s_cloth_pipeline;
extern int         s_sex;   // 0=Male 1=Female; set by Init()

// Per-bone scales — OGRE has two independent operations:
//   s_boneScales = setBoneSize       → scales vertices around bone origin (vertex deformation only)
//   s_posScale   = setBonePositionalSize → scales bone's bind translation from parent (position only)
// At neutral sliders both = (1,1,1). They are INDEPENDENT — vertex scale does NOT propagate position.
extern float s_boneScales[30][3]; // setBoneSize vertex scale
extern float s_posScale[30][3];   // setBonePositionalSize (default identity)
// World-space deformation matrices — OGRE-style hierarchical:
//   new_world[i] = new_world[parent] * (bind_local[i] with translation scaled by child s_posScale)
//   ws_mat[i]    = new_world[i] * S[i] * inv_bind[i]
// Parent scale moves child bones (scales local translation), but does NOT cascade into
// child vertex scaling — only this bone's own S[i] affects its vertices.
// At neutral S=I: new_world[i]=bind[i], ws_mat[i]=I.
extern float s_ws_mat[30][16];
extern float s_inv_bind[30][16];   // inverseBindMatrices from GLB (world→bone local)
extern float s_bind[30][16];       // bind matrices = inv(inv_bind)
extern float s_bind_local[30][16]; // bind_local[i] = inv_bind[parent] * bind[i]
extern int8_t s_bone_parent[30];   // parent joint index, -1 for root
extern float s_idle_rot[30][4];    // idle_stand_normal frame-0 quaternion (xyzw) per bone
extern bool  s_idle_has_rot[30];   // true if bone has explicit rotation channel in idle_stand_normal
extern bool  s_idle_loaded;

// ── Pose mesh + OzzAnimator (identical to in-game render pipeline) ───────────
extern SkinMesh    s_pose_mesh;
extern OzzAnimator s_pose_ozz;
extern int         s_pose_idle_clip;
extern int         s_pose_postures_clip;
extern int         s_pose_neck_clip;
extern int         s_pose_shoulder_clip;

// ── Breathing animation ───────────────────────────────────────────────────────
struct BreathChan {
    float* times = nullptr;
    float* quats = nullptr;
    float* trans = nullptr;
    int    rcount = 0;
    int    tcount = 0;
};
extern BreathChan s_breath[30];
extern float      s_breath_len;
extern bool       s_breath_loaded;

// ── Slider pose animations (Kenshi RE: postures/neck_set/shoulder_set) ────────
// Sampled at t = anim_length * slider_value * 0.01 (Kenshi RE line 19458).
struct SliderAnim {
    float rot0[30][4];   // keyframe 0 (slider=0)
    float rot_mid[30][4];// keyframe at slider=50 (middle frame)
    float rot1[30][4];   // last keyframe (slider=100)
    bool  has[30];
    float length = 0.f;
    bool  loaded = false;
    // Full keyframe storage for N-keyframe anims (e.g. "postures" has 6 frames).
    static constexpr int MAX_KEYS = 8;
    int   key_count = 0;
    float key_times[MAX_KEYS];
    float key_rot[MAX_KEYS][30][4];
};
extern SliderAnim s_anim_postures;    // body[4]  Posture    → "postures"
extern SliderAnim s_anim_neck_set;    // body[5]  Shoulder set → "neck set" (Kenshi naming)
extern SliderAnim s_anim_shoulder_set;// body[6]  Neck pos   → "shoulder set"


// ── Additional state (scattered later in the original single file: RTT dims,
// camera orbit, morph target buffers, body/skin/hair sliders, portrait cfg) ──
extern GpuColorTexture s_color;
extern GpuColorTexture s_depth;
extern int s_rtt_w;
extern int s_rtt_h;
extern float s_yaw;
extern float s_pit;
extern float s_dist;
extern float    s_lookat_y;  // vertical pivot offset (0=full body, ~0.9=waist, ~0.88*h=face)
extern ImVec2   s_d0;
extern float    s_y0;
extern uint64_t s_anim_epoch_ms; // breathing phase reset epoch
extern Vtx*   s_base_verts_cpu;   // persistent base mesh (not freed after GPU upload)
extern int    s_base_vc;
extern float* s_morph_deltas;   // [morph_count × vc × 3] heap-allocated
extern int    s_morph_count;
extern char   s_morph_names[32][48];
extern float  s_morph_weights[32];
extern bool   s_morphs_dirty;
extern float s_height;
extern float s_bulk;
extern float s_leg_y;  // cached from SetBoneScalesFromDef for foot grounding
extern float s_skin[3];
extern float s_str;
extern float s_sat;
extern float s_bri;
extern float s_muscle;
extern float s_hair[3];
extern float s_pose_rot[30][4];
extern float s_pose_tra[30][3];
struct PortraitCfg {
    float portrait_dist     = 0.72f;
    float portrait_offset_y = 0.88f;
    float portrait_fov      = 0.78f;
    float body_dist         = 3.5f;
    float body_pit          = -0.06f;
    float body_lookat_y     = 0.9f;
};
extern PortraitCfg s_pcfg;
extern bool        s_pcfg_loaded;

// ── M4 (column-major mat4) — needed by extern'd math helpers below ───────────
struct M4 { float m[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; };

// ── Cross-TU helper declarations ──────────────────────────────────────────────
// These stay non-static (unlike most private helpers, which remain static/
// file-local in whichever .cpp defines them) because they're called from a
// DIFFERENT split file than the one that defines them:
//   quat_mul/m4mul/LoadSliderAnim/SampleAnimAtTime/mat3_to_quat/m4inv_rigid/
//   m4_mul/m4_persp/m4_rotY/m4_rotX/m4_translate: defined in
//     editor_char_preview_anim.cpp, called from _assets.cpp and/or _runtime.cpp
//   ensure_rtt/s_load_mesh/s_load_textures/s_create_pipelines: defined in
//     editor_char_preview_assets.cpp, called from _runtime.cpp (Init/DrawInImGui)
void quat_mul(float out[4], const float a[4], const float b[4]);
void m4mul(float* C, const float* A, const float* B);
void LoadSliderAnim(cgltf_data* d, int* node_to_ji, SliderAnim& out, const char* name);
void SampleAnimAtTime(const SliderAnim& sa, int bone, float t, float out[4]);
void mat3_to_quat(float q[4], const float M[16]);
void m4inv_rigid(float* out, const float* M);
M4   m4_mul(const M4& a, const M4& b);
M4   m4_persp(float fov, float asp, float n, float f);
M4   m4_rotY(float a);
M4   m4_rotX(float a);
M4   m4_translate(float x, float y, float z);

void ensure_rtt(int w, int h);
bool s_load_mesh(const char* glb_path);
void s_load_textures(const char* tex_path);
bool s_create_pipelines(const char* glb_path);
#endif // MD_SDL_GPU
