#include "editor_char_preview_sdlgpu_internal.h"
#ifdef MD_SDL_GPU

bool Init(const char* glb_path, const char* tex_path) {
    s_sex = (glb_path && strstr(glb_path, "female")) ? 1 : 0;
    if (!s_load_mesh(glb_path))         return false;
    s_load_textures(tex_path);
    if (!s_create_pipelines(glb_path))  return false;

    // Load default hair style (hair01)
    LoadHairStyle(0);

    // Load default clothes (Slave Shirt + Cargo Pants)
    SetClothingItem(1);   // slot0: Slave Shirt
    SetClothingItem(9);   // slot1: Cargo Pants

    // Load hair shading params from file (falls back to defaults if missing)
    HairShading::Load("game/data/chars/hair_shading.txt");

    s_ok=true;
    return true;
}

// ── RenderFrame: render T-pose to RTT (call before ImGui render) ──────────────
void RenderFrame(md::GpuCommandBufferHandle cmd) {
    if (!s_ok||!s_color.SDLTexture()||s_rtt_w<4||s_rtt_h<4) return;

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
        md::GpuDeviceHandle dev = md::GpuDevice::Get().SDLDevice();
        uint32_t up_sz = (uint32_t)(vc * sizeof(Vtx));
        SDL_GPUTransferBufferCreateInfo mtb={};
        mtb.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; mtb.size=up_sz;
        SDL_GPUTransferBuffer* mtr=GpuCreateTransferBuffer(dev,&mtb);
        if (mtr) {
            void* mp=GpuMapTransfer(mtr,false);
            if (mp){memcpy(mp,s_mbuf,up_sz);GpuUnmapTransfer(mtr);}
            GpuCopyPass cp;
            cp.Begin(cmd);
            SDL_GPUTransferBufferLocation msrc={mtr,0};
            SDL_GPUBufferRegion mdst={s_vbo.SDLBuffer(),0,up_sz};
            cp.UploadBuffer(msrc,mdst,false);
            cp.End();
            GpuReleaseTransferBuffer(dev,mtr);
        }
        s_morphs_dirty = false;
    }

    // Upload bone world-scale matrices (120×1 texture) via copy pass
    if (s_bones_tex) {
        md::GpuDeviceHandle dev=md::GpuDevice::Get().SDLDevice();
        uint32_t up_sz=120*4*4;
        SDL_GPUTransferBufferCreateInfo tb={};
        tb.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tb.size=up_sz;
        SDL_GPUTransferBuffer* tr=GpuCreateTransferBuffer(dev,&tb);
        void* mp=GpuMapTransfer(tr,false);
        if(mp){memcpy(mp,s_ws_mat,up_sz);GpuUnmapTransfer(tr);}
        GpuCopyPass cp;
        cp.Begin(cmd);
        SDL_GPUTextureTransferInfo src={tr,0,(uint32_t)120,(uint32_t)1};
        SDL_GPUTextureRegion dst={s_bones_tex,0,0,0,0,0,120,1,1};
        cp.UploadTexture(src,dst,false);
        cp.End();
        GpuReleaseTransferBuffer(dev,tr);
    }

    // Hierarchical bone propagation (sl[12] *= parent[0]) already moves origins —
    // per-bone H in setSpine([0]=H) IS setMovementScale. No separate model Y-scale.
    // Translate: put feet (bind-pose Y ≈ -0.95) at ground (Y=0); offset scales with H.
    M4 model = m4_translate(0.f, -s_height*0.95f, 0.f);

    // Foot grounding: for non-default leg_Y the calf/foot bind-pose X-translation
    // is scaled by leg_Y → feet drift above platform top. Shift character-only downward.
    // foot_adj = (default_leg_Y - actual_leg_Y) * (calf_bind_X + foot_bind_X)
    //          = (0.95 - s_leg_y) * (0.439 + 0.461) = (0.95 - s_leg_y) * 0.9
    float foot_adj = (0.95f - s_leg_y) * 0.9f;
    M4 char_model  = m4_translate(0.f, -(s_height*0.95f + foot_adj), 0.f);

    // Orbit view + perspective
    M4 view = m4_mul(m4_translate(0.f, -s_lookat_y, -s_dist), m4_mul(m4_rotX(s_pit), m4_rotY(s_yaw)));
    float asp=(float)s_rtt_w/(float)s_rtt_h;
    M4 proj = m4_persp(0.78f, asp, 0.05f, 10.f);
    M4 mvp      = m4_mul(proj, m4_mul(view, model));       // scene geometry (platform, pole)
    M4 char_mvp = m4_mul(proj, m4_mul(view, char_model));  // character + hair

    // Eye position in MODEL space for hair shader (V = normalize(eye_model - bone_pos)).
    // eye_world = inv(view) translation column.
    // eye_model = inv(model) * eye_world = eye_world + (0, +height*0.95, 0)
    // because model = translate(0, -height*0.95, 0) so inv(model) = translate(0, +height*0.95, 0).
    // This avoids passing a second mat4 in the vertex UB (would hit 128-byte push-constant limit).
    {
        float inv_v[16]; m4inv_rigid(inv_v, view.m);
        HairShading::g_params.eye_pos[0] = inv_v[12];
        HairShading::g_params.eye_pos[1] = inv_v[13] + s_height * 0.95f + foot_adj;
        HairShading::g_params.eye_pos[2] = inv_v[14];
        HairShading::g_params.eye_pos[3] = 0.f;
    }

    // Render pass on RTT
    GpuCommandBuffer cb;
    GpuCommandBuffer::ColorPassDesc cpd;
    cpd.cmd            = cmd;
    cpd.color_tex[0]      = s_color.SDLTexture();
    cpd.depth_tex      = s_depth.SDLTexture();
    cpd.clear_color[0] = 0.f; cpd.clear_color[1] = 0.f;
    cpd.clear_color[2] = 0.f; cpd.clear_color[3] = 1.f; // bg pipeline overwrites this
    cpd.clear_depth    = 1.f;
    cpd.load_color     = false; // CLEAR
    cpd.load_depth     = false; // CLEAR
    cb.BeginColorPass(cpd);
    SDL_GPURenderPass* rp = cb.SDLPass();
    if (!rp) return;

    GpuPassView pv = GpuPassView::FromRaw(rp, cmd);

    // ── Background: sky + perspective ground plane ───────────────────────────
    if (s_bg_pipeline.SDLPipeline()) {
        pv.BindPipeline(&s_bg_pipeline);

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
        GpuPushFragmentUniforms(cmd, 0, &bgu, sizeof(bgu));
        // Bind desert ground textures (set=2, bindings 0 and 1)
        if (s_bg_sand.SDLTexture() && s_bg_dune.SDLTexture()) {
            SDL_GPUTextureSamplerBinding bg_tex[2] = {
                {s_bg_sand.SDLTexture(), s_bg_sand.SDLSampler()},
                {s_bg_dune.SDLTexture(), s_bg_dune.SDLSampler()}
            };
            pv.BindFragmentSamplers(0, bg_tex, 2);
        }
        pv.Draw(3, 1, 0, 0);
    }

    // ── Scene geometry: anthropometer pole ──────────────────────────────────────
    if (s_scene_pipeline.SDLPipeline() && s_scene_vbo.SDLBuffer() && s_scene_ni > 0) {
        pv.BindPipeline(&s_scene_pipeline);
        pv.BindVertexBuffer(&s_scene_vbo);
        pv.BindIndexBuffer(&s_scene_ibo, SDL_GPU_INDEXELEMENTSIZE_32BIT);
        VU svu; memcpy(svu.mvp, mvp.m, 64);
        pv.PushVertexUniforms(0, &svu, sizeof(svu));
        pv.DrawIndexed(s_scene_ni, 1, 0, 0, 0);
    }

    if (!s_pipeline.SDLPipeline() || !s_vbo.SDLBuffer() || !s_ibo.SDLBuffer() ||
        !s_tex.SDLTexture()        || !s_tex.SDLSampler()        ||
        !s_tex_head.SDLTexture()   || !s_tex_head.SDLSampler()   ||
        !s_tex_muscle.SDLTexture() || !s_tex_muscle.SDLSampler() ||
        !s_tex_blood.SDLTexture()  || !s_tex_blood.SDLSampler()) {
        cb.EndPass(); return;
    }

    pv.BindPipeline(&s_pipeline);
    pv.BindVertexBuffer(&s_vbo);
    pv.BindIndexBuffer(&s_ibo, SDL_GPU_INDEXELEMENTSIZE_32BIT);

    VU vu; memcpy(vu.mvp, char_mvp.m, 64);
    pv.PushVertexUniforms(0, &vu, sizeof(vu));
    pv.PushVertexUniforms(1, s_ws_mat, sizeof(s_ws_mat));

    FU fu{};
    fu.skin[0]=s_skin[0]; fu.skin[1]=s_skin[1]; fu.skin[2]=s_skin[2];
    fu.str=s_str; fu.sat=s_sat; fu.bri=s_bri; fu.muscle=s_muscle;
    fu.hair[0]=s_hair[0]; fu.hair[1]=s_hair[1]; fu.hair[2]=s_hair[2];
    pv.PushFragmentUniforms(0, &fu, sizeof(fu));

    SDL_GPUTextureSamplerBinding ftb[4] = {
        { s_tex.SDLTexture(),        s_tex.SDLSampler()        },
        { s_tex_head.SDLTexture(),   s_tex_head.SDLSampler()   },
        { s_tex_muscle.SDLTexture(), s_tex_muscle.SDLSampler() },
        { s_tex_blood.SDLTexture(),  s_tex_blood.SDLSampler()  },
    };
    pv.BindFragmentSamplers(0, ftb, 4);

    pv.DrawIndexed((uint32_t)s_ni, 1, 0, 0, 0);

    // ── Clothing render (after body, before hair) ─────────────────────────────
    if (s_clothes_visible && s_cloth_pipeline.SDLPipeline()) {
        for (int sl = 0; sl < 3; ++sl) {
            if (!s_cloth[sl].loaded || s_cloth[sl].ni == 0) continue;
            pv.BindPipeline(&s_cloth_pipeline);
            pv.BindVertexBuffer(&s_cloth[sl].vbo);
            pv.BindIndexBuffer(&s_cloth[sl].ibo, SDL_GPU_INDEXELEMENTSIZE_16BIT);
            VU cvu; memcpy(cvu.mvp, char_mvp.m, 64);
            pv.PushVertexUniforms(0, &cvu, sizeof(cvu));
            pv.PushVertexUniforms(1, s_ws_mat, sizeof(s_ws_mat));
            ClothFU cfu;
            cfu.color[0]=s_cloth_color[sl][0]; cfu.color[1]=s_cloth_color[sl][1];
            cfu.color[2]=s_cloth_color[sl][2]; cfu.pad=0;
            pv.PushFragmentUniforms(0, &cfu, sizeof(cfu));
            pv.DrawIndexed((uint32_t)s_cloth[sl].ni, 1, 0, 0, 0);
        }
    }

    // ── Hair render (same render pass, after character) ───────────────────────
    if (s_hair_pipeline.SDLPipeline() && s_hair_vbo.SDLBuffer() &&
        s_hair_ibo.SDLBuffer() && s_hair_ni > 0) {
        pv.BindPipeline(&s_hair_pipeline);
        // Vertex: bone matrices texture (same as character)
        SDL_GPUTextureSamplerBinding hvtb = { s_bones_tex, s_bones_sampler };
        pv.BindVertexSamplers(0, &hvtb, 1);
        // Vertex uniform: MVP only (64 bytes — safe on Intel HD 520).
        // eye_pos is passed in model space via frag UB so no second matrix needed.
        VU hvu; memcpy(hvu.mvp, char_mvp.m, 64);
        pv.PushVertexUniforms(0, &hvu, sizeof(hvu));
        // Fragment uniform slot 0: hair color
        HairFU hfu; hfu.hair[0]=s_hair[0]; hfu.hair[1]=s_hair[1]; hfu.hair[2]=s_hair[2]; hfu.pad=0;
        pv.PushFragmentUniforms(0, &hfu, sizeof(hfu));
        // Fragment uniform slot 1: hair shading params (eye_pos updated each frame above)
        pv.PushFragmentUniforms(1, &HairShading::g_params, sizeof(HairShadingFU));
        // Geometry
        pv.BindVertexBuffer(&s_hair_vbo);
        pv.BindIndexBuffer(&s_hair_ibo, SDL_GPU_INDEXELEMENTSIZE_16BIT);
        pv.DrawIndexed((uint32_t)s_hair_ni, 1, 0, 0, 0);
    }

    cb.EndPass();
}

