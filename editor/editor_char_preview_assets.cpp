#include "editor_char_preview_sdlgpu_internal.h"
#ifdef MD_SDL_GPU
#include <monkey_dust/render/gpu_hal.h>

// Public API state (extern-declared in editor_char_preview_sdlgpu.h, accessed
// directly by character_editor.h) -- definitions live here, hair/clothing-
// asset-loading related.
int             s_hair_style   = 0;     // index into s_hair_styles
const char* const s_hair_styles[30] = {
    "hair01","hair02","hair03","hair04","hair05","hair06","hair07","hair08",
    "haircut_male01","haircut_male02","haircut_male03","haircut_male04","haircut_male05",
    "haircut_male06","haircut_male07","haircut_male08","haircut_male09","haircut_male10",
    "haircut_female01","haircut_female02","haircut_female03","haircut_female04",
    "haircut_female05","haircut_female06","haircut_female07","haircut_female08",
    "hairlongbald","frizzhair","nutfro","afrohair",
};
bool  s_clothes_visible          = true;
int   s_cloth_sel[3]             = {0, 7, 0};   // Cargo Pants default
float s_cloth_color[3][3]        = { {0.62f,0.59f,0.53f}, {0.28f,0.22f,0.14f}, {0.3f,0.3f,0.3f} };

void ensure_rtt(int w, int h) {
    if (w==s_rtt_w && h==s_rtt_h && s_color.SDLTexture()) return;
    s_color.Shutdown();
    s_depth.Shutdown();
    s_rtt_w=w; s_rtt_h=h;
    s_color.Init(w, h, SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM,
                 SDL_GPU_TEXTUREUSAGE_COLOR_TARGET|SDL_GPU_TEXTUREUSAGE_SAMPLER);
    s_depth.Init(w, h, SDL_GPU_TEXTUREFORMAT_D32_FLOAT,
                 SDL_GPU_TEXTUREUSAGE_DEPTH_STENCIL_TARGET);
}

// ── LoadHairStyle: load a hair GLB as pos+norm static mesh ───────────────────
bool LoadHairStyle(int style_idx) {
    if (style_idx < 0 || style_idx >= s_hair_style_count) return false;
    char path[256];
    snprintf(path, sizeof(path), "game/data/hair/%s.glb", s_hair_styles[style_idx]);

    cgltf_options opt{}; cgltf_data* d = nullptr;
    if (cgltf_parse_file(&opt, path, &d) != cgltf_result_success) {
        fprintf(stderr, "[Hair] missing: %s\n", path); return false;
    }
    cgltf_load_buffers(&opt, d, path);

    auto& pr = d->meshes[0].primitives[0];
    cgltf_accessor* ap = nullptr;
    cgltf_accessor* an = nullptr;
    for (int i=0; i<(int)pr.attributes_count; ++i) {
        if (pr.attributes[i].type == cgltf_attribute_type_position) ap = pr.attributes[i].data;
        if (pr.attributes[i].type == cgltf_attribute_type_normal)   an = pr.attributes[i].data;
    }
    if (!ap || !pr.indices) { cgltf_free(d); return false; }

    int nv = (int)ap->count;
    struct HV { float px,py,pz, nx,ny,nz; };
    std::vector<HV> verts(nv);
    for (int i=0; i<nv; ++i) {
        float p[3]={0,0,0}, n[3]={0,1,0};
        cgltf_accessor_read_float(ap, i, p, 3);
        if (an) cgltf_accessor_read_float(an, i, n, 3);
        verts[i] = {p[0],p[1],p[2], n[0],n[1],n[2]};
    }
    int ni = (int)pr.indices->count;
    std::vector<uint16_t> idx(ni);
    for (int i=0; i<ni; ++i)
        idx[i] = (uint16_t)cgltf_accessor_read_index(pr.indices, i);
    cgltf_free(d);

    s_hair_vbo.Init(0x8892/*GL_ARRAY_BUFFER*/,   verts.data(), (uint32_t)(nv * sizeof(HV)));
    s_hair_ibo.Init(0x8893/*GL_ELEMENT_ARRAY_BUFFER*/, idx.data(), (uint32_t)(ni * 2));
    s_hair_ni = ni;
    s_hair_style = style_idx;
    fprintf(stdout, "[Hair] loaded %s (%dv %di)\n", s_hair_styles[style_idx], nv, ni/3);
    return true;
}

