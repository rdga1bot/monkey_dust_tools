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
static GpuPipeline     s_pipeline;
static GpuStaticBuffer s_vbo;
static GpuStaticBuffer s_ibo;
static GpuTexture      s_tex;
static GpuTexture      s_tex_muscle;  // 1×1 neutral muscle mask
static GpuTexture      s_tex_blood;   // 1×1 clear blood overlay
// Bone scale texture: 30×1 RGBA32F — raw SDL_GPU (GpuTexture only supports RGBA8)
static SDL_GPUTexture* s_bones_tex     = nullptr;
static SDL_GPUSampler* s_bones_sampler = nullptr;
static int             s_ni  = 0;
static bool            s_ok  = false;

// Per-bone scale (xyz) — 30 bones, updated each frame
static float s_boneScales[30][3]; // [bone_idx][xyz]
// World-space deformation matrices — OGRE-style hierarchical:
//   new_world[i] = new_world[parent] * (bind_local[i] with translation scaled by parent S)
//   ws_mat[i]    = new_world[i] * S[i] * inv_bind[i]
// Parent scale moves child bones (scales local translation), but does NOT cascade into
// child vertex scaling — only this bone's own S[i] affects its vertices.
// At neutral S=I: new_world[i]=bind[i], ws_mat[i]=I.
static float s_ws_mat[30][16];
static float s_inv_bind[30][16];   // inverseBindMatrices from GLB (world→bone local)
static float s_bind[30][16];       // bind matrices = inv(inv_bind)
static float s_bind_local[30][16]; // bind_local[i] = inv_bind[parent] * bind[i]
static int8_t s_bone_parent[30];   // parent joint index, -1 for root

