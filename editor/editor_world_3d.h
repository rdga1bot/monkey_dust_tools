#pragma once
// editor_world_3d.h — 3D terrain world editor (32km Kenshi world).
// OpenGL path only. In SDL_GPU mode this is a stub — replaced by TerrainRenderer.
#ifndef MD_SDL_GPU

#include "imgui.h"
#include "stb_image.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>

namespace WorldEditor3D {

// ── Constants ──────────────────────────────────────────────────────────────────
static constexpr int ATLAS_ZONES   = 64;
static constexpr int ATLAS_VERTS   = 65;
static constexpr int MESH_N        = 256;      // terrain grid resolution
static constexpr int HMAP_SIZE     = 512;      // heightmap texture size
static constexpr float WORLD_M     = 32000.f;  // world size in meters
static constexpr float ZONE_M      = WORLD_M / ATLAS_ZONES;  // 500 m per zone

// ── Shaders ────────────────────────────────────────────────────────────────────
static const char* TERRAIN_VS = R"(
#version 430 core
layout(location=0) in vec2 aXZ;   // [0..1]
uniform sampler2D uHmap;
uniform mat4 uMVP;
uniform float uScale;    // world size in meters (32000)
uniform float uHscale;   // height exaggeration (default 1.0)
out vec2 vUV;
out float vH;
out float vDist;
uniform vec3 uCamPos;

void main() {
    float h  = texture(uHmap, aXZ).r * uHscale;
    vec3  wp = vec3(aXZ.x * uScale, h, aXZ.y * uScale);
    gl_Position = uMVP * vec4(wp, 1.0);
    vUV   = aXZ;
    vH    = h;
    vDist = length(wp - uCamPos);
})";

static const char* TERRAIN_FS = R"(
#version 430 core
in  vec2  vUV;
in  float vH;
in  float vDist;
uniform sampler2D uColorTex;
uniform float uFogStart;
uniform float uFogEnd;
uniform vec4  uZoneRect;  // xmin, zmin, xmax, zmax in [0..1]
out vec4 fragColor;

void main() {
    vec3 col = texture(uColorTex, vUV).rgb;

    // Highlight active 7x7 zone rect
    bool inZone = (vUV.x >= uZoneRect.x && vUV.x <= uZoneRect.z &&
                   vUV.y >= uZoneRect.y && vUV.y <= uZoneRect.w);
    float edgeDist = min(min(abs(vUV.x - uZoneRect.x), abs(vUV.x - uZoneRect.z)),
                         min(abs(vUV.y - uZoneRect.y), abs(vUV.y - uZoneRect.w)));
    if (edgeDist < 0.003) col = mix(col, vec3(1.0, 0.8, 0.0), 0.8); // gold border

    // Simple directional lighting from normal (finite differences approximated by vH)
    col *= 0.85 + vH * 0.0015;   // very mild height brightening

    // Height-based tinting (vH in meters)
    if (vH < 2.0)   col = mix(col, vec3(0.2, 0.4, 0.6), clamp(2.0-vH, 0.0, 0.5));
    if (vH > 220.0) col = mix(col, vec3(0.9, 0.95, 1.0), clamp((vH-220.0)*0.015, 0.0, 0.7));

    // Atmospheric fog
    float fogF = clamp((uFogEnd - vDist) / (uFogEnd - uFogStart), 0.0, 1.0);
    col = mix(vec3(0.55, 0.62, 0.70), col, fogF);

    fragColor = vec4(col, 1.0);
})";

static const char* LINE_VS = R"(
#version 430 core
layout(location=0) in vec3 aPos;
uniform mat4 uMVP;
void main() { gl_Position = uMVP * vec4(aPos, 1.0); })";

static const char* LINE_FS = R"(
#version 430 core
uniform vec4 uColor;
out vec4 fragColor;
void main() { fragColor = uColor; })";

