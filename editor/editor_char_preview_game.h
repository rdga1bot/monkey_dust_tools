#pragma once
#ifdef MD_SDL_GPU
// editor_char_preview_game.h — Character preview: char_preview.vert + char_preview.frag.
// Per-fragment texture sampling (body/head/muscle/blood), proper skin tinting (18% blend).
// Bone matrices via 120×1 RGBA32F vertex sampler (vert_storage_bufs=0 → avoids Intel HD 520
// vert_storage_bufs + frag_samplers binding bug).  Same public API as CharPreviewSDLGPU.

#include "imgui.h"
#include <monkey_dust/render/hair_shading.h>
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
#include <monkey_dust/render/skin_mesh.h>
#include <monkey_dust/render/char_customization.h>
#include <monkey_dust/platform/window.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "stb_image.h"
#include "cgltf.h"
#include <cstring>
#include <cstdio>
#include <cmath>

namespace CharPreviewGame {

// ── Scene: background + platform + anthropometer pole ────────────────────────
static GpuPipeline     s_bg_pipeline;
static GpuTexture      s_bg_sand;
static GpuTexture      s_bg_dune;
static GpuPipeline     s_scene_pipeline;
static GpuStaticBuffer s_scene_vbo;
static GpuStaticBuffer s_scene_ibo;
static int             s_scene_ni = 0;

// ── Character state ───────────────────────────────────────────────────────────
static SkinMesh        s_mesh;
static GpuPipeline     s_pipeline;
static GpuStaticBuffer s_skin_vbo;           // SkinVertex stride=52 (re-uploaded for morphs)
// Bone matrix texture: kept for hair pipeline (char_hair.vert uses vert sampler, frag_samplers=0 → passes Intel validation).
// Body/cloth pipelines now use a UBO (set=1 binding=1) to avoid vert_samplers+frag_samplers Intel Gen9 hang.
static SDL_GPUTexture* s_bones_tex     = nullptr;
static SDL_GPUSampler* s_bones_sampler = nullptr;
// CPU copy of bone matrices for body/cloth UBO push (30 mat4 = 1920 bytes).
static float           s_bones_cpu[30*16];
// Fragment textures (set=2 binding=0..3): body, head, muscle, blood
static GpuTexture      s_tex_body;
static GpuTexture      s_tex_head;
static GpuTexture      s_tex_muscle;
static GpuTexture      s_tex_blood;
// RTT
static SDL_GPUTexture* s_color_rtt = nullptr;
static SDL_GPUTexture* s_depth_rtt = nullptr;
static int             s_rtt_w = 0, s_rtt_h = 0;
// Lifecycle
static bool  s_ok          = false;
static int   s_idle_clip   = -1;
static bool  s_morphs_dirty = true;
static float s_body_cache[CHARCC_BODY_N];
static float s_face_cache[CHARCC_FACE_N];
// Camera
static float s_yaw = 0.f, s_pit = -0.06f, s_dist = 3.5f, s_lookat_y = 0.9f;
static float s_height = 1.f;
// Appearance (passed to FU each frame — no CPU pre-baking needed)
static float s_skin_rgb[3] = {0.82f, 0.65f, 0.52f};
static float s_skin_str    = 0.55f;
static float s_sat         = 1.f, s_bri = 0.f;
static float s_muscle      = 0.f;
static float s_hair[3]     = {0.25f, 0.15f, 0.08f};

// ── Hair state ────────────────────────────────────────────────────────────────
struct HairFU { float hair[3]; float pad; };
static GpuPipeline     s_hair_pipeline;
static GpuStaticBuffer s_hair_vbo;
static GpuStaticBuffer s_hair_ibo;
static int             s_hair_ni          = 0;
static int             s_hair_style       = 0;
static const char* const s_hair_styles[] = {
    "hair01","hair02","hair03","hair04","hair05","hair06","hair07","hair08",
    "haircut_male01","haircut_male02","haircut_male03","haircut_male04","haircut_male05",
    "haircut_male06","haircut_male07","haircut_male08","haircut_male09","haircut_male10",
    "haircut_female01","haircut_female02","haircut_female03","haircut_female04",
    "haircut_female05","haircut_female06","haircut_female07","haircut_female08",
    "hairlongbald","frizzhair","nutfro","afrohair",
};
constexpr int          s_hair_style_count = 30;

// ── Clothes state ─────────────────────────────────────────────────────────────
struct ClothFU { float color[3]; float pad; };
struct ClothSlot {
    GpuStaticBuffer vbo, ibo;
    int ni = 0;
    bool loaded = false;
};
static constexpr int CLOTH_MAX_SLOTS = 3;   // top, bottom, accessory
static ClothSlot       s_cloth[CLOTH_MAX_SLOTS];
static GpuPipeline     s_cloth_pipeline;
static bool            s_clothes_visible = true;

static int   s_sex   = 0;    // 0=Male 1=Female — set by Init(), drives cloth path selection
static float s_leg_y = 0.95f; // cached leg scale from body sliders; drives foot grounding

struct ClothItemDef {
    const char* name;
    const char* path_m;   // game/data/clothes/*_m.clothbin or shared
    const char* path_f;   // nullptr = fall back to path_m
    float       color[3];
    int         slot;
};
static const ClothItemDef s_cloth_items[] = {
    // slot 0 — top
    {"None",         nullptr,                                          nullptr,                                       {0,0,0},             0},
    {"Slave Shirt",  "game/data/clothes/slave_shirt.clothbin",        "game/data/clothes/slave_shirt_f.clothbin",   {0.62f,0.59f,0.53f}, 0},
    {"Drifter Coat", "game/data/clothes/drifter_coat.clothbin",       "game/data/clothes/drifter_coat_f.clothbin",  {0.25f,0.20f,0.14f}, 0},
    {"Jacket",       "game/data/clothes/jacket.clothbin",             "game/data/clothes/jacket_f.clothbin",        {0.30f,0.22f,0.15f}, 0},
    {"Male Coat",    "game/data/clothes/male_coat.clothbin",          "game/data/clothes/drifter_coat2_f.clothbin", {0.20f,0.16f,0.10f}, 0},
    {"Monk Coat",    "game/data/clothes/monk_coat.clothbin",          "game/data/clothes/monk_coat_f.clothbin",     {0.18f,0.14f,0.09f}, 0},
    {"Samurai Top",  "game/data/clothes/samurai_top.clothbin",        "game/data/clothes/samurai_top_f.clothbin",   {0.15f,0.12f,0.08f}, 0},
    // slot 1 — bottom
    {"None",         nullptr,                                          nullptr,                                       {0,0,0},             1},
    {"Drifter Pants","game/data/clothes/drifter_pants.clothbin",      "game/data/clothes/drifter_pants_f.clothbin", {0.22f,0.18f,0.12f}, 1},
    {"Cargo Pants",  "game/data/clothes/cargopants.clothbin",         "game/data/clothes/cargopants_f.clothbin",    {0.28f,0.22f,0.14f}, 1},
    {"Shorts",       "game/data/clothes/shorts.clothbin",             "game/data/clothes/shorts_f.clothbin",        {0.35f,0.28f,0.18f}, 1},
    {"Half Pants",   "game/data/clothes/trousers.clothbin",           "game/data/clothes/halfpants_f.clothbin",     {0.20f,0.16f,0.10f}, 1},
    {"Monk Pants",   "game/data/clothes/monk_pants.clothbin",         "game/data/clothes/monk_pants_f.clothbin",    {0.18f,0.14f,0.09f}, 1},
    {"Samurai Bot",  "game/data/clothes/samurai_bot.clothbin",        "game/data/clothes/samurai_bot_f.clothbin",   {0.15f,0.12f,0.08f}, 1},
    {"Slave Dress",  "game/data/clothes/slave_dress.clothbin",        "game/data/clothes/slave_dress_f.clothbin",   {0.62f,0.59f,0.53f}, 1},
};
static constexpr int CLOTH_ITEM_COUNT = (int)(sizeof(s_cloth_items)/sizeof(s_cloth_items[0]));
static int  s_cloth_sel[CLOTH_MAX_SLOTS] = {0, 7, 0};
static float s_cloth_color[CLOTH_MAX_SLOTS][3] = {
    {0.62f,0.59f,0.53f}, {0.22f,0.18f,0.12f}, {0.3f,0.3f,0.3f}
};

// ── Math helpers ──────────────────────────────────────────────────────────────
static void m4_persp(float* m, float fovy, float asp, float zn, float zf) {
    float f = 1.f/tanf(fovy*.5f); memset(m,0,64);
    m[0]=f/asp; m[5]=f; m[10]=(zf+zn)/(zn-zf); m[11]=-1.f;
    m[14]=2.f*zf*zn/(zn-zf);
}
static void m4_lookat(float* m, const float e[3], const float at[3]) {
    float f[3]={at[0]-e[0],at[1]-e[1],at[2]-e[2]};
    float fl=sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]); if(fl>0){f[0]/=fl;f[1]/=fl;f[2]/=fl;}
    float r[3]={f[1]*0.f-f[2]*1.f,f[2]*0.f-f[0]*0.f,f[0]*1.f-f[1]*0.f};
    float rl=sqrtf(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]); if(rl>0){r[0]/=rl;r[1]/=rl;r[2]/=rl;}
    float up[3]={r[1]*f[2]-r[2]*f[1],r[2]*f[0]-r[0]*f[2],r[0]*f[1]-r[1]*f[0]};
    m[0]=r[0];m[4]=r[1];m[8] =r[2];m[12]=-(r[0]*e[0]+r[1]*e[1]+r[2]*e[2]);
    m[1]=up[0];m[5]=up[1];m[9]=up[2];m[13]=-(up[0]*e[0]+up[1]*e[1]+up[2]*e[2]);
    m[2]=-f[0];m[6]=-f[1];m[10]=-f[2];m[14]=f[0]*e[0]+f[1]*e[1]+f[2]*e[2];
    m[3]=0;m[7]=0;m[11]=0;m[15]=1;
}
static void m4_mul(float* C, const float* A, const float* B) {
    for(int r=0;r<4;r++) for(int c=0;c<4;c++){
        float s=0; for(int k=0;k<4;k++) s+=A[r+k*4]*B[k+c*4]; C[r+c*4]=s;
    }
}
static void m4inv_rigid(float* out, const float* M) {
    for(int r=0;r<3;r++) for(int c=0;c<3;c++) out[c*4+r]=M[r*4+c];
    for(int c=0;c<4;c++) out[c*4+3]=(c==3)?1.f:0.f;
    float tx=M[12],ty=M[13],tz=M[14];
    out[12]=-(out[0]*tx+out[4]*ty+out[8]*tz);
    out[13]=-(out[1]*tx+out[5]*ty+out[9]*tz);
    out[14]=-(out[2]*tx+out[6]*ty+out[10]*tz); out[15]=1.f;
}
static bool s_feq(float a, float b) { return fabsf(a-b)<1e-5f; }

