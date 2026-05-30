#pragma once
#ifdef MD_SDL_GPU
// editor_char_preview_game.h — Character preview using the EXACT game rendering path.
// animated.vert + npc_instanced.frag, SkinVertex stride=52, per-vertex RGBA8 body
// colors (slot=1 VBO), 4 vertex SSBOs (Transform/Visible/Faction/Bones).
// Drop-in for CharPreviewSDLGPU — same public API used by character_editor.h.

#include "imgui.h"
#include <monkey_dust/render/gpu_device.h>
#include <monkey_dust/render/gpu_hal.h>
#include <monkey_dust/render/skin_mesh.h>
#include <monkey_dust/render/ssbo.h>
#include <monkey_dust/platform/window.h>
#include <SDL3/SDL.h>
#include <SDL3/SDL_gpu.h>
#include "stb_image.h"
#include <cstring>
#include <cstdio>
#include <cmath>

namespace CharPreviewGame {

// ── State ─────────────────────────────────────────────────────────────────────
static SkinMesh        s_mesh;
static GpuPipeline     s_pipeline;
static GpuStaticBuffer s_skin_vbo;        // SkinVertex stride=52 (re-uploaded for morphs)
static GpuStaticBuffer s_body_color_vbo;  // RGBA8 per vertex (re-baked for skin changes)
// SSBOs: set=0 b=0..3 in animated.vert
static SSBO s_transform_ssbo;  // vec4 xzyr × 1 entry
static SSBO s_visible_ssbo;    // uint32 × 1 entry = 0
static SSBO s_faction_ssbo;    // uint32 × 1 entry = 0
static SSBO s_bones_ssbo;      // mat4 × MAX_SKIN_BONES
// RTT
static SDL_GPUTexture* s_color_rtt = nullptr;
static SDL_GPUTexture* s_depth_rtt = nullptr;
static int             s_rtt_w = 0, s_rtt_h = 0;
// Lifecycle
static bool s_ok           = false;
static int  s_idle_clip    = -1;
static bool s_morphs_dirty = true;
static bool s_colors_dirty = true;
// Camera
static float s_yaw = 0.f, s_pit = -0.06f, s_dist = 2.6f, s_lookat_y = 0.9f;
static float s_height = 1.f;
// Skin
static float s_skin_rgb[3] = {0.9f, 0.7f, 0.55f};
static float s_skin_str = 1.f, s_sat = 1.f, s_bri = 0.f;
// Body texture (CPU pixels for re-baking)
static unsigned char* s_body_px = nullptr;
static int            s_body_w = 0, s_body_h = 0;

// ── Math helpers ──────────────────────────────────────────────────────────────
static void m4_persp(float* m, float fovy, float asp, float zn, float zf) {
    float f = 1.f/tanf(fovy*.5f); memset(m,0,64);
    m[0]=f/asp; m[5]=f; m[10]=(zf+zn)/(zn-zf); m[11]=-1.f;
    m[14]=2.f*zf*zn/(zn-zf);
}
static void m4_lookat(float* m, const float e[3], const float at[3]) {
    float f[3]={at[0]-e[0],at[1]-e[1],at[2]-e[2]};
    float fl=sqrtf(f[0]*f[0]+f[1]*f[1]+f[2]*f[2]); if(fl>0){f[0]/=fl;f[1]/=fl;f[2]/=fl;}
    float u[3]={0,1,0};
    float r[3]={f[1]*u[2]-f[2]*u[1],f[2]*u[0]-f[0]*u[2],f[0]*u[1]-f[1]*u[0]};
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
static bool s_feq(float a, float b) { return fabsf(a-b)<1e-5f; }

// ── Morph index by name ───────────────────────────────────────────────────────
static int morph_by_name(const char* n) {
    for(int i=0;i<s_mesh.MorphCount();i++)
        if(strcmp(s_mesh.MorphName(i),n)==0) return i;
    return -1;
}

// ── Colorize: same formula as npc_instanced.frag ─────────────────────────────
static void s_colorize(float& r, float& g, float& b, float sat, float bri) {
    float lum = r*.299f+g*.587f+b*.114f;
    r = lum+(r-lum)*sat; g = lum+(g-lum)*sat; b = lum+(b-lum)*sat;
    r=fmaxf(0.f,fminf(1.f,r+bri));
    g=fmaxf(0.f,fminf(1.f,g+bri));
    b=fmaxf(0.f,fminf(1.f,b+bri));
}

// ── Pre-bake body colors ──────────────────────────────────────────────────────
static void bake_body_colors() {
    if (!s_body_px || !s_mesh.loaded) return;
    uint32_t nv = s_mesh.VertCount();
    const SkinVertex* verts = s_mesh.CpuVerts();
    static uint32_t baked[24000]; if(nv>24000) nv=24000;
    for(uint32_t i=0;i<nv;++i) {
        float u=verts[i].u-floorf(verts[i].u), v=verts[i].v-floorf(verts[i].v);
        int tx=(int)(u*(float)s_body_w); if(tx>=s_body_w)tx=s_body_w-1;
        int ty=(int)(v*(float)s_body_h); if(ty>=s_body_h)ty=s_body_h-1;
        const unsigned char* p=s_body_px+(ty*s_body_w+tx)*4;
        float r=p[0]/255.f,g=p[1]/255.f,b=p[2]/255.f;
        r=r+(s_skin_rgb[0]-r)*s_skin_str; g=g+(s_skin_rgb[1]-g)*s_skin_str; b=b+(s_skin_rgb[2]-b)*s_skin_str;
        s_colorize(r,g,b,s_sat,s_bri);
        baked[i]=((uint32_t)(r*255.f)&0xFF)|(((uint32_t)(g*255.f)&0xFF)<<8)|(((uint32_t)(b*255.f)&0xFF)<<16)|0xFF000000u;
    }
    s_body_color_vbo.Shutdown();
    s_body_color_vbo.Init(0x8892u, baked, nv*sizeof(uint32_t));
    s_colors_dirty=false;
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
    ci.format=SDL_GPU_TEXTUREFORMAT_D32_FLOAT;  // D32_FLOAT avoids Intel Gen9 GPU hang
    s_depth_rtt=SDL_CreateGPUTexture(dev,&ci);
    s_rtt_w=w; s_rtt_h=h;
}

// ── Init ──────────────────────────────────────────────────────────────────────
static bool Init(const char* glb_path, const char* tex_path) {
    s_ok=false;
    s_skin_vbo.Shutdown(); s_body_color_vbo.Shutdown();
    if(s_body_px){stbi_image_free(s_body_px);s_body_px=nullptr;}

    if(!s_mesh.LoadGLB(glb_path)){
        fprintf(stderr,"[CharPreviewGame] LoadGLB failed: %s\n",glb_path); return false;
    }
    s_idle_clip=s_mesh.ClipIndexByName("idle_stand_normal");
    fprintf(stdout,"[CharPreviewGame] %s verts=%u clips=%d idle=%d morphs=%d\n",
            glb_path,s_mesh.VertCount(),s_mesh.ClipCount(),s_idle_clip,s_mesh.MorphCount());

    // Initial vertex upload (no morphs yet)
    s_skin_vbo.Init(0x8892u, s_mesh.CpuVerts(), s_mesh.VertCount()*sizeof(SkinVertex));

    // Load + bake body texture colors
    if(tex_path){
        int nc; s_body_px=stbi_load(tex_path,&s_body_w,&s_body_h,&nc,4);
        if(!s_body_px) fprintf(stderr,"[CharPreviewGame] tex missing: %s\n",tex_path);
    }
    if(!s_body_px){
        // Fallback: 1×1 default skin tone
        s_body_w=s_body_h=1;
        s_body_px=(unsigned char*)malloc(4);
        s_body_px[0]=230;s_body_px[1]=185;s_body_px[2]=145;s_body_px[3]=255;
    }
    bake_body_colors();

    // SSBOs — 1 character instance at origin
    float xzyr[4]={0,0,0,0};
    s_transform_ssbo.Init(16);    s_transform_ssbo.Upload(xzyr,16);
    uint32_t vi=0;
    s_visible_ssbo.Init(4);       s_visible_ssbo.Upload(&vi,4);
    uint32_t fi=0;
    s_faction_ssbo.Init(4);       s_faction_ssbo.Upload(&fi,4);
    s_bones_ssbo.Init(MAX_SKIN_BONES*16*(int)sizeof(float));

    // Pipeline — IDENTICAL to NpcRender::Init()
    GpuPipeline::Desc pd;
    pd.vert_path         = "shaders/animated.vert";
    pd.frag_path         = "shaders/npc_instanced.frag";
    pd.vert_uniform_bufs = 1;
    pd.vert_storage_bufs = 4;   // Transform Visible Faction Bones
    pd.frag_uniform_bufs = 1;
    pd.frag_samplers     = 0;   // body color pre-baked into vertex attr
    pd.has_depth_target  = true;
    pd.raster.depth_compare_op = SDL_GPU_COMPAREOP_LESS_OR_EQUAL;
    pd.raster.depth_write      = true;
    // Vertex layout: slot=0 SkinVertex + slot=1 RGBA8 body color (per-vertex)
    pd.layout.attribs[0]={0, 0,  GpuAttribFmt::F3   };  // aPos
    pd.layout.attribs[1]={2, 12, GpuAttribFmt::F3   };  // aNormal
    pd.layout.attribs[2]={5, 24, GpuAttribFmt::F2   };  // aTexCoord
    pd.layout.attribs[3]={8, 32, GpuAttribFmt::U8x4 };  // aJoints
    pd.layout.attribs[4]={9, 36, GpuAttribFmt::F4   };  // aWeights
    pd.layout.count=5; pd.layout.stride=52;
    pd.layout.inst_attribs[0]={10,0,GpuAttribFmt::U8x4_NORM}; // aBodyColor
    pd.layout.inst_count=1; pd.layout.inst_stride=4; pd.layout.inst_per_vertex=true;

    if(!s_pipeline.Create(pd)){
        fprintf(stderr,"[CharPreviewGame] pipeline failed\n"); return false;
    }
    s_morphs_dirty=true; s_ok=true;
    return true;
}

// ── RenderFrame ───────────────────────────────────────────────────────────────
static void RenderFrame(SDL_GPUCommandBuffer* cmd) {
    if(!s_ok||s_rtt_w<4||s_rtt_h<4||!s_color_rtt||!s_depth_rtt) return;
    if(!s_pipeline.SDLPipeline()||!s_skin_vbo.SDLBuffer()||!s_body_color_vbo.SDLBuffer()) return;
    if(!s_mesh.ibo.SDLBuffer()) return;

    if(s_morphs_dirty) upload_morphed();
    if(s_colors_dirty) bake_body_colors();

    // Bones: GetFinalBonesFull + ApplyInvBind — exact game path.
    // Upload via standalone Upload() (not UploadInCmd) to avoid copy/render pass ordering issues.
    if(s_idle_clip>=0){
        static float bones[MAX_SKIN_BONES*16];
        // GetFinalBonesFull already applies inv_bind internally — result is GPU-ready.
        // Do NOT call ApplyInvBind after — that would double-apply and cause explosion.
        s_mesh.GetFinalBonesFull(s_idle_clip,0.f,bones,nullptr);
        s_bones_ssbo.Upload(bones, MAX_SKIN_BONES*16*(int)sizeof(float));
    }
    (void)cmd;

    // Camera
    float camX=s_dist*sinf(s_yaw)*cosf(s_pit);
    float camY=s_lookat_y*s_height+s_dist*sinf(s_pit);
    float camZ=s_dist*cosf(s_yaw)*cosf(s_pit);
    float eye[3]={camX,camY,camZ}, at[3]={0.f,s_lookat_y*s_height,0.f};
    float proj[16],view[16],vp[16];
    m4_persp(proj,0.72f,(float)s_rtt_w/(float)s_rtt_h,0.05f,50.f);
    m4_lookat(view,eye,at); m4_mul(vp,proj,view);

    // VertUBO: viewProj + factionColors
    struct { float vp[16]; float fc[16]; } v_ubo;
    memcpy(v_ubo.vp,vp,64);
    static const float kFC[16]={1,1,1,1, .86f,.2f,.2f,1, .2f,.59f,.86f,1, .86f,.78f,.2f,1};
    memcpy(v_ubo.fc,kFC,64);

    // FragUBO: LightUniforms (same struct layout as npc_instanced.frag)
    struct LightUBO {
        float sunDir[4],sunColor[4],ambientColor[4],cameraPos[4];
        float fogColor[4],fogParams[4],lightViewProj[48],cascadeSplits[4];
        float probe[24]; float snow_level,_sp[3],sds,shs,sho,_sp2;
    } f_ubo{};
    float sd[3]={.5f,.8f,.3f},sl=sqrtf(sd[0]*sd[0]+sd[1]*sd[1]+sd[2]*sd[2]);
    f_ubo.sunDir[0]=sd[0]/sl;f_ubo.sunDir[1]=sd[1]/sl;f_ubo.sunDir[2]=sd[2]/sl;
    f_ubo.sunColor[0]=1.f;f_ubo.sunColor[1]=.95f;f_ubo.sunColor[2]=.8f;f_ubo.sunColor[3]=1;
    f_ubo.ambientColor[0]=.3f;f_ubo.ambientColor[1]=.32f;f_ubo.ambientColor[2]=.38f;
    f_ubo.cameraPos[0]=eye[0];f_ubo.cameraPos[1]=eye[1];f_ubo.cameraPos[2]=eye[2];
    f_ubo.fogColor[0]=.7f;f_ubo.fogColor[1]=.75f;f_ubo.fogColor[2]=.85f;
    f_ubo.fogParams[0]=99.f;f_ubo.fogParams[1]=100.f;  // no visible fog
    for(int i=0;i<6;i++){f_ubo.probe[i*4]=.3f;f_ubo.probe[i*4+1]=.32f;f_ubo.probe[i*4+2]=.38f;}
    f_ubo.sds=1;f_ubo.shs=1;

    // Render pass
    SDL_GPUColorTargetInfo ci{};
    ci.texture=s_color_rtt; ci.load_op=SDL_GPU_LOADOP_CLEAR; ci.store_op=SDL_GPU_STOREOP_STORE;
    ci.clear_color={0.48f,0.52f,0.6f,1.f};
    SDL_GPUDepthStencilTargetInfo di{};
    di.texture=s_depth_rtt; di.load_op=SDL_GPU_LOADOP_CLEAR; di.store_op=SDL_GPU_STOREOP_DONT_CARE;
    di.stencil_load_op=SDL_GPU_LOADOP_DONT_CARE; di.clear_depth=1.f;
    SDL_GPURenderPass* rp=SDL_BeginGPURenderPass(cmd,&ci,1,&di);
    if(!rp) return;

    SDL_BindGPUGraphicsPipeline(rp, s_pipeline.SDLPipeline());

    // Slot=0: SkinVertex, Slot=1: body colors
    SDL_GPUBufferBinding vb[2]={{s_skin_vbo.SDLBuffer(),0},{s_body_color_vbo.SDLBuffer(),0}};
    SDL_BindGPUVertexBuffers(rp,0,vb,2);

    // Index buffer
    SDL_GPUBufferBinding ib={s_mesh.ibo.SDLBuffer(),0};
    SDL_BindGPUIndexBuffer(rp,&ib,s_mesh.indices_u16?SDL_GPU_INDEXELEMENTSIZE_16BIT:SDL_GPU_INDEXELEMENTSIZE_32BIT);

    // Vertex storage buffers (same order as animated.vert set=0 b=0..3)
    SDL_GPUBuffer* v_sb[4]={
        s_transform_ssbo.SDLBuffer(),
        s_visible_ssbo.SDLBuffer(),
        s_faction_ssbo.SDLBuffer(),
        s_bones_ssbo.SDLBuffer()
    };
    SDL_BindGPUVertexStorageBuffers(rp,0,v_sb,4);

    SDL_PushGPUVertexUniformData(cmd,0,&v_ubo,sizeof(v_ubo));
    SDL_PushGPUFragmentUniformData(cmd,0,&f_ubo,sizeof(f_ubo));
    SDL_DrawGPUIndexedPrimitives(rp,s_mesh.index_count,1,0,0,0);
    SDL_EndGPURenderPass(rp);
}

// ── Public API (same as CharPreviewSDLGPU) ────────────────────────────────────
static void SetBoneScalesFromDef(const float /*body*/[18], const float /*face*/[24]) {
    // Pose computed via GetFinalBonesFull in RenderFrame — no manual bone scaling needed.
}
static void SetBodyMorphWeights(const float body[18], const float /*face*/[24]) {
    auto pd=[](float v,float n,float r)->float{float d=(v-n)/r;return d<0?0:(d>1?1:d);};
    struct{const char*n;float w;} bms[]={
        {"fat",     (pd(body[13],100,90)*.65f+pd(body[15],100,45)*.20f+pd(body[12],100,40)*.15f)*.3f},
        {"muscular",(pd(body[9], 100,45)*.65f+pd(body[8], 100,10)*.15f+pd(body[12],100,40)*.20f)*.3f},
        {"longlegs", pd(body[7],100,15)*.3f},
        {"broadshdr",(pd(body[3],100,20)*.55f+pd(body[8],100,10)*.45f)*.3f},
        {"tall",     pd(body[2],100,20)*.15f},
    };
    for(auto& bm:bms){int mi=morph_by_name(bm.n);
        if(mi>=0&&!s_feq(s_mesh.morph_weights[mi],bm.w)){s_mesh.morph_weights[mi]=bm.w;s_morphs_dirty=true;}}
}
static void SetMorphWeightsFromFace(const float face[],const float def[],const float lo[],const float hi[]) {
    struct FM{int idx;const char*pos,*neg;};
    static const FM kM[]={
        { 5,"big_eyes",nullptr},{ 6,"narrow_eyes",nullptr},{ 7,"close_eyes",nullptr},
        { 8,"high_eyes",nullptr},{ 9,"wide_nose",nullptr},{10,"long_nose",nullptr},
        {11,"arch_nose",nullptr},{12,"tiltup_nose","tiltdown_nose"},{13,"wide_cheekbones","narrow_cheekbones"},
        {14,"shallow_eyes",nullptr},{15,"tiltup_brow","tiltdown_brow"},{16,"high_brow","low_brow"},
        {18,"wide_mouth",nullptr},{20,"big_mouth",nullptr},{21,"overbite","underbite"},
        {22,"tiltup_eyes","tiltdown_eyes"},{23,"high_nose",nullptr},
    };
    for(auto&m:kM){
        float val=face[m.idx],d=def[m.idx];
        if(m.pos){float rhi=hi[m.idx]-d;int mi=morph_by_name(m.pos);
            if(mi>=0&&rhi>1e-6f){float w=fmaxf(0,fminf(1,(val-d)/rhi));w=floorf(w*10)/10;
                if(!s_feq(s_mesh.morph_weights[mi],w)){s_mesh.morph_weights[mi]=w;s_morphs_dirty=true;}}}
        if(m.neg){float rlo=d-lo[m.idx];int mi=morph_by_name(m.neg);
            if(mi>=0&&rlo>1e-6f){float w=fmaxf(0,fminf(1,(d-val)/rlo));w=floorf(w*10)/10;
                if(!s_feq(s_mesh.morph_weights[mi],w)){s_mesh.morph_weights[mi]=w;s_morphs_dirty=true;}}}
    }
}
static void DrawInImGui(float W, float H,
                        float height_scale, float /*bulk*/,
                        const float skin_rgb[3], float skin_str,
                        float sat, float bri,
                        float muscle=0.f, const float* /*hair*/=nullptr)
{
    if(!s_ok){ImGui::Dummy({W,H});return;}
    int iw=(int)W,ih=(int)H; if(iw<4||ih<4)return;
    // Update appearance
    if(!s_feq(s_skin_rgb[0],skin_rgb[0])||!s_feq(s_skin_rgb[1],skin_rgb[1])||
       !s_feq(s_skin_rgb[2],skin_rgb[2])||!s_feq(s_skin_str,skin_str)||
       !s_feq(s_sat,sat)||!s_feq(s_bri,bri)){
        s_skin_rgb[0]=skin_rgb[0];s_skin_rgb[1]=skin_rgb[1];s_skin_rgb[2]=skin_rgb[2];
        s_skin_str=skin_str;s_sat=sat;s_bri=bri;s_colors_dirty=true;
    }
    s_height=height_scale; (void)muscle;
    ensure_rtt(iw,ih);
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
    if(tab==0){s_dist=2.6f;s_pit=-0.06f;s_lookat_y=0.9f;}
    else      {s_dist=0.72f;s_pit=0.f;s_yaw=0.f;s_lookat_y=1.55f;}
}
static void ResetAnimPhase() { /* idle pose is static at t=0 — no phase to reset */ }
static void DumpState(FILE* f=stdout) { fprintf(f,"[CharPreviewGame] ok=%d idle_clip=%d verts=%u morphs=%d\n",
                                       (int)s_ok,s_idle_clip,s_mesh.VertCount(),s_mesh.MorphCount()); }

} // namespace CharPreviewGame
#endif // MD_SDL_GPU
