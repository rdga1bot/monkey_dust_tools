#include "editor_char_preview_sdlgpu_internal.h"
#ifdef MD_SDL_GPU

// ── State storage (declared extern in the internal header; this TU owns it) ──
GpuPipeline     s_bg_pipeline;
GpuTexture      s_bg_sand;   // desert_sand.jpg — ground texture
GpuTexture      s_bg_dune;   // desert_dune.jpg — large-scale dune pattern
GpuPipeline     s_scene_pipeline;  // anthropometer pole, flat-color
GpuStaticBuffer s_scene_vbo;
GpuStaticBuffer s_scene_ibo;
int             s_scene_ni = 0;
GpuPipeline     s_pipeline;
GpuStaticBuffer s_vbo;
GpuStaticBuffer s_ibo;
GpuTexture      s_tex;
GpuTexture      s_tex_head;    // head/face diffuse (V<0 UV island)
GpuTexture      s_tex_muscle;  // 1×1 neutral muscle mask
GpuTexture      s_tex_blood;   // 1×1 clear blood overlay
md::GpuTextureHandle s_bones_tex     = nullptr;
SDL_GPUSampler* s_bones_sampler = nullptr;
int             s_ni  = 0;
bool            s_ok  = false;
GpuPipeline     s_hair_pipeline;
GpuStaticBuffer s_hair_vbo;
GpuStaticBuffer s_hair_ibo;
int             s_hair_ni      = 0;     // index count of current style
ClothSlot   s_cloth[3];
GpuPipeline s_cloth_pipeline;
int         s_sex         = 0;   // 0=Male 1=Female; set by Init()
float s_boneScales[30][3]; // setBoneSize vertex scale
float s_posScale[30][3];   // setBonePositionalSize (default identity)
float s_ws_mat[30][16];
float s_inv_bind[30][16];   // inverseBindMatrices from GLB (world→bone local)
float s_bind[30][16];       // bind matrices = inv(inv_bind)
float s_bind_local[30][16]; // bind_local[i] = inv_bind[parent] * bind[i]
int8_t s_bone_parent[30];   // parent joint index, -1 for root
float s_idle_rot[30][4];    // idle_stand_normal frame-0 quaternion (xyzw) per bone
bool  s_idle_has_rot[30];   // true if bone has explicit rotation channel in idle_stand_normal
bool  s_idle_loaded = false;
SkinMesh    s_pose_mesh;
OzzAnimator s_pose_ozz;
int         s_pose_idle_clip     = -1;
int         s_pose_postures_clip = -1;
int         s_pose_neck_clip     = -1;
int         s_pose_shoulder_clip = -1;
BreathChan s_breath[30];
float      s_breath_len = 0.f;
bool       s_breath_loaded = false;
SliderAnim s_anim_postures;    // body[4]  Posture    → "postures"
SliderAnim s_anim_neck_set;    // body[5]  Shoulder set → "neck set" (Kenshi naming)
SliderAnim s_anim_shoulder_set;// body[6]  Neck pos   → "shoulder set"
GpuColorTexture s_color;
GpuColorTexture s_depth;
int s_rtt_w = 0;
int s_rtt_h = 0;
float s_yaw = 0.18f;
float s_pit = -0.06f;
float s_dist = 3.5f;
float    s_lookat_y = 0.9f;  // vertical pivot offset (0=full body, ~0.9=waist, ~0.88*h=face)
ImVec2   s_d0;
float    s_y0;
uint64_t s_anim_epoch_ms = 0; // breathing phase reset epoch
Vtx*   s_base_verts_cpu  = nullptr;   // persistent base mesh (not freed after GPU upload)
int    s_base_vc         = 0;
float* s_morph_deltas    = nullptr;   // [morph_count × vc × 3] heap-allocated
int    s_morph_count     = 0;
char   s_morph_names[32][48]  = {};
float  s_morph_weights[32]    = {};
bool   s_morphs_dirty    = false;
float s_height = 1.f;
float s_bulk = 1.f;
float s_leg_y  = 0.95f;  // cached from SetBoneScalesFromDef for foot grounding
float s_skin[3] = {0.82f, 0.65f, 0.52f};
float s_str = 0.55f;
float s_sat = 1.f;
float s_bri = 0.f;
float s_muscle = 0.f;
float s_hair[3] = {0.18f, 0.12f, 0.08f};
float s_pose_rot[30][4];
float s_pose_tra[30][3];
PortraitCfg s_pcfg;
bool        s_pcfg_loaded = false;

