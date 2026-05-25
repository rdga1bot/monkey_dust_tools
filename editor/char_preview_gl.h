#pragma once
// char_preview_gl.h — 3D character preview (OpenGL only).
// In SDL_GPU editor mode (MD_SDL_GPU) this is a no-op stub;
// the full SDL_GPU preview will be added as editor_char_preview_sdlgpu.h.
#ifndef MD_SDL_GPU

#include "imgui.h"
#include "cgltf.h"       // engine/src/render/cgltf.h (CGLTF_IMPLEMENTATION in cgltf_impl.cpp)
#include "stb_image.h"   // engine/src/vendor/stb_image.h
#include <cstring>
#include <cstdio>
#include <cmath>

namespace CharPreviewGL {

// ── Internal mat4 (column-major, no GLM needed) ───────────────────────────────
struct M4 {
    float m[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
};
static M4 m4_mul(const M4& a, const M4& b) {
    M4 c; memset(c.m, 0, 64);
    for (int i=0;i<4;i++) for (int j=0;j<4;j++) for (int k=0;k<4;k++)
        c.m[i*4+j] += a.m[k*4+j] * b.m[i*4+k];
    return c;
}
static M4 m4_persp(float fov, float asp, float n, float f) {
    M4 r; memset(r.m, 0, 64);
    float t = 1.f / tanf(fov * 0.5f);
    r.m[0]=t/asp; r.m[5]=t;
    r.m[10]=(f+n)/(n-f); r.m[11]=-1.f;
    r.m[14]=(2.f*f*n)/(n-f);
    return r;
}
static M4 m4_rotY(float a) {
    M4 r; r.m[0]=cosf(a); r.m[2]=sinf(a); r.m[8]=-sinf(a); r.m[10]=cosf(a);
    return r;
}
static M4 m4_rotX(float a) {
    M4 r; r.m[5]=cosf(a); r.m[6]=-sinf(a); r.m[9]=sinf(a); r.m[10]=cosf(a);
    return r;
}
static M4 m4_translate(float x, float y, float z) {
    M4 r; r.m[12]=x; r.m[13]=y; r.m[14]=z;
    return r;
}

// ── GL helpers ────────────────────────────────────────────────────────────────
static GLuint gl_shader(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char buf[512]; glGetShaderInfoLog(s, 512, nullptr, buf);
        fprintf(stderr, "[CharPreview] shader: %s\n", buf);
    }
    return s;
}
static GLuint gl_program(const char* vs_src, const char* fs_src) {
    GLuint vs = gl_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = gl_shader(GL_FRAGMENT_SHADER, fs_src);
    GLuint p = glCreateProgram();
    glAttachShader(p, vs); glAttachShader(p, fs);
    glLinkProgram(p);
    glDeleteShader(vs); glDeleteShader(fs);
    return p;
}

// ── Shaders ───────────────────────────────────────────────────────────────────
static const char* VS = R"(
#version 430 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNorm;
layout(location=2) in vec2 aUV;
uniform mat4 uMVP;
uniform mat4 uNorm;
out vec3 vN;
out vec2 vUV;
void main() {
    gl_Position = uMVP * vec4(aPos, 1.0);
    vN  = normalize(mat3(uNorm) * aNorm);
    vUV = aUV;
})";

static const char* FS = R"(
#version 430 core
in  vec3 vN;
in  vec2 vUV;
uniform sampler2D uTex;
uniform vec3  uSkin;    // base skin tint RGB
uniform float uStr;     // tint strength
uniform float uSat;     // saturation multiplier
uniform float uBri;     // brightness offset
out vec4 fragColor;

