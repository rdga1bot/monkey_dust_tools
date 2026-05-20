// ozz_bake — converts md_human.glb (cgltf) to ozz skeleton + animation files.
// Run once from repo root:  ./build/tools/ozz_bake
// Outputs: game/data/anim/md_human.ozz  (skeleton)
//          game/data/anim/md_<clip>.ozz  (one per animation clip)
#define CGLTF_IMPLEMENTATION
#include "../../engine/src/render/cgltf.h"

#include <ozz/animation/offline/animation_builder.h>
#include <ozz/animation/offline/raw_animation.h>
#include <ozz/animation/offline/raw_skeleton.h>
#include <ozz/animation/offline/skeleton_builder.h>
#include <ozz/animation/runtime/animation.h>
#include <ozz/animation/runtime/skeleton.h>
#include <ozz/base/io/archive.h>
#include <ozz/base/io/stream.h>
#include <ozz/base/maths/transform.h>

#include <cstdio>
#include <cstring>
#include <cmath>
#include <cassert>

static constexpr const char* GLB_PATH   = "game/data/props/md_human.glb";
static constexpr const char* SKEL_OUT   = "game/data/anim/md_human.ozz";
static constexpr const char* ANIM_DIR   = "game/data/anim/";

static inline ozz::math::Quaternion NormQ(ozz::math::Quaternion q) {
    float l = sqrtf(q.x*q.x + q.y*q.y + q.z*q.z + q.w*q.w);
    if (l < 1e-9f) return {0,0,0,1};
    return {q.x/l, q.y/l, q.z/l, q.w/l};
}

// ── Build RawSkeleton from cgltf skin ───────────────────────────────────────
static bool build_skeleton(cgltf_data* d,
                            ozz::animation::offline::RawSkeleton& raw_skel)
{
    if (d->skins_count == 0) { fprintf(stderr, "[ozz_bake] No skin in GLB\n"); return false; }
    cgltf_skin& skin = d->skins[0];
    int nb = (int)skin.joints_count;

    // Build parent map
    std::vector<int> parents(nb, -1);
    for (int i = 0; i < nb; ++i) {
        cgltf_node* n = skin.joints[i];
        if (!n->parent) continue;
        for (int j = 0; j < nb; ++j)
            if (skin.joints[j] == n->parent) { parents[i] = j; break; }
    }

    // Build ozz joint tree (recursive via roots)
    struct Build {
        static void add(ozz::animation::offline::RawSkeleton::Joint& joint,
                        cgltf_skin& skin, int idx, const std::vector<int>& parents) {
            cgltf_node* n = skin.joints[idx];
            joint.name = n->name ? n->name : "joint";

            float t[3]={0,0,0}, r[4]={0,0,0,1}, s[3]={1,1,1};
            if (n->has_translation) { t[0]=n->translation[0]; t[1]=n->translation[1]; t[2]=n->translation[2]; }
            if (n->has_rotation)    { r[0]=n->rotation[0];    r[1]=n->rotation[1];    r[2]=n->rotation[2];    r[3]=n->rotation[3]; }
            if (n->has_scale)       { s[0]=n->scale[0];       s[1]=n->scale[1];       s[2]=n->scale[2]; }
            joint.transform.translation = {t[0], t[1], t[2]};
            joint.transform.rotation    = NormQ({r[0], r[1], r[2], r[3]});
            joint.transform.scale       = {s[0], s[1], s[2]};

            // Add children
            int nb2 = (int)skin.joints_count;
            for (int c = 0; c < nb2; ++c) {
                if (parents[c] != idx) continue;
                joint.children.push_back({});
                add(joint.children.back(), skin, c, parents);
            }
        }
    };

    // Find roots (parent == -1)
    for (int i = 0; i < nb; ++i) {
        if (parents[i] >= 0) continue;
        raw_skel.roots.push_back({});
        Build::add(raw_skel.roots.back(), skin, i, parents);
    }
    return raw_skel.Validate();
}