// ── Hot-reload: recreate all char-preview pipelines from current SPV files ───
void ReloadPipelines() {
    s_bg_pipeline.Reload();
    s_scene_pipeline.Reload();
    s_cloth_pipeline.Reload();
    s_pipeline.Reload();
    s_hair_pipeline.Reload();
    fprintf(stdout, "[CharPreview] Pipelines reloaded\n");
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
void SetBoneScalesFromDef(const float body[18], const float face[24]) {
    if (s_pose_idle_clip < 0 || !s_pose_mesh.loaded) return;

    CharScales scales;
    CharCustomization_ComputeScales(body, face, s_pose_mesh.bone_count, scales);

    // Apply slider animation as a full quaternion delta composed onto qrot_delta[bone].
    // Delta = anim_at_neutral^{-1} * anim_at_current (identity at neutral → no change).
    // Multiple animations compose: subsequent deltas are post-multiplied onto existing ones.
    auto apply_slider = [&](const SliderAnim& sa, float slider_val, float neutral_val) {
        if (!sa.loaded || sa.key_count == 0) return;
        float pn = sa.length * neutral_val * 0.01f;
        float pt = sa.length * slider_val  * 0.01f;
        for (int bone = 0; bone < 30; bone++) {
            if (!sa.has[bone]) continue;
            float q_n[4], q_p[4];
            SampleAnimAtTime(sa, bone, pn, q_n);
            SampleAnimAtTime(sa, bone, pt, q_p);
            float qn_inv[4] = {-q_n[0], -q_n[1], -q_n[2], q_n[3]};
            float q_d[4]; quat_mul(q_d, qn_inv, q_p);
            // Compose onto existing delta (identity * q_d = q_d on first write).
            float* qc = scales.qrot_delta[bone];
            float q0 = qc[3]*q_d[0] + qc[0]*q_d[3] + qc[1]*q_d[2] - qc[2]*q_d[1];
            float q1 = qc[3]*q_d[1] - qc[0]*q_d[2] + qc[1]*q_d[3] + qc[2]*q_d[0];
            float q2 = qc[3]*q_d[2] + qc[0]*q_d[1] - qc[1]*q_d[0] + qc[2]*q_d[3];
            float q3 = qc[3]*q_d[3] - qc[0]*q_d[0] - qc[1]*q_d[1] - qc[2]*q_d[2];
            qc[0]=q0; qc[1]=q1; qc[2]=q2; qc[3]=q3;
            scales.rot[bone] = 0.f;
        }
    };
    apply_slider(s_anim_postures,     body[4], 35.f);  // kBodyDef[4]
    apply_slider(s_anim_neck_set,     body[5], 68.f);  // kBodyDef[5]
    apply_slider(s_anim_shoulder_set, body[6], 53.f);  // kBodyDef[6]

    // s_leg_y for foot grounding
    {
        auto cl4 = [](float x) { return x<0.1f?0.1f:(x>4.f?4.f:x); };
        s_leg_y = cl4(cl4(body[2]/100.f) + cl4(body[7]/100.f) - 1.f) * 0.95f;
    }

    // GetFinalBonesScaled writes MAX_SKIN_BONES entries — use flat buffer to avoid overflow.
    static float ws_flat[MAX_SKIN_BONES * 16];
    s_pose_mesh.GetFinalBonesScaled(s_pose_idle_clip, 0.f, scales, ws_flat);
    for (int i = 0; i < 30; ++i)
        memcpy(s_ws_mat[i], ws_flat + i * 16, 64);
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
void SetBodyMorphWeights(const float body[18], const float face[24]) {
    auto set = [](const char* n, float w) {
        int mi=s_morph_idx_by_name(n);
        if(mi>=0) s_morph_weights[mi]=w<0.f?0.f:(w>1.f?1.f:w);
    };
    // pd: positive deviation above neutral (0 at neutral, 1 at neutral+range)
    auto pd = [](float v, float neu, float rng) -> float {
        float d=(v-neu)/rng; return d<0.f?0.f:(d>1.f?1.f:d);
    };
    // nd: negative deviation below neutral (0 at neutral, 1 at neutral-range)
    auto nd = [](float v, float neu, float rng) -> float {
        float d=(neu-v)/rng; return d<0.f?0.f:(d>1.f?1.f:d);
    };

    // ── existing 6 morphs ────────────────────────────────────────────────────
    set("fat",      (pd(body[13],100.f,90.f)*0.65f
                   + pd(body[15],100.f,45.f)*0.20f
                   + pd(body[12],100.f,40.f)*0.15f) * 0.3f);

    set("muscular", (pd(body[9], 100.f,45.f)*0.65f
                   + pd(body[8], 100.f,10.f)*0.15f
                   + pd(body[12],100.f,40.f)*0.20f) * 0.3f);

    set("longlegs", pd(body[7], 100.f,15.f) * 0.3f);
    set("bighead",  pd(face[0], 100.f,10.f) * 0.3f);

    set("broadshdr",(pd(body[3], 100.f,20.f)*0.55f
                   + pd(body[8], 100.f,10.f)*0.45f) * 0.6f);

    set("tall",     pd(body[2], 100.f,20.f) * 0.15f);

    // ── нові 4 морфи ─────────────────────────────────────────────────────────
    // female_body: широкі стегна + вузькі плечі → hourglass силует
    // body[15]=Hips(75-145), body[8]=Shoulders(90-110)
    set("female_body", (pd(body[15],100.f,45.f)*0.6f
                      + nd(body[8], 100.f,10.f)*0.4f) * 0.55f);

    // wide_hips: прямо від Hips слайдера (доповнює female_body)
    set("wide_hips",   pd(body[15],100.f,45.f) * 0.4f);

    // stocky: активний тільки коли Frame HIGH і Height LOW разом
    // body[3]=Frame(80-120), body[2]=Height(80-120)
    set("stocky",      pd(body[3],100.f,20.f) * nd(body[2],100.f,20.f) * 1.2f);

    // narrow_torso: активний коли Frame LOW (lean тулуб)
    set("narrow_torso",nd(body[3],100.f,20.f) * 0.4f);

    s_morphs_dirty = true;
}

// Call once per frame with current face[], def[], lo[], hi[] arrays.
void SetMorphWeightsFromFace(const float face[], const float def[],
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
            if (!strcmp(key,"body_lookat_y"))     s_pcfg.body_lookat_y     = val;
        }
    }
    fclose(f);
    s_pcfg_loaded = true;
    fprintf(stdout,"[CharPreview] portrait.cfg: dist=%.2f offset_y=%.2f\n",
            s_pcfg.portrait_dist, s_pcfg.portrait_offset_y);
}