vec3 colorize(vec3 c, float sat, float bri) {
    float lum = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(lum), c, sat);
    return clamp(c + bri, 0.0, 1.0);
}
void main() {
    vec3 L = normalize(vec3(0.6, 1.2, 0.8));
    vec3 N = normalize(vN);
    float diff = clamp(dot(N, L), 0.0, 1.0);
    float light = diff * 0.65 + 0.35; // diffuse + ambient

    vec3 body = texture(uTex, vUV).rgb;
    body = mix(body, uSkin, uStr * 0.18); // subtle skin tint
    body = colorize(body, uSat, uBri);
    body *= light;

    // ACES filmic + gamma
    body = clamp((body*(2.51*body+0.03))/(body*(2.43*body+0.59)+0.14),0.0,1.0);
    body = sqrt(body * sqrt(body));

    fragColor = vec4(body, 1.0);
})";

// ── Vertex layout: pos(12) + norm(12) + uv(8) = 32 bytes ─────────────────────
struct Vtx { float px,py,pz, nx,ny,nz, u,v; };

// ── State ─────────────────────────────────────────────────────────────────────
static GLuint s_fbo=0, s_col=0, s_dep=0;
static GLuint s_vao=0, s_vbo=0, s_ebo=0;
static GLuint s_tex=0, s_prog=0;
static int    s_ni=0, s_fw=0, s_fh=0;
static float  s_yaw=0.18f, s_pit=-0.06f, s_dist=2.6f;
static bool   s_drag=false;
static ImVec2 s_d0;
static float  s_y0, s_p0;
static bool   s_ok=false;

// ── Resize FBO ────────────────────────────────────────────────────────────────
static void ensure_fbo(int w, int h) {
    if (w==s_fw && h==s_fh && s_fbo) return;
    if (s_fbo) {
        glDeleteFramebuffers(1,&s_fbo);
        glDeleteTextures(1,&s_col);
        glDeleteRenderbuffers(1,&s_dep);
    }
    s_fw=w; s_fh=h;
    glGenFramebuffers(1,&s_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER,s_fbo);
    glGenTextures(1,&s_col);
    glBindTexture(GL_TEXTURE_2D,s_col);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA8,w,h,0,GL_RGBA,GL_UNSIGNED_BYTE,nullptr);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s_col,0);
    glGenRenderbuffers(1,&s_dep);
    glBindRenderbuffer(GL_RENDERBUFFER,s_dep);
    glRenderbufferStorage(GL_RENDERBUFFER,GL_DEPTH_COMPONENT24,w,h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_RENDERBUFFER,s_dep);
    glBindFramebuffer(GL_FRAMEBUFFER,0);
}