static void quat_nlerp(float out[4], const float a[4], const float b[4], float t) {
    float dot = a[0]*b[0]+a[1]*b[1]+a[2]*b[2]+a[3]*b[3];
    float s = dot < 0.f ? -1.f : 1.f;
    float r[4]; for(int i=0;i<4;i++) r[i]=a[i]+(b[i]*s-a[i])*t;
    float len=sqrtf(r[0]*r[0]+r[1]*r[1]+r[2]*r[2]+r[3]*r[3]);
    if(len>1e-6f){float il=1.f/len;for(int i=0;i<4;i++)out[i]=r[i]*il;}
    else {out[0]=0;out[1]=0;out[2]=0;out[3]=1;}
}
// Additive quaternion accumulation for OGRE ANIMBLEND_AVERAGE.
// Hemisphere-corrects q relative to current sum before adding.
static void quat_blend_add(float sum[4], const float q[4]) {
    float dot=sum[0]*q[0]+sum[1]*q[1]+sum[2]*q[2]+sum[3]*q[3];
    float s=dot<0.f?-1.f:1.f;
    sum[0]+=s*q[0]; sum[1]+=s*q[1]; sum[2]+=s*q[2]; sum[3]+=s*q[3];
}
static void quat_blend_normalize(float q[4]) {
    float len=sqrtf(q[0]*q[0]+q[1]*q[1]+q[2]*q[2]+q[3]*q[3]);
    if(len>1e-6f){float il=1.f/len;q[0]*=il;q[1]*=il;q[2]*=il;q[3]*=il;}
    else{q[0]=0;q[1]=0;q[2]=0;q[3]=1;}
}
// Hamilton product a*b (xyzw storage).
void quat_mul(float out[4], const float a[4], const float b[4]) {
    out[0] = a[3]*b[0] + a[0]*b[3] + a[1]*b[2] - a[2]*b[1];
    out[1] = a[3]*b[1] - a[0]*b[2] + a[1]*b[3] + a[2]*b[0];
    out[2] = a[3]*b[2] + a[0]*b[1] - a[1]*b[0] + a[2]*b[3];
    out[3] = a[3]*b[3] - a[0]*b[0] - a[1]*b[1] - a[2]*b[2];
}

// All rotations come from idle_stand_normal. Breathing contributes only translation (vertical sway).
// Rationale: breathing noarms stores absolute rotations that differ from idle — applying them
// directly diverges from the in-game idle_stand_normal pose. The game blends idle+breathing,
// but we just use idle to exactly match the game's visual output.
static const bool kBreathRotList[30] = {
    false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false,
    false, false, false, false, false, false, false, false, false, false,
};

// Sample breathing animation at time t (seconds). Fills pose_rot[30][4] and pose_tra[30][3].
// Rotations: only kBreathRotList bones use breathing; others use idle_stand_normal.
// Translations: ROOT uses breathing (vertical sway); others use bind_local.
static void SampleBreathing(float t, float pose_rot[30][4], float pose_tra[30][3]) {
    for (int i = 0; i < 30; i++) {
        // ── rotation ──────────────────────────────────────────────────────
        BreathChan& bc = s_breath[i];
        if (kBreathRotList[i] && bc.rcount >= 2) {
            // binary search for left bracket
            int lo=0, hi=bc.rcount-2;
            while (lo<hi) { int mid=(lo+hi+1)/2; if(bc.times[mid]<=t) lo=mid; else hi=mid-1; }
            int k = lo;
            float dt = bc.times[k+1]-bc.times[k];
            float alpha = (dt>1e-7f) ? (t-bc.times[k])/dt : 0.f;
            alpha = alpha<0.f?0.f:(alpha>1.f?1.f:alpha);
            quat_nlerp(pose_rot[i], bc.quats+k*4, bc.quats+(k+1)*4, alpha);
        } else if (kBreathRotList[i] && bc.rcount==1) {
            memcpy(pose_rot[i], bc.quats, 16);
        } else {
            memcpy(pose_rot[i], s_idle_rot[i], 16);
        }
        // ── translation: always bind_local ────────────────────────────────
        // Breathing translation (ROOT/Pelvis bob) is in ROOT-local space (90° Y rotation).
        // Applying it shifts the character forward in world space → visible lean.
        // The game uses static idle_stand_normal pose without breathing translation.
        pose_tra[i][0]=s_bind_local[i][12];
        pose_tra[i][1]=s_bind_local[i][13];
        pose_tra[i][2]=s_bind_local[i][14];
    }
}