// ── Morph index by name ───────────────────────────────────────────────────────
static int morph_by_name(const char* n) {
    for(int i=0;i<s_mesh.MorphCount();i++)
        if(strcmp(s_mesh.MorphName(i),n)==0) return i;
    return -1;
}

// ── Upload morphed vertices ───────────────────────────────────────────────────
static void upload_morphed() {
    if(!s_mesh.loaded) return;
    static SkinVertex tmp[20000];
    uint32_t nv=s_mesh.VertCount(); if(nv>20000)nv=20000;
    s_mesh.WriteMorphedVerts(tmp);
    s_skin_vbo.Shutdown();
    s_skin_vbo.Init(0x8892u, tmp, nv*sizeof(SkinVertex));
    s_morphs_dirty=false;
}

// ── LoadHairStyle ─────────────────────────────────────────────────────────────
static bool LoadHairStyle(int idx) {
    if(idx<0||idx>=s_hair_style_count) return false;
    char path[256];
    snprintf(path,sizeof(path),"game/data/hair/%s.glb",s_hair_styles[idx]);
    cgltf_options opt{}; cgltf_data* d=nullptr;
    if(cgltf_parse_file(&opt,path,&d)!=cgltf_result_success){
        fprintf(stderr,"[Hair] missing: %s\n",path); return false;
    }
    cgltf_load_buffers(&opt,d,path);
    auto& pr=d->meshes[0].primitives[0];
    cgltf_accessor* ap=nullptr; cgltf_accessor* an=nullptr;
    for(int i=0;i<(int)pr.attributes_count;++i){
        if(pr.attributes[i].type==cgltf_attribute_type_position) ap=pr.attributes[i].data;
        if(pr.attributes[i].type==cgltf_attribute_type_normal)   an=pr.attributes[i].data;
    }
    if(!ap||!pr.indices){cgltf_free(d);return false;}
    int nv=(int)ap->count;
    struct HV{float px,py,pz,nx,ny,nz;};
    static HV vbuf[8192]; if(nv>(int)(sizeof(vbuf)/sizeof(HV))) nv=(int)(sizeof(vbuf)/sizeof(HV));
    for(int i=0;i<nv;++i){
        float p[3]={0,0,0},n[3]={0,1,0};
        cgltf_accessor_read_float(ap,i,p,3);
        if(an) cgltf_accessor_read_float(an,i,n,3);
        vbuf[i]={p[0],p[1],p[2],n[0],n[1],n[2]};
    }
    int ni=(int)pr.indices->count;
    static uint16_t ibuf[32768]; if(ni>(int)(sizeof(ibuf)/sizeof(uint16_t))) ni=(int)(sizeof(ibuf)/sizeof(uint16_t));
    for(int i=0;i<ni;++i) ibuf[i]=(uint16_t)cgltf_accessor_read_index(pr.indices,i);
    cgltf_free(d);
    s_hair_vbo.Shutdown(); s_hair_ibo.Shutdown();
    s_hair_vbo.Init(0x8892u,vbuf,(uint32_t)(nv*sizeof(HV)));
    s_hair_ibo.Init(0x8893u,ibuf,(uint32_t)(ni*2));
    s_hair_ni=ni; s_hair_style=idx;
    fprintf(stdout,"[Hair] %s (%dv %di)\n",s_hair_styles[idx],nv,ni/3);
    return true;
}