// ── Init: load GLB (T-pose) + body texture ────────────────────────────────────
static bool Init(const char* glb, const char* tex) {
    // cgltf
    cgltf_options o={};
    cgltf_data* d=nullptr;
    if (cgltf_parse_file(&o,glb,&d) != cgltf_result_success) {
        fprintf(stderr,"[CharPreview] glb load failed: %s\n", glb); return false;
    }
    cgltf_load_buffers(&o,d,glb);
    if (!d->meshes_count || !d->meshes[0].primitives_count) { cgltf_free(d); return false; }

    cgltf_primitive& pr = d->meshes[0].primitives[0];
    cgltf_accessor *ap=nullptr, *an=nullptr, *au=nullptr;
    for (size_t i=0;i<pr.attributes_count;i++) {
        auto& a = pr.attributes[i];
        if (a.type==cgltf_attribute_type_position) ap=a.data;
        else if (a.type==cgltf_attribute_type_normal) an=a.data;
        else if (a.type==cgltf_attribute_type_texcoord && !au) au=a.data;
    }
    if (!ap) { cgltf_free(d); return false; }

    size_t vc = ap->count;
    Vtx* vb = new Vtx[vc]; memset(vb,0,vc*sizeof(Vtx));
    for (size_t i=0;i<vc;i++) {
        float v[3]={};
        cgltf_accessor_read_float(ap,i,v,3);
        vb[i].px=v[0]; vb[i].py=v[1]; vb[i].pz=v[2];
    }
    if (an) for (size_t i=0;i<vc;i++) {
        float v[3]={};
        cgltf_accessor_read_float(an,i,v,3);
        vb[i].nx=v[0]; vb[i].ny=v[1]; vb[i].nz=v[2];
    }
    if (au) for (size_t i=0;i<vc;i++) {
        float v[2]={};
        cgltf_accessor_read_float(au,i,v,2);
        vb[i].u=v[0]; vb[i].v=v[1];
    }

    s_ni = (int)pr.indices->count;
    unsigned* ib = new unsigned[s_ni];
    for (int i=0;i<s_ni;i++)
        ib[i]=(unsigned)cgltf_accessor_read_index(pr.indices,i);
    cgltf_free(d);

    glGenVertexArrays(1,&s_vao); glGenBuffers(1,&s_vbo); glGenBuffers(1,&s_ebo);
    glBindVertexArray(s_vao);
    glBindBuffer(GL_ARRAY_BUFFER,s_vbo);
    glBufferData(GL_ARRAY_BUFFER,(GLsizeiptr)(vc*sizeof(Vtx)),vb,GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER,s_ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,(GLsizeiptr)(s_ni*sizeof(unsigned)),ib,GL_STATIC_DRAW);
    glEnableVertexAttribArray(0); glVertexAttribPointer(0,3,GL_FLOAT,GL_FALSE,sizeof(Vtx),(void*)0);
    glEnableVertexAttribArray(1); glVertexAttribPointer(1,3,GL_FLOAT,GL_FALSE,sizeof(Vtx),(void*)12);
    glEnableVertexAttribArray(2); glVertexAttribPointer(2,2,GL_FLOAT,GL_FALSE,sizeof(Vtx),(void*)24);
    glBindVertexArray(0);
    delete[] vb; delete[] ib;

    // Body texture
    stbi_set_flip_vertically_on_load(0);
    int tw,th,tc; unsigned char* td = stbi_load(tex,&tw,&th,&tc,4);
    glGenTextures(1,&s_tex);
    glBindTexture(GL_TEXTURE_2D,s_tex);
    if (td) {
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,tw,th,0,GL_RGBA,GL_UNSIGNED_BYTE,td);
        glGenerateMipmap(GL_TEXTURE_2D);
        stbi_image_free(td);
    } else {
        unsigned char w4[4]={200,162,122,255};
        glTexImage2D(GL_TEXTURE_2D,0,GL_RGBA,1,1,0,GL_RGBA,GL_UNSIGNED_BYTE,w4);
    }
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D,0);

    s_prog = gl_program(VS, FS);
    s_ok = true;
    return true;
}