// col-major mat4 multiply: C = A * B
void m4mul(float* C, const float* A, const float* B) {
    float T[16];
    for (int j=0;j<4;j++) for (int i=0;i<4;i++) {
        float s=0.f; for (int k=0;k<4;k++) s+=A[k*4+i]*B[j*4+k]; T[j*4+i]=s;
    }
    memcpy(C,T,64);
}
// Load SliderAnim from GLB animation by name.
// Kenshi RE (line 19458): setTimePosition(length * slider_value * 0.01)
// → alpha=slider*0.01 maps linearly to animation time.
// rot0 = first keyframe, rot1 = LAST keyframe.
// For 3-keyframe anims (shoulder set, neck set), setTimePosition at alpha=0.5
// hits EXACTLY the middle keyframe — stored in rot_mid for direct use.
void LoadSliderAnim(cgltf_data* d, int* node_to_ji, SliderAnim& out, const char* name) {
    out.loaded = false; out.length = 0.f; out.key_count = 0;
    memset(out.has, 0, sizeof(out.has));
    for(int i=0;i<30;i++){
        out.rot0[i][3]=1; out.rot_mid[i][3]=1; out.rot1[i][3]=1;
    }
    for(int ai=0;ai<(int)d->animations_count;++ai){
        cgltf_animation& anim=d->animations[ai];
        if (!anim.name||strcmp(anim.name,name)!=0) continue;
        // Read keyframe count and times from first rotation channel.
        int kf_cnt = 0;
        for(int ci=0;ci<(int)anim.channels_count;++ci){
            cgltf_animation_channel& ch=anim.channels[ci];
            if (!ch.sampler||!ch.sampler->input||ch.sampler->input->count==0) continue;
            if (ch.target_path!=cgltf_animation_path_type_rotation) continue;
            if (out.length==0.f){
                float lt=0.f;
                cgltf_accessor_read_float(ch.sampler->input, ch.sampler->input->count-1, &lt, 1);
                out.length = lt;
            }
            kf_cnt = (int)ch.sampler->input->count;
            int store = kf_cnt < SliderAnim::MAX_KEYS ? kf_cnt : SliderAnim::MAX_KEYS;
            out.key_count = store;
            for(int k=0;k<store;k++)
                cgltf_accessor_read_float(ch.sampler->input, k, &out.key_times[k], 1);
            // init all key_rot slots to identity
            for(int k=0;k<store;k++)
                for(int b=0;b<30;b++){out.key_rot[k][b][0]=0;out.key_rot[k][b][1]=0;out.key_rot[k][b][2]=0;out.key_rot[k][b][3]=1;}
            break;
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
            else        memcpy(out.rot1[ji], out.rot0[ji], 16);
            { int mid = cnt/2; if(mid<0)mid=0; if(mid>=cnt)mid=cnt-1;
              cgltf_accessor_read_float(ch.sampler->output, mid, out.rot_mid[ji], 4); }
            // Store all keyframes for full time-based sampling.
            int store = cnt < SliderAnim::MAX_KEYS ? cnt : SliderAnim::MAX_KEYS;
            for(int k=0;k<store;k++)
                cgltf_accessor_read_float(ch.sampler->output, k, out.key_rot[k][ji], 4);
            out.has[ji]=true;
        }
        out.loaded=true;
        fprintf(stdout,"[CharPreview] slider anim '%s': %d keyframes, length=%.4fs\n",
                name, kf_cnt, out.length);
        break;
    }
}