// ── LoadClothingSlot: load .clothbin into a ClothSlot ────────────────────────
//   .clothbin: magic(4) + nv(4) + ni(4) + flags(4) + nv*SkinVertex(52) + ni*u16
static bool LoadClothingSlot(int slot, const char* path) {
    if(slot<0||slot>=CLOTH_MAX_SLOTS) return false;
    s_cloth[slot].vbo.Shutdown(); s_cloth[slot].ibo.Shutdown();
    s_cloth[slot].ni=0; s_cloth[slot].loaded=false;
    if(!path) return true;  // "None" clears the slot

    FILE* f=fopen(path,"rb"); if(!f){fprintf(stderr,"[Cloth] missing: %s\n",path);return false;}
    uint32_t hdr[4]; fread(hdr,4,4,f);
    if(hdr[0]!=0x544F4C43u){fclose(f);fprintf(stderr,"[Cloth] bad magic\n");return false;}
    uint32_t nv=hdr[1], ni=hdr[2], flags=hdr[3];
    bool u32_idx=(flags&1)!=0;

    static char vtmp[20000*52]; // max 20k verts
    if(nv>20000){fclose(f);fprintf(stderr,"[Cloth] too many verts\n");return false;}
    fread(vtmp,52,nv,f);
    s_cloth[slot].vbo.Shutdown();
    s_cloth[slot].vbo.Init(0x8892u,vtmp,(uint32_t)(nv*52));

    uint32_t idx_bytes=ni*(u32_idx?4:2);
    static char itmp[65536*2];
    if(idx_bytes>sizeof(itmp)){fclose(f);fprintf(stderr,"[Cloth] too many indices\n");return false;}
    fread(itmp,1,idx_bytes,f);
    fclose(f);
    s_cloth[slot].ibo.Shutdown();
    s_cloth[slot].ibo.Init(0x8893u,itmp,idx_bytes);
    s_cloth[slot].ni=(int)ni;
    s_cloth[slot].loaded=true;
    fprintf(stdout,"[Cloth] slot%d: %s (%uv %ut)\n",slot,path,nv,ni/3);
    return true;
}

// ── SetClothingItem: select item for slot, update color ───────────────────────
static void SetClothingItem(int item_idx) {
    if(item_idx<0||item_idx>=CLOTH_ITEM_COUNT) return;
    const ClothItemDef& d=s_cloth_items[item_idx];
    int slot=d.slot;
    s_cloth_sel[slot]=item_idx;
    s_cloth_color[slot][0]=d.color[0]; s_cloth_color[slot][1]=d.color[1]; s_cloth_color[slot][2]=d.color[2];
    // Choose female path when female body is loaded; fall back to male if no female variant.
    const char* path = (s_sex==1 && d.path_f) ? d.path_f : d.path_m;
    LoadClothingSlot(slot, path);
}

// ── RTT ───────────────────────────────────────────────────────────────────────
static void ensure_rtt(int w, int h) {
    if(s_rtt_w==w&&s_rtt_h==h) return;
    auto* dev=md::GpuDevice::Get().SDLDevice(); if(!dev)return;
    if(s_color_rtt){SDL_ReleaseGPUTexture(dev,s_color_rtt);s_color_rtt=nullptr;}
    if(s_depth_rtt){SDL_ReleaseGPUTexture(dev,s_depth_rtt);s_depth_rtt=nullptr;}
    SDL_GPUTextureCreateInfo ci{};
    ci.type=SDL_GPU_TEXTURETYPE_2D; ci.width=(uint32_t)w; ci.height=(uint32_t)h;
    ci.layer_count_or_depth=1; ci.num_levels=1;
    ci.usage=SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER;
    ci.format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    s_color_rtt=SDL_CreateGPUTexture(dev,&ci);
    ci.usage=SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET;
    ci.format=SDL_GPU_TEXTUREFORMAT_D32_FLOAT;
    s_depth_rtt=SDL_CreateGPUTexture(dev,&ci);
    s_rtt_w=w; s_rtt_h=h;
}