// ── Draw: render to FBO, display as ImGui::Image ─────────────────────────────
// height_scale, bulk_scale: from CharDef (applied as model matrix scale)
// skin_rgb, skin_str, sat, bri: colorise params
static void DrawInImGui(float W, float H,
                        float height_scale, float bulk_scale,
                        const float skin_rgb[3], float skin_str,
                        float sat, float bri)
{
    if (!s_ok) return;
    int iw=(int)W, ih=(int)H;
    if (iw<4||ih<4) return;
    ensure_fbo(iw,ih);

    // Drag orbit input — sample BEFORE image so InvisibleButton captures mouse
    ImVec2 origin = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton("##cpv", {W, H},
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    bool hov = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();
    if (hov && io.MouseClicked[0]) {
        s_drag=true; s_d0=io.MousePos; s_y0=s_yaw; s_p0=s_pit;
    }
    if (s_drag) {
        if (io.MouseDown[0]) {
            s_yaw = s_y0 + (io.MousePos.x - s_d0.x) * 0.007f;
            s_pit = s_p0 + (io.MousePos.y - s_d0.y) * 0.005f;
            if (s_pit < -0.45f) s_pit=-0.45f;
            if (s_pit >  0.45f) s_pit= 0.45f;
        } else s_drag=false;
    }
    // Scroll wheel zoom (only when hovered)
    if (hov && io.MouseWheel != 0.f) {
        s_dist -= io.MouseWheel * 0.18f;
        if (s_dist < 0.5f) s_dist = 0.5f;
        if (s_dist > 6.0f) s_dist = 6.0f;
    }
    // Draw image on top of invisible button region
    ImGui::GetWindowDrawList()->AddImage(
        (ImTextureID)(intptr_t)s_col,
        origin, {origin.x+W, origin.y+H},
        {0.f,1.f},{1.f,0.f}   // flip Y: GL FBO origin is bottom-left
    );
    // Drag hint
    if (hov) {
        ImGui::GetWindowDrawList()->AddRect(origin,{origin.x+W,origin.y+H},
            IM_COL32(80,120,200,120), 2.f);
    }

    // ── Render to FBO ─────────────────────────────────────────────────────────
    GLint pf; glGetIntegerv(GL_FRAMEBUFFER_BINDING,&pf);
    GLint pv[4]; glGetIntegerv(GL_VIEWPORT,pv);

    glBindFramebuffer(GL_FRAMEBUFFER, s_fbo);
    glViewport(0,0,iw,ih);
    glClearColor(0.10f,0.10f,0.13f,1.f);
    glClear(GL_COLOR_BUFFER_BIT|GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_LESS);
    glEnable(GL_CULL_FACE);  glCullFace(GL_BACK);

    glUseProgram(s_prog);

    // Model: scale by height/bulk, translate down so feet at bottom
    M4 ms; ms.m[0]=bulk_scale; ms.m[5]=height_scale; ms.m[10]=bulk_scale;
    M4 mt = m4_translate(0.f, -0.95f*height_scale, 0.f);
    M4 model = m4_mul(ms, mt);  // scale then translate
    // Orbit view
    M4 rv = m4_mul(m4_rotX(s_pit), m4_rotY(s_yaw));
    M4 view = m4_mul(rv, m4_translate(0.f, 0.f, -s_dist));
    M4 proj = m4_persp(0.78f,(float)iw/(float)ih, 0.05f, 10.f);
    M4 mvp  = m4_mul(proj, m4_mul(view, model));

    glUniformMatrix4fv(glGetUniformLocation(s_prog,"uMVP"),  1,GL_FALSE,mvp.m);
    glUniformMatrix4fv(glGetUniformLocation(s_prog,"uNorm"), 1,GL_FALSE,model.m);
    glUniform3fv(glGetUniformLocation(s_prog,"uSkin"),       1,skin_rgb);
    glUniform1f (glGetUniformLocation(s_prog,"uStr"),        skin_str);
    glUniform1f (glGetUniformLocation(s_prog,"uSat"),        sat);
    glUniform1f (glGetUniformLocation(s_prog,"uBri"),        bri);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, s_tex);
    glUniform1i(glGetUniformLocation(s_prog,"uTex"), 0);

    glBindVertexArray(s_vao);
    glDrawElements(GL_TRIANGLES, s_ni, GL_UNSIGNED_INT, nullptr);
    glBindVertexArray(0);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glBindFramebuffer(GL_FRAMEBUFFER,(GLuint)pf);
    glViewport(pv[0],pv[1],pv[2],pv[3]);
}

} // namespace CharPreviewGL

#endif // !MD_SDL_GPU
// SDL_GPU stub
#ifdef MD_SDL_GPU
namespace CharPreviewGL {
static bool Init(const char*, const char*) { return false; }
static void DrawInImGui(float W, float H, float, float,
    const float*, float, float, float) {
    ImGui::Dummy({W, H});
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
        IM_COL32(28,28,34,255));
    ImVec2 c = {(ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().x)*0.5f,
                (ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y)*0.5f};
    ImGui::GetWindowDrawList()->AddText(
        {c.x - 80, c.y - 8}, IM_COL32(120,120,140,200),
        "3D preview: SDL_GPU mode");
}
} // namespace CharPreviewGL
#endif // MD_SDL_GPU