// ── Camera preset per active tab ──────────────────────────────────────────────
void SetCameraForTab(int tab) {
    if (!s_pcfg_loaded) LoadPortraitCfg();
    if (tab == 0) {
        s_dist     = s_pcfg.body_dist;
        s_pit      = s_pcfg.body_pit;
        s_lookat_y = s_pcfg.body_lookat_y;
    } else {
        s_dist     = s_pcfg.portrait_dist;
        s_pit      = 0.f;
        s_yaw      = 0.f;
        s_lookat_y = s_height * s_pcfg.portrait_offset_y;
    }
}

// ── DrawInImGui: orbit input + show RTT ──────────────────────────────────────
void DrawInImGui(float W, float H,
                        float height_scale, float bulk_scale,
                        const float skin_rgb[3], float skin_str,
                        float sat, float bri,
                        float muscle,
                        const float hair_rgb[3])
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
    bool dragging = hov && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.f);
    // Auto-rotation runs only while portrait mode is active AND user hasn't taken manual control.
    // First RMB drag disables portrait mode so the user can hold a custom view for screenshots.
    if (dragging) s_portrait_mode = false;
    if (s_portrait_mode) {
        uint64_t ms = SDL_GetTicks() - s_anim_epoch_ms;
        uint32_t fr = (uint32_t)(ms / 33);
        s_yaw = ((float)(fr % 500) / 1000.f - 0.25f) * 3.14159f * 0.6f;
        s_pit = ((float)(fr % 252) / 1000.f - 0.083f) * 3.14159f * 0.25f;
    }

    // RMB drag — negate both deltas to match standard orbit-camera convention.
    if (dragging) {
        s_yaw -= io.MouseDelta.x * 0.007f;
        s_pit -= io.MouseDelta.y * 0.005f;
        s_pit = fmaxf(-1.4f, fminf(1.4f, s_pit));
    }
    // Scroll = zoom
    if (hov && io.MouseWheel!=0.f) {
        s_dist-=io.MouseWheel*0.18f;
        if (s_dist<0.5f) s_dist=0.5f;
        if (s_dist>6.0f) s_dist=6.0f;
    }

    // Display RTT (UV Y not flipped — SDL_GPU origin is top-left)
    if (s_color.SDLTexture())
        ImGui::GetWindowDrawList()->AddImage(
            (ImTextureID)s_color.SDLTexture(), origin, {origin.x+W,origin.y+H});
    else
        ImGui::GetWindowDrawList()->AddRectFilled(origin,{origin.x+W,origin.y+H},
            IM_COL32(20,20,28,255));

    if (hov)
        ImGui::GetWindowDrawList()->AddRect(origin,{origin.x+W,origin.y+H},
            IM_COL32(80,120,200,120),2.f);

    // ── GPU debug overlay — toggle with Ctrl+D ────────────────────────────────
    // Shows real-time uniform values and pipeline state. Helps diagnose shader bugs
    // without RenderDoc: if hair_color shows white here → HairFU binding is wrong.
    {
        static bool s_show_debug = false;
        if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) s_show_debug = !s_show_debug;
        if (s_show_debug) {
            ImDrawList* dl = ImGui::GetWindowDrawList();
            ImVec2 p = {origin.x + 4.f, origin.y + 4.f};
            auto txt = [&](const char* t, ImU32 col = IM_COL32(255,255,100,230)) {
                dl->AddText(p, IM_COL32(0,0,0,200), t);  // shadow
                p.y -= 1.f; auto pp = ImVec2{p.x-1,p.y};
                dl->AddText(pp, col, t);
                p.y += 14.f;
            };
            char buf[128];
            txt("[Ctrl+D] GPU Debug");
            snprintf(buf,sizeof(buf),"hair_color: %.2f %.2f %.2f",s_hair[0],s_hair[1],s_hair[2]);
            bool bad_hair = (s_hair[0]>0.9f && s_hair[1]>0.9f && s_hair[2]>0.9f);
            txt(buf, bad_hair ? IM_COL32(255,80,80,230) : IM_COL32(100,255,100,230));
            snprintf(buf,sizeof(buf),"eye_model: %.2f %.2f %.2f",
                HairShading::g_params.eye_pos[0],
                HairShading::g_params.eye_pos[1],
                HairShading::g_params.eye_pos[2]);
            txt(buf);
            snprintf(buf,sizeof(buf),"pipeline: %s  hair_pipe: %s",
                s_pipeline.SDLPipeline() ? "OK" : "FAIL",
                s_hair_pipeline.SDLPipeline() ? "OK" : "FAIL");
            bool bad_pipe = !s_pipeline.SDLPipeline() || !s_hair_pipeline.SDLPipeline();
            txt(buf, bad_pipe ? IM_COL32(255,80,80,230) : IM_COL32(100,255,100,230));
            snprintf(buf,sizeof(buf),"bone21 diag: %.2f  hair_ni: %d",
                s_ws_mat[21][0], s_hair_ni);
            txt(buf);
            snprintf(buf,sizeof(buf),"dist:%.2f yaw:%.1f pit:%.1f",
                s_dist, s_yaw*57.3f, s_pit*57.3f);
            txt(buf);
        }
    }

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