// ── Init ──────────────────────────────────────────────────────────────────────
static bool Init(const char* glb_path, const char* tex_path) {
    s_ok=false;
    s_skin_vbo.Shutdown();
    // Detect sex from path — drives clothing variant selection.
    s_sex = (glb_path && strstr(glb_path, "female")) ? 1 : 0;

    if(!s_mesh.LoadGLB(glb_path)){
        fprintf(stderr,"[CharPreviewGame] LoadGLB failed: %s\n",glb_path); return false;
    }
    s_idle_clip=s_mesh.ClipIndexByName("idle_stand_normal");
    fprintf(stdout,"[CharPreviewGame] %s verts=%u clips=%d idle=%d morphs=%d\n",
            glb_path,s_mesh.VertCount(),s_mesh.ClipCount(),s_idle_clip,s_mesh.MorphCount());

    s_skin_vbo.Init(0x8892u, s_mesh.CpuVerts(), s_mesh.VertCount()*sizeof(SkinVertex));

    // ── Load fragment textures ───────────────────────────────────────────────
    GpuSamplerDesc lin; // default linear sampler
    auto load_tex=[&](GpuTexture& t, const char* path, uint8_t fr, uint8_t fg, uint8_t fb, uint8_t fa=255){
        stbi_set_flip_vertically_on_load(0);
        int w,h,nc; unsigned char* d=stbi_load(path,&w,&h,&nc,4);
        if(d){ t.InitFromMemory(d,w,h,lin); stbi_image_free(d); }
        else { uint8_t fb4[4]={fr,fg,fb,fa}; t.InitFromMemory(fb4,1,1,lin); }
    };
    load_tex(s_tex_body, tex_path ? tex_path : "", 210,170,130);  // fallback: skin tone

    // Head texture: derive path by replacing "_body" with "_head"
    char head_path[512]="";
    if(tex_path){ const char* p=strstr(tex_path,"_body");
        if(p){ int n=(int)(p-tex_path); snprintf(head_path,sizeof(head_path),"%.*s_head%s",n,tex_path,p+5); } }
    load_tex(s_tex_head, head_path[0]?head_path:"", 210,170,130);

    // Muscle mask: neutral grey; blood: transparent
    { uint8_t p[4]={128,128,128,255}; s_tex_muscle.InitFromMemory(p,1,1,lin); }
    { uint8_t p[4]={0,0,0,0};         s_tex_blood.InitFromMemory(p,1,1,lin); }

    // ── Bone matrix texture: 120×1 RGBA32F ───────────────────────────────────
    SDL_GPUDevice* dev=md::GpuDevice::Get().SDLDevice();
    {
        SDL_GPUTextureCreateInfo ti{};
        ti.type=SDL_GPU_TEXTURETYPE_2D;
        ti.format=SDL_GPU_TEXTUREFORMAT_R32G32B32A32_FLOAT;
        ti.usage=SDL_GPU_TEXTUREUSAGE_SAMPLER;
        ti.width=120; ti.height=1; ti.layer_count_or_depth=1; ti.num_levels=1;
        s_bones_tex=SDL_CreateGPUTexture(dev,&ti);

        SDL_GPUSamplerCreateInfo si{};
        si.min_filter=SDL_GPU_FILTER_NEAREST; si.mag_filter=SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        s_bones_sampler=SDL_CreateGPUSampler(dev,&si);

        // Upload identity matrices for first frame
        static float identity[120*4]; memset(identity,0,sizeof(identity));
        for(int i=0;i<30;i++){identity[i*16+0]=1;identity[i*16+5]=1;identity[i*16+10]=1;identity[i*16+15]=1;}
        uint32_t up_sz=120*4*sizeof(float);
        SDL_GPUTransferBufferCreateInfo tb{}; tb.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tb.size=up_sz;
        SDL_GPUTransferBuffer* tr=SDL_CreateGPUTransferBuffer(dev,&tb);
        if(tr){ void* mp=SDL_MapGPUTransferBuffer(dev,tr,false);
            if(mp){memcpy(mp,identity,up_sz);SDL_UnmapGPUTransferBuffer(dev,tr);}
            SDL_GPUCommandBuffer* uc=SDL_AcquireGPUCommandBuffer(dev);
            if(uc){ SDL_GPUCopyPass* cp=SDL_BeginGPUCopyPass(uc);
                SDL_GPUTextureTransferInfo src={tr,0,120,1};
                SDL_GPUTextureRegion dst={s_bones_tex,0,0,0,0,0,120,1,1};
                SDL_UploadToGPUTexture(cp,&src,&dst,false);
                SDL_EndGPUCopyPass(cp); SDL_SubmitGPUCommandBuffer(uc); }
            SDL_ReleaseGPUTransferBuffer(dev,tr); }
    }

    // ── Character pipeline: char_preview.vert + char_preview.frag ────────────
    // vert_samplers=0 to avoid Intel HD 520 Gen9 ANV hang (vert_samplers+frag_samplers>0).
    // Bone matrices via UBO (set=1 binding=1: BoneMats, 30 mat4 = 1920 bytes).
    {
        GpuPipeline::Desc pd;
        pd.vert_path         = "shaders/char_preview.vert";
        pd.frag_path         = "shaders/char_preview.frag";
        pd.vert_uniform_bufs = 2;      // set=1 binding=0: VU (MVP 64B), binding=1: BoneMats (30 mat4)
        pd.vert_samplers     = 0;      // was 1 — removed to fix Intel Gen9 ANV hang
        pd.frag_samplers     = 4;      // set=2 binding=0..3: body,head,muscle,blood
        pd.frag_uniform_bufs = 1;      // set=3 binding=0: FU (skin/sat/bri/hair)
        pd.has_depth_target  = true;
        pd.raster.depth_compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
        pd.raster.depth_write      = true;
        // Vertex layout: SkinVertex stride=52 (char_preview.vert locations 0-4)
        pd.layout.attribs[0]={0, 0,  GpuAttribFmt::F3   };  // aPos
        pd.layout.attribs[1]={1, 12, GpuAttribFmt::F3   };  // aNorm
        pd.layout.attribs[2]={2, 24, GpuAttribFmt::F2   };  // aUV
        pd.layout.attribs[3]={3, 32, GpuAttribFmt::U8x4 };  // aJoints
        pd.layout.attribs[4]={4, 36, GpuAttribFmt::F4   };  // aWeights
        pd.layout.count=5; pd.layout.stride=52;
        if(!s_pipeline.Create(pd)){
            fprintf(stderr,"[CharPreviewGame] pipeline failed\n"); return false;
        }
    }

    // ── Background pipeline ───────────────────────────────────────────────────
    {
        GpuPipeline::Desc bgpd;
        bgpd.vert_path="shaders/char_bg.vert"; bgpd.frag_path="shaders/char_bg.frag";
        bgpd.layout.count=0; bgpd.layout.stride=0;
        bgpd.raster.depth_test=false; bgpd.raster.depth_write=false; bgpd.raster.cull_back=false;
        bgpd.frag_samplers=2; bgpd.frag_uniform_bufs=1;
        bgpd.has_depth_target=true;
        bgpd.color_format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        s_bg_pipeline.Create(bgpd);
        GpuSamplerDesc rep; rep.wrap_s=rep.wrap_t=GpuSamplerDesc::Wrap::REPEAT;
        auto load_rep=[&](GpuTexture& t, const char* path){
            stbi_set_flip_vertically_on_load(0);
            int w,h,c; unsigned char* d=stbi_load(path,&w,&h,&c,4);
            if(d){t.InitFromMemory(d,w,h,rep);stbi_image_free(d);}
            else{uint8_t fb[4]={160,130,80,255};t.InitFromMemory(fb,1,1,rep);}
        };
        load_rep(s_bg_sand,"game/data/textures/terrain/desert_sand.jpg");
        load_rep(s_bg_dune,"game/data/textures/terrain/desert_dune.jpg");
    }

    // ── Scene pipeline: platform + pole ──────────────────────────────────────
    {
        GpuPipeline::Desc spd;
        spd.vert_path="shaders/char_scene.vert"; spd.frag_path="shaders/char_scene.frag";
        spd.layout.count=2; spd.layout.stride=24;
        spd.layout.attribs[0]={0,0, GpuAttribFmt::F3};
        spd.layout.attribs[1]={1,12,GpuAttribFmt::F3};
        spd.raster.depth_test=true; spd.raster.depth_write=true; spd.raster.cull_back=false;
        spd.vert_uniform_bufs=1; spd.has_depth_target=true;
        spd.color_format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        s_scene_pipeline.Create(spd);

        struct SV{float x,y,z,r,g,b;};
        static SV verts[2048]; static uint32_t idxs[8192]; int vi=0,ii=0;
        auto quad=[&](SV a,SV b,SV c,SV d){
            int base=vi;
            verts[vi++]=a;verts[vi++]=b;verts[vi++]=c;verts[vi++]=d;
            idxs[ii++]=(uint32_t)base;idxs[ii++]=(uint32_t)base+1;idxs[ii++]=(uint32_t)base+2;
            idxs[ii++]=(uint32_t)base;idxs[ii++]=(uint32_t)base+2;idxs[ii++]=(uint32_t)base+3;
        };

        // Platform dimensions
        float bx0=-0.955f,bx1=0.955f,bz0=-0.70f,bz1=0.90f,pt=0.01f,pb=-0.13f;
        float pw=0.26f,pgap=0.015f;

        // 7 planks — top face + front edge
        float px=bx0;
        for(int p=0;p<7;p++){
            float px0=px,px1=px+pw,sh=(p%2==0)?0.f:0.06f;
            float dr=0.32f+sh,dg=0.20f+sh*0.7f,db=0.11f+sh*0.4f;
            float sr=0.38f+sh,sg=0.25f+sh*0.7f,sb=0.14f+sh*0.4f;
            // top
            quad({px0,pt,bz0,sr,sg,sb},{px1,pt,bz0,sr,sg,sb},{px1,pt,bz1,dr,dg,db},{px0,pt,bz1,dr,dg,db});
            // front edge (Z=bz1 face)
            quad({px0,pb,bz1,.18f,.11f,.06f},{px1,pb,bz1,.18f,.11f,.06f},{px1,pt,bz1,.22f,.14f,.08f},{px0,pt,bz1,.22f,.14f,.08f});
            px+=pw+pgap;
        }

        // Platform solid sides (makes it look grounded, not floating)
        // Left face (X=bx0)
        quad({bx0,pb,bz0,.14f,.08f,.04f},{bx0,pb,bz1,.14f,.08f,.04f},{bx0,pt,bz1,.17f,.10f,.06f},{bx0,pt,bz0,.17f,.10f,.06f});
        // Right face (X=bx1)
        quad({bx1,pb,bz1,.14f,.08f,.04f},{bx1,pb,bz0,.14f,.08f,.04f},{bx1,pt,bz0,.17f,.10f,.06f},{bx1,pt,bz1,.17f,.10f,.06f});
        // Back face (Z=bz0)
        quad({bx1,pb,bz0,.13f,.08f,.04f},{bx0,pb,bz0,.13f,.08f,.04f},{bx0,pt,bz0,.16f,.10f,.05f},{bx1,pt,bz0,.16f,.10f,.05f});
        // Bottom (Z=bz0 → bz1, flush with desert sand at pb=-0.13)
        quad({bx0,pb,bz1,.11f,.07f,.04f},{bx1,pb,bz1,.11f,.07f,.04f},{bx1,pb,bz0,.11f,.07f,.04f},{bx0,pb,bz0,.11f,.07f,.04f});

        // Anthropometer pole
        float px0=0.630f,px1=0.658f,pz0=-0.014f,pz1=0.014f,py0=0.f,py1=2.10f;
        float pr=0.30f,pg=0.22f,ppb=0.15f;
        quad({px0,py0,pz1,pr,pg,ppb},{px1,py0,pz1,pr,pg,ppb},{px1,py1,pz1,pr,pg,ppb},{px0,py1,pz1,pr,pg,ppb});
        quad({px1,py0,pz0,.22f,.16f,.10f},{px0,py0,pz0,.22f,.16f,.10f},{px0,py1,pz0,.22f,.16f,.10f},{px1,py1,pz0,.22f,.16f,.10f});
        quad({px1,py0,pz0,.26f,.19f,.12f},{px1,py0,pz1,.26f,.19f,.12f},{px1,py1,pz1,.26f,.19f,.12f},{px1,py1,pz0,.26f,.19f,.12f});
        // Tick marks
        float tr=0.55f,tg=0.45f,tb=0.32f;
        for(int ti=1;ti<=20;ti++){
            float ty=(float)ti*.10f,th=0.007f;
            bool major=(ti%10==0),medium=(ti%5==0);
            float tw=major?.11f:(medium?.07f:.04f);
            float tx0=px1,tx1=px1+tw;
            quad({tx0,ty+th,pz0,tr,tg,tb},{tx1,ty+th,pz0,tr,tg,tb},{tx1,ty+th,pz1,tr,tg,tb},{tx0,ty+th,pz1,tr,tg,tb});
            quad({tx0,ty-th,pz1,tr*.8f,tg*.8f,tb*.8f},{tx1,ty-th,pz1,tr*.8f,tg*.8f,tb*.8f},{tx1,ty+th,pz1,tr,tg,tb},{tx0,ty+th,pz1,tr,tg,tb});
        }
        s_scene_ni=ii;
        if(ii>0){
            s_scene_vbo.Init(0x8892u,verts,(uint32_t)(vi*sizeof(SV)));
            s_scene_ibo.Init(0x8893u,idxs,(uint32_t)(ii*sizeof(uint32_t)));
        }
    }

    for(int i=0;i<CHARCC_BODY_N;i++) s_body_cache[i]=100.f;
    for(int i=0;i<CHARCC_FACE_N;i++) s_face_cache[i]=100.f;
    s_morphs_dirty=true;

    // ── Hair pipeline: char_hair.vert + char_hair.frag ───────────────────────
    {
        GpuPipeline::Desc hpd;
        hpd.vert_path="shaders/char_hair.vert"; hpd.frag_path="shaders/char_hair.frag";
        hpd.layout.count=2; hpd.layout.stride=24;
        hpd.layout.attribs[0]={0, 0,GpuAttribFmt::F3};   // aPos
        hpd.layout.attribs[1]={1,12,GpuAttribFmt::F3};   // aNorm
        hpd.vert_samplers    =1;   // set=0: uBoneMats
        hpd.vert_uniform_bufs=1;   // set=1: VU
        hpd.frag_samplers    =0;
        hpd.frag_uniform_bufs=2;   // set=3 binding=0: HairFU, binding=1: HairShadingFU
        hpd.raster.depth_test=true; hpd.raster.depth_write=true; hpd.raster.cull_back=false;
        hpd.has_depth_target=true;
        hpd.color_format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        if(!s_hair_pipeline.Create(hpd))
            fprintf(stderr,"[Hair] pipeline create failed\n");
    }
    LoadHairStyle(0);

    // ── Clothes pipeline: char_preview.vert + char_clothes.frag ──────────────
    {
        GpuPipeline::Desc cpd;
        cpd.vert_path="shaders/char_preview.vert"; cpd.frag_path="shaders/char_clothes.frag";
        cpd.layout.count=5; cpd.layout.stride=52;   // SkinVertex layout
        cpd.layout.attribs[0]={0, 0,  GpuAttribFmt::F3   };  // aPos
        cpd.layout.attribs[1]={1, 12, GpuAttribFmt::F3   };  // aNorm
        cpd.layout.attribs[2]={2, 24, GpuAttribFmt::F2   };  // aUV (unused in frag)
        cpd.layout.attribs[3]={3, 32, GpuAttribFmt::U8x4 };  // aJoints
        cpd.layout.attribs[4]={4, 36, GpuAttribFmt::F4   };  // aWeights
        cpd.vert_samplers    =0;   // was 1 — removed, char_preview.vert now uses UBO for bones
        cpd.vert_uniform_bufs=2;   // set=1 binding=0: VU (mvp), binding=1: BoneMats (30 mat4)
        cpd.frag_samplers    =0;
        cpd.frag_uniform_bufs=1;   // set=3: ClothFU
        cpd.raster.depth_test=true; cpd.raster.depth_write=true; cpd.raster.cull_back=true;
        cpd.has_depth_target=true;
        cpd.color_format=SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        if(!s_cloth_pipeline.Create(cpd))
            fprintf(stderr,"[Cloth] pipeline create failed\n");
    }
    // Load default clothes (slave shirt + cargo pants)
    SetClothingItem(1);   // slot0: Slave Shirt
    SetClothingItem(9);   // slot1: Cargo Pants

    s_ok=true;
    return true;
}