// Sample a SliderAnim by animation time, interpolating between adjacent keyframes.
// Kenshi RE: timePos = length * slider * 0.01
void SampleAnimAtTime(const SliderAnim& sa, int bone, float t, float out[4]) {
    int n = sa.key_count;
    if (n <= 0) { memcpy(out, sa.rot0[bone], 16); return; }
    if (n == 1 || t <= sa.key_times[0]) { memcpy(out, sa.key_rot[0][bone], 16); return; }
    if (t >= sa.key_times[n-1]) { memcpy(out, sa.key_rot[n-1][bone], 16); return; }
    for (int k = 1; k < n; k++) {
        if (t <= sa.key_times[k]) {
            float span = sa.key_times[k] - sa.key_times[k-1];
            float alpha = span > 1e-6f ? (t - sa.key_times[k-1]) / span : 0.f;
            quat_nlerp(out, sa.key_rot[k-1][bone], sa.key_rot[k][bone], alpha);
            return;
        }
    }
    memcpy(out, sa.key_rot[n-1][bone], 16);
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
void mat3_to_quat(float q[4], const float M[16]) {
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
void m4inv_rigid(float* out, const float* M) {
    for (int r=0;r<3;r++) for (int c=0;c<3;c++) out[c*4+r]=M[r*4+c];
    for (int c=0;c<4;c++) out[c*4+3]=(c==3)?1.f:0.f;
    float tx=M[12],ty=M[13],tz=M[14];
    out[12]=-(out[0]*tx+out[4]*ty+out[8]*tz);
    out[13]=-(out[1]*tx+out[5]*ty+out[9]*tz);
    out[14]=-(out[2]*tx+out[6]*ty+out[10]*tz);
    out[15]=1.f;
}



// Reset breathing animation to t=0 (natural rest) — call on RAND/RESET.
void ResetAnimPhase() { s_anim_epoch_ms = SDL_GetTicks(); }

// ── Morph target (blend shape) state ─────────────────────────────────────────

// Params saved by DrawInImGui → consumed by RenderFrame

void DumpState(FILE* f) {
    fprintf(f, "[CharPreview]\n");
    fprintf(f, "  ok=%d  ni=%d  rtt=%dx%d\n", s_ok, s_ni, s_rtt_w, s_rtt_h);
    fprintf(f, "  color_tex=%s  depth_tex=%s\n", s_color.SDLTexture() ? "ok" : "null", s_depth.SDLTexture() ? "ok" : "null");
    fprintf(f, "  yaw=%.4f  pit=%.4f  dist=%.4f\n", s_yaw, s_pit, s_dist);
    fprintf(f, "  skin=%.3f,%.3f,%.3f  str=%.3f\n", s_skin[0], s_skin[1], s_skin[2], s_str);
    fprintf(f, "  sat=%.3f  bri=%.3f  muscle=%.3f\n", s_sat, s_bri, s_muscle);
    fprintf(f, "  hair=%.3f,%.3f,%.3f\n", s_hair[0], s_hair[1], s_hair[2]);
    fprintf(f, "  height=%.3f  bulk=%.3f\n\n", s_height, s_bulk);
}

// ── Mat4 (column-major) ───────────────────────────────────────────────────────
M4 m4_mul(const M4& a, const M4& b) {
    M4 c; memset(c.m,0,64);
    for(int i=0;i<4;i++) for(int j=0;j<4;j++) for(int k=0;k<4;k++)
        c.m[i*4+j] += a.m[k*4+j]*b.m[i*4+k];
    return c;
}
M4 m4_persp(float fov, float asp, float n, float f) {
    M4 r; memset(r.m,0,64);
    float t=1.f/tanf(fov*0.5f);
    r.m[0]=t/asp; r.m[5]=t;
    r.m[10]=(f+n)/(n-f); r.m[11]=-1.f; r.m[14]=(2.f*f*n)/(n-f);
    return r;
}
M4 m4_rotY(float a) { M4 r; r.m[0]=cosf(a);r.m[2]=sinf(a);r.m[8]=-sinf(a);r.m[10]=cosf(a); return r; }
M4 m4_rotX(float a) { M4 r; r.m[5]=cosf(a);r.m[6]=-sinf(a);r.m[9]=sinf(a);r.m[10]=cosf(a); return r; }
M4 m4_translate(float x, float y, float z) { M4 r; r.m[12]=x; r.m[13]=y; r.m[14]=z; return r; }

// Effective pose (filled by SetBoneScalesFromDef each frame, read by debug overlay)

// ── RTT management ────────────────────────────────────────────────────────────

}  // namespace CharPreviewSDLGPU
#endif // MD_SDL_GPU