// ── Mat4 (column-major) ────────────────────────────────────────────────────────
struct M4 { float m[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; };
static M4 m4_mul(const M4& a,const M4& b){
    M4 c; memset(c.m,0,64);
    for(int i=0;i<4;i++) for(int j=0;j<4;j++) for(int k=0;k<4;k++)
        c.m[i*4+j]+=a.m[k*4+j]*b.m[i*4+k];
    return c;
}
static M4 m4_persp(float fov,float asp,float n,float f){
    M4 r; memset(r.m,0,64);
    float t=1.f/tanf(fov*.5f);
    r.m[0]=t/asp; r.m[5]=t;
    r.m[10]=(f+n)/(n-f); r.m[11]=-1.f; r.m[14]=(2.f*f*n)/(n-f);
    return r;
}
// Standard fly-camera view matrix (column-major).
// pitch > 0 = look down; pitch < 0 = look up.
static M4 m4_look(float ex,float ey,float ez,float yaw,float pitch){
    float sy=sinf(yaw), cy=cosf(yaw);
    float sp=sinf(pitch),cp=cosf(pitch);
    // Forward (into screen): yaw rotates in XZ, pitch tilts in Y
    float fx= sy*cp, fy=-sp, fz= cy*cp;
    // Right = cross(world_up=(0,1,0), forward) — horizontal
    // = (0*fz-1*fy, 1*fx-0*fz, 0*fy-0*fx) = (-fy,fx,0) ... no
    // Right = cross(forward, world_up): (fy*0-fz*1, fz*0-fx*0, fx*1-fy*0) = (-fz,0,fx)
    // Normalize: |(-fz,0,fx)| = sqrt(fz²+fx²) = sqrt(cy²cp²+sy²cp²) = cp
    float rx=-fz/cp, ry=0, rz=fx/cp;  // = (-cy, 0, sy) — normalized right
    // Up = cross(right, forward)
    float ux=ry*fz-rz*fy, uy=rz*fx-rx*fz, uz=rx*fy-ry*fx;
    M4 r;
    // Column-major: m[col*4+row]
    r.m[0]=rx; r.m[1]=ux; r.m[2]=-fx; r.m[3]=0;
    r.m[4]=ry; r.m[5]=uy; r.m[6]=-fy; r.m[7]=0;
    r.m[8]=rz; r.m[9]=uz; r.m[10]=-fz;r.m[11]=0;
    r.m[12]=-(rx*ex+ry*ey+rz*ez);
    r.m[13]=-(ux*ex+uy*ey+uz*ez);
    r.m[14]= (fx*ex+fy*ey+fz*ez);
    r.m[15]=1;
    return r;
}

// ── GL helpers ─────────────────────────────────────────────────────────────────
static GLuint gl_prog(const char* vs, const char* fs){
    auto mksh=[](GLenum t,const char* s)->GLuint{
        GLuint sh=glCreateShader(t); glShaderSource(sh,1,&s,0); glCompileShader(sh);
        GLint ok; glGetShaderiv(sh,GL_COMPILE_STATUS,&ok);
        if(!ok){char b[512];glGetShaderInfoLog(sh,512,0,b);fprintf(stderr,"[W3D] %s\n",b);}
        return sh;
    };
    GLuint v=mksh(GL_VERTEX_SHADER,vs),f=mksh(GL_FRAGMENT_SHADER,fs);
    GLuint p=glCreateProgram(); glAttachShader(p,v); glAttachShader(p,f);
    glLinkProgram(p); glDeleteShader(v); glDeleteShader(f); return p;
}

// ── State ──────────────────────────────────────────────────────────────────────
static GLuint s_fbo=0, s_col=0, s_dep=0;
static GLuint s_vao=0, s_vbo=0, s_ebo=0;
static GLuint s_hmap=0, s_ctex=0;
static GLuint s_prog=0, s_lprog=0;
static int    s_ni=0, s_fw=0, s_fh=0;

// Camera (position in meters)
static float s_cx=16000.f, s_cy=3000.f, s_cz=16000.f; // center, 3km high
static float s_yaw=0.f, s_pitch=0.6f;   // pitch>0 = look down
static float s_speed=300.f;              // m/s
static bool  s_fly=false, s_rmb=false, s_ok=false;
static ImVec2 s_rmbp;
static float  s_ryaw, s_rpitch;

// Game zone offset in the atlas (7x7 zones within 64x64)
static int s_zone_ox=0, s_zone_oz=0;  // loaded from terrain_config eventually

static void ensure_fbo(int w,int h){
    if(w==s_fw&&h==s_fh&&s_fbo) return;
    if(s_fbo){glDeleteFramebuffers(1,&s_fbo);glDeleteTextures(1,&s_col);glDeleteRenderbuffers(1,&s_dep);}
    s_fw=w; s_fh=h;
    glGenFramebuffers(1,&s_fbo); glBindFramebuffer(GL_FRAMEBUFFER,s_fbo);
    glGenTextures(1,&s_col); glBindTexture(GL_TEXTURE_2D,s_col);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,0);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s_col,0);
    glGenRenderbuffers(1,&s_dep); glBindRenderbuffer(GL_RENDERBUFFER,s_dep);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,w,h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,s_dep);
    glBindFramebuffer(GL_FRAMEBUFFER,0);
}

