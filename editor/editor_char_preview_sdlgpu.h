#pragma once
#ifdef MD_SDL_GPU
// editor_char_preview_sdlgpu.h — SDL_GPU character preview RTT for the standalone editor.
// T-pose md_human.glb rendered to an off-screen texture, displayed via ImGui::Image.
// RenderFrame() must be called once per frame before ImGui render (from main.cpp).
// DrawInImGui() is called inside the Characters tab.

#include "imgui.h"
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
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
static GpuPipeline     s_bg_pipeline;
static GpuTexture      s_bg_sand;   // desert_sand.jpg — ground texture
static GpuTexture      s_bg_dune;   // desert_dune.jpg — large-scale dune pattern
static GpuPipeline     s_scene_pipeline;  // platform + pole flat-color
static GpuStaticBuffer s_scene_vbo;
static GpuStaticBuffer s_scene_ibo;
static int             s_scene_ni = 0;
static GpuPipeline     s_pipeline;
static GpuStaticBuffer s_vbo;
static GpuStaticBuffer s_ibo;
static GpuTexture      s_tex;
static GpuTexture      s_tex_head;    // head/face diffuse (V<0 UV island)
static GpuTexture      s_tex_muscle;  // 1×1 neutral muscle mask
static GpuTexture      s_tex_blood;   // 1×1 clear blood overlay
// Bone scale texture: 30×1 RGBA32F — raw SDL_GPU (GpuTexture only supports RGBA8)
static SDL_GPUTexture* s_bones_tex     = nullptr;
static SDL_GPUSampler* s_bones_sampler = nullptr;
static int             s_ni  = 0;
static bool            s_ok  = false;

// Per-bone scales — OGRE has two independent operations:
//   s_boneScales = setBoneSize       → scales vertices around bone origin (vertex deformation only)
//   s_posScale   = setBonePositionalSize → scales bone's bind translation from parent (position only)
// At neutral sliders both = (1,1,1). They are INDEPENDENT — vertex scale does NOT propagate position.
static float s_boneScales[30][3]; // setBoneSize vertex scale
static float s_posScale[30][3];   // setBonePositionalSize (default identity)
// World-space deformation matrices — OGRE-style hierarchical:
//   new_world[i] = new_world[parent] * (bind_local[i] with translation scaled by child s_posScale)
//   ws_mat[i]    = new_world[i] * S[i] * inv_bind[i]
// Parent scale moves child bones (scales local translation), but does NOT cascade into
// child vertex scaling — only this bone's own S[i] affects its vertices.
// At neutral S=I: new_world[i]=bind[i], ws_mat[i]=I.
static float s_ws_mat[30][16];
static float s_inv_bind[30][16];   // inverseBindMatrices from GLB (world→bone local)
static float s_bind[30][16];       // bind matrices = inv(inv_bind)
static float s_bind_local[30][16]; // bind_local[i] = inv_bind[parent] * bind[i]
static int8_t s_bone_parent[30];   // parent joint index, -1 for root
static float s_idle_rot[30][4];    // idle_stand_normal last-frame quaternion (xyzw) per bone
static bool  s_idle_has_rot[30];   // true if bone has explicit rotation channel in idle_stand_normal
static bool  s_idle_loaded = false;

// ── Breathing animation ───────────────────────────────────────────────────────
struct BreathChan {
    float* times = nullptr;
    float* quats = nullptr;
    float* trans = nullptr;
    int    rcount = 0;
    int    tcount = 0;
};
static BreathChan s_breath[30];
static float      s_breath_len = 0.f;
static bool       s_breath_loaded = false;

// ── Slider pose animations (Kenshi RE: postures/neck_set/shoulder_set) ────────
// Each has 2 keyframes. Sampled at t = anim_length * slider_value * 0.01
// to freeze the skeleton in the slider's posed position.
struct SliderAnim {
    float rot0[30][4]; // frame 0 quaternions (xyzw)
    float rot1[30][4]; // frame 1 quaternions
    bool  has[30];     // bone has this channel
    float length = 0.f;
    bool  loaded = false;
};
static SliderAnim s_anim_postures;    // body[4]  Posture    → "postures"
static SliderAnim s_anim_neck_set;    // body[5]  Shoulder set → "neck set" (Kenshi naming)
static SliderAnim s_anim_shoulder_set;// body[6]  Neck pos   → "shoulder set"

// NLERP: normalised linear interpolation of two quaternions.
static void quat_nlerp(float out[4], const float a[4], const float b[4], float t) {
    float dot = a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
    float s = dot < 0.f ? -1.f : 1.f;
    float r[4]; for(int i=0;i<4;i++) r[i]=a[i]+(b[i]*s-a[i])*t;
    float len=sqrtf(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]+r[3]*r[3]);
    if(len>1e-6f){float il=1.f/len;for(int i=0;i<4;i++)out[i]=r[i]*il;}
    else {out[0]=0;out[1]=0;out[2]=0;out[3]=1;}
}

// Sample breathing animation at time t (seconds). Fills pose_rot[30][4] and pose_tra[30][3].
// For bones without a breathing channel, falls back to s_idle_rot[].
static void SampleBreathing(float t, float pose_rot[30][4], float pose_tra[30][3]) {
    for (int i = 0; i < 30; i++) {
        // ── rotation ──────────────────────────────────────────────────────
        BreathChan& bc = s_breath[i];
        if (bc.rcount >= 2) {
            // binary search for left bracket
            int lo=0, hi=bc.rcount-2;
            while (lo<hi) { int mid=(lo+hi+1)/2; if(bc.times[mid]<=t) lo=mid; else hi=mid-1; }
            int k = lo;
            float dt = bc.times[k+1]-bc.times[k];
            float alpha = (dt>1e-7f) ? (t-bc.times[k])/dt : 0.f;
            alpha = alpha<0.f?0.f:(alpha>1.f?1.f:alpha);
            quat_nlerp(pose_rot[i], bc.quats+k*4, bc.quats+(k+1)*4, alpha);
        } else if (bc.rcount==1) {
            memcpy(pose_rot[i], bc.quats, 16);
        } else {
            memcpy(pose_rot[i], s_idle_rot[i], 16);
        }
        // ── translation ───────────────────────────────────────────────────
        if (bc.tcount >= 2) {
            int lo=0, hi=bc.tcount-2;
            while (lo<hi) { int mid=(lo+hi+1)/2; if(bc.times[mid]<=t) lo=mid; else hi=mid-1; }
            int k = lo;
            float dt = bc.times[k+1]-bc.times[k];
            float alpha = (dt>1e-7f) ? (t-bc.times[k])/dt : 0.f;
            alpha = alpha<0.f?0.f:(alpha>1.f?1.f:alpha);
            float* p0=bc.trans+k*3; float* p1=bc.trans+(k+1)*3;
            for(int j=0;j<3;j++) pose_tra[i][j]=p0[j]+alpha*(p1[j]-p0[j]);
        } else if (bc.tcount==1) {
            memcpy(pose_tra[i], bc.trans, 12);
        } else {
            pose_tra[i][0]=s_bind_local[i][12];
            pose_tra[i][1]=s_bind_local[i][13];
            pose_tra[i][2]=s_bind_local[i][14];
        }
    }
}

// col-major mat4 multiply: C = A * B
static void m4mul(float* C, const float* A, const float* B) {
    float T[16];
    for (int j=0;j<4;j++) for (int i=0;i<4;i++) {
        float s=0.f; for (int k=0;k<4;k++) s+=A[k*4+i]*B[j*4+k]; T[j*4+i]=s;
    }
    memcpy(C,T,64);
}
// Load a 2-keyframe SliderAnim from GLB animation by name.
// Alpha = slider_value * 0.01  (Kenshi formula: time = length * value * 0.01)
static void LoadSliderAnim(cgltf_data* d, int* node_to_ji, SliderAnim& out, const char* name) {
    out.loaded = false; out.length = 0.f;
    memset(out.has, 0, sizeof(out.has));
    for(int i=0;i<30;i++){out.rot0[i][3]=1;out.rot1[i][3]=1;}
    for(int ai=0;ai<(int)d->animations_count;++ai){
        cgltf_animation& anim=d->animations[ai];
        if (!anim.name||strcmp(anim.name,name)!=0) continue;
        // Get length from first input accessor
        for(int ci=0;ci<(int)anim.channels_count&&out.length==0.f;++ci){
            cgltf_animation_channel& ch=anim.channels[ci];
            if (!ch.sampler||!ch.sampler->input||ch.sampler->input->count==0) continue;
            float lt=0.f;
            cgltf_accessor_read_float(ch.sampler->input, ch.sampler->input->count-1, &lt, 1);
            out.length = lt;
        }
        for(int ci=0;ci<(int)anim.channels_count;++ci){
            cgltf_animation_channel& ch=anim.channels[ci];
            if (!ch.target_node||!ch.sampler||!ch.sampler->output) continue;
            if (ch.target_path!=cgltf_animation_path_type_rotation) continue;
            int ni=(int)(ch.target_node-d->nodes);
            if (ni<0||ni>=2048) continue;
            int ji=node_to_ji[ni];
            if (ji<0||ji>=30) continue;
            int cnt=(int)ch.sampler->output->count;
            if (cnt>=1) cgltf_accessor_read_float(ch.sampler->output, 0, out.rot0[ji], 4);
            if (cnt>=2) cgltf_accessor_read_float(ch.sampler->output, cnt-1, out.rot1[ji], 4);
            else memcpy(out.rot1[ji], out.rot0[ji], 16);
            out.has[ji]=true;
        }
        out.loaded=true;
        fprintf(stdout,"[CharPreview] slider anim '%s': length=%.4fs\n",name,out.length);
        break;
    }
}

// Sample a SliderAnim and blend with existing pose_rot (ANIMBLEND_AVERAGE).
// Kenshi RE: breathing weight=0.95, slider weight=1.0
//   blend_factor = w_slider / (w_breath + w_slider) = 1.0/1.95 ≈ 0.513
// At alpha=0: slider contributes frame 0 (bind reference) blended 51% with breathing.
// At alpha>0: slider contributes the posed frame blended 51% with breathing.
// NOTE: We skip when alpha<0.01 so default Posture=0 doesn't dampen breathing.
static void ApplySliderAnim(const SliderAnim& sa, float alpha, float pose_rot[30][4]) {
    if (!sa.loaded) return;
    if (alpha < 0.01f) return;   // skip at default-zero — keep full breathing
    float a = alpha > 1.f ? 1.f : alpha;
    static constexpr float BLEND = 1.0f / (0.95f + 1.0f);  // ≈ 0.513
    for(int i=0;i<30;i++){
        if (!sa.has[i]) continue;
        float slr[4];
        quat_nlerp(slr, sa.rot0[i], sa.rot1[i], a);
        // ANIMBLEND_AVERAGE: mix current breathing pose with slider pose
        quat_nlerp(pose_rot[i], pose_rot[i], slr, BLEND);
    }
}