#ifdef MONKEY_DUST_EDITOR
    // ── Bone debug overlay ────────────────────────────────────────────────────
    // Shows key bone angles, bind vs current, and data source.
    // Toggle with static bool; ImGui collapsing header.
    static bool s_dbg_bones = false;
    ImGui::SetCursorScreenPos({origin.x+4, origin.y+4});
    ImGui::PushStyleColor(ImGuiCol_Header,        IM_COL32(30,30,50,200));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, IM_COL32(50,50,80,200));
    ImGui::PushStyleColor(ImGuiCol_Text,          IM_COL32(200,220,255,255));
    if (ImGui::CollapsingHeader("Bone Debug##cpv")) {
        s_dbg_bones = true;
        static const struct { int ji; const char* name; } kDBG[] = {
            {0,"ROOT"},{1,"Pelvis"},
            {12,"Spine"},{13,"Spine1"},{14,"Spine2"},
            {15,"L Clav"},{16,"L Arm"},{20,"Neck"},{21,"Head"},
            {25,"R Clav"},{26,"R Arm"},
            {2,"L Thigh"},{3,"L Calf"},
        };
        auto q2deg=[](const float q[4]) -> float {
            float w=q[3]<-1.f?-1.f:(q[3]>1.f?1.f:q[3]);
            return 2.f*acosf(w)*57.2957795f;
        };
        auto bind_q=[](int i, float q[4]){
            // extract quaternion from bind_local col-major mat4
            float m[16]; memcpy(m, s_bind_local[i], 64);
            float t=m[0]+m[5]+m[10];
            if(t>0.f){float s=0.5f/sqrtf(t+1.f);
                q[3]=0.25f/s;q[0]=(m[6]-m[9])*s;q[1]=(m[8]-m[2])*s;q[2]=(m[1]-m[4])*s;
            } else if(m[0]>m[5]&&m[0]>m[10]){float s=2.f*sqrtf(1.f+m[0]-m[5]-m[10]);
                q[3]=(m[6]-m[9])/s;q[0]=0.25f*s;q[1]=(m[4]+m[1])/s;q[2]=(m[8]+m[2])/s;
            } else if(m[5]>m[10]){float s=2.f*sqrtf(1.f+m[5]-m[0]-m[10]);
                q[3]=(m[8]-m[2])/s;q[0]=(m[4]+m[1])/s;q[1]=0.25f*s;q[2]=(m[9]+m[6])/s;
            } else{float s=2.f*sqrtf(1.f+m[10]-m[0]-m[5]);
                q[3]=(m[1]-m[4])/s;q[0]=(m[8]+m[2])/s;q[1]=(m[9]+m[6])/s;q[2]=0.25f*s;}
        };
        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255,255,255,255));
        ImGui::TextUnformatted("bone           cur°  bind°  diff°");
        ImGui::Separator();
        for (auto& d : kDBG) {
            float bq[4]; bind_q(d.ji, bq);
            float cur = q2deg(s_pose_rot[d.ji]);
            float bnd = q2deg(bq);
            float dif = cur - bnd;
            ImU32 col = fabsf(dif) < 1.f ? IM_COL32(150,255,150,255) :
                        fabsf(dif) < 5.f ? IM_COL32(255,255,100,255) :
                                           IM_COL32(255,100,100,255);
            ImGui::PushStyleColor(ImGuiCol_Text, col);
            ImGui::Text("%-12s %6.1f %6.1f %+6.1f", d.name, cur, bnd, dif);
            ImGui::PopStyleColor();
        }
        ImGui::Separator();
        // Source info
        bool postures_on = s_anim_postures.loaded;
        bool idle_on     = s_idle_loaded;
        bool breath_on   = s_breath_loaded;
        ImGui::Text("idle:%s breath:%s postures:%s",
            idle_on?"OK":"--", breath_on?"OK":"--", postures_on?"OK":"--");
        ImGui::PopStyleColor();
    }
    ImGui::PopStyleColor(3);
#endif
}

} // namespace CharPreviewSDLGPU

#endif // MD_SDL_GPU