// ── Load heightmap ─────────────────────────────────────────────────────────────
// Format: magic(4)+zw(4)+zh(4)+verts(4) header; per zone: hmin(4)+hmax(4)+verts² floats
static bool LoadHeightmap(const char* path){
    FILE* f=fopen(path,"rb"); if(!f){fprintf(stderr,"[W3D] hmap open fail\n");return false;}
    uint32_t magic,zw,zh,vp;
    fread(&magic,4,1,f); fread(&zw,4,1,f); fread(&zh,4,1,f); fread(&vp,4,1,f);
    if(magic!=0x414D4800u||zw!=64||zh!=64||vp!=65){
        fprintf(stderr,"[W3D] bad hmap header %08X %u %u %u\n",magic,zw,zh,vp);
        fclose(f); return false;
    }
    // Build HMAP_SIZE×HMAP_SIZE texture by sampling zone heights
    static float hbuf[HMAP_SIZE*HMAP_SIZE];
    // Per-zone: read hmin+hmax+65*65 floats
    static float zone_h[ATLAS_VERTS*ATLAS_VERTS];
    float gmin=1e9f, gmax=-1e9f;
    // First pass: read all zones into a flat array
    static float all_h[ATLAS_ZONES*ATLAS_ZONES]; // zone average heights (for fast access)
    // We'll build the full hmap on a coarse 512×512 grid
    // Each texel covers (WORLD_M/HMAP_SIZE) km = 0.0625 km
    // Each zone is 0.5 km, so 8 texels per zone
    // Sample from zone data: for texel (tx,ty), zone = (tx*ATLAS_ZONES/HMAP_SIZE, ty*...)
    // And within zone: position = fractional part * 64

    // Store all loaded heights for sampling
    static float* all_z = nullptr;
    if(!all_z) all_z = new float[(size_t)ATLAS_ZONES*ATLAS_ZONES*ATLAS_VERTS*ATLAS_VERTS];
    static float hmin_z[ATLAS_ZONES*ATLAS_ZONES], hmax_z[ATLAS_ZONES*ATLAS_ZONES];

    for(int zi=0;zi<ATLAS_ZONES*ATLAS_ZONES;zi++){
        float hmin,hmax;
        fread(&hmin,4,1,f); fread(&hmax,4,1,f);
        hmin_z[zi]=hmin; hmax_z[zi]=hmax;
        fread(all_z + (size_t)zi*ATLAS_VERTS*ATLAS_VERTS, 4, ATLAS_VERTS*ATLAS_VERTS, f);
        if(hmin<gmin) gmin=hmin;
        if(hmax>gmax) gmax=hmax;
    }
    fclose(f);
    fprintf(stdout,"[W3D] hmap loaded, H=[%.1f, %.1f]\n",gmin,gmax);

    // Build HMAP_SIZE×HMAP_SIZE float texture
    float hrange = (gmax-gmin)>0.1f ? gmax-gmin : 1.f;
    for(int ty=0;ty<HMAP_SIZE;ty++){
        for(int tx=0;tx<HMAP_SIZE;tx++){
            float wu = (float)tx/(HMAP_SIZE-1); // [0..1]
            float wv = (float)ty/(HMAP_SIZE-1);
            // Which zone?
            float zfx = wu*(ATLAS_ZONES-1);
            float zfz = wv*(ATLAS_ZONES-1);
            int zx=(int)zfx, zz=(int)zfz;
            if(zx>=ATLAS_ZONES-1) zx=ATLAS_ZONES-2;
            if(zz>=ATLAS_ZONES-1) zz=ATLAS_ZONES-2;
            float fx=zfx-zx, fz=zfz-zz;
            // Bilinear between 4 zone centers
            int zi00=zz*ATLAS_ZONES+zx, zi10=zz*ATLAS_ZONES+(zx+1);
            int zi01=(zz+1)*ATLAS_ZONES+zx, zi11=(zz+1)*ATLAS_ZONES+(zx+1);
            // Within zone, sample center
            int vx=(int)(fx*64), vz=(int)(fz*64);
            float h00=all_z[(size_t)zi00*ATLAS_VERTS*ATLAS_VERTS+vz*ATLAS_VERTS+vx];
            float h10=all_z[(size_t)zi10*ATLAS_VERTS*ATLAS_VERTS+vz*ATLAS_VERTS+vx];
            float h01=all_z[(size_t)zi01*ATLAS_VERTS*ATLAS_VERTS+vz*ATLAS_VERTS+vx];
            float h11=all_z[(size_t)zi11*ATLAS_VERTS*ATLAS_VERTS+vz*ATLAS_VERTS+vx];
            float h=(h00*(1-fx)+h10*fx)*(1-fz)+(h01*(1-fx)+h11*fx)*fz;
            hbuf[ty*HMAP_SIZE+tx]=h;
        }
    }
    glGenTextures(1,&s_hmap);
    glBindTexture(GL_TEXTURE_2D,s_hmap);
    glTexImage2D(GL_TEXTURE_2D,0,GL_R32F,HMAP_SIZE,HMAP_SIZE,0,GL_RED,GL_FLOAT,hbuf);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D,0);

    delete[] all_z; all_z=nullptr;
    return true;
}

