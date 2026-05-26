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
struct VU { float mvp[16]; float normalMat[16]; };  // 128 bytes, set=1
struct FU {                                          // 48 bytes, set=3
    float skin[3]; float str;
    float sat; float bri; float muscle; float pad;
    float hair[3]; float hairpad;
};

// ── State ─────────────────────────────────────────────────────────────────────
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

// Per-bone scale (xyz) + pivot_y (w) — 30 bones, updated each frame
static float s_boneScales[30][4]; // [bone_idx][xyzw]

static SDL_GPUTexture* s_color = nullptr;
static SDL_GPUTexture* s_depth = nullptr;
static int             s_rtt_w = 0, s_rtt_h = 0;

static float  s_yaw = 0.18f, s_pit = -0.06f, s_dist = 2.6f;
static bool   s_drag = false;
static ImVec2 s_d0;
static float  s_y0;

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
        float v[4]={}; cgltf_accessor_read_float(aj,i,v,4);
        vb[i].ji[0]=(uint8_t)v[0]; vb[i].ji[1]=(uint8_t)v[1];
        vb[i].ji[2]=(uint8_t)v[2]; vb[i].ji[3]=(uint8_t)v[3];
    }
    if (aw) for (size_t i=0;i<vc;i++) {
        float v[4]={}; cgltf_accessor_read_float(aw,i,v,4);
        vb[i].wt[0]=v[0]; vb[i].wt[1]=v[1];
        vb[i].wt[2]=v[2]; vb[i].wt[3]=v[3];
    }

    s_ni=(int)pr.indices->count;
    uint32_t* ib=new uint32_t[s_ni];
    for (int i=0;i<s_ni;i++) ib[i]=(uint32_t)cgltf_accessor_read_index(pr.indices,(size_t)i);
    cgltf_free(d);

    s_vbo.Init(0x8892u, vb, (uint32_t)(vc*sizeof(Vtx)));   // 0x8892=GL_ARRAY_BUFFER
    s_ibo.Init(0x8893u, ib, (uint32_t)(s_ni*sizeof(uint32_t))); // 0x8893=GL_ELEMENT_ARRAY_BUFFER
    delete[] vb; delete[] ib;

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
    // Bone scale texture: 30×1 RGBA32F — GpuTexture only does RGBA8, so raw SDL_GPU
    {
        for (int i=0;i<30;i++){ s_boneScales[i][0]=1;s_boneScales[i][1]=1;s_boneScales[i][2]=1;s_boneScales[i][3]=0; }
        SDL_GPUDevice* dev = md::GpuDevice::Get().SDLDevice();
        SDL_GPUTextureCreateInfo ti={};
        ti.type=SDL_GPU_TEXTURETYPE_2D;
        ti.format=SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        ti.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width=30; ti.height=1; ti.layer_count_or_depth=1; ti.num_levels=1;
        s_bones_tex=SDL_CreateGPUTexture(dev,&ti);
        if (!s_bones_tex) {
            fprintf(stderr,"[CharPreview] bones tex create failed: %s\n",SDL_GetError());
        }
        SDL_GPUSamplerCreateInfo si={};
        si.min_filter=SDL_GPU_FILTER_NEAREST; si.mag_filter=SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        s_bones_sampler=SDL_CreateGPUSampler(dev,&si);
        // Initial upload (only if texture was created)
        if (s_bones_tex) {
            uint32_t up_sz=30*4*4; // 30 texels × 4 channels × 4 bytes
            SDL_GPUTransferBufferCreateInfo tb={};
            tb.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tb.size=up_sz;
            SDL_GPUTransferBuffer* tr=SDL_CreateGPUTransferBuffer(dev,&tb);
            if (tr) {
                void* mp=SDL_MapGPUTransferBuffer(dev,tr,false);
                if(mp){memcpy(mp,s_boneScales,up_sz);SDL_UnmapGPUTransferBuffer(dev,tr);}
                SDL_GPUCommandBuffer* uc=SDL_AcquireGPUCommandBuffer(dev);
                if (uc) {
                    SDL_GPUCopyPass* cp=SDL_BeginGPUCopyPass(uc);
                    SDL_GPUTextureTransferInfo src={tr,0,(uint32_t)30,(uint32_t)1};
                    SDL_GPUTextureRegion dst={s_bones_tex,0,0,0,0,0,30,1,1};
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
    s_ok=true;
    return true;
}

// ── RenderFrame: render T-pose to RTT (call before ImGui render) ──────────────
static void RenderFrame(SDL_GPUCommandBuffer* cmd) {
    if (!s_ok||!s_color||s_rtt_w<4||s_rtt_h<4) return;

    // Upload bone scales via copy pass (before render pass)
    if (s_bones_tex) {
        SDL_GPUDevice* dev=md::GpuDevice::Get().SDLDevice();
        uint32_t up_sz=30*4*4;
        SDL_GPUTransferBufferCreateInfo tb={};
        tb.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tb.size=up_sz;
        SDL_GPUTransferBuffer* tr=SDL_CreateGPUTransferBuffer(dev,&tb);
        void* mp=SDL_MapGPUTransferBuffer(dev,tr,false);
        if(mp){memcpy(mp,s_boneScales,up_sz);SDL_UnmapGPUTransferBuffer(dev,tr);}
        SDL_GPUCopyPass* cp=SDL_BeginGPUCopyPass(cmd);
        SDL_GPUTextureTransferInfo src={tr,0,(uint32_t)30,(uint32_t)1};
        SDL_GPUTextureRegion dst={s_bones_tex,0,0,0,0,0,30,1,1};
        SDL_UploadToGPUTexture(cp,&src,&dst,false);
        SDL_EndGPUCopyPass(cp);
        SDL_ReleaseGPUTransferBuffer(dev,tr);
    }

    // Model: scale by bulk/height, translate feet to origin
    M4 ms; ms.m[0]=s_bulk; ms.m[5]=s_height; ms.m[10]=s_bulk;
    M4 mt = m4_translate(0.f, -0.95f*s_height, 0.f);
    M4 model = m4_mul(ms, mt);
    // Normal matrix: inverse of scale = scale(1/bulk, 1/height, 1/bulk)
    M4 norm; norm.m[0]=1.f/s_bulk; norm.m[5]=1.f/s_height; norm.m[10]=1.f/s_bulk;

    // Orbit view + perspective
    M4 view = m4_mul(m4_translate(0.f,0.f,-s_dist), m4_mul(m4_rotX(s_pit), m4_rotY(s_yaw)));
    float asp=(float)s_rtt_w/(float)s_rtt_h;
    M4 proj = m4_persp(0.78f, asp, 0.05f, 10.f);
    M4 mvp  = m4_mul(proj, m4_mul(view, model));

    // Render pass on RTT
    SDL_GPUColorTargetInfo ct={};
    ct.texture=s_color; ct.load_op=SDL_GPU_LOADOP_CLEAR;
    ct.store_op=SDL_GPU_STOREOP_STORE;
    ct.clear_color={0.10f,0.10f,0.13f,1.f};

    SDL_GPUDepthStencilTargetInfo di={};
    di.texture=s_depth; di.clear_depth=1.f;
    di.load_op=SDL_GPU_LOADOP_CLEAR; di.store_op=SDL_GPU_STOREOP_STORE;
    di.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE;
    di.stencil_store_op=SDL_GPU_STOREOP_DONT_CARE;

    SDL_GPURenderPass* rp=SDL_BeginGPURenderPass(cmd,&ct,1,&di);
    if (!rp) return;

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

    VU vu; memcpy(vu.mvp, mvp.m,64); memcpy(vu.normalMat,norm.m,64);
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
// body[18] and face[24] are the 0-100 slider values from character_editor Def.
static void SetBoneScalesFromDef(const float body[18], const float face[24]) {
    for (int i=0;i<30;i++){ s_boneScales[i][0]=1;s_boneScales[i][1]=1;s_boneScales[i][2]=1;s_boneScales[i][3]=0; }

    // t: normalised deviation from slider midpoint; range ≈ -0.5..+0.5
    auto t=[](float v){ return (v-50.f)/100.f; };

    // ── BODY ────────────────────────────────────────────────────────────────
    // Leg length (body[7], def=100): thigh+calf Y
    float ly=1.f+t(body[7])*0.5f;
    for(int ji:{2,3,7,8}) s_boneScales[ji][1]=ly;

    // Legs (body[16], def=100): thigh+calf thickness (XZ)
    float lxz=1.f+t(body[16])*0.5f;
    for(int ji:{2,3,7,8}){ s_boneScales[ji][0]*=lxz; s_boneScales[ji][2]*=lxz; }

    // Shoulders (body[8], def=100): clavicle + upper arm XZ
    float sh=1.f+t(body[8])*0.5f;
    for(int ji:{15,25,16,26}){ s_boneScales[ji][0]=sh; s_boneScales[ji][2]=sh; }

    // Arm bulk (body[9], def=40): arm XZ thickness
    float arm=1.f+t(body[9])*0.6f;
    for(int ji:{16,17,26,27}){ s_boneScales[ji][0]*=arm; s_boneScales[ji][2]*=arm; }

    // Hands (body[11], def=40): hand uniform
    float hnd=1.f+t(body[11])*0.5f;
    for(int ji:{18,28}){ s_boneScales[ji][0]=hnd; s_boneScales[ji][1]=hnd; s_boneScales[ji][2]=hnd; }

    // Chest (body[12], def=100): Spine2 XZ
    float cst=1.f+t(body[12])*0.5f;
    s_boneScales[14][0]=cst; s_boneScales[14][2]=cst;

    // Stomach (body[13], def=40): Spine XZ
    float stm=1.f+t(body[13])*0.4f;
    s_boneScales[12][0]=stm; s_boneScales[12][2]=stm;

    // Hips (body[15], def=40): Pelvis XZ
    float hps=1.f+t(body[15])*0.5f;
    s_boneScales[1][0]=hps; s_boneScales[1][2]=hps;

    // Bot build (body[10], def=40): lower body volume (pelvis XZ)
    float bot=1.f+t(body[10])*0.4f;
    s_boneScales[1][0]*=bot; s_boneScales[1][2]*=bot;

    // Foot size (body[17], def=40): foot+toe uniform
    float ft=1.f+t(body[17])*0.6f;
    for(int ji:{4,5,6,9,10,11}){ s_boneScales[ji][0]=ft; s_boneScales[ji][1]=ft; s_boneScales[ji][2]=ft; }

    // ── FACE ────────────────────────────────────────────────────────────────
    // Head size (face[0], def=40): head uniform
    float hdsz=1.f+t(face[0])*0.6f;
    s_boneScales[21][0]=hdsz; s_boneScales[21][1]=hdsz; s_boneScales[21][2]=hdsz;

    // Neck length (face[2], def=40): neck Y
    s_boneScales[20][1]=1.f+t(face[2])*0.5f;
    // Neck width (face[3], def=40): neck XZ
    float nxz=1.f+t(face[3])*0.4f;
    s_boneScales[20][0]=nxz; s_boneScales[20][2]=nxz;

    // ── Pivot Y (model-space bone centre, ~1.8 total height) ─────────────
    static const float kPivY[30]={
        0.95f,  // 0 Bip01
        0.95f,  // 1 Pelvis
        0.80f,  // 2 L Thigh
        0.45f,  // 3 L Calf
        0.05f,  // 4 L Foot
        0.01f,  // 5 L Toe0
        0.00f,  // 6 L Toe0Nub
        0.80f,  // 7 R Thigh
        0.45f,  // 8 R Calf
        0.05f,  // 9 R Foot
        0.01f,  //10 R Toe0
        0.00f,  //11 R Toe0Nub
        1.00f,  //12 Spine
        1.10f,  //13 Spine1
        1.25f,  //14 Spine2
        1.35f,  //15 L Clavicle
        1.35f,  //16 L UpperArm
        1.15f,  //17 L Forearm
        0.95f,  //18 L Hand
        1.00f,  //19 Prop1
        1.50f,  //20 Neck
        1.65f,  //21 Head
        1.75f,  //22 HeadNub
        1.70f,  //23 Jaw
        1.68f,  //24 JawNub
        1.35f,  //25 R Clavicle
        1.35f,  //26 R UpperArm
        1.15f,  //27 R Forearm
        0.95f,  //28 R Hand
        1.00f,  //29 Prop2
    };
    for(int i=0;i<30;i++) s_boneScales[i][3]=kPivY[i];
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