// Extract unit quaternion (xyzw) from col-major 4×4 rotation matrix.
static void mat3_to_quat(float q[4], const float M[16]) {
    float t = M[0]+M[5]+M[10];
    if (t>0.f){ float s=0.5f/sqrtf(t+1.f);
        q[3]=0.25f/s; q[0]=(M[6]-M[9])*s; q[1]=(M[8]-M[2])*s; q[2]=(M[1]-M[4])*s;
    } else if (M[0]>M[5]&&M[0]>M[10]){ float s=2.f*sqrtf(1.f+M[0]-M[5]-M[10]);
        q[3]=(M[6]-M[9])/s; q[0]=0.25f*s; q[1]=(M[4]+M[1])/s; q[2]=(M[8]+M[2])/s;
    } else if (M[5]>M[10]){ float s=2.f*sqrtf(1.f+M[5]-M[0]-M[10]);
        q[3]=(M[8]-M[2])/s; q[0]=(M[4]+M[1])/s; q[1]=0.25f*s; q[2]=(M[9]+M[6])/s;
    } else { float s=2.f*sqrtf(1.f+M[10]-M[0]-M[5]);
        q[3]=(M[1]-M[4])/s; q[0]=(M[8]+M[2])/s; q[1]=(M[9]+M[6])/s; q[2]=0.25f*s; }
}
// Build col-major mat4 from unit quaternion q=(xyzw) + translation t.
static void m4_from_quat_t(float* M, const float q[4], const float t[3]) {
    float x=q[0],y=q[1],z=q[2],w=q[3];
    float x2=x+x,y2=y+y,z2=z+z;
    float xx=x*x2,xy=x*y2,xz=x*z2,yy=y*y2,yz=y*z2,zz=z*z2,wx=w*x2,wy=w*y2,wz=w*z2;
    M[0]=1-(yy+zz); M[1]=xy+wz;     M[2]=xz-wy;      M[3]=0;
    M[4]=xy-wz;     M[5]=1-(xx+zz); M[6]=yz+wx;      M[7]=0;
    M[8]=xz+wy;     M[9]=yz-wx;     M[10]=1-(xx+yy); M[11]=0;
    M[12]=t[0];     M[13]=t[1];     M[14]=t[2];      M[15]=1;
}
// Invert a rigid-body matrix (rotation+translation only).
static void m4inv_rigid(float* out, const float* M) {
    for (int r=0;r<3;r++) for (int c=0;c<3;c++) out[c*4+r]=M[r*4+c];
    for (int c=0;c<4;c++) out[c*4+3]=(c==3)?1.f:0.f;
    float tx=M[12],ty=M[13],tz=M[14];
    out[12]=-(out[0]*tx+out[4]*ty+out[8]*tz);
    out[13]=-(out[1]*tx+out[5]*ty+out[9]*tz);
    out[14]=-(out[2]*tx+out[6]*ty+out[10]*tz);
    out[15]=1.f;
}

static SDL_GPUTexture* s_color = nullptr;
static SDL_GPUTexture* s_depth = nullptr;
static int             s_rtt_w = 0, s_rtt_h = 0;

static float    s_yaw = 0.18f, s_pit = -0.06f, s_dist = 2.6f;
static float    s_lookat_y = 0.f;   // vertical pivot offset (0=full body, ~0.88=face)
static bool     s_drag = false;
static ImVec2   s_d0;
static float    s_y0;
static uint64_t s_anim_epoch_ms = 0; // breathing phase reset epoch

// Reset breathing animation to t=0 (natural rest) — call on RAND/RESET.
static void ResetAnimPhase() { s_anim_epoch_ms = SDL_GetTicks(); }

// ── Morph target (blend shape) state ─────────────────────────────────────────
static Vtx*   s_base_verts_cpu  = nullptr;   // persistent base mesh (not freed after GPU upload)
static int    s_base_vc         = 0;
static float* s_morph_deltas    = nullptr;   // [morph_count × vc × 3] heap-allocated
static int    s_morph_count     = 0;
static char   s_morph_names[32][48]  = {};
static float  s_morph_weights[32]    = {};
static bool   s_morphs_dirty    = false;

// Params saved by DrawInImGui → consumed by RenderFrame
static float s_height = 1.f, s_bulk = 1.f;
static float s_skin[3] = {0.82f, 0.65f, 0.52f};
static float s_str = 0.55f, s_sat = 1.f, s_bri = 0.f;
static float s_muscle = 0.f;
static float s_hair[3] = {0.18f, 0.12f, 0.08f};

static void DumpState(FILE* f) {
    fprintf(f, "[CharPreview]\n");
    fprintf(f, "  ok=%d  ni=%d  rtt=%dx%d\n", s_ok, s_ni, s_rtt_w, s_rtt_h);
    fprintf(f, "  color_tex=%s  depth_tex=%s\n", s_color ? "ok" : "null", s_depth ? "ok" : "null");
    fprintf(f, "  yaw=%.4f  pit=%.4f  dist=%.4f\n", s_yaw, s_pit, s_dist);
    fprintf(f, "  skin=%.3f,%.3f,%.3f  str=%.3f\n", s_skin[0], s_skin[1], s_skin[2], s_str);
    fprintf(f, "  sat=%.3f  bri=%.3f  muscle=%.3f\n", s_sat, s_bri, s_muscle);
    fprintf(f, "  hair=%.3f,%.3f,%.3f\n", s_hair[0], s_hair[1], s_hair[2]);
    fprintf(f, "  height=%.3f  bulk=%.3f\n\n", s_height, s_bulk);
}