// ── LoadClothingSlot: read .clothbin (magic 'COLT' + nv + ni + flags + verts + indices) ──
static bool LoadClothingSlot(int slot, const char* path) {
    if (slot < 0 || slot >= 3) return false;
    s_cloth[slot].vbo.Shutdown(); s_cloth[slot].ibo.Shutdown();
    s_cloth[slot].ni = 0; s_cloth[slot].loaded = false;
    if (!path) return true;   // "None" — clear slot
    FILE* fp = fopen(path, "rb");
    if (!fp) { fprintf(stderr, "[Cloth] missing: %s\n", path); return false; }
    uint32_t hdr[4]; fread(hdr, 4, 4, fp);
    if (hdr[0] != 0x544F4C43u) { fclose(fp); fprintf(stderr, "[Cloth] bad magic\n"); return false; }
    uint32_t nv = hdr[1], ni = hdr[2], flags = hdr[3];
    bool u32 = (flags & 1) != 0;
    static char vbuf[20000*52];
    if (nv > 20000) { fclose(fp); return false; }
    fread(vbuf, 52, nv, fp);
    s_cloth[slot].vbo.Shutdown();
    s_cloth[slot].vbo.Init(0x8892u, vbuf, nv * 52);
    uint32_t ibytes = ni * (u32 ? 4 : 2);
    static char ibuf[65536*2];
    if (ibytes > sizeof(ibuf)) { fclose(fp); return false; }
    fread(ibuf, 1, ibytes, fp);
    fclose(fp);
    s_cloth[slot].ibo.Shutdown();
    s_cloth[slot].ibo.Init(0x8893u, ibuf, ibytes);
    s_cloth[slot].ni = (int)ni;
    s_cloth[slot].loaded = true;
    fprintf(stdout, "[Cloth] slot%d: %s (%uv %ut)\n", slot, path, nv, ni/3);
    return true;
}

// ── SetClothingItem: select clothing item by index in s_cloth_items ───────────
void SetClothingItem(int item_idx) {
    constexpr int kN = (int)(sizeof(s_cloth_items)/sizeof(s_cloth_items[0]));
    if (item_idx < 0 || item_idx >= kN) return;
    const ClothItemDef& d = s_cloth_items[item_idx];
    int slot = d.slot;
    s_cloth_sel[slot]     = item_idx;
    s_cloth_color[slot][0] = d.color[0];
    s_cloth_color[slot][1] = d.color[1];
    s_cloth_color[slot][2] = d.color[2];
    const char* path = (s_sex == 1 && d.path_f) ? d.path_f : d.path_m;
    LoadClothingSlot(slot, path);
}