// ── Upload bone matrices to 120×1 RGBA32F texture (copy pass) ────────────────
static void upload_bones(SDL_GPUCommandBuffer* cmd, float t) {
    if(!s_bones_tex||s_idle_clip<0) return;
    static float bones[MAX_SKIN_BONES*16];
    CharScales scales;
    CharCustomization_ComputeScales(s_body_cache, s_face_cache, s_mesh.bone_count, scales);
    s_mesh.GetFinalBonesScaled(s_idle_clip, t, scales, bones);
    memcpy(s_bones_cpu, bones, 30*16*sizeof(float));

    SDL_GPUDevice* dev=md::GpuDevice::Get().SDLDevice();
    uint32_t up_sz=120*4*sizeof(float);
    SDL_GPUTransferBufferCreateInfo tb{}; tb.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tb.size=up_sz;
    SDL_GPUTransferBuffer* tr=SDL_CreateGPUTransferBuffer(dev,&tb);
    if(!tr) return;
    void* mp=SDL_MapGPUTransferBuffer(dev,tr,false);
    if(mp){ memcpy(mp,bones,up_sz); SDL_UnmapGPUTransferBuffer(dev,tr); }
    SDL_GPUCopyPass* cp=SDL_BeginGPUCopyPass(cmd);
    SDL_GPUTextureTransferInfo src={tr,0,120,1};
    SDL_GPUTextureRegion dst={s_bones_tex,0,0,0,0,0,120,1,1};
    SDL_UploadToGPUTexture(cp,&src,&dst,false);
    SDL_EndGPUCopyPass(cp);
    SDL_ReleaseGPUTransferBuffer(dev,tr);
}