// ── Mat4 (column-major) ───────────────────────────────────────────────────────
struct M4 { float m[16] = {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; };
static M4 m4_mul(const M4& a, const M4& b) {
    M4 c; memset(c.m,0,64);
    for(int i=0;i<4;i++) for(int j=0;j<4;j++) for(int k=0;k<4;k++)
        c.m[i*4+j] += a.m[k*4+j]*b.m[i*4+k];
    return c;
}
static M4 m4_persp(float fov, float asp, float n, float f) {
    M4 r; memset(r.m,0,64);
    float t=1.f/tanf(fov*0.5f);
    r.m[0]=t/asp; r.m[5]=t;
    r.m[10]=(f+n)/(n-f); r.m[11]=-1.f; r.m[14]=(2.f*f*n)/(n-f);
    return r;
}
static M4 m4_rotY(float a) { M4 r; r.m[0]=cosf(a);r.m[2]=sinf(a);r.m[8]=-sinf(a);r.m[10]=cosf(a); return r; }
static M4 m4_rotX(float a) { M4 r; r.m[5]=cosf(a);r.m[6]=-sinf(a);r.m[9]=sinf(a);r.m[10]=cosf(a); return r; }
static M4 m4_translate(float x, float y, float z) { M4 r; r.m[12]=x; r.m[13]=y; r.m[14]=z; return r; }

// ── RTT management ────────────────────────────────────────────────────────────
static void ensure_rtt(int w, int h) {
    if (w==s_rtt_w && h==s_rtt_h && s_color) return;
    SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
    if (s_color) SDL_ReleaseGPUTexture(dev, s_color);
    if (s_depth) SDL_ReleaseGPUTexture(dev, s_depth);
    s_rtt_w=w; s_rtt_h=h;
    SDL_GPUTextureCreateInfo ci={};
    ci.type=SDL_GPU_TEXTURETYPE_2D;
    ci.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    ci.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.width=(uint32_t)w; ci.height=(uint32_t)h;
    ci.layer_count_or_depth=1; ci.num_levels=1;
    s_color=SDL_CreateGPUTexture(dev,&ci);
    ci.format=SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    ci.usage=SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    s_depth=SDL_CreateGPUTexture(dev,&ci);
}

// ── Init: load GLB (T-pose) + body texture + pipeline ─────────────────────────
static bool Init(const char* glb_path, const char* tex_path) {
    // Load mesh via cgltf — extract pos, norm, uv only (no skinning needed for T-pose)
    cgltf_options o={};
    cgltf_data* d=nullptr;
    if (cgltf_parse_file(&o,glb_path,&d)!=cgltf_result_success) {
        fprintf(stderr,"[CharPreview] glb load failed: %s\n",glb_path); return false;
    }
    cgltf_load_buffers(&o,d,glb_path);
    if (!d->meshes_count||!d->meshes[0].primitives_count) { cgltf_free(d); return false; }

    cgltf_primitive& pr=d->meshes[0].primitives[0];
    cgltf_accessor *ap=nullptr,*an=nullptr,*au=nullptr,*aj=nullptr,*aw=nullptr;
    for (size_t i=0;i<pr.attributes_count;i++) {
        auto& a=pr.attributes[i];
        if      (a.type==cgltf_attribute_type_position) ap=a.data;
        else if (a.type==cgltf_attribute_type_normal)   an=a.data;
        else if (a.type==cgltf_attribute_type_texcoord&&!au) au=a.data;
        else if (a.type==cgltf_attribute_type_joints&&!aj)   aj=a.data;
        else if (a.type==cgltf_attribute_type_weights&&!aw)  aw=a.data;
    }
    if (!ap||!pr.indices) { cgltf_free(d); return false; }

    size_t vc=ap->count;
    Vtx* vb=new Vtx[vc]; memset(vb,0,vc*sizeof(Vtx));
    for (size_t i=0;i<vc;i++) {
        float v[3]={}; cgltf_accessor_read_float(ap,i,v,3);
        vb[i].px=v[0]; vb[i].py=v[1]; vb[i].pz=v[2];
    }
    if (an) for (size_t i=0;i<vc;i++) {
        float v[3]={}; cgltf_accessor_read_float(an,i,v,3);
        vb[i].nx=v[0]; vb[i].ny=v[1]; vb[i].nz=v[2];
    }
    if (au) for (size_t i=0;i<vc;i++) {
        float v[2]={}; cgltf_accessor_read_float(au,i,v,2);
        vb[i].u=v[0]; vb[i].v=v[1];
    }
    if (aj) for (size_t i=0;i<vc;i++) {
        // cgltf_accessor_read_uint gives raw integer indices, not normalized floats.
        cgltf_uint u[4]={}; cgltf_accessor_read_uint(aj,i,u,4);
        vb[i].ji[0]=(uint8_t)u[0]; vb[i].ji[1]=(uint8_t)u[1];
        vb[i].ji[2]=(uint8_t)u[2]; vb[i].ji[3]=(uint8_t)u[3];
    }
    if (aw) for (size_t i=0;i<vc;i++) {
        float v[4]={}; cgltf_accessor_read_float(aw,i,v,4);
        vb[i].wt[0]=v[0]; vb[i].wt[1]=v[1];
        vb[i].wt[2]=v[2]; vb[i].wt[3]=v[3];
    }

    // ── Shorts anti-Z-fight bias ─────────────────────────────────────────────
    // The shorts/underwear geometry is co-planar with the body skin in the same primitive.
    // Vertices in the shorts UV zone (body_V ∈ [0.37, 0.92]) are pushed 1.5mm outward
    // along their normals so shorts always win the depth test against body skin beneath.
    // This is invisible at any render distance but eliminates the skin-bleed-through artifact.
    if (an) {
        for (size_t i = 0; i < vc; i++) {
            float v = vb[i].v;
            if (v >= 0.37f && v <= 0.92f) {
                const float bias = 0.0015f;
                vb[i].px += vb[i].nx * bias;
                vb[i].py += vb[i].ny * bias;
                vb[i].pz += vb[i].nz * bias;
            }
        }
    }

    // ── Morph targets — load before cgltf_free ───────────────────────────────
    delete[] s_base_verts_cpu; s_base_verts_cpu = vb;  // take ownership (vb NOT freed below)
    s_base_vc = (int)vc;

    free(s_morph_deltas); s_morph_deltas = nullptr;
    s_morph_count = 0;
    memset(s_morph_names,  0, sizeof(s_morph_names));
    memset(s_morph_weights,0, sizeof(s_morph_weights));

    int n_mt = (int)pr.targets_count;
    if (n_mt > 32) n_mt = 32;
    if (n_mt > 0) {
        // Parse targetNames from mesh extras JSON
        const char* ej = d->meshes[0].extras.data;
        int names_found = 0;
        if (ej) {
            const char* p = strstr(ej, "\"targetNames\"");
            if (p) { p = strchr(p, '['); if (p) { ++p;
                while (*p && names_found < n_mt) {
                    while (*p && *p != '"' && *p != ']') ++p;
                    if (!*p || *p == ']') break;
                    ++p; int len=0;
                    while (*p && *p != '"' && len<47) s_morph_names[names_found][len++]=*p++;
                    s_morph_names[names_found][len]='\0';
                    if (*p=='"') ++p;
                    ++names_found;
                }
            }}
        }
        s_morph_deltas = (float*)calloc((size_t)n_mt * vc * 3, sizeof(float));
        if (s_morph_deltas) {
            for (int mt = 0; mt < n_mt; ++mt) {
                if (mt >= names_found || s_morph_names[mt][0]=='\0')
                    snprintf(s_morph_names[mt], 48, "morph_%d", mt);
                cgltf_morph_target& tgt = pr.targets[mt];
                for (cgltf_size ai = 0; ai < tgt.attributes_count; ++ai) {
                    if (tgt.attributes[ai].type != cgltf_attribute_type_position) continue;
                    cgltf_accessor* dacc = tgt.attributes[ai].data;
                    float* base = s_morph_deltas + (size_t)mt * vc * 3;
                    cgltf_size rn = dacc->count < vc ? dacc->count : vc;
                    for (cgltf_size vi = 0; vi < rn; ++vi)
                        cgltf_accessor_read_float(dacc, vi, base + vi*3, 3);
                    break;
                }
            }
            s_morph_count = n_mt;
        }
        fprintf(stdout,"[CharPreview] %d morph targets\n", s_morph_count);
    }
    s_morphs_dirty = false;

    // ── Inverse bind matrices + parent hierarchy ─────────────────────────────
    for (int i=0;i<30;i++){
        memset(s_inv_bind[i],0,64);
        s_inv_bind[i][0]=s_inv_bind[i][5]=s_inv_bind[i][10]=s_inv_bind[i][15]=1.f;
        memcpy(s_bind[i],s_inv_bind[i],64);
        s_bone_parent[i]=-1;
    }
    if (d->skins_count>0) {
        cgltf_skin& sk=d->skins[0];
        if (sk.inverse_bind_matrices) {
            int n=(int)sk.inverse_bind_matrices->count; if(n>30)n=30;
            for (int i=0;i<n;i++) {
                cgltf_accessor_read_float(sk.inverse_bind_matrices,(size_t)i,s_inv_bind[i],16);
                m4inv_rigid(s_bind[i], s_inv_bind[i]);
            }
            fprintf(stdout,"[CharPreview] %d inv_bind+bind loaded\n",n);
        }
        // Build parent hierarchy from joint node tree
        int jn=(int)sk.joints_count; if(jn>30)jn=30;
        static int node_to_ji[2048]; memset(node_to_ji,-1,sizeof(node_to_ji));
        for (int ji=0;ji<jn;ji++) {
            int ni=(int)(sk.joints[ji]-d->nodes);
            if(ni>=0&&ni<2048) node_to_ji[ni]=ji;
        }
        for (int ji=0;ji<jn;ji++) {
            cgltf_node* n=sk.joints[ji];
            if (n->parent) {
                int pni=(int)(n->parent-d->nodes);
                if(pni>=0&&pni<2048&&node_to_ji[pni]>=0) s_bone_parent[ji]=(int8_t)node_to_ji[pni];
            }
        }
        fprintf(stdout,"[CharPreview] hierarchy loaded for %d bones\n",jn);

        // Load idle_stand_normal — sample frame 0 for spine/neck/head lean.
        // Arms are excluded from the whitelist, so their frame doesn't matter.
        s_idle_loaded = false;
        memset(s_idle_has_rot, 0, sizeof(s_idle_has_rot));
        for (int bi=0;bi<30;bi++){
            s_idle_rot[bi][0]=0;s_idle_rot[bi][1]=0;s_idle_rot[bi][2]=0;s_idle_rot[bi][3]=1;
        }
        for (int ai=0;ai<(int)d->animations_count;++ai){
            cgltf_animation& anim=d->animations[ai];
            if (!anim.name||strcmp(anim.name,"idle_stand_normal")!=0) continue;
            // idle_stand_normal has 2 keyframes: t=0 (T-pose) and t=0.033 (natural stand).
            // Use last frame = natural standing pose with arms at sides.
            for (int ci=0;ci<(int)anim.channels_count;++ci){
                cgltf_animation_channel& ch=anim.channels[ci];
                if (!ch.target_node||!ch.sampler) continue;
                if (ch.target_path!=cgltf_animation_path_type_rotation) continue;
                int ni=(int)(ch.target_node-d->nodes);
                if (ni<0||ni>=2048) continue;
                int ji=node_to_ji[ni];
                if (ji<0||ji>=30) continue;
                if (ch.sampler->output&&ch.sampler->output->count>0) {
                    size_t last = ch.sampler->output->count - 1;
                    cgltf_accessor_read_float(ch.sampler->output, last, s_idle_rot[ji], 4);
                    s_idle_has_rot[ji]=true;
                }
            }
            s_idle_loaded=true;
            fprintf(stdout,"[CharPreview] idle_stand_normal: %d rot channels loaded (last frame)\n",(int)anim.channels_count);
            break;
        }

        // ── Load breathing animation ──────────────────────────────────────────
        s_breath_loaded = false;
        s_breath_len = 0.f;
        memset(s_breath, 0, sizeof(s_breath));
        for (int ai=0;ai<(int)d->animations_count;++ai){
            cgltf_animation& anim=d->animations[ai];
            // "breathing noarms" = same torso sway without arm channels (Kenshi RE)
            if (!anim.name||strcmp(anim.name,"breathing noarms")!=0) continue;
            for (int ci=0;ci<(int)anim.channels_count&&s_breath_len==0.f;++ci){
                cgltf_animation_channel& ch2=anim.channels[ci];
                if (!ch2.sampler||!ch2.sampler->input||ch2.sampler->input->count==0) continue;
                float lt=0.f;
                cgltf_accessor_read_float(ch2.sampler->input, ch2.sampler->input->count-1, &lt, 1);
                s_breath_len = lt;
            }
            for (int ci=0;ci<(int)anim.channels_count;++ci){
                cgltf_animation_channel& bch=anim.channels[ci];
                if (!bch.target_node||!bch.sampler||!bch.sampler->input||!bch.sampler->output) continue;
                int bni=(int)(bch.target_node-d->nodes);
                if (bni<0||bni>=2048) continue;
                int bji=node_to_ji[bni];
                if (bji<0||bji>=30) continue;
                int cnt=(int)bch.sampler->output->count;
                if (cnt<=0) continue;
                float* tbuf=(float*)malloc((size_t)cnt*sizeof(float));
                if (!tbuf) continue;
                for(int k=0;k<cnt;k++) cgltf_accessor_read_float(bch.sampler->input,k,&tbuf[k],1);
                if (bch.target_path==cgltf_animation_path_type_rotation) {
                    float* qbuf=(float*)malloc((size_t)cnt*4*sizeof(float));
                    if (!qbuf){free(tbuf);continue;}
                    for(int k=0;k<cnt;k++) cgltf_accessor_read_float(bch.sampler->output,k,qbuf+k*4,4);
                    free(s_breath[bji].quats); free(s_breath[bji].times);
                    s_breath[bji].quats=qbuf; s_breath[bji].times=tbuf; s_breath[bji].rcount=cnt;
                } else if (bch.target_path==cgltf_animation_path_type_translation) {
                    float* pbuf=(float*)malloc((size_t)cnt*3*sizeof(float));
                    if (!pbuf){free(tbuf);continue;}
                    for(int k=0;k<cnt;k++) cgltf_accessor_read_float(bch.sampler->output,k,pbuf+k*3,3);
                    if (s_breath[bji].rcount>0) free(tbuf);
                    else { free(s_breath[bji].times); s_breath[bji].times=tbuf; }
                    free(s_breath[bji].trans);
                    s_breath[bji].trans=pbuf; s_breath[bji].tcount=cnt;
                } else {
                    free(tbuf);
                }
            }
            s_breath_loaded=true;
            fprintf(stdout,"[CharPreview] breathing: %.2fs loaded\n", s_breath_len);
            break;
        }

        // ── Slider pose animations (RE: Kenshi maps body sliders to OGRE anims) ──
        LoadSliderAnim(d, node_to_ji, s_anim_postures,     "postures");
        LoadSliderAnim(d, node_to_ji, s_anim_neck_set,     "neck set");
        LoadSliderAnim(d, node_to_ji, s_anim_shoulder_set, "shoulder set");
    }
    // Precompute bind_local[i] = inv_bind[parent] * bind[i] (local bind TRS in parent space)
    for (int i=0;i<30;i++) {
        if (s_bone_parent[i]<0)
            memcpy(s_bind_local[i], s_bind[i], 64);
        else
            m4mul(s_bind_local[i], s_inv_bind[(int)s_bone_parent[i]], s_bind[i]);
    }
    // For idle pose: bones without a rotation channel use bind-pose quaternion
    // so m4_from_quat_t gives identical result to the T-pose bind_local matrix.
    if (s_idle_loaded) {
        for (int i=0;i<30;i++)
            if (!s_idle_has_rot[i])
                mat3_to_quat(s_idle_rot[i], s_bind_local[i]);
    }

    s_ni=(int)pr.indices->count;
    uint32_t* ib=new uint32_t[s_ni];
    for (int i=0;i<s_ni;i++) ib[i]=(uint32_t)cgltf_accessor_read_index(pr.indices,(size_t)i);
    cgltf_free(d);

    s_vbo.Init(0x8892u, s_base_verts_cpu, (uint32_t)(vc*sizeof(Vtx)));
    s_ibo.Init(0x8893u, ib, (uint32_t)(s_ni*sizeof(uint32_t)));
    delete[] ib;  // vb is owned by s_base_verts_cpu — do NOT delete here

    // Body texture
    {
        stbi_set_flip_vertically_on_load(0);
        int tw,th,tc; unsigned char* td=stbi_load(tex_path,&tw,&th,&tc,4);
        GpuSamplerDesc sd; sd.min_filter=GpuSamplerDesc::Filter::LINEAR_MIPMAP;
        sd.mag_filter=GpuSamplerDesc::Filter::LINEAR;
        if (td) {
            s_tex.InitFromMemory(td,tw,th,sd);
            stbi_image_free(td);
        } else {
            uint8_t fb[4]={200,162,122,255};
            s_tex.InitFromMemory(fb,1,1,sd);
        }
    }
    // Head/face texture (Kenshi: separate human_male_head_diffuse_HI atlas, UV V<0)
    {
        // Derive head tex path: same dir as body tex, replace filename
        // Derive head tex: replace "_body" with "_head" in filename.
        // male:   md_human_body.png         → md_human_head.png
        // female: md_human_female_body.png  → md_human_female_head.png
        char head_path[512]; strncpy(head_path, tex_path, 511);
        char* bp = strstr(head_path, "_body");
        if (bp) memcpy(bp, "_head", 5);  // same length, safe in-place replace
        stbi_set_flip_vertically_on_load(0);
        int hw,hh,hc; unsigned char* hd=stbi_load(head_path,&hw,&hh,&hc,4);
        GpuSamplerDesc hsd; hsd.min_filter=GpuSamplerDesc::Filter::LINEAR_MIPMAP;
        hsd.mag_filter=GpuSamplerDesc::Filter::LINEAR;
        if (hd) {
            s_tex_head.InitFromMemory(hd,hw,hh,hsd);
            stbi_image_free(hd);
        } else {
            uint8_t fb[4]={200,162,122,255};  // fallback: skin tone 1×1
            s_tex_head.InitFromMemory(fb,1,1,hsd);
            fprintf(stderr,"[CharPreview] head tex not found: %s\n", head_path);
        }
    }
    // Muscle mask placeholder: mid-grey (r=0.5 → neutral muscle detail)
    { uint8_t p[4]={128,128,128,255}; GpuSamplerDesc sd; s_tex_muscle.InitFromMemory(p,1,1,sd); }
    // Blood overlay placeholder: fully transparent (no wounds)
    { uint8_t p[4]={0,0,0,0}; GpuSamplerDesc sd; s_tex_blood.InitFromMemory(p,1,1,sd); }
    // Bone matrix texture: 120×1 RGBA32F — 30 bones × 4 columns per mat4
    // Each bone i occupies texels [i*4 .. i*4+3] = columns 0-3 of the 4×4 Ws matrix.
    {
        for (int i=0;i<30;i++){
            s_boneScales[i][0]=1;s_boneScales[i][1]=1;s_boneScales[i][2]=1;
            s_posScale[i][0]=1;s_posScale[i][1]=1;s_posScale[i][2]=1;
        }
        // Build initial identity Ws matrices before first upload
        for (int i=0;i<30;i++){
            memset(s_ws_mat[i],0,64);
            s_ws_mat[i][0]=s_ws_mat[i][5]=s_ws_mat[i][10]=s_ws_mat[i][15]=1.f;
        }
        SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
        SDL_GPUTextureCreateInfo ti={};
        ti.type=SDL_GPU_TEXTURETYPE_2D;
        ti.format=SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        ti.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width=120; ti.height=1; ti.layer_count_or_depth=1; ti.num_levels=1;
        s_bones_tex=SDL_CreateGPUTexture(dev,&ti);
        if (!s_bones_tex) {
            fprintf(stderr,"[CharPreview] bone mat tex create failed: %s\n",SDL_GetError());
        }
        SDL_GPUSamplerCreateInfo si={};
        si.min_filter=SDL_GPU_FILTER_NEAREST; si.mag_filter=SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        s_bones_sampler=SDL_CreateGPUSampler(dev,&si);
        if (s_bones_tex) {
            uint32_t up_sz=120*4*4; // 120 texels × 4 channels × 4 bytes
            SDL_GPUTransferBufferCreateInfo tb={};
            tb.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tb.size=up_sz;
            SDL_GPUTransferBuffer* tr=SDL_CreateGPUTransferBuffer(dev,&tb);
            if (tr) {
                void* mp=SDL_MapGPUTransferBuffer(dev,tr,false);
                if(mp){memcpy(mp,s_ws_mat,up_sz);SDL_UnmapGPUTransferBuffer(dev,tr);}
                SDL_GPUCommandBuffer* uc=SDL_AcquireGPUCommandBuffer(dev);
                if (uc) {
                    SDL_GPUCopyPass* cp=SDL_BeginGPUCopyPass(uc);
                    SDL_GPUTextureTransferInfo src={tr,0,(uint32_t)120,(uint32_t)1};
                    SDL_GPUTextureRegion dst={s_bones_tex,0,0,0,0,0,120,1,1};
                    SDL_UploadToGPUTexture(cp,&src,&dst,false);
                    SDL_EndGPUCopyPass(cp);
                    SDL_SubmitGPUCommandBuffer(uc);
                }
                SDL_ReleaseGPUTransferBuffer(dev,tr);
            }
        }
    }

    // Pipeline
    GpuPipeline::Desc pd;
    pd.vert_path = "shaders/char_preview.vert";
    pd.frag_path = "shaders/char_preview.frag";
    pd.layout.count = 5;
    pd.layout.stride = sizeof(Vtx);
    pd.layout.attribs[0] = { 0,  0, GpuAttribFmt::F3 };    // aPos
    pd.layout.attribs[1] = { 1, 12, GpuAttribFmt::F3 };    // aNorm
    pd.layout.attribs[2] = { 2, 24, GpuAttribFmt::F2 };    // aUV
    pd.layout.attribs[3] = { 3, 32, GpuAttribFmt::U8x4 };  // aJoints
    pd.layout.attribs[4] = { 4, 36, GpuAttribFmt::F4 };    // aWeights
    pd.raster.depth_test = true;
    pd.raster.depth_write = true;
    pd.raster.cull_back = true;   // inner torso faces must be culled — cull_back=false caused arm/torso Z-fight
    pd.vert_uniform_bufs = 1;   // set=1 binding=0: VU
    pd.vert_samplers = 1;       // set=1 binding=1: uBoneScales (bone scale texture)
    pd.frag_samplers = 4;       // set=2: body_diffuse, head_diffuse, muscle_mask, blood_overlay
    pd.frag_uniform_bufs = 1;   // set=3
    pd.color_format = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    pd.has_depth_target = true;

    if (!s_pipeline.Create(pd)) {
        fprintf(stderr,"[CharPreview] pipeline create failed\n"); return false;
    }

    // Background gradient pipeline — fullscreen tri, no VBO, no depth test/write
    {
        GpuPipeline::Desc bgpd;
        bgpd.vert_path = "shaders/char_bg.vert";
        bgpd.frag_path = "shaders/char_bg.frag";
        bgpd.layout.count  = 0;
        bgpd.layout.stride = 0;
        bgpd.raster.depth_test  = false;
        bgpd.raster.depth_write = false;
        bgpd.raster.cull_back   = false;
        bgpd.vert_uniform_bufs  = 0;
        bgpd.vert_samplers      = 0;
        bgpd.frag_samplers      = 2;   // set=2 binding=0: uSand, binding=1: uDune
        bgpd.frag_uniform_bufs  = 1;   // set=3 binding=0: BgUU (ray-ground uniforms)
        bgpd.color_format       = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        bgpd.has_depth_target   = true;
        if (!s_bg_pipeline.Create(bgpd))
            fprintf(stderr,"[CharPreview] bg pipeline create failed\n");

        // Load desert ground textures
        GpuSamplerDesc rep; rep.min_filter=GpuSamplerDesc::Filter::LINEAR_MIPMAP;
        rep.mag_filter=GpuSamplerDesc::Filter::LINEAR;
        rep.wrap_s=rep.wrap_t=GpuSamplerDesc::Wrap::REPEAT;
        auto load_bg=[&](GpuTexture& tex, const char* path){
            stbi_set_flip_vertically_on_load(0);
            int tw,th,tc;
            unsigned char* td=stbi_load(path,&tw,&th,&tc,4);
            if (td){ tex.InitFromMemory(td,tw,th,rep); stbi_image_free(td); }
            else {
                uint8_t fb[4]={160,130,80,255};
                tex.InitFromMemory(fb,1,1,rep);
                fprintf(stderr,"[CharPreview] bg tex missing: %s\n",path);
            }
        };
        load_bg(s_bg_sand,"game/data/textures/terrain/desert_sand.jpg");
        load_bg(s_bg_dune,"game/data/textures/terrain/desert_dune.jpg");
    }

    // ── Scene pipeline: platform planks + anthropometer pole ─────────────────
    {
        GpuPipeline::Desc spd;
        spd.vert_path = "shaders/char_scene.vert";
        spd.frag_path = "shaders/char_scene.frag";
        spd.layout.count  = 2;
        spd.layout.stride = 24;
        spd.layout.attribs[0] = {0, 0,  GpuAttribFmt::F3};   // aPos   at offset 0
        spd.layout.attribs[1] = {1, 12, GpuAttribFmt::F3};   // aColor at offset 12
        spd.raster.depth_test  = true;
        spd.raster.depth_write = true;
        spd.raster.cull_back   = false;  // open geometry — no closed mesh, all faces needed
        spd.vert_uniform_bufs  = 1;   // set=1 binding=0: MVP
        spd.vert_samplers      = 0;
        spd.frag_samplers      = 0;
        spd.frag_uniform_bufs  = 0;
        spd.color_format       = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        spd.has_depth_target   = true;
        if (!s_scene_pipeline.Create(spd))
            fprintf(stderr,"[CharPreview] scene pipeline create failed\n");

        // ── Procedural geometry: platform + anthropometer ───────────────────
        // Coordinate system: Y=0 = character feet level.
        // Platform: flat raised dock, planks run in Z, Y from -0.14 to 0.
        // Pole: X=+0.65, Z=0, Y from 0 to 2.2m, with tick marks.

        struct SV { float x,y,z, r,g,b; };
        static SV verts[1024];
        static uint16_t idxs[4096];
        int vi=0, ii=0;

        auto quad=[&](SV a,SV b,SV c,SV d){
            // two triangles: abc + acd
            uint16_t base=(uint16_t)vi;
            verts[vi++]=a; verts[vi++]=b; verts[vi++]=c; verts[vi++]=d;
            idxs[ii++]=base; idxs[ii++]=base+1; idxs[ii++]=base+2;
            idxs[ii++]=base; idxs[ii++]=base+2; idxs[ii++]=base+3;
        };
        auto rgb=[](float r,float g,float b){ return SV{0,0,0,r,g,b}; };

        // ── Platform planks ───────────────────────────────────────────────
        // 7 planks, each 0.26m wide, 0.015m gap, running Z: -0.7 to +0.9
        // Total width: 7*0.26 + 6*0.015 = 1.82 + 0.09 = 1.91m → x: -0.955..+0.955
        float pz0=-0.7f, pz1=0.9f;
        float px=-0.955f;
        float plank_w=0.26f, gap=0.015f;
        float py_top=0.01f, py_bot=-0.13f;  // +1cm above foot Y=0 to avoid Z-fight
        for(int p=0;p<7;p++){
            float px0=px, px1=px+plank_w;
            float shade=(p%2==0)?0.f:0.06f;
            float dr=0.32f+shade, dg=0.20f+shade*0.7f, db=0.11f+shade*0.4f;
            float sr=0.38f+shade, sg=0.25f+shade*0.7f, sb=0.14f+shade*0.4f;
            SV tl={px0,py_top,pz0,sr,sg,sb}, tr={px1,py_top,pz0,sr,sg,sb};
            SV bl={px0,py_top,pz1,dr,dg,db}, br={px1,py_top,pz1,dr,dg,db};
            quad(tl,tr,br,bl);   // top face
            // Front edge face (Z=pz1, facing +Z)
            SV fe0={px0,py_bot,pz1,0.18f,0.11f,0.06f};
            SV fe1={px1,py_bot,pz1,0.18f,0.11f,0.06f};
            SV fe2={px1,py_top,pz1,0.22f,0.14f,0.08f};
            SV fe3={px0,py_top,pz1,0.22f,0.14f,0.08f};
            quad(fe0,fe1,fe2,fe3);
            px += plank_w + gap;
        }

        // ── Anthropometer pole ────────────────────────────────────────────
        // Positioned X=+0.65 (viewer's right of character), Z=0
        float pole_x0=0.630f, pole_x1=0.658f;
        float pole_z0=-0.014f, pole_z1=0.014f;
        float pole_y0=0.f,   pole_y1=2.10f;
        float pr2=0.30f, pg2=0.22f, pb2=0.15f;
        // Front face (+Z)
        quad({pole_x0,pole_y0,pole_z1,pr2,pg2,pb2},{pole_x1,pole_y0,pole_z1,pr2,pg2,pb2},
             {pole_x1,pole_y1,pole_z1,pr2,pg2,pb2},{pole_x0,pole_y1,pole_z1,pr2,pg2,pb2});
        // Back face (-Z)
        float pr3=0.22f,pg3=0.16f,pb3=0.10f;
        quad({pole_x1,pole_y0,pole_z0,pr3,pg3,pb3},{pole_x0,pole_y0,pole_z0,pr3,pg3,pb3},
             {pole_x0,pole_y1,pole_z0,pr3,pg3,pb3},{pole_x1,pole_y1,pole_z0,pr3,pg3,pb3});
        // Right face (+X)
        float pr4=0.26f,pg4=0.19f,pb4=0.12f;
        quad({pole_x1,pole_y0,pole_z0,pr4,pg4,pb4},{pole_x1,pole_y0,pole_z1,pr4,pg4,pb4},
             {pole_x1,pole_y1,pole_z1,pr4,pg4,pb4},{pole_x1,pole_y1,pole_z0,pr4,pg4,pb4});

        // ── Tick marks (extending to the right of the pole) ───────────────
        float tick_r=0.55f, tick_g=0.45f, tick_b=0.32f;
        for(int ti=1; ti<=20; ti++){
            float ty = (float)ti * 0.10f;
            float th = 0.007f;      // half-height
            bool major = (ti % 10 == 0);
            bool medium= (ti % 5  == 0);
            float tw = major ? 0.11f : (medium ? 0.07f : 0.04f);
            float tx0=pole_x1, tx1=pole_x1+tw;
            float tz0=pole_z0, tz1=pole_z1;
            // Top face of tick
            quad({tx0,ty+th,tz0,tick_r,tick_g,tick_b},{tx1,ty+th,tz0,tick_r,tick_g,tick_b},
                 {tx1,ty+th,tz1,tick_r,tick_g,tick_b},{tx0,ty+th,tz1,tick_r,tick_g,tick_b});
            // Front face of tick
            quad({tx0,ty-th,tz1,tick_r*0.8f,tick_g*0.8f,tick_b*0.8f},
                 {tx1,ty-th,tz1,tick_r*0.8f,tick_g*0.8f,tick_b*0.8f},
                 {tx1,ty+th,tz1,tick_r,tick_g,tick_b},
                 {tx0,ty+th,tz1,tick_r,tick_g,tick_b});
        }

        s_scene_ni = ii;
        if (ii > 0) {
            // Expand uint16 indices to uint32 for GpuStaticBuffer
            uint32_t* idx32 = new uint32_t[ii];
            for(int k=0;k<ii;k++) idx32[k]=(uint32_t)idxs[k];
            s_scene_vbo.Init(0x8892u, verts, (uint32_t)(vi*sizeof(SV)));
            s_scene_ibo.Init(0x8893u, idx32, (uint32_t)(ii*sizeof(uint32_t)));
            delete[] idx32;
        }
    }

    s_ok=true;
    return true;
}

// ── RenderFrame: render T-pose to RTT (call before ImGui render) ──────────────
static void RenderFrame(SDL_GPUCommandBuffer* cmd) {
    if (!s_ok||!s_color||s_rtt_w<4||s_rtt_h<4) return;

    // Upload morphed vertex positions if any blend shape weights changed
    if (s_morphs_dirty && s_base_verts_cpu && s_morph_count > 0 && s_vbo.SDLBuffer()) {
        static Vtx s_mbuf[131072];
        size_t vc = (size_t)s_base_vc;
        memcpy(s_mbuf, s_base_verts_cpu, vc * sizeof(Vtx));
        for (int m = 0; m < s_morph_count; ++m) {
            float w = s_morph_weights[m];
            if (w == 0.f) continue;
            const float* dl = s_morph_deltas + (size_t)m * vc * 3;
            for (size_t v = 0; v < vc; ++v) {
                s_mbuf[v].px += w * dl[v*3+0];
                s_mbuf[v].py += w * dl[v*3+1];
                s_mbuf[v].pz += w * dl[v*3+2];
            }
        }
        SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
        uint32_t up_sz = (uint32_t)(vc * sizeof(Vtx));
        SDL_GPUTransferBufferCreateInfo mtb={};
        mtb.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; mtb.size=up_sz;
        SDL_GPUTransferBuffer* mtr=SDL_CreateGPUTransferBuffer(dev,&mtb);
        if (mtr) {
            void* mp=SDL_MapGPUTransferBuffer(dev,mtr,false);
            if (mp){memcpy(mp,s_mbuf,up_sz);SDL_UnmapGPUTransferBuffer(dev,mtr);}
            SDL_GPUCopyPass* cp=SDL_BeginGPUCopyPass(cmd);
            SDL_GPUTransferBufferLocation msrc={mtr,0};
            SDL_GPUBufferRegion mdst={s_vbo.SDLBuffer(),0,up_sz};
            SDL_UploadToGPUBuffer(cp,&msrc,&mdst,false);
            SDL_EndGPUCopyPass(cp);
            SDL_ReleaseGPUTransferBuffer(dev,mtr);
        }
        s_morphs_dirty = false;
    }

    // Upload bone world-scale matrices (120×1 texture) via copy pass
    if (s_bones_tex) {
        SDL_GPUDevice* dev=md::GpuDevice::Get().SDLDevice();
        uint32_t up_sz=120*4*4;
        SDL_GPUTransferBufferCreateInfo tb={};
        tb.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tb.size=up_sz;
        SDL_GPUTransferBuffer* tr=SDL_CreateGPUTransferBuffer(dev,&tb);
        void* mp=SDL_MapGPUTransferBuffer(dev,tr,false);
        if(mp){memcpy(mp,s_ws_mat,up_sz);SDL_UnmapGPUTransferBuffer(dev,tr);}
        SDL_GPUCopyPass* cp=SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src={tr,0,(uint32_t)120,(uint32_t)1};
        SDL_GPUTextureRegion dst={s_bones_tex,0,0,0,0,0,120,1,1};
        SDL_UploadToGPUTexture(cp,&src,&dst,false);
        SDL_EndGPUCopyPass(cp);
        SDL_ReleaseGPUTransferBuffer(dev,tr);
    }

    // Hierarchical bone propagation (sl[12] *= parent[0]) already moves origins —
    // per-bone H in setSpine([0]=H) IS setMovementScale. No separate model Y-scale.
    // Translate: put feet (bind-pose Y ≈ -0.95) at ground (Y=0); offset scales with H.
    M4 model = m4_translate(0.f, -s_height*0.95f, 0.f);

    // Orbit view + perspective
    M4 view = m4_mul(m4_translate(0.f, -s_lookat_y, -s_dist), m4_mul(m4_rotX(s_pit), m4_rotY(s_yaw)));
    float asp=(float)s_rtt_w/(float)s_rtt_h;
    M4 proj = m4_persp(0.78f, asp, 0.05f, 10.f);
    M4 mvp  = m4_mul(proj, m4_mul(view, model));

    // Render pass on RTT
    SDL_GPUColorTargetInfo ct={};
    ct.texture=s_color; ct.load_op=SDL_GPU_LOADOP_CLEAR;
    ct.store_op=SDL_GPU_STOREOP_STORE;
    ct.clear_color={0.0f,0.0f,0.0f,1.f};  // bg pipeline overwrites this

    SDL_GPUDepthStencilTargetInfo di={};
    di.texture=s_depth; di.clear_depth=1.f;
    di.load_op=SDL_GPU_LOADOP_CLEAR; di.store_op=SDL_GPU_STOREOP_STORE;
    di.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE;
    di.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* rp=SDL_BeginGPURenderPass(cmd,&ct,1,&di);
    if (!rp) return;

    // ── Background: sky + perspective ground plane ───────────────────────────
    if (s_bg_pipeline.SDLPipeline()) {
        SDL_BindGPUGraphicsPipeline(rp, s_bg_pipeline.SDLPipeline());

        // Compute camera world-space vectors from view matrix inverse
        float inv_view[16]; m4inv_rigid(inv_view, view.m);
        // inv_view col-major: col0=right, col1=up, col2=-fwd, col3=eye_pos
        float tan_vfov = tanf(0.39f);   // half of 0.78 fov
        float tan_hfov = tan_vfov * asp;
        struct BgUU {
            float right[4];   // xyz=cam_right,   w=tan_hfov
            float up[4];      // xyz=cam_up,       w=tan_vfov
            float fwd[4];     // xyz=cam_fwd,      w=ground_y in world
            float eye[4];     // xyz=cam_pos world, w=0
        } bgu;
        bgu.right[0]=inv_view[0]; bgu.right[1]=inv_view[1]; bgu.right[2]=inv_view[2]; bgu.right[3]=tan_hfov;
        bgu.up[0]=inv_view[4];    bgu.up[1]=inv_view[5];    bgu.up[2]=inv_view[6];    bgu.up[3]=tan_vfov;
        // Camera forward = -col2 of inv_view (camera looks along -Z in view space)
        bgu.fwd[0]=-inv_view[8];  bgu.fwd[1]=-inv_view[9];  bgu.fwd[2]=-inv_view[10];
        bgu.fwd[3] = -(s_height*0.95f);  // ground_y in world = model offset (feet at Y=0)
        bgu.eye[0]=inv_view[12];  bgu.eye[1]=inv_view[13];  bgu.eye[2]=inv_view[14];  bgu.eye[3]=0;
        SDL_PushGPUFragmentUniformData(cmd, 0, &bgu, sizeof(bgu));
        // Bind desert ground textures (set=2, bindings 0 and 1)
        if (s_bg_sand.SDLTexture() && s_bg_dune.SDLTexture()) {
            SDL_GPUTextureSamplerBinding bg_tex[2] = {
                {s_bg_sand.SDLTexture(), s_bg_sand.SDLSampler()},
                {s_bg_dune.SDLTexture(), s_bg_dune.SDLSampler()}
            };
            SDL_BindGPUFragmentSamplers(rp, 0, bg_tex, 2);
        }
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    }

    // ── Scene geometry: platform + pole ──────────────────────────────────────
    if (s_scene_pipeline.SDLPipeline() && s_scene_vbo.SDLBuffer() && s_scene_ni > 0) {
        SDL_BindGPUGraphicsPipeline(rp, s_scene_pipeline.SDLPipeline());
        SDL_GPUBufferBinding svb{s_scene_vbo.SDLBuffer(),0u};
        SDL_BindGPUVertexBuffers(rp,0,&svb,1);
        SDL_GPUBufferBinding sib{s_scene_ibo.SDLBuffer(),0u};
        SDL_BindGPUIndexBuffer(rp,&sib,SDL_GPU_INDEXELEMENTSIZE_32BIT);
        VU svu; memcpy(svu.mvp, mvp.m, 64);
        SDL_PushGPUVertexUniformData(cmd,0,&svu,sizeof(svu));
        SDL_DrawGPUIndexedPrimitives(rp,s_scene_ni,1,0,0,0);
    }

    if (!s_pipeline.SDLPipeline() || !s_vbo.SDLBuffer() || !s_ibo.SDLBuffer() ||
        !s_tex.SDLTexture()        || !s_tex.SDLSampler()        ||
        !s_tex_head.SDLTexture()   || !s_tex_head.SDLSampler()   ||
        !s_tex_muscle.SDLTexture() || !s_tex_muscle.SDLSampler() ||
        !s_tex_blood.SDLTexture()  || !s_tex_blood.SDLSampler()  ||
        !s_bones_tex               || !s_bones_sampler) {
        SDL_EndGPURenderPass(rp); return;
    }

    SDL_BindGPUGraphicsPipeline(rp, s_pipeline.SDLPipeline());

    SDL_GPUBufferBinding vb{s_vbo.SDLBuffer(),0u};
    SDL_BindGPUVertexBuffers(rp,0,&vb,1);

    SDL_GPUBufferBinding ib{s_ibo.SDLBuffer(),0u};
    SDL_BindGPUIndexBuffer(rp,&ib,SDL_GPU_INDEXELEMENTSIZE_32BIT);

    // Bind bone scale texture as vertex sampler slot 0
    SDL_GPUTextureSamplerBinding bsb{s_bones_tex, s_bones_sampler};
    SDL_BindGPUVertexSamplers(rp,0,&bsb,1);

    VU vu; memcpy(vu.mvp, mvp.m, 64);
    SDL_PushGPUVertexUniformData(cmd,0,&vu,sizeof(vu));

    FU fu{};
    fu.skin[0]=s_skin[0]; fu.skin[1]=s_skin[1]; fu.skin[2]=s_skin[2];
    fu.str=s_str; fu.sat=s_sat; fu.bri=s_bri; fu.muscle=s_muscle;
    fu.hair[0]=s_hair[0]; fu.hair[1]=s_hair[1]; fu.hair[2]=s_hair[2];
    SDL_PushGPUFragmentUniformData(cmd,0,&fu,sizeof(fu));

    SDL_GPUTextureSamplerBinding ftb[4] = {
        { s_tex.SDLTexture(),        s_tex.SDLSampler()        },
        { s_tex_head.SDLTexture(),   s_tex_head.SDLSampler()   },
        { s_tex_muscle.SDLTexture(), s_tex_muscle.SDLSampler() },
        { s_tex_blood.SDLTexture(),  s_tex_blood.SDLSampler()  },
    };
    SDL_BindGPUFragmentSamplers(rp,0,ftb,4);

    SDL_DrawGPUIndexedPrimitives(rp,(uint32_t)s_ni,1,0,0,0);
    SDL_EndGPURenderPass(rp);
}

// ── Bone scale CPU → GPU: call once per frame before DrawInImGui ─────────────
// All body/face neutrals = Kenshi range midpoints (kBodyDef / kFaceDef).
// scale = val/neutral → 1.0 at neutral. Clamped [0.1, 4].
//
// Body neutrals (corrected per male_editor.cfg):
//   [2]Ht=100 [3]Fr=100 [7]LL=100 [8]Sho=100 [9]Arm=107.5
//   [10]Wst=100 [11]Hnd=100 [12]Cst=100 [13]Stm=100 [15]Hip=100 [16]Leg=100 [17]Ft=100
// Face neutrals: [0]HdSz=100 [1]HdSh=100 [2]Nk=108 [3]NkW=110 [17]Jaw=100
// RE-verified axis assignments (kenshi_x64.exe.c analysis):
//   scale = slider_value / 100.0f  (direct linear, neutral=100)
//   Stomach  → Bip01 Spine  Z only  (belly DEPTH, not width — prevents barrel-chest)
//   Waist    → Bip01 Spine1 XZ
//   Chest    → Bip01 Spine2 XZ
//   Arm bulk → UpperArm+Forearm XZ
//   Frame    → global body-width multiplier on torso XZ
// Kenshi RE (kenshi_x64.exe.c FUN_140015b63 = lerp, verified 2026-05-29).
// IMPORTANT: md_human.glb mesh was exported at Kenshi slider=100 proportions —
// NOT raw T-pose. So slider=100 → ws_mat=I (no deformation) is CORRECT.
// Race multipliers (0.70/1.27/etc.) apply to raw T-pose only; our mesh already
// bakes slider=100 state → do NOT apply race multipliers here.
// Bone indices — confirmed from md_human_t.glb (the actual loaded file):
//   0=Bip01(root)  1=Pelvis
//   2=L Thigh  3=L Calf  4=L Foot  5=L Toe0  6=L Toe0Nub
//   7=R Thigh  8=R Calf  9=R Foot 10=R Toe0 11=R Toe0Nub
//  12=Spine  13=Spine1  14=Spine2
//  15=L Clavicle 16=L UpperArm 17=L Forearm 18=L Hand  19=Prop1
//  20=Neck   21=Head    22=HeadNub 23=Jaw    24=JawNub
//  25=R Clavicle 26=R UpperArm 27=R Forearm 28=R Hand  29=Prop2
// Bone axes: localX=+worldY(height) for spine/leg; localX=arm-direction for arm bones.
// setBoneSize → s_boneScales (vertex deformation only, doesn't move children).
// setBonePositionalSize → s_posScale (scales bind translation from parent).
static void SetBoneScalesFromDef(const float body[18], const float face[24]) {
    for(int i=0;i<30;i++){
        s_boneScales[i][0]=1;s_boneScales[i][1]=1;s_boneScales[i][2]=1;
        s_posScale[i][0]=1;s_posScale[i][1]=1;s_posScale[i][2]=1;
    }

    // ── Sample breathing animation at current time ─────────────────────────
    static float s_pose_rot[30][4];
    static float s_pose_tra[30][3];
    if (s_breath_loaded && s_breath_len > 0.f) {
        float t = fmodf((float)((SDL_GetTicks() - s_anim_epoch_ms) * 0.001), s_breath_len);
        SampleBreathing(t, s_pose_rot, s_pose_tra);
    } else {
        // Fallback: copy idle pose
        for(int i=0;i<30;i++){
            memcpy(s_pose_rot[i], s_idle_rot[i], 16);
            s_pose_tra[i][0]=s_bind_local[i][12];
            s_pose_tra[i][1]=s_bind_local[i][13];
            s_pose_tra[i][2]=s_bind_local[i][14];
        }
    }

    // Apply slider pose animations on top of breathing (Kenshi RE: setTimePosition formula)
    // time = anim_length * slider_value * 0.01  → normalised alpha = slider_value * 0.01
    ApplySliderAnim(s_anim_postures,     body[4] * 0.01f, s_pose_rot);
    ApplySliderAnim(s_anim_shoulder_set, body[5] * 0.01f, s_pose_rot); // Shoulder set → "shoulder set" anim
    ApplySliderAnim(s_anim_neck_set,     body[6] * 0.01f, s_pose_rot); // Neck pos     → "neck set" anim

    auto cl=[](float x) -> float { return x<0.1f?0.1f:(x>4.f?4.f:x); };
    auto comp=[&](float x, float k) -> float { return cl(1.f + (x - 1.f)*k); };
    // setBS(i, wy, wx, wz): s_boneScales for bone i. wy=local-X scale (height/length axis),
    // wx=local-Y scale (lateral), wz=local-Z scale (depth).
    auto setBS=[&](int i, float wy, float wx, float wz){
        s_boneScales[i][0]=wy; s_boneScales[i][1]=wx; s_boneScales[i][2]=wz;
    };

    float H   = cl(body[2]  / 100.f);  // Height
    float Fr  = cl(body[3]  / 100.f);  // Frame
    float LL  = cl(body[7]  / 100.f);  // Leg length
    float Wa  = cl(body[10] / 100.f);  // Waist
    float St  = cl(body[13] / 100.f);  // Stomach
    float Ch  = cl(body[12] / 100.f);  // Chest
    float Ab  = cl(body[9]  / 100.f);  // Arm bulk
    float Sh  = cl(body[8]  / 100.f);  // Shoulders
    float Hips= cl(body[15] / 100.f);  // Hips
    float LgB = cl(body[16] / 100.f);  // Legs bulk
    float Hn  = cl(body[11] / 100.f);  // Hands
    float Ft  = cl(body[17] / 100.f);  // Feet
    float LgS = 1.f;  // LegShape — not in our body[], default neutral=1.0

    // All factors = 1.0 at slider=100 → ws_mat=I → bind-pose appearance. ✓
    // Kenshi RE: FUN_140015b63, race mults = 1.0 for Greenlander (baked into mesh).

    // ── Lower body ────────────────────────────────────────────────────────────
    // overall_XZ = (LgShape*LgBulk + (Hips-1)/3) * Frame   [Kenshi line 263670]
    float overall_XZ = cl((LgS * LgB + (Hips - 1.f) / 3.f) * Fr);
    float leg_Y = cl((H + LL - 1.f) * 0.95f);  // vertex height for thigh+calf

    // Pelvis [1]: setBoneSize(comp(Hips,0.6)*Fr, H, comp(Hips,0.6)*Fr)   [Kenshi RE line 263670]
    setBS(1, H, comp(Hips,0.6f)*Fr, comp(Hips,0.6f)*Fr);

    // Thighs [2,7]: setBoneSize Y=(H+LL-1)*0.95, XZ=overall_XZ
    // Thighs also get setBonePositionalSize Y = Fr*(2-H)*Hips (hip width adjustment)
    // bind_local[thigh] = (0, ±0.099, 0) — Y is the lateral offset in Pelvis space
    // → scale sl[1] (Y-translation = lateral) by thigh_pos_Y
    setBS(2, leg_Y, overall_XZ, overall_XZ);
    setBS(7, leg_Y, overall_XZ, overall_XZ);
    float thigh_pos = cl(Fr * (2.f - H) * Hips);
    s_posScale[2][1] = thigh_pos;  // Pelvis-local Y = lateral direction for thigh
    s_posScale[7][1] = thigh_pos;

    // Calves [3,8]: same vertex Y, different XZ (no setBonePositionalSize)
    // bind_local[calf] = (+0.439, 0, 0) — X is along thigh bone direction
    // Calf follows thigh origin (no posScale) — leg length from vertex deformation only
    float calf_XZ = cl((2.f - LgS) * LgB * Fr);
    setBS(3, leg_Y * calf_XZ, calf_XZ*calf_XZ, calf_XZ*calf_XZ);  // wy scales with bulk; XZ quadratic
    setBS(8, leg_Y * calf_XZ, calf_XZ*calf_XZ, calf_XZ*calf_XZ);

    // Feet [4,9]: setBoneSize(Feet²*H², LL*Ft*H, Feet²*H²)
    float FtH = cl(Ft * H);
    setBS(4, cl(LL * FtH), cl(FtH*FtH), cl(FtH*FtH));
    setBS(9, cl(LL * FtH), cl(FtH*FtH), cl(FtH*FtH));

    // Toes [5,10]: FtH² uniform + posScale Z=FtH (toe extends forward with foot size)
    float FtH2 = FtH*FtH;
    for(int ji:{5,10}){ s_boneScales[ji][0]=FtH2; s_boneScales[ji][1]=FtH2; s_boneScales[ji][2]=FtH2; }
    s_posScale[5][2] = FtH;
    s_posScale[10][2] = FtH;

    // ── Torso ─────────────────────────────────────────────────────────────────
    // Spine [12]: setBoneSize(comp(Hips,0.6)*Fr, H, comp(Hips,0.6)*St*Fr)   [line 263808/817]
    float HipsC = comp(Hips, 0.6f);
    setBS(12, H, HipsC*Fr, HipsC*St*Fr);

    // Spine1 [13]: setBoneSize(Waist*Fr, H, Stomach*Fr)   [line 263835/842]
    setBS(13, H, Wa*Fr, St*Fr);

    // Spine2 [14]: setBoneSize(comp(Ch,0.45)*Fr, H, comp(Ch,0.9)*Fr)   [line 263851/858]
    setBS(14, H, comp(Ch,0.45f)*Fr, comp(Ch,0.9f)*Fr);

    // ── Arms ──────────────────────────────────────────────────────────────────
    // Clavicles [15,25]: (Sh*Fr, comp(Sh,0.3)*Fr, Sh*Fr)   [line 264048/057]
    float ShY = comp(Sh, 0.3f)*Fr;
    for(int ji:{15,25}){ s_boneScales[ji][0]=Sh*Fr; s_boneScales[ji][1]=ShY; s_boneScales[ji][2]=Sh*Fr; }

    // UpperArms [16,26]: setBoneSize(Ab²Fr², H*Ab*Fr, comp(Ab,1.5)*Ab*Fr²)  [line 264001/011]
    //   setBonePositionalSize(Sh*comp(Ch,0.45), 1, 1) — scales X (arm direction) offset
    float AbFr = Ab*Fr;
    float AbZ  = comp(Ab, 1.5f)*Fr;
    for(int ji:{16,26}){ s_boneScales[ji][0]=AbFr*AbFr; s_boneScales[ji][1]=H*AbFr; s_boneScales[ji][2]=AbZ*AbFr; }
    // bind_local[upperarm] = (+0.165, 0, 0) — X along clavicle/arm direction
    float arm_pos = cl(Sh * H);  // Kenshi: Shoulders * Height (not comp(Ch,0.45))
    s_posScale[16][0] = arm_pos;
    s_posScale[26][0] = arm_pos;

    // Forearms [17,27]: setBoneSize(Ab²Fr², H*Ab*Fr, Ab²Fr²)   [line 264024/034]
    for(int ji:{17,27}){ s_boneScales[ji][0]=AbFr*AbFr; s_boneScales[ji][1]=H*AbFr; s_boneScales[ji][2]=AbFr*AbFr; }

    // Hands [18,28]: setBoneSize(AbFr*Hn², H*Hn², AbFr*Hn²)   [line 264099/109]
    for(int ji:{18,28}){ s_boneScales[ji][0]=AbFr*Hn*Hn; s_boneScales[ji][1]=H*Hn*Hn; s_boneScales[ji][2]=AbFr*Hn*Hn; }

    // ── Head/Neck ─────────────────────────────────────────────────────────────
    float Nw  = cl(face[3]  / 100.f);  // Neck width
    float Nl  = cl(face[4]  / 100.f);  // Neck length
    float jaw = cl(face[17] / 100.f);  // Jaw — also drives Neck bone Z (depth)
    // Neck [20]: wy=Nl, wx=Nw*Fr, wz=jaw*Fr   [Kenshi RE line 264237]
    setBS(20, Nl, Nw*Fr, jaw*Fr);

    // Head [21]: wy=comp(Fr,0.25)*Hd, wx=same*Hsp, wz=same   [line 264251]
    float Hd  = cl(face[0] / 100.f);
    float FrH = comp(Fr, 0.25f);
    float Hsp = cl(face[1] / 100.f);
    setBS(21, FrH*Hd, FrH*Hd*Hsp, FrH*Hd);

    // Jaw [23]: wy=FrH*Hd, wx=FrH*Hd*Hsp*jaw, wz=FrH*Hd   [line 264290]
    s_boneScales[23][0]=FrH*Hd; s_boneScales[23][1]=FrH*Hd*Hsp*jaw; s_boneScales[23][2]=FrH*Hd;

    // OGRE-style hierarchical: setBoneSize and setBonePositionalSize are INDEPENDENT.
    //   s_posScale[i] scales bone i's bind translation from its parent (position only).
    //   s_boneScales[i] scales vertices around bone i (does NOT affect child positions).
    //   new_world[i] = new_world[parent] * (bind_local[i] with translation * s_posScale[i])
    //   ws_mat[i]    = new_world[i] * diag(s_boneScales[i]) * inv_bind[i]
    // Three-tier pose system (matches Kenshi "breathing noarms" design):
    //   kBreathList  — spine/root/neck: breathing animation (torso sway)
    //   kIdleArmList — arm chain: idle_stand_normal last frame (natural rest, no float)
    //   neither      — legs: bind pose (T-pose, no ground issues)
    static const bool kBreathList[30] = {
        true,  true,                    // [0]=ROOT [1]=Pelvis
        false,false,false,false,false,  // [2-6]  legs — bind pose
        false,false,false,false,false,  // [7-11] legs — bind pose
        true,  true,  true,             // [12-14] Spine Spine1 Spine2
        false, false, false, false, false, // [15-19] L arm — idle pose
        true,  true,  false, true, false,  // [20-24] Neck Head Jaw
        false, false, false, false, false  // [25-29] R arm — idle pose
    };
    static const bool kIdleArmList[30] = {
        false, false,
        false,false,false,false,false,
        false,false,false,false,false,
        false, false, false,
        true,  true,  true,  true, false,  // [15-19] L Clav UpperArm Forearm Hand
        false, false, false, false, false,
        true,  true,  true,  true, false   // [25-29] R Clav UpperArm Forearm Hand
    };
    float new_world[30][16];
    for (int i = 0; i < 30; i++) {
        float sl[16];
        if (kBreathList[i]) {
            float tp[3] = {
                s_pose_tra[i][0] * s_posScale[i][0],
                s_pose_tra[i][1] * s_posScale[i][1],
                s_pose_tra[i][2] * s_posScale[i][2]
            };
            m4_from_quat_t(sl, s_pose_rot[i], tp);
        } else if (kIdleArmList[i]) {
            // Arms: idle_stand_normal last frame (natural rest pose, away from shorts)
            float tp[3] = {
                s_bind_local[i][12] * s_posScale[i][0],
                s_bind_local[i][13] * s_posScale[i][1],
                s_bind_local[i][14] * s_posScale[i][2]
            };
            m4_from_quat_t(sl, s_idle_rot[i], tp);
        } else {
            memcpy(sl, s_bind_local[i], 64);
            sl[12] *= s_posScale[i][0];
            sl[13] *= s_posScale[i][1];
            sl[14] *= s_posScale[i][2];
        }
        if (s_bone_parent[i] < 0)
            memcpy(new_world[i], sl, 64);
        else
            m4mul(new_world[i], new_world[(int)s_bone_parent[i]], sl);

        float sx=s_boneScales[i][0], sy=s_boneScales[i][1], sz=s_boneScales[i][2];
        float S[16]={sx,0,0,0, 0,sy,0,0, 0,0,sz,0, 0,0,0,1};
        float tmp[16];
        m4mul(tmp, S, s_inv_bind[i]);
        m4mul(s_ws_mat[i], new_world[i], tmp);
    }
}

// ── Face morph target wiring ─────────────────────────────────────────────────
// face[i] → (positive morph name, negative morph name).
// Weight at neutral (def): 0.  Above def → pos weight.  Below def → neg weight.
struct FME { int idx; const char* pos; const char* neg; };
// face[] index → (pos morph name, neg morph name)
// All indices map to kFaceLbl[] in character_editor.h.
// face[14]="Eyes depth"  → shallow_eyes   (was unused "Cheekbone ht.")
// face[22]="Eyes tilt"   → tiltup/tiltdown_eyes (was unused "Chin width")
// face[23]="Nose pos."   → high_nose      (was unused "Chin protrusion")
// face[10]="Nose length" → long_nose      (corrected from high_nose)
static const FME kFaceMorphMap[] = {
    {  5, "big_eyes",           nullptr            },  // Eye size
    {  6, "narrow_eyes",        nullptr            },  // Eye shape
    {  7, "close_eyes",         nullptr            },  // Eye spacing
    {  8, "high_eyes",          nullptr            },  // Eye height
    {  9, "wide_nose",          nullptr            },  // Nose width
    { 10, "long_nose",          nullptr            },  // Nose length  ← fixed (was high_nose)
    { 11, "arch_nose",          nullptr            },  // Nose depth
    { 12, "tiltup_nose",        "tiltdown_nose"    },  // Nose tip
    { 13, "wide_cheekbones",    "narrow_cheekbones"},  // Cheekbone
    { 14, "shallow_eyes",       nullptr            },  // Eyes depth   ← new
    { 15, "tiltup_brow",        "tiltdown_brow"    },  // Brow
    { 16, "high_brow",          "low_brow"         },  // Brow height
    { 18, "wide_mouth",         nullptr            },  // Mouth width
    { 20, "big_mouth",          nullptr            },  // Lips
    { 21, "overbite",           "underbite"        },  // Chin
    { 22, "tiltup_eyes",        "tiltdown_eyes"    },  // Eyes tilt    ← new
    { 23, "high_nose",          nullptr            },  // Nose pos.    ← new
};
static constexpr int kFaceMorphMapN = 17;

static int s_morph_idx_by_name(const char* name) {
    for (int i = 0; i < s_morph_count; ++i)
        if (strcmp(s_morph_names[i], name) == 0) return i;
    return -1;
}

// Body morph weights — SECONDARY organic layer (bone scale is primary, matches Kenshi RE).
// Kenshi uses bone-only; morphs here add soft-tissue deformation on top.
// Weights are reduced (0.3x) to avoid fighting with bone scale.
// Negative deviations → no morph (bone scale handles the "thin" side).
static void SetBodyMorphWeights(const float body[18], const float face[24]) {
    auto set = [](const char* n, float w) {
        int mi=s_morph_idx_by_name(n);
        if(mi>=0) s_morph_weights[mi]=w<0.f?0.f:(w>1.f?1.f:w);
    };
    auto pd = [](float v, float neu, float rng) -> float {
        float d=(v-neu)/rng; return d<0.f?0.f:(d>1.f?1.f:d);
    };
    set("fat",      (pd(body[13],100.f,90.f)*0.65f
                   + pd(body[15],100.f,45.f)*0.20f
                   + pd(body[12],100.f,40.f)*0.15f) * 0.3f);

    set("muscular", (pd(body[9], 100.f,45.f)*0.65f
                   + pd(body[8], 100.f,10.f)*0.15f
                   + pd(body[12],100.f,40.f)*0.20f) * 0.3f);

    set("longlegs", pd(body[7], 100.f,15.f) * 0.3f);
    set("bighead",  pd(face[0], 100.f,10.f) * 0.3f);

    set("broadshdr",(pd(body[3], 100.f,20.f)*0.55f
                   + pd(body[8], 100.f,10.f)*0.45f) * 0.3f);

    set("tall",     pd(body[2], 100.f,20.f) * 0.15f);

    s_morphs_dirty = true;
}

// Call once per frame with current face[], def[], lo[], hi[] arrays.
static void SetMorphWeightsFromFace(const float face[], const float def[],
                                     const float lo[],  const float hi[]) {
    memset(s_morph_weights, 0, sizeof(s_morph_weights));
    for (int e = 0; e < kFaceMorphMapN; ++e) {
        const FME& m = kFaceMorphMap[e];
        float val = face[m.idx];
        float d   = def[m.idx];
        float rhi = hi[m.idx] - d;
        float rlo = d - lo[m.idx];
        if (m.pos && rhi > 1e-6f) {
            int mi = s_morph_idx_by_name(m.pos);
            if (mi >= 0) {
                float w = (val - d) / rhi;
                w = w < 0.f ? 0.f : (w > 1.f ? 1.f : w);
                // Kenshi RE: floorf(value * 10) → 10 discrete steps (0.0,0.1,...,1.0)
                s_morph_weights[mi] = floorf(w * 10.f) / 10.f;
            }
        }
        if (m.neg && rlo > 1e-6f) {
            int mi = s_morph_idx_by_name(m.neg);
            if (mi >= 0) {
                float w = (d - val) / rlo;
                w = w < 0.f ? 0.f : (w > 1.f ? 1.f : w);
                s_morph_weights[mi] = floorf(w * 10.f) / 10.f;
            }
        }
    }
    s_morphs_dirty = true;
}

// ── Portrait config (game/data/chars/portrait.cfg) ───────────────────────────
struct PortraitCfg {
    float portrait_dist     = 0.72f;
    float portrait_offset_y = 0.88f;
    float portrait_fov      = 0.78f;
    float body_dist         = 2.6f;
    float body_pit          = -0.06f;
};
static PortraitCfg s_pcfg;
static bool        s_pcfg_loaded = false;

static void LoadPortraitCfg() {
    FILE* f = fopen("game/data/chars/portrait.cfg", "r");
    if (!f) return;
    char line[128];
    while (fgets(line, sizeof(line), f)) {
        if (line[0]=='#'||line[0]=='\n') continue;
        char key[64]; float val;
        if (sscanf(line, "%63s = %f", key, &val) == 2) {
            if (!strcmp(key,"portrait_dist"))     s_pcfg.portrait_dist     = val;
            if (!strcmp(key,"portrait_offset_y")) s_pcfg.portrait_offset_y = val;
            if (!strcmp(key,"portrait_fov"))      s_pcfg.portrait_fov      = val;
            if (!strcmp(key,"body_dist"))         s_pcfg.body_dist         = val;
            if (!strcmp(key,"body_pit"))          s_pcfg.body_pit          = val;
        }
    }
    fclose(f);
    s_pcfg_loaded = true;
    fprintf(stdout,"[CharPreview] portrait.cfg: dist=%.2f offset_y=%.2f\n",
            s_pcfg.portrait_dist, s_pcfg.portrait_offset_y);
}

// ── Camera preset per active tab ──────────────────────────────────────────────
static void SetCameraForTab(int tab) {
    if (!s_pcfg_loaded) LoadPortraitCfg();
    if (tab == 0) {
        s_dist     = s_pcfg.body_dist;
        s_pit      = s_pcfg.body_pit;
        s_lookat_y = 0.f;
    } else {
        s_dist     = s_pcfg.portrait_dist;
        s_pit      = 0.f;
        s_yaw      = 0.f;
        s_lookat_y = s_height * s_pcfg.portrait_offset_y;
    }
}

// ── DrawInImGui: orbit input + show RTT ──────────────────────────────────────
static void DrawInImGui(float W, float H,
                        float height_scale, float bulk_scale,
                        const float skin_rgb[3], float skin_str,
                        float sat, float bri,
                        float muscle = 0.f,
                        const float hair_rgb[3] = nullptr)
{
    if (!s_ok) {
        ImGui::Dummy({W,H});
        return;
    }
    int iw=(int)W, ih=(int)H;
    if (iw<4||ih<4) return;

    // Save params for RenderFrame
    s_height=height_scale; s_bulk=bulk_scale;
    s_skin[0]=skin_rgb[0]; s_skin[1]=skin_rgb[1]; s_skin[2]=skin_rgb[2];
    s_str=skin_str; s_sat=sat; s_bri=bri;
    s_muscle=muscle;
    if (hair_rgb) { s_hair[0]=hair_rgb[0]; s_hair[1]=hair_rgb[1]; s_hair[2]=hair_rgb[2]; }

    // Create/resize RTT here so s_color is stable when AddImage captures it
    ensure_rtt(iw, ih);

    ImVec2 origin=ImGui::GetCursorScreenPos();

    // Invisible button captures mouse input (RMB for orbit)
    ImGui::InvisibleButton("##cpv",{W,H}, ImGuiButtonFlags_MouseButtonRight);
    bool hov=ImGui::IsItemHovered();
    ImGuiIO& io=ImGui::GetIO();

    // Portrait auto-rotation (Kenshi RE: yaw oscillates slowly when not dragging)
    // yaw = ((frame%500)/1000 - 0.25) * PI  → ±0.785 rad (±45°) at ~30fps
    static bool s_portrait_mode = false;
    // Switch portrait mode on tab change (set by SetCameraForTab)
    static int  s_last_tab_for_portrait = 0;
    if (s_last_tab_for_portrait != (s_lookat_y > 0.5f ? 1 : 0)) {
        s_last_tab_for_portrait = (s_lookat_y > 0.5f ? 1 : 0);
        s_portrait_mode = (s_lookat_y > 0.5f);
    }
    if (s_portrait_mode && !s_drag) {
        uint64_t ms = SDL_GetTicks() - s_anim_epoch_ms;
        uint32_t fr = (uint32_t)(ms / 33);
        s_yaw = ((float)(fr % 500) / 1000.f - 0.25f) * 3.14159f * 0.6f;
        s_pit = ((float)(fr % 252) / 1000.f - 0.083f) * 3.14159f * 0.25f;
    }

    // RMB drag = yaw only (no pitch) — interrupts auto-rotation
    if (hov && io.MouseClicked[1]) { s_drag=true; s_d0=io.MousePos; s_y0=s_yaw; }
    if (s_drag) {
        if (io.MouseDown[1]) s_yaw = s_y0 + (io.MousePos.x - s_d0.x) * 0.007f;
        else s_drag = false;
    }
    // Scroll = zoom
    if (hov && io.MouseWheel!=0.f) {
        s_dist-=io.MouseWheel*0.18f;
        if (s_dist<0.5f) s_dist=0.5f;
        if (s_dist>6.0f) s_dist=6.0f;
    }

    // Display RTT (UV Y not flipped — SDL_GPU origin is top-left)
    if (s_color)
        ImGui::GetWindowDrawList()->AddImage(
            (ImTextureID)s_color, origin, {origin.x+W,origin.y+H});
    else
        ImGui::GetWindowDrawList()->AddRectFilled(origin,{origin.x+W,origin.y+H},
            IM_COL32(20,20,28,255));

    if (hov)
        ImGui::GetWindowDrawList()->AddRect(origin,{origin.x+W,origin.y+H},
            IM_COL32(80,120,200,120),2.f);

    // ── Height ruler overlay (right edge) ────────────────────────────────────
    // project (0, world_y, 0) → screen_y using the same view+proj as RenderFrame
    {
        M4 view = m4_mul(m4_translate(0.f,-s_lookat_y,-s_dist), m4_mul(m4_rotX(s_pit), m4_rotY(s_yaw)));
        M4 proj = m4_persp(0.78f, (float)iw/(float)ih, 0.05f, 10.f);
        M4 vp   = m4_mul(proj, view);

        // Map real-world meters → model-space Y → world-space Y → screen Y
        // MODEL_HEIGHT ≈ 1.80 model units = KENSHI_H_M (1.73m) at scale 1.0
        static constexpr float MODEL_TOP    = 1.80f;  // T-pose crown Y in model units
        static constexpr float KENSHI_H_M   = 1.73f;  // meters at height_scale=1.0
        static constexpr float M_PER_UNIT   = KENSHI_H_M / MODEL_TOP;

        auto screen_y_for_m = [&](float h_m) -> float {
            float model_y = h_m / M_PER_UNIT;
            // model → world: scale(h, h, h) * translate(0, -0.95*h, 0)
            float world_y = height_scale * (model_y - 0.95f * height_scale);
            // project (0, world_y, 0, 1) — column-major: row1 = m[1],m[5],m[9],m[13]
            float cy = vp.m[5]*world_y + vp.m[13];
            float cw = vp.m[7]*world_y + vp.m[15];
            float ndc_y = (fabsf(cw) > 1e-6f) ? cy/cw : 0.f;
            return origin.y + (1.f - ndc_y) * 0.5f * H;
        };

        ImDrawList* dl = ImGui::GetWindowDrawList();
        float rx = origin.x + W - 28.f;   // ruler X centre line
        float char_h_m = height_scale * KENSHI_H_M;

        // Background strip
        dl->AddRectFilled({rx-2, origin.y}, {rx+26, origin.y+H}, IM_COL32(0,0,0,90));

        // Ticks every 0.25m, labels every 0.5m, range 0..2.5m
        for (int tick = 0; tick <= 10; ++tick) {
            float h_m = tick * 0.25f;
            float sy = screen_y_for_m(h_m);
            if (sy < origin.y - 2.f || sy > origin.y + H + 2.f) continue;

            bool major = (tick % 2 == 0);
            float tw = major ? 10.f : 5.f;
            ImU32 col = major ? IM_COL32(220,220,220,220) : IM_COL32(160,160,160,140);
            dl->AddLine({rx, sy}, {rx + tw, sy}, col, 1.f);

            if (major && h_m > 0.1f) {
                char lbl[8]; snprintf(lbl, sizeof(lbl), "%.1fm", h_m);
                dl->AddText({rx + 13.f, sy - 6.f}, col, lbl);
            }
        }

        // Vertical ruler line
        float sy0 = screen_y_for_m(0.f), sy_top = screen_y_for_m(2.5f);
        dl->AddLine({rx, sy_top}, {rx, sy0}, IM_COL32(200,200,200,160), 1.f);

        // Character height marker (bold tick + label)
        float sy_h = screen_y_for_m(char_h_m);
        if (sy_h >= origin.y && sy_h <= origin.y + H) {
            dl->AddLine({rx - 3.f, sy_h}, {rx + 14.f, sy_h}, IM_COL32(100,200,255,255), 2.f);
            char lbl[12]; snprintf(lbl, sizeof(lbl), "%.2fm", char_h_m);
            dl->AddText({rx + 13.f, sy_h - 7.f}, IM_COL32(100,200,255,255), lbl);
        }
    }

    // Hint
    ImGui::SetCursorScreenPos({origin.x+4, origin.y+H-20});
    ImGui::PushStyleColor(ImGuiCol_Text,IM_COL32(160,160,180,160));
    ImGui::TextUnformatted("RMB=rotate  Scroll=zoom");
    ImGui::PopStyleColor();
}

} // namespace CharPreviewSDLGPU

#endif // MD_SDL_GPU