// ── s_load_mesh: parse GLB, extract geometry, morph targets, skeleton, animations ─
// Returns true on success. Fills s_base_verts_cpu, s_base_vc, s_morph_*, s_inv_bind,
// s_bind, s_bind_local, s_bone_parent, s_idle_rot/loaded, s_breath*, s_anim_* sliders,
// s_ni, s_vbo, s_ibo.
bool s_load_mesh(const char* glb_path) {
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
        // Hard-coded morph order for md_human_t.glb / md_human_female_t.glb.
        // cgltf extras.data is unreliable across platforms; order matches Python audit.
        // md_human_t.glb: 29 morphs in this exact order (verified 2026-05-30).
        static const char* kKnownMorphs[] = {
            "wide_cheekbones","narrow_cheekbones","big_mouth","wide_mouth",
            "wide_nose","long_nose","arch_nose","high_nose",
            "high_brow","low_brow","shallow_eyes","narrow_eyes","close_eyes",
            "tiltdown_eyes","tiltup_eyes","high_eyes","big_eyes",
            "tiltup_nose","tiltdown_nose","tiltdown_brow","tiltup_brow",
            "overbite","underbite",
            "tall","fat","muscular","longlegs","bighead","broadshdr"
        };
        static constexpr int kKnownN = (int)(sizeof(kKnownMorphs)/sizeof(kKnownMorphs[0]));
        int names_found = 0;
        // Try extras JSON first (works when cgltf populates it)
        const char* ej = d->meshes[0].extras.data;
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
        // Fallback: use hard-coded order if extras failed or count mismatch
        if (names_found == 0 && n_mt == kKnownN) {
            for (int i = 0; i < kKnownN; ++i)
                snprintf(s_morph_names[i], 48, "%s", kKnownMorphs[i]);
            names_found = kKnownN;
            fprintf(stdout,"[CharPreview] morph names: using hard-coded order (%d)\n", kKnownN);
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
            // Frame selection per bone:
            //   UpperArm L(ji=16) / R(ji=26): frame 1 → natural hang (matches game idle).
            //   All other bones: frame 0 → head upright, spine/legs stable.
            // shoulder_set is skipped for UpperArm in SetBoneScalesFromDef so frame 1
            // is not over-ridden by the additive blend (which would spread arms wide).
            for (int ci=0;ci<(int)anim.channels_count;++ci){
                cgltf_animation_channel& ch=anim.channels[ci];
                if (!ch.target_node||!ch.sampler) continue;
                if (ch.target_path!=cgltf_animation_path_type_rotation) continue;
                int ni=(int)(ch.target_node-d->nodes);
                if (ni<0||ni>=2048) continue;
                int ji=node_to_ji[ni];
                if (ji<0||ji>=30) continue;
                if (ch.sampler->output&&ch.sampler->output->count>0) {
                    int n_out = (int)ch.sampler->output->count;
                    int fi = ((ji==16||ji==26) && n_out>1) ? 1 : 0;
                    cgltf_accessor_read_float(ch.sampler->output, fi, s_idle_rot[ji], 4);
                    s_idle_has_rot[ji]=true;
                }
            }
            s_idle_loaded=true;
            fprintf(stdout,"[CharPreview] idle_stand_normal: f1 UpperArm L/R, f0 all other bones\n");
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
    // Normalize slider animation quaternions to same hemisphere as idle.
    // Some bones (e.g. Head, R Clavicle) store idle with negative w while slider
    // animations store positive w — same rotation, opposite sign. Any blend between
    // opposite-hemisphere quats gives wrong intermediate values.
    // Fix: for each slider anim bone, if dot(idle, rot) < 0, negate rot (same rotation, safe hemisphere).
    auto align_to_idle = [](SliderAnim& sa) {
        if (!sa.loaded) return;
        for (int i=0;i<30;i++) {
            float dot0=sa.rot0[i][0]*s_idle_rot[i][0]+sa.rot0[i][1]*s_idle_rot[i][1]+
                        sa.rot0[i][2]*s_idle_rot[i][2]+sa.rot0[i][3]*s_idle_rot[i][3];
            if (dot0<0.f){sa.rot0[i][0]*=-1;sa.rot0[i][1]*=-1;sa.rot0[i][2]*=-1;sa.rot0[i][3]*=-1;}
            float dot1=sa.rot1[i][0]*s_idle_rot[i][0]+sa.rot1[i][1]*s_idle_rot[i][1]+
                        sa.rot1[i][2]*s_idle_rot[i][2]+sa.rot1[i][3]*s_idle_rot[i][3];
            if (dot1<0.f){sa.rot1[i][0]*=-1;sa.rot1[i][1]*=-1;sa.rot1[i][2]*=-1;sa.rot1[i][3]*=-1;}
            // Also align intermediate key_rot frames used by SampleAnimAtTime.
            for (int k=0;k<sa.key_count;k++) {
                float dk=sa.key_rot[k][i][0]*s_idle_rot[i][0]+sa.key_rot[k][i][1]*s_idle_rot[i][1]+
                          sa.key_rot[k][i][2]*s_idle_rot[i][2]+sa.key_rot[k][i][3]*s_idle_rot[i][3];
                if (dk<0.f){sa.key_rot[k][i][0]*=-1;sa.key_rot[k][i][1]*=-1;sa.key_rot[k][i][2]*=-1;sa.key_rot[k][i][3]*=-1;}
            }
        }
    };
    align_to_idle(s_anim_postures);
    align_to_idle(s_anim_shoulder_set);
    align_to_idle(s_anim_neck_set);

    s_ni=(int)pr.indices->count;
    uint32_t* ib=new uint32_t[s_ni];
    for (int i=0;i<s_ni;i++) ib[i]=(uint32_t)cgltf_accessor_read_index(pr.indices,(size_t)i);
    cgltf_free(d);

    s_vbo.Init(0x8892u, s_base_verts_cpu, (uint32_t)(vc*sizeof(Vtx)));
    s_ibo.Init(0x8893u, ib, (uint32_t)(s_ni*sizeof(uint32_t)));
    delete[] ib;  // vb is owned by s_base_verts_cpu — do NOT delete here
    return true;
}

// ── s_load_textures: upload body/head/muscle/blood textures + bone matrix texture ─
void s_load_textures(const char* tex_path) {
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
        s_bones_tex=GpuCreateTexture(dev,&ti);
        if (!s_bones_tex) {
            fprintf(stderr,"[CharPreview] bone mat tex create failed: %s\n",SDL_GetError());
        }
        SDL_GPUSamplerCreateInfo si={};
        si.min_filter=SDL_GPU_FILTER_NEAREST; si.mag_filter=SDL_GPU_FILTER_NEAREST;
        si.mipmap_mode=SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        si.address_mode_u=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_v=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        si.address_mode_w=SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        s_bones_sampler=GpuCreateSampler(dev,&si);
        if (s_bones_tex) {
            uint32_t up_sz=120*4*4; // 120 texels × 4 channels × 4 bytes
            SDL_GPUTransferBufferCreateInfo tb={};
            tb.usage=SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD; tb.size=up_sz;
            SDL_GPUTransferBuffer* tr=GpuCreateTransferBuffer(dev,&tb);
            if (tr) {
                void* mp=GpuMapTransfer(tr,false);
                if(mp){memcpy(mp,s_ws_mat,up_sz);GpuUnmapTransfer(tr);}
                SDL_GPUCommandBuffer* uc=md::GpuDevice::Get().AcquireCommandBuffer();
                if (uc) {
                    GpuCopyPass cp;
                    cp.Begin(uc);
                    SDL_GPUTextureTransferInfo src={tr,0,(uint32_t)120,(uint32_t)1};
                    SDL_GPUTextureRegion dst={s_bones_tex,0,0,0,0,0,120,1,1};
                    cp.UploadTexture(src,dst,false);
                    cp.End();
                    md::GpuDevice::Get().Submit(uc);
                }
                GpuReleaseTransferBuffer(dev,tr);
            }
        }
    }
}