// col-major mat4 multiply: C = A * B
static void m4mul(float* C, const float* A, const float* B) {
    float T[16];
    for (int j=0;j<4;j++) for (int i=0;i<4;i++) {
        float s=0.f; for (int k=0;k<4;k++) s+=A[k*4+i]*B[j*4+k]; T[j*4+i]=s;
    }
    memcpy(C,T,64);
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

static float  s_yaw = 0.18f, s_pit = -0.06f, s_dist = 2.6f;
static bool   s_drag = false;
static ImVec2 s_d0;
static float  s_y0;

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
    }
    // Precompute bind_local[i] = inv_bind[parent] * bind[i] (local bind TRS in parent space)
    for (int i=0;i<30;i++) {
        if (s_bone_parent[i]<0)
            memcpy(s_bind_local[i], s_bind[i], 64);
        else
            m4mul(s_bind_local[i], s_inv_bind[(int)s_bone_parent[i]], s_bind[i]);
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
    // Muscle mask placeholder: mid-grey (r=0.5 → neutral muscle detail)
    { uint8_t p[4]={128,128,128,255}; GpuSamplerDesc sd; s_tex_muscle.InitFromMemory(p,1,1,sd); }
    // Blood overlay placeholder: fully transparent (no wounds)
    { uint8_t p[4]={0,0,0,0}; GpuSamplerDesc sd; s_tex_blood.InitFromMemory(p,1,1,sd); }
    // Bone matrix texture: 120×1 RGBA32F — 30 bones × 4 columns per mat4
    // Each bone i occupies texels [i*4 .. i*4+3] = columns 0-3 of the 4×4 Ws matrix.
    {
        for (int i=0;i<30;i++){ s_boneScales[i][0]=1;s_boneScales[i][1]=1;s_boneScales[i][2]=1; }
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
    pd.raster.cull_back = true;
    pd.vert_uniform_bufs = 1;   // set=1 binding=0: VU
    pd.vert_samplers = 1;       // set=1 binding=1: uBoneScales (bone scale texture)
    pd.frag_samplers = 3;       // set=2: body_diffuse, muscle_mask, blood_overlay
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
        bgpd.frag_samplers      = 0;
        bgpd.frag_uniform_bufs  = 0;
        bgpd.color_format       = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        bgpd.has_depth_target   = true;
        if (!s_bg_pipeline.Create(bgpd))
            fprintf(stderr,"[CharPreview] bg pipeline create failed\n");
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

    // Model: scale by bulk/height, translate feet to origin
    M4 ms; ms.m[0]=s_bulk; ms.m[5]=s_height; ms.m[10]=s_bulk;
    M4 mt = m4_translate(0.f, -0.95f*s_height, 0.f);
    M4 model = m4_mul(ms, mt);

    // Orbit view + perspective
    M4 view = m4_mul(m4_translate(0.f,0.f,-s_dist), m4_mul(m4_rotX(s_pit), m4_rotY(s_yaw)));
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

    // ── Background gradient (fullscreen tri, no VBO) ─────────────────────────
    if (s_bg_pipeline.SDLPipeline()) {
        SDL_BindGPUGraphicsPipeline(rp, s_bg_pipeline.SDLPipeline());
        SDL_DrawGPUPrimitives(rp, 3, 1, 0, 0);
    }

    if (!s_pipeline.SDLPipeline() || !s_vbo.SDLBuffer() || !s_ibo.SDLBuffer() ||
        !s_tex.SDLTexture()        || !s_tex.SDLSampler()        ||
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

    SDL_GPUTextureSamplerBinding ftb[3] = {
        { s_tex.SDLTexture(),        s_tex.SDLSampler()        },
        { s_tex_muscle.SDLTexture(), s_tex_muscle.SDLSampler() },
        { s_tex_blood.SDLTexture(),  s_tex_blood.SDLSampler()  },
    };
    SDL_BindGPUFragmentSamplers(rp,0,ftb,3);

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
// RE-verified mapping from kenshi_x64.exe.c FUN_140015b63 (lines 263517-264450).
// Kenshi uses bone scale ONLY — no blend shapes for body proportions.
// comp(x,k) = (x-1)*k+1  compresses deviation toward neutral (reduces rubber-tube).
// H=Height*0.01, Fr=Frame*0.01  — H scales Y chain, Fr scales all XZ.
// GLB bone local axis mapping (probe 2026-05-28, inverseBindMatrices analysis):
// Spine chain (0-14,20,21): local X = +worldY (HEIGHT), local Y = +worldX (LATERAL), local Z = -worldZ (DEPTH)
// Arm bones (16-18,26-28): local X = -worldY (arm girth vert), local Y = +worldX (arm LENGTH), local Z = +worldZ
// Clavicles (15,25): local X = ±worldX, local Y = ±worldY → formula stays same (scale magnitude = world)
//
// CRITICAL: the Kenshi RE formulas give (world-X-scale, world-Y-scale, world-Z-scale).
// We must remap to (local-X, local-Y, local-Z) for the ws_mat = bind*S*inv_bind computation.
// Spine/Pelvis/Leg: [localX=worldY, localY=worldX, localZ≈worldZ] → swap first two values.
// Arms: already mapped correctly (local Y = arm length = worldX = "height direction for arms").
//
// Shorthand: set3(i, wY_val, wX_val, wZ_val) — pass height, lateral, depth in world terms.
static void SetBoneScalesFromDef(const float body[18], const float face[24]) {
    for(int i=0;i<30;i++){ s_boneScales[i][0]=1;s_boneScales[i][1]=1;s_boneScales[i][2]=1; }

    auto clamp=[](float x) -> float { return x<0.1f?0.1f:(x>4.f?4.f:x); };
    auto comp=[&](float x, float k) -> float { return clamp(1.f + (x - 1.f)*k); };
    // Remap world-space scale (wy=height, wx=lateral, wz=depth) to bone local (lx,ly,lz).
    // For spine/pelvis/leg bones: local X = worldY, local Y = worldX → swap.
    auto setSpine=[&](int i, float wy, float wx, float wz){
        s_boneScales[i][0]=wy; s_boneScales[i][1]=wx; s_boneScales[i][2]=wz;
    };

    float H  = clamp(body[2]  / 100.f);
    float Fr = clamp(body[3]  / 100.f);
    float LL = clamp(body[7]  / 100.f);
    float Wa = clamp(body[10] / 100.f);
    float St = clamp(body[13] / 100.f);
    float Ch = clamp(body[12] / 100.f);
    float Ab = clamp(body[9]  / 100.f);
    float Sh = clamp(body[8]  / 100.f);
    float Hd = clamp(face[0]  / 100.f);
    float Nl = clamp(face[2]  / 108.f);
    float Nw = clamp(face[3]  / 110.f);
    float Ft = clamp(body[17] / 100.f * H);

    // Pelvis [1]: localX=worldY=H, localY=worldX=LL*Fr, localZ=worldZ=LL*Fr
    setSpine(1, H, LL*Fr, LL*Fr);

    // Thighs/Calves [2,3,7,8]: localX=worldY=LL*H (leg length+height), localY/Z=LgBulk
    float LgBulk = clamp(body[16] / 100.f * Fr);
    for(int ji:{2,3,7,8}) setSpine(ji, LL*H, LgBulk, LgBulk);

    // Feet [4,9]: localX=worldY=LL, localY/Z=Ft*Ft
    for(int ji:{4,9})      setSpine(ji, LL, Ft*Ft, Ft*Ft);
    // Toes: uniform
    for(int ji:{5,6,10,11}){ s_boneScales[ji][0]=Ft*Ft; s_boneScales[ji][1]=Ft*Ft; s_boneScales[ji][2]=Ft*Ft; }

    // Spine [12]: localX=H, localY=comp(Wa,0.6)*Fr, localZ=comp(Wa,0.6)*St*Fr
    float WaC = comp(Wa, 0.6f);
    setSpine(12, H, WaC*Fr, WaC*St*Fr);

    // Spine1 [13]: localX=H, localY=Ch*Fr, localZ=St*Fr
    setSpine(13, H, Ch*Fr, St*Fr);

    // Spine2 [14]: localX=H, localY=comp(Ch,0.45)*Fr, localZ=comp(Ch,0.9)*Fr
    setSpine(14, H, comp(Ch,0.45f)*Fr, comp(Ch,0.9f)*Fr);

    // Clavicles [15,25]: local X≈worldX, local Y≈worldY — no swap needed
    float ShY = comp(Sh, 0.3f)*Fr;
    for(int ji:{15,25}){ s_boneScales[ji][0]=Sh*Fr; s_boneScales[ji][1]=ShY; s_boneScales[ji][2]=Sh*Fr; }

    // UpperArm [16,26]: localX=-worldY(arm girth), localY=worldX(arm length), localZ=worldZ
    // local axes for arms differ from spine — no swap needed here.
    float AbFr = Ab*Fr;
    float AbZ  = comp(Ab, 1.5f)*Fr;
    for(int ji:{16,26}){
        s_boneScales[ji][0]=AbFr*AbFr;
        s_boneScales[ji][1]=H*AbFr;
        s_boneScales[ji][2]=AbZ*AbFr;
    }
    for(int ji:{17,27}){
        s_boneScales[ji][0]=AbFr*AbFr;
        s_boneScales[ji][1]=H*AbFr;
        s_boneScales[ji][2]=AbFr*AbFr;
    }
    float Hn = clamp(body[11] / 100.f);
    for(int ji:{18,28}){ s_boneScales[ji][0]=AbFr*Hn*Hn; s_boneScales[ji][1]=H*AbFr*Hn; s_boneScales[ji][2]=AbFr*Hn*Hn; }

    // Neck [20]: localX=worldY=Nl (neck length), localY=worldX=Nw*Fr, localZ=Nw*Fr
    setSpine(20, Nl, Nw*Fr, Nw*Fr);

    // Head [21]: localX=worldY=Hd, localY=worldX=FrH*Hd*Hsp, localZ=worldZ=FrH*Hd
    float FrH = comp(Fr, 0.25f);
    float Hsp = clamp(face[1] / 100.f);
    setSpine(21, Hd, clamp(FrH*Hd*Hsp), FrH*Hd);

    // Jaw [23]: local X = +worldZ (different from spine pattern — use raw RE values)
    float jaw = clamp(face[17] / 100.f);
    s_boneScales[23][0]=jaw*FrH*Hd*Hsp; s_boneScales[23][1]=Hd; s_boneScales[23][2]=FrH*Hd;

    // OGRE-style hierarchical scale:
    //   Parent S moves children (scales bind_local translation), children scale own vertices only.
    //   new_world[i] = new_world[parent] * bind_local_with_scaled_translation[i]
    //   ws_mat[i]    = new_world[i] * S[i] * inv_bind[i]
    // No scale cascade — each bone's vertices only scale by its own S[i].
    float new_world[30][16];
    for (int i = 0; i < 30; i++) {
        // Copy bind_local, then scale its translation by parent's bone scale
        float sl[16]; memcpy(sl, s_bind_local[i], 64);
        if (s_bone_parent[i] >= 0) {
            int p = s_bone_parent[i];
            sl[12] *= s_boneScales[p][0];
            sl[13] *= s_boneScales[p][1];
            sl[14] *= s_boneScales[p][2];
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
                s_morph_weights[mi] = w < 0.f ? 0.f : (w > 1.f ? 1.f : w);
            }
        }
        if (m.neg && rlo > 1e-6f) {
            int mi = s_morph_idx_by_name(m.neg);
            if (mi >= 0) {
                float w = (d - val) / rlo;
                s_morph_weights[mi] = w < 0.f ? 0.f : (w > 1.f ? 1.f : w);
            }
        }
    }
    s_morphs_dirty = true;
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

    // Invisible button captures mouse input
    ImGui::InvisibleButton("##cpv",{W,H}, ImGuiButtonFlags_MouseButtonLeft);
    bool hov=ImGui::IsItemHovered();
    ImGuiIO& io=ImGui::GetIO();

    // LMB drag = yaw only (no pitch)
    if (hov && io.MouseClicked[0]) { s_drag=true; s_d0=io.MousePos; s_y0=s_yaw; }
    if (s_drag) {
        if (io.MouseDown[0]) s_yaw = s_y0 + (io.MousePos.x - s_d0.x) * 0.007f;
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
        M4 view = m4_mul(m4_translate(0.f,0.f,-s_dist), m4_mul(m4_rotX(s_pit), m4_rotY(s_yaw)));
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
    ImGui::TextUnformatted("LMB=rotate  Scroll=zoom");
    ImGui::PopStyleColor();
}

} // namespace CharPreviewSDLGPU

#endif // MD_SDL_GPU