static bool LoadColorTex(const char* path){
    // Load md_terrain.png — stb_image (force-included via CMake glad.h mechanism)
    int w,h,c; unsigned char* d=stbi_load(path,&w,&h,&c,3);
    if(!d){fprintf(stderr,"[W3D] terrain tex not found: %s\n",path); return false;}
    glGenTextures(1,&s_ctex);
    glBindTexture(GL_TEXTURE_2D,s_ctex);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB,w,h,0,GL_RGB,GL_UNSIGNED_BYTE,d);
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    stbi_image_free(d);
    fprintf(stdout,"[W3D] terrain tex %dx%d ok\n",w,h);
    return true;
}

static void BuildMesh(){
    // MESH_N×MESH_N grid of (x,z)∈[0..1] vertices
    int V=(MESH_N+1)*(MESH_N+1);
    float* vb=new float[V*2];
    for(int z=0;z<=MESH_N;z++) for(int x=0;x<=MESH_N;x++){
        int i=(z*(MESH_N+1)+x)*2;
        vb[i+0]=(float)x/MESH_N;
        vb[i+1]=(float)z/MESH_N;
    }
    s_ni=MESH_N*MESH_N*6;
    unsigned* ib=new unsigned[s_ni];
    int k=0;
    for(int z=0;z<MESH_N;z++) for(int x=0;x<MESH_N;x++){
        unsigned a=z*(MESH_N+1)+x, b=a+1, c=a+(MESH_N+1), d=c+1;
        ib[k++]=a; ib[k++]=b; ib[k++]=c;
        ib[k++]=b; ib[k++]=d; ib[k++]=c;
    }
    glGenVertexArrays(1,&s_vao); glGenBuffers(1,&s_vbo); glGenBuffers(1,&s_ebo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER,s_vbo);
    glBufferData(GL_ARRAY_BUFFER,V*2*sizeof(float),vb,GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,s_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,s_ni*sizeof(unsigned),ib,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,8,(void*)0);
    glBindVertexArray(0);
    delete[] vb; delete[] ib;
}

// ── Init (call once) ───────────────────────────────────────────────────────────
static bool Init(const char* hmap_path, const char* color_path,
                 int zone_offset_x=0, int zone_offset_z=0){
    s_zone_ox=zone_offset_x; s_zone_oz=zone_offset_z;
    if(!LoadHeightmap(hmap_path)) return false;
    LoadColorTex(color_path);  // optional — solid color if missing
    BuildMesh();
    s_prog  = gl_prog(TERRAIN_VS, TERRAIN_FS);
    s_lprog = gl_prog(LINE_VS,    LINE_FS);
    s_ok=true;
    return true;
}