// ── s_create_pipelines: create char/bg/scene/hair pipelines + scene geometry ─────
bool s_create_pipelines(const char* glb_path) {
    // Character body pipeline
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
    pd.vert_uniform_bufs = 2;   // set=1 binding=0: VU, binding=1: BoneMats (30 mat4)
    pd.vert_samplers = 0;
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

    // ── Scene pipeline: anthropometer pole ─────────────────
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

        // ── Procedural geometry: anthropometer pole ───────────────────
        // Coordinate system: Y=0 = character feet level.
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

    // Load game mesh for bone pose — identical computation to in-game NPC render.
    // glb_path may be "md_human_t.glb" (with morphs); game mesh is always md_human.glb.
    {
        const char* pose_path = strstr(glb_path, "female")
            ? "game/data/props/md_human.glb"   // female pose mesh (same skeleton)
            : "game/data/props/md_human.glb";
        if (s_pose_mesh.LoadGLB(pose_path)) {
            s_pose_idle_clip     = s_pose_mesh.ClipIndexByName("idle_stand_normal");
            s_pose_postures_clip = s_pose_mesh.ClipIndexByName("postures");
            s_pose_neck_clip     = s_pose_mesh.ClipIndexByName("neck set");
            s_pose_shoulder_clip = s_pose_mesh.ClipIndexByName("shoulder set");
            if (s_pose_ozz.Init(s_pose_mesh))
                fprintf(stdout, "[CharPreview] OzzAnimator ready, idle=%d postures=%d neck=%d shoulder=%d\n",
                        s_pose_idle_clip, s_pose_postures_clip, s_pose_neck_clip, s_pose_shoulder_clip);
            else
                fprintf(stderr, "[CharPreview] OzzAnimator init failed\n");
        } else {
            fprintf(stderr, "[CharPreview] WARN: could not load pose mesh %s\n", pose_path);
        }
    }

    // ── Hair pipeline ─────────────────────────────────────────────────────────
    {
        GpuPipeline::Desc hpd;
        hpd.vert_path = "shaders/char_hair.vert";
        hpd.frag_path = "shaders/char_hair.frag";
        hpd.layout.count  = 2;
        hpd.layout.stride = 24;  // vec3 pos + vec3 norm = 24 bytes
        hpd.layout.attribs[0] = {0,  0, GpuAttribFmt::F3};   // aPos
        hpd.layout.attribs[1] = {1, 12, GpuAttribFmt::F3};   // aNorm
        hpd.raster.depth_test  = true;
        hpd.raster.depth_write = true;
        hpd.raster.cull_back   = false;   // hair = double-sided
        hpd.vert_samplers      = 1;       // set=0: uBoneMats
        hpd.vert_uniform_bufs  = 1;       // set=1: VU (mvp)
        hpd.frag_samplers      = 0;
        hpd.frag_uniform_bufs  = 2;       // set=3 binding=0: HairFU, binding=1: HairShadingFU
        hpd.color_format       = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        hpd.has_depth_target   = true;
        if (!s_hair_pipeline.Create(hpd))
            fprintf(stderr, "[Hair] pipeline create failed\n");
    }

    // ── Clothing pipeline (char_preview.vert + char_clothes.frag) ────────────
    {
        GpuPipeline::Desc cpd;
        cpd.vert_path = "shaders/char_preview.vert";
        cpd.frag_path = "shaders/char_clothes.frag";
        cpd.layout.count  = 5; cpd.layout.stride = 52;
        cpd.layout.attribs[0] = {0,  0, GpuAttribFmt::F3   };  // aPos
        cpd.layout.attribs[1] = {1, 12, GpuAttribFmt::F3   };  // aNorm
        cpd.layout.attribs[2] = {2, 24, GpuAttribFmt::F2   };  // aUV
        cpd.layout.attribs[3] = {3, 32, GpuAttribFmt::U8x4 };  // aJoints
        cpd.layout.attribs[4] = {4, 36, GpuAttribFmt::F4   };  // aWeights
        cpd.raster.depth_test  = true;
        cpd.raster.depth_write = true;
        cpd.raster.cull_back   = true;
        cpd.vert_samplers      = 0;
        cpd.vert_uniform_bufs  = 2;   // slot0: VU (mvp), slot1: BoneMats
        cpd.frag_samplers      = 0;
        cpd.frag_uniform_bufs  = 1;   // slot0: ClothFU (color)
        cpd.color_format       = SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
        cpd.has_depth_target   = true;
        if (!s_cloth_pipeline.Create(cpd))
            fprintf(stderr, "[Cloth] pipeline create failed\n");
    }

    return true;
}

// ── Init: load GLB (T-pose) + body texture + pipeline ─────────────────────────

}  // namespace CharPreviewSDLGPU
#endif // MD_SDL_GPU