// ── RenderFrame ───────────────────────────────────────────────────────────────
static void RenderFrame(SDL_GPUCommandBuffer* cmd) {
    if(!s_ok||s_rtt_w<4||s_rtt_h<4||!s_color_rtt||!s_depth_rtt) return;
    if(!s_pipeline.SDLPipeline()||!s_skin_vbo.SDLBuffer()) return;
    if(!s_mesh.ibo.SDLBuffer()) return;

    if(s_morphs_dirty) upload_morphed();

    // Frame 0 only: last frame of idle_stand_normal breaks head tilt + hand clipping.
    float anim_t = 0.f;

    // Upload bone matrices to texture via copy pass BEFORE render pass
    upload_bones(cmd, anim_t);

    // Camera
    float camX=s_dist*sinf(s_yaw)*cosf(s_pit);
    float camY=s_lookat_y*s_height+s_dist*sinf(s_pit);
    float camZ=s_dist*cosf(s_yaw)*cosf(s_pit);
    float eye[3]={camX,camY,camZ}, at[3]={0.f,s_lookat_y*s_height,0.f};
    float proj[16],view[16],vp[16];
    m4_persp(proj,0.72f,(float)s_rtt_w/(float)s_rtt_h,0.05f,50.f);
    m4_lookat(view,eye,at); m4_mul(vp,proj,view);

    // char_preview.frag FU: skin tint, sat/bri, hair
    struct FU { float skin[3],str; float sat,bri,muscle,pad; float hair[3],hairpad; } fu{};
    fu.skin[0]=s_skin_rgb[0]; fu.skin[1]=s_skin_rgb[1]; fu.skin[2]=s_skin_rgb[2]; fu.str=s_skin_str;
    fu.sat=s_sat; fu.bri=s_bri; fu.muscle=s_muscle;
    fu.hair[0]=s_hair[0]; fu.hair[1]=s_hair[1]; fu.hair[2]=s_hair[2];

    // Render pass
    SDL_GPUColorTargetInfo ci{};
    ci.texture=s_color_rtt; ci.load_op=SDL_GPU_LOADOP_CLEAR; ci.store_op=SDL_GPU_STOREOP_STORE;
    ci.clear_color={0.48f,0.52f,0.6f,1.f};
    SDL_GPUDepthStencilTargetInfo di{};
    di.texture=s_depth_rtt; di.load_op=SDL_GPU_LOADOP_CLEAR; di.store_op=SDL_GPU_STOREOP_DONT_CARE;
    di.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE; di.clear_depth=1.f;
    SDL_GPURenderPass* rp=SDL_BeginGPURenderPass(cmd,&ci,1,&di);
    if(!rp) return;

    // ── Background ───────────────────────────────────────────────────────────
    if(s_bg_pipeline.SDLPipeline()){
        SDL_BindGPUGraphicsPipeline(rp,s_bg_pipeline.SDLPipeline());
        float inv_view[16]; m4inv_rigid(inv_view,view);
        float asp=(float)s_rtt_w/(float)s_rtt_h;
        float tan_vfov=tanf(0.36f), tan_hfov=tan_vfov*asp;
        struct{float right[4],up[4],fwd[4],eye4[4];} bgu;
        bgu.right[0]=inv_view[0];bgu.right[1]=inv_view[1];bgu.right[2]=inv_view[2];bgu.right[3]=tan_hfov;
        bgu.up[0]=inv_view[4];   bgu.up[1]=inv_view[5];   bgu.up[2]=inv_view[6];   bgu.up[3]=tan_vfov;
        bgu.fwd[0]=-inv_view[8]; bgu.fwd[1]=-inv_view[9]; bgu.fwd[2]=-inv_view[10]; bgu.fwd[3]=-0.13f;
        bgu.eye4[0]=inv_view[12];bgu.eye4[1]=inv_view[13];bgu.eye4[2]=inv_view[14];bgu.eye4[3]=0;
        SDL_PushGPUFragmentUniformData(cmd,0,&bgu,sizeof(bgu));
        if(s_bg_sand.SDLTexture()&&s_bg_dune.SDLTexture()){
            SDL_GPUTextureSamplerBinding bg_tex[2]={
                {s_bg_sand.SDLTexture(),s_bg_sand.SDLSampler()},
                {s_bg_dune.SDLTexture(),s_bg_dune.SDLSampler()}};
            SDL_BindGPUFragmentSamplers(rp,0,bg_tex,2);
        }
        SDL_DrawGPUPrimitives(rp,3,1,0,0);
    }

    // ── Platform + pole ──────────────────────────────────────────────────────
    if(s_scene_pipeline.SDLPipeline()&&s_scene_vbo.SDLBuffer()&&s_scene_ni>0){
        SDL_BindGPUGraphicsPipeline(rp,s_scene_pipeline.SDLPipeline());
        SDL_GPUBufferBinding svb={s_scene_vbo.SDLBuffer(),0};
        SDL_BindGPUVertexBuffers(rp,0,&svb,1);
        SDL_GPUBufferBinding sib={s_scene_ibo.SDLBuffer(),0};
        SDL_BindGPUIndexBuffer(rp,&sib,SDL_GPU_INDEXELEMENTSIZE_32BIT);
        SDL_PushGPUVertexUniformData(cmd,0,vp,64);
        SDL_DrawGPUIndexedPrimitives(rp,s_scene_ni,1,0,0,0);
    }

    // Foot grounding: raise character so feet land on platform top (y=pt=0.01).
    // foot_adj = (s_leg_y - 0.95) * 0.9: longer legs drift down without this.
    // Apply to char/hair/clothes VP only — platform stays at vp (world y=0).
    float char_vp[16];
    {
        float ty = (s_leg_y - 0.95f) * 0.9f;
        memcpy(char_vp, vp, 64);
        // vp * translate(0, ty, 0): adds ty*col1(vp) to col3(vp)
        char_vp[12] += vp[4] * ty;
        char_vp[13] += vp[5] * ty;
        char_vp[14] += vp[6] * ty;
        char_vp[15] += vp[7] * ty;
    }

    // ── Character mesh ────────────────────────────────────────────────────────
    if(s_pipeline.SDLPipeline()&&s_skin_vbo.SDLBuffer()){
        SDL_BindGPUGraphicsPipeline(rp,s_pipeline.SDLPipeline());

        SDL_GPUBufferBinding vb={s_skin_vbo.SDLBuffer(),0};
        SDL_BindGPUVertexBuffers(rp,0,&vb,1);

        SDL_GPUBufferBinding ib={s_mesh.ibo.SDLBuffer(),0};
        SDL_BindGPUIndexBuffer(rp,&ib,s_mesh.indices_u16?SDL_GPU_INDEXELEMENTSIZE_16BIT:SDL_GPU_INDEXELEMENTSIZE_32BIT);

        // Vertex uniforms: MVP (set=1 binding=0), BoneMats 30 mat4 (set=1 binding=1)
        SDL_PushGPUVertexUniformData(cmd,0,char_vp,64);
        SDL_PushGPUVertexUniformData(cmd,1,s_bones_cpu,30*16*sizeof(float));

        // Fragment samplers: body, head, muscle, blood (set=2 binding=0..3)
        SDL_GPUTextureSamplerBinding ftb[4]={
            {s_tex_body.SDLTexture(),   s_tex_body.SDLSampler()  },
            {s_tex_head.SDLTexture(),   s_tex_head.SDLSampler()  },
            {s_tex_muscle.SDLTexture(), s_tex_muscle.SDLSampler()},
            {s_tex_blood.SDLTexture(),  s_tex_blood.SDLSampler() },
        };
        SDL_BindGPUFragmentSamplers(rp,0,ftb,4);

        // Fragment uniform: skin/sat/bri/hair (set=3 binding=0)
        SDL_PushGPUFragmentUniformData(cmd,0,&fu,sizeof(fu));

        SDL_DrawGPUIndexedPrimitives(rp,s_mesh.index_count,1,0,0,0);
    }

    // ── Clothing mesh (all slots) ─────────────────────────────────────────────
    if(s_clothes_visible && s_cloth_pipeline.SDLPipeline()) {
        for(int sl=0; sl<CLOTH_MAX_SLOTS; ++sl) {
            if(!s_cloth[sl].loaded || s_cloth[sl].ni==0) continue;
            SDL_BindGPUGraphicsPipeline(rp, s_cloth_pipeline.SDLPipeline());
            SDL_PushGPUVertexUniformData(cmd, 0, char_vp, 64);
            SDL_PushGPUVertexUniformData(cmd, 1, s_bones_cpu, 30*16*sizeof(float));
            ClothFU cfu;
            cfu.color[0]=s_cloth_color[sl][0]; cfu.color[1]=s_cloth_color[sl][1];
            cfu.color[2]=s_cloth_color[sl][2]; cfu.pad=0;
            SDL_PushGPUFragmentUniformData(cmd, 0, &cfu, sizeof(cfu));
            SDL_GPUBufferBinding cvb{s_cloth[sl].vbo.SDLBuffer(), 0};
            SDL_BindGPUVertexBuffers(rp, 0, &cvb, 1);
            SDL_GPUBufferBinding cib{s_cloth[sl].ibo.SDLBuffer(), 0};
            SDL_BindGPUIndexBuffer(rp, &cib, SDL_GPU_INDEXELEMENTSIZE_16BIT);
            SDL_DrawGPUIndexedPrimitives(rp, (uint32_t)s_cloth[sl].ni, 1, 0, 0, 0);
        }
    }

    // ── Hair mesh ─────────────────────────────────────────────────────────────
    if(s_hair_pipeline.SDLPipeline()&&s_hair_vbo.SDLBuffer()&&s_hair_ni>0){
        SDL_BindGPUGraphicsPipeline(rp,s_hair_pipeline.SDLPipeline());
        SDL_GPUTextureSamplerBinding hbsb{s_bones_tex,s_bones_sampler};
        SDL_BindGPUVertexSamplers(rp,0,&hbsb,1);
        SDL_PushGPUVertexUniformData(cmd,0,char_vp,64);
        HairFU hfu; hfu.hair[0]=s_hair[0]; hfu.hair[1]=s_hair[1]; hfu.hair[2]=s_hair[2]; hfu.pad=0;
        SDL_PushGPUFragmentUniformData(cmd,0,&hfu,sizeof(hfu));
        SDL_PushGPUFragmentUniformData(cmd,1,&HairShading::g_params,sizeof(HairShadingFU));
        SDL_GPUBufferBinding hvb{s_hair_vbo.SDLBuffer(),0};
        SDL_BindGPUVertexBuffers(rp,0,&hvb,1);
        SDL_GPUBufferBinding hib{s_hair_ibo.SDLBuffer(),0};
        SDL_BindGPUIndexBuffer(rp,&hib,SDL_GPU_INDEXELEMENTSIZE_16BIT);
        SDL_DrawGPUIndexedPrimitives(rp,(uint32_t)s_hair_ni,1,0,0,0);
    }

    SDL_EndGPURenderPass(rp);
}