// ── Build RawAnimation for one cgltf_animation ──────────────────────────────
static bool build_animation(cgltf_data* d,
                             cgltf_animation& ca,
                             int nb_joints,
                             ozz::animation::offline::RawAnimation& raw_anim)
{
    raw_anim.name     = ca.name ? ca.name : "anim";
    raw_anim.duration = 0.f;
    raw_anim.tracks.resize((size_t)nb_joints);

    cgltf_skin& skin = d->skins[0];

    for (cgltf_size ci = 0; ci < ca.channels_count; ++ci) {
        cgltf_animation_channel& ch = ca.channels[ci];
        if (!ch.target_node || !ch.sampler) continue;
        if (ch.target_path != cgltf_animation_path_type_translation &&
            ch.target_path != cgltf_animation_path_type_rotation) continue;

        // Find joint index
        int bi = -1;
        for (int j = 0; j < nb_joints; ++j)
            if (skin.joints[j] == ch.target_node) { bi = j; break; }
        if (bi < 0) continue;

        cgltf_accessor* times_acc = ch.sampler->input;
        cgltf_accessor* vals_acc  = ch.sampler->output;
        int nkf = (int)times_acc->count;

        auto& track = raw_anim.tracks[(size_t)bi];

        for (int k = 0; k < nkf; ++k) {
            float t_k = 0;
            cgltf_accessor_read_float(times_acc, (cgltf_size)k, &t_k, 1);
            if (t_k > raw_anim.duration) raw_anim.duration = t_k;

            if (ch.target_path == cgltf_animation_path_type_translation) {
                float v[3] = {0,0,0};
                cgltf_accessor_read_float(vals_acc, (cgltf_size)k, v, 3);
                ozz::animation::offline::RawAnimation::TranslationKey key;
                key.time  = t_k;
                key.value = {v[0], v[1], v[2]};
                track.translations.push_back(key);
            } else {
                float v[4] = {0,0,0,1};
                cgltf_accessor_read_float(vals_acc, (cgltf_size)k, v, 4);
                ozz::animation::offline::RawAnimation::RotationKey key;
                key.time  = t_k;
                key.value = NormQ({v[0], v[1], v[2], v[3]});
                track.rotations.push_back(key);
            }
        }
    }
    // Ensure minimum 1s duration
    if (raw_anim.duration < 1e-3f) raw_anim.duration = 1.0f;
    return raw_anim.Validate();
}

static bool save_skeleton(const ozz::animation::Skeleton& skel, const char* path) {
    ozz::io::File f(path, "wb");
    if (!f.opened()) { fprintf(stderr, "[ozz_bake] Cannot open %s\n", path); return false; }
    ozz::io::OArchive archive(&f);
    archive << skel;
    return true;
}

static bool save_animation(const ozz::animation::Animation& anim, const char* path) {
    ozz::io::File f(path, "wb");
    if (!f.opened()) { fprintf(stderr, "[ozz_bake] Cannot open %s\n", path); return false; }
    ozz::io::OArchive archive(&f);
    archive << anim;
    return true;
}

int main() {
    // Make output dir
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "mkdir -p %s", ANIM_DIR);
    system(cmd);

    cgltf_options opts = {};
    cgltf_data* data = nullptr;
    if (cgltf_parse_file(&opts, GLB_PATH, &data) != cgltf_result_success ||
        cgltf_load_buffers(&opts, data, GLB_PATH) != cgltf_result_success) {
        fprintf(stderr, "[ozz_bake] Failed to load %s\n", GLB_PATH);
        return 1;
    }

    int nb_joints = (int)data->skins[0].joints_count;
    fprintf(stdout, "[ozz_bake] GLB loaded: %d joints, %zu animations\n",
            nb_joints, data->animations_count);

    // ── Skeleton ──────────────────────────────────────────────────────────────
    ozz::animation::offline::RawSkeleton raw_skel;
    if (!build_skeleton(data, raw_skel)) {
        fprintf(stderr, "[ozz_bake] Skeleton build failed\n");
        cgltf_free(data); return 1;
    }
    ozz::animation::offline::SkeletonBuilder skel_builder;
    auto rt_skel = skel_builder(raw_skel);
    if (!rt_skel) { fprintf(stderr, "[ozz_bake] SkeletonBuilder failed\n"); cgltf_free(data); return 1; }
    if (!save_skeleton(*rt_skel, SKEL_OUT)) { cgltf_free(data); return 1; }
    fprintf(stdout, "[ozz_bake] Skeleton → %s  (%d joints)\n", SKEL_OUT, nb_joints);

    // ── Animations ────────────────────────────────────────────────────────────
    ozz::animation::offline::AnimationBuilder anim_builder;
    int saved = 0;
    for (cgltf_size ai = 0; ai < data->animations_count; ++ai) {
        cgltf_animation& ca = data->animations[ai];
        const char* name = ca.name ? ca.name : "anim";

        ozz::animation::offline::RawAnimation raw_anim;
        if (!build_animation(data, ca, nb_joints, raw_anim)) {
            fprintf(stderr, "[ozz_bake] RawAnimation invalid: %s\n", name);
            continue;
        }

        auto rt_anim = anim_builder(raw_anim);
        if (!rt_anim) { fprintf(stderr, "[ozz_bake] AnimationBuilder failed: %s\n", name); continue; }

        char out_path[256];
        snprintf(out_path, sizeof(out_path), "%s%s.ozz", ANIM_DIR, name);
        if (!save_animation(*rt_anim, out_path)) continue;
        fprintf(stdout, "[ozz_bake]   %s → %.2fs\n", out_path, raw_anim.duration);
        ++saved;
    }

    cgltf_free(data);
    fprintf(stdout, "[ozz_bake] Done: skeleton + %d animations\n", saved);
    return (saved > 0) ? 0 : 1;
}