// ── Input helper: world-space move direction ───────────────────────────────────
static void move_cam(float forward, float up, float right){
    float sy=sinf(s_yaw), cy=cosf(s_yaw);
    float cp=cosf(s_pitch);
    // Forward in XZ (ignore pitch for movement — easier to navigate)
    s_cx += forward * sy + right * cy;
    s_cz += forward * cy - right * sy;
    s_cy += up;
    if(s_cx<0) s_cx=0; if(s_cx>WORLD_M) s_cx=WORLD_M;
    if(s_cz<0) s_cz=0; if(s_cz>WORLD_M) s_cz=WORLD_M;
    if(s_cy<10.f) s_cy=10.f; if(s_cy>25000.f) s_cy=25000.f;
    (void)cp;
}

// ── Main draw ──────────────────────────────────────────────────────────────────
static void Draw(float W, float H, float dt){
    if(!s_ok) return;
    int iw=(int)W, ih=(int)H; if(iw<8||ih<8) return;
    ensure_fbo(iw,ih);

    // ── Input capture ──────────────────────────────────────────────────────────
    ImVec2 origin=ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##w3d",{W,H},
        ImGuiButtonFlags_MouseButtonLeft|ImGuiButtonFlags_MouseButtonRight|
        ImGuiButtonFlags_MouseButtonMiddle);
    bool hov=ImGui::IsItemHovered();
    ImGuiIO& io=ImGui::GetIO();

    // Right-mouse look
    if(hov&&io.MouseClicked[1]){
        s_rmb=true; s_rmbp=io.MousePos; s_ryaw=s_yaw; s_rpitch=s_pitch;
    }
    if(s_rmb){
        if(io.MouseDown[1]){
            s_yaw   =s_ryaw  +(io.MousePos.x-s_rmbp.x)*0.0018f;
            s_pitch =s_rpitch+(io.MousePos.y-s_rmbp.y)*0.0014f;
            if(s_pitch< -0.3f) s_pitch=-0.3f;  // slight look-up max
            if(s_pitch>  1.3f) s_pitch= 1.3f;  // near-vertical look-down
        } else s_rmb=false;
    }

    // WASD keyboard (only when hovered/active)
    if(hov||ImGui::IsItemActive()){
        float sp=s_speed*dt;
        if(ImGui::IsKeyDown(ImGuiKey_W)||ImGui::IsKeyDown(ImGuiKey_UpArrow))
            move_cam( sp, 0, 0);
        if(ImGui::IsKeyDown(ImGuiKey_S)||ImGui::IsKeyDown(ImGuiKey_DownArrow))
            move_cam(-sp, 0, 0);
        if(ImGui::IsKeyDown(ImGuiKey_A))
            move_cam( 0, 0,-sp);
        if(ImGui::IsKeyDown(ImGuiKey_D))
            move_cam( 0, 0, sp);
        if(ImGui::IsKeyDown(ImGuiKey_Q)||ImGui::IsKeyDown(ImGuiKey_PageDown))
            move_cam( 0,-sp, 0);
        if(ImGui::IsKeyDown(ImGuiKey_E)||ImGui::IsKeyDown(ImGuiKey_PageUp))
            move_cam( 0, sp, 0);
    }

    // Scroll: adjust move speed
    if(hov&&io.MouseWheel!=0.f){
        if(io.KeyCtrl){
            // Ctrl+scroll → adjust WASD speed
            s_speed*=powf(1.3f, io.MouseWheel);
            if(s_speed<10.f)    s_speed=10.f;
            if(s_speed>15000.f) s_speed=15000.f;
        } else {
            // Scroll → zoom: move forward + adjust altitude proportionally
            float step = s_cy * 0.12f * io.MouseWheel;
            move_cam(step, 0, 0);
            if(io.MouseWheel > 0) s_cy = fmaxf(s_cy * 0.92f, 50.f);
            if(io.MouseWheel < 0) s_cy = fminf(s_cy * 1.08f, 25000.f);
        }
    }

    // ── Render to FBO ──────────────────────────────────────────────────────────
    GLint pf; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&pf);
    GLint pv[4]; glGetIntegerv(GL_VIEWPORT,pv);

    glBindFramebuffer(GL_FRAMEBUFFER,s_fbo);
    glViewport(0,0,iw,ih);
    glClearColor(0.55f,0.62f,0.70f,1.f); // sky colour
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);

    M4 proj=m4_persp(0.85f,(float)iw/(float)ih, 10.f, 80000.f);
    M4 view=m4_look(s_cx,s_cy,s_cz,s_yaw,s_pitch);
    M4 mvp =m4_mul(proj,view);

    // Height scale: game uses Y in world meters; 1 km = 1000 m
    // Kenshi height ~0-500m; in game units, CHUNK_SIZE=500 → height maybe in same units
    // We store height raw from file, scale so 30 units ≈ visual mountain height on 32km world
    float hscale=1.0f; // tuned below

    glUseProgram(s_prog);
    glUniformMatrix4fv(glGetUniformLocation(s_prog,"uMVP"),1,GL_FALSE,mvp.m);
    glUniform1f(glGetUniformLocation(s_prog,"uScale"),WORLD_M);
    glUniform1f(glGetUniformLocation(s_prog,"uHscale"),hscale);
    glUniform3f(glGetUniformLocation(s_prog,"uCamPos"),s_cx,s_cy,s_cz);
    glUniform1f(glGetUniformLocation(s_prog,"uFogStart"), 8000.f);
    glUniform1f(glGetUniformLocation(s_prog,"uFogEnd"),  40000.f);

    // Zone rect: game uses 7x7 zones starting at offset (s_zone_ox, s_zone_oz)
    float zx0=(float)s_zone_ox/ATLAS_ZONES, zx1=(float)(s_zone_ox+7)/ATLAS_ZONES;
    float zz0=(float)s_zone_oz/ATLAS_ZONES, zz1=(float)(s_zone_oz+7)/ATLAS_ZONES;
    glUniform4f(glGetUniformLocation(s_prog,"uZoneRect"),zx0,zz0,zx1,zz1);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D,s_hmap);
    glUniform1i(glGetUniformLocation(s_prog,"uHmap"),0);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D,s_ctex?s_ctex:s_hmap);
    glUniform1i(glGetUniformLocation(s_prog,"uColorTex"),1);

    glBindVertexArray(s_vao);
    glDrawElements(GL_TRIANGLES,s_ni,GL_UNSIGNED_INT,0);
    glBindVertexArray(0);

    glDisable(GL_DEPTH_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)pf);
    glViewport(pv[0],pv[1],pv[2],pv[3]);

    // ── Display ────────────────────────────────────────────────────────────────
    ImGui::GetWindowDrawList()->AddImage(
        (ImTextureID)(intptr_t)s_col,
        origin,{origin.x+W,origin.y+H},{0.f,1.f},{1.f,0.f});

    // Border
    if(hov)
        ImGui::GetWindowDrawList()->AddRect(origin,{origin.x+W,origin.y+H},
            IM_COL32(80,140,220,150),1.f);

    // ── HUD overlay ────────────────────────────────────────────────────────────
    ImGui::SetCursorScreenPos({origin.x+8, origin.y+8});
    ImGui::PushStyleColor(ImGuiCol_Text,IM_COL32(255,255,200,220));
    ImGui::Text("Pos: (%.0f, %.0f) m  |  Alt: %.0f m  |  Speed: %.0f m/s  [Scroll to adjust]",
        s_cx, s_cz, s_cy, s_speed);
    ImGui::Text("RMB=look  WASD=move  Q/E=up/down  Scroll=zoom  Ctrl+Scroll=speed");
    ImGui::PopStyleColor();
}

} // namespace WorldEditor3D

#else // MD_SDL_GPU — stub (SDL_GPU terrain viewer integrated directly in main.cpp)
namespace WorldEditor3D {
static bool Init(const char*, const char*, int=0, int=0) { return false; }
static void Draw(float W, float H, float) {
    ImGui::TextDisabled("3D World: SDL_GPU TerrainRenderer integration in progress.");
    ImGui::Dummy({W, H - 20});
}
} // namespace WorldEditor3D
#endif // !MD_SDL_GPU