// ── Public API ────────────────────────────────────────────────────────────────
static void SetBoneScalesFromDef(const float body[18], const float face[24]) {
    memcpy(s_body_cache, body, CHARCC_BODY_N * sizeof(float));
    memcpy(s_face_cache, face, CHARCC_FACE_N * sizeof(float));
    // leg_Y = clamp(H + LL - 1): body[2]=height, body[7]=leg_length (same formula as char_customization.cpp)
    float ly = body[2] / 100.f + body[7] / 100.f - 1.f;
    s_leg_y = ly < 0.f ? 0.f : (ly > 1.f ? 1.f : ly);
}
static void SetBodyMorphWeights(const float body[18], const float face[24]) {
    auto pd=[](float v,float n,float r)->float{float d=(v-n)/r;return d<0?0:(d>1?1:d);};
    struct{const char*n;float w;} bms[]={
        {"fat",     (pd(body[13],100,90)*.65f+pd(body[15],100,45)*.20f+pd(body[12],100,40)*.15f)*.3f},
        {"muscular",(pd(body[9], 100,45)*.65f+pd(body[8], 100,10)*.15f+pd(body[12],100,40)*.20f)*.3f},
        {"longlegs", pd(body[7],100,15)*.3f},
        {"broadshdr",(pd(body[3],100,20)*.55f+pd(body[8],100,10)*.45f)*.3f},
        {"tall",     pd(body[2],100,20)*.15f},
    };
    (void)face;
    bool changed=false;
    for(auto& bm:bms){int mi=morph_by_name(bm.n);
        if(mi>=0&&!s_feq(s_mesh.morph_weights[mi],bm.w)){s_mesh.morph_weights[mi]=bm.w;changed=true;}}
    if(changed) s_morphs_dirty=true;
}
static void SetMorphWeightsFromFace(const float face[], const float def[],
                                     const float lo[], const float hi[]) {
    CharCustomization_ApplyMorphs(face, def, lo, hi, s_mesh);
    s_morphs_dirty = true;
}
static void DrawInImGui(float W, float H,
                        float height_scale, float /*bulk*/,
                        const float skin_rgb[3], float skin_str,
                        float sat, float bri,
                        float muscle=0.f, const float* hair=nullptr)
{
    if(!s_ok){ImGui::Dummy({W,H});return;}
    int iw=(int)W,ih=(int)H; if(iw<4||ih<4)return;
    s_skin_rgb[0]=skin_rgb[0]; s_skin_rgb[1]=skin_rgb[1]; s_skin_rgb[2]=skin_rgb[2];
    s_skin_str=skin_str; s_sat=sat; s_bri=bri;
    s_height=height_scale; s_muscle=muscle;
    if(hair){s_hair[0]=hair[0];s_hair[1]=hair[1];s_hair[2]=hair[2];}
    ensure_rtt(iw,ih);
    // Render character preview into RTT using a dedicated command buffer.
    // Safe to acquire/submit here: SDL_GPU allows multiple in-flight CMDs;
    // the preview texture is ready before ImGui renders to swapchain.
    {
        SDL_GPUDevice* dev=md::GpuDevice::Get().SDLDevice();
        if(dev){
            SDL_GPUCommandBuffer* cmd=SDL_AcquireGPUCommandBuffer(dev);
            if(cmd){ RenderFrame(cmd); SDL_SubmitGPUCommandBuffer(cmd); }
        }
    }
    ImVec2 org=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##cpgv",{W,H},ImGuiButtonFlags_MouseButtonRight);
    bool hov=ImGui::IsItemHovered();
    ImGuiIO& io=ImGui::GetIO();
    if(hov&&ImGui::IsMouseDragging(ImGuiMouseButton_Right)){
        s_yaw-=io.MouseDelta.x*.01f;
        s_pit+=io.MouseDelta.y*.008f; s_pit=fmaxf(-1.4f,fminf(1.4f,s_pit));
    }
    if(hov&&io.MouseWheel!=0) s_dist=fmaxf(.5f,fminf(6.f,s_dist-io.MouseWheel*.2f));
    if(s_color_rtt)
        ImGui::GetWindowDrawList()->AddImage(
            (ImTextureID)(intptr_t)s_color_rtt, org, {org.x+W,org.y+H});
}
static void SetCameraForTab(int tab) {
    if(tab==0){s_dist=3.5f; s_pit=-0.06f; s_lookat_y=0.9f;}
    else      {s_dist=0.85f; s_pit=0.f; s_yaw=0.f; s_lookat_y=1.76f;}
}
static void ResetAnimPhase() {}
static void ReloadPipelines() {}  // no-op: game path recreates pipelines via Init()
static void DumpState(FILE* f=stdout) { fprintf(f,"[CharPreviewGame] ok=%d idle=%d verts=%u morphs=%d\n",
                                       (int)s_ok,s_idle_clip,s_mesh.VertCount(),s_mesh.MorphCount()); }

} // namespace CharPreviewGame
#endif // MD_SDL_GPU
