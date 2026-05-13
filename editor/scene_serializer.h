#pragma once
#ifdef MONKEY_DUST_EDITOR
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/components/building.h>
#include <monkey_dust/components/inventory.h>
#include <monkey_dust/components/player_controller.h>
#include <monkey_dust/components/ai_script.h>
#include "building/build_system.h"
#include <monkey_dust/platform/md_log.h>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace SceneSerializer {

static constexpr int MAX_EXPORT_ENTITIES = 2048;

// ── Export ────────────────────────────────────────────────────────────────────
inline bool Export(const char* path) {
    auto& reg = Registry::Get();

    // Atomic write: write to .tmp then rename
    char tmp_path[256];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

    FILE* f = fopen(tmp_path, "w");
    if (!f) {
        MD_LOG(MD_LOG_ERROR, "[SceneSerializer] Cannot open %s for write", tmp_path);
        return false;
    }

    long ts = (long)time(nullptr);
    fprintf(f, "{\n  \"version\": 1,\n  \"timestamp\": %ld,\n  \"entities\": [\n", ts);

    int count = 0;
    bool first = true;

    for (auto e : reg.storage<entt::entity>()) {
        if (!reg.valid(e)) continue;
        if (!reg.all_of<WorldTransform>(e)) continue;
        if (count >= MAX_EXPORT_ENTITIES) {
            MD_LOG(MD_LOG_WARNING, "[SceneSerializer] Truncated at %d entities", MAX_EXPORT_ENTITIES);
            break;
        }

        const auto& tr = reg.get<WorldTransform>(e);
        uint32_t id = (uint32_t)entt::to_integral(e);

        if (!first) fprintf(f, ",\n");
        first = false;

        fprintf(f, "    {\n      \"id\": %u,\n      \"components\": {", id);

        // Transform (always present here)
        fprintf(f, "\n        \"transform\": {\"x\":%.4f,\"y\":%.4f,\"z\":%.4f,\"rot_y\":%.4f}",
                tr.x, tr.y, tr.z, tr.rot_y);

        if (reg.all_of<Health>(e)) {
            const auto& hp = reg.get<Health>(e);
            fprintf(f, ",\n        \"health\": {\"current\":%.2f,\"max\":%.2f}",
                    hp.current, hp.max);
        }

        if (reg.all_of<AIAgent>(e)) {
            const auto& ai = reg.get<AIAgent>(e);
            fprintf(f, ",\n        \"ai\": {\"faction_id\":%u,\"lod_level\":%u,"
                       "\"bt_template_id\":%u,\"personal_relation\":%d}",
                    ai.faction_id, (uint32_t)ai.lod_level,
                    (uint32_t)ai.bt_template_id, (int)ai.personal_relation);
        }

        if (reg.all_of<Combat>(e)) {
            const auto& c = reg.get<Combat>(e);
            fprintf(f, ",\n        \"combat\": {\"weapon_type\":%d,\"weapon_dmg\":%.2f,"
                       "\"is_dead\":%d}",
                    (int)c.weapon.type, c.weapon.damage, c.is_dead ? 1 : 0);
        }

        if (reg.all_of<Building>(e)) {
            const auto& b = reg.get<Building>(e);
            fprintf(f, ",\n        \"building\": {\"def_id\":%u,\"grid_x\":%d,\"grid_z\":%d,"
                       "\"active\":%d,\"progress\":%.2f}",
                    b.def_id, b.grid_x, b.grid_z, b.active ? 1 : 0, b.progress_s);
        }

        if (reg.all_of<Inventory>(e)) {
            const auto& inv = reg.get<Inventory>(e);
            fprintf(f, ",\n        \"inventory\": {\"slots\":[");
            for (int i = 0; i < inv.slot_count; ++i) {
                if (i > 0) fprintf(f, ",");
                fprintf(f, "{\"id\":%u,\"amount\":%d}", inv.item_ids[i], inv.amounts[i]);
            }
            fprintf(f, "]}");
        }

        if (reg.all_of<PlayerController>(e)) {
            const auto& pc = reg.get<PlayerController>(e);
            fprintf(f, ",\n        \"player\": {\"move_speed\":%.2f,"
                       "\"attack_cooldown_ms\":%u}",
                    pc.move_speed, pc.attack_cooldown_ms);
        }

        if (reg.all_of<AIScript>(e)) {
            const auto& sc = reg.get<AIScript>(e);
            fprintf(f, ",\n        \"aiscript\": {\"func\":\"%s\"}", sc.script_func);
        }

        fprintf(f, "\n      }\n    }");
        count++;
    }

    fprintf(f, "\n  ]\n}\n");
    fclose(f);

    // Atomic rename
    if (rename(tmp_path, path) != 0) {
        MD_LOG(MD_LOG_ERROR, "[SceneSerializer] Rename failed: %s → %s", tmp_path, path);
        return false;
    }

    MD_LOG(MD_LOG_INFO, "[SceneSerializer] Exported %d entities to %s", count, path);
    return true;
}

// ── Helpers for Import ────────────────────────────────────────────────────────
static bool SsParsFloat(const char* p, const char* key, float& out) {
    const char* found = strstr(p, key);
    if (!found) return false;
    found += strlen(key);
    while (*found == ':' || *found == ' ') ++found;
    out = strtof(found, nullptr);
    return true;
}

static bool SsParsInt(const char* p, const char* key, int& out) {
    const char* found = strstr(p, key);
    if (!found) return false;
    found += strlen(key);
    while (*found == ':' || *found == ' ') ++found;
    out = (int)strtol(found, nullptr, 10);
    return true;
}

static bool SsParsStr(const char* p, const char* key, char* out, int maxlen) {
    const char* found = strstr(p, key);
    if (!found) return false;
    found += strlen(key);
    while (*found == ':' || *found == ' ' || *found == '"') ++found;
    int i = 0;
    while (*found && *found != '"' && i < maxlen - 1)
        out[i++] = *found++;
    out[i] = '\0';
    return true;
}

// ── Import ────────────────────────────────────────────────────────────────────
inline bool Import(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) {
        MD_LOG(MD_LOG_ERROR, "[SceneSerializer] Cannot open %s", path);
        return false;
    }

    // Read full file into fixed buffer (max 4 MB)
    static constexpr int BUF_SIZE = 4 * 1024 * 1024;
    static char buf[BUF_SIZE];
    int len = (int)fread(buf, 1, BUF_SIZE - 1, f);
    fclose(f);
    buf[len] = '\0';

    // Verify version
    int version = 0;
    SsParsInt(buf, "\"version\"", version);
    if (version != 1) {
        MD_LOG(MD_LOG_WARNING, "[SceneSerializer] Unknown version %d", version);
    }

    auto& reg = Registry::Get();
    reg.clear();

    int count = 0;
    const char* cursor = buf;

    while (count < MAX_EXPORT_ENTITIES) {
        // Find next entity block by "id":
        const char* id_pos = strstr(cursor, "\"id\"");
        if (!id_pos) break;
        cursor = id_pos + 4;

        int eid_raw = 0;
        if (!SsParsInt(id_pos, "\"id\"", eid_raw)) continue;

        // Find "components": block for this entity
        const char* comp_pos = strstr(cursor, "\"components\"");
        const char* next_id  = strstr(cursor, "\"id\"");
        if (!comp_pos) break;
        // Don't read past the next entity block
        if (next_id && next_id < comp_pos) { cursor = next_id; continue; }

        auto e = reg.create();
        auto& tr = reg.emplace<WorldTransform>(e);

        // Transform
        const char* tr_pos = strstr(comp_pos, "\"transform\"");
        if (tr_pos) {
            SsParsFloat(tr_pos, "\"x\"",     tr.x);
            SsParsFloat(tr_pos, "\"y\"",     tr.y);
            SsParsFloat(tr_pos, "\"z\"",     tr.z);
            SsParsFloat(tr_pos, "\"rot_y\"", tr.rot_y);
        }


        // Health
        const char* hp_pos = strstr(comp_pos, "\"health\"");
        if (hp_pos) {
            auto& hp = reg.emplace<Health>(e);
            SsParsFloat(hp_pos, "\"current\"", hp.current);
            SsParsFloat(hp_pos, "\"max\"",     hp.max);
        }

        // AIAgent
        const char* ai_pos = strstr(comp_pos, "\"ai\"");
        if (ai_pos) {
            auto& ai = reg.emplace<AIAgent>(e);
            int fi = 0, li = 0, ti = 0, pr = 0;
            SsParsInt(ai_pos, "\"faction_id\"",    fi); ai.faction_id    = (uint32_t)fi;
            SsParsInt(ai_pos, "\"lod_level\"",     li); ai.lod_level     = (uint8_t)li;
            SsParsInt(ai_pos, "\"bt_template_id\"",ti); ai.bt_template_id= (uint8_t)ti;
            SsParsInt(ai_pos, "\"personal_relation\"", pr);
            ai.personal_relation = (int8_t)pr;
        }

        // Combat
        const char* cb_pos = strstr(comp_pos, "\"combat\"");
        if (cb_pos) {
            auto& c = reg.emplace<Combat>(e, Combat::MakeBandit());
            int wt = 0, dead = 0; float wd = 28.f;
            SsParsInt  (cb_pos, "\"weapon_type\"", wt);
            SsParsFloat(cb_pos, "\"weapon_dmg\"",  wd);
            SsParsInt  (cb_pos, "\"is_dead\"",     dead);
            c.weapon.type   = (DamageType)wt;
            c.weapon.damage = wd;
            c.is_dead       = (dead != 0);
        }

        // Building
        const char* bld_pos = strstr(comp_pos, "\"building\"");
        if (bld_pos) {
            auto& b = reg.emplace<Building>(e);
            int gx = 0, gz = 0, act = 0, di = 0;
            float prog = 0.f;
            SsParsInt  (bld_pos, "\"def_id\"",   di);   b.def_id     = (uint32_t)di;
            SsParsInt  (bld_pos, "\"grid_x\"",   gx);   b.grid_x     = gx;
            SsParsInt  (bld_pos, "\"grid_z\"",   gz);   b.grid_z     = gz;
            SsParsInt  (bld_pos, "\"active\"",   act);  b.active     = (act != 0);
            SsParsFloat(bld_pos, "\"progress\"", prog); b.progress_s = prog;
        }

        // Inventory
        const char* inv_pos = strstr(comp_pos, "\"inventory\"");
        if (inv_pos) {
            auto& inv = reg.emplace<Inventory>(e);
            inv.Clear();
            const char* slots = strstr(inv_pos, "\"slots\"");
            if (slots) {
                const char* sc = strstr(slots, "[");
                if (sc) {
                    const char* next_bld = strstr(comp_pos, "\"building\"");
                    while (inv.slot_count < INV_MAX_SLOTS) {
                        const char* obj = strstr(sc, "{");
                        if (!obj) break;
                        if (next_bld && obj > next_bld) break;
                        int sid = 0, sam = 0;
                        SsParsInt(obj, "\"id\"",     sid);
                        SsParsInt(obj, "\"amount\"", sam);
                        if (sid > 0) inv.Add((uint32_t)sid, sam);
                        sc = obj + 1;
                    }
                }
            }
        }

        // PlayerController
        const char* pc_pos = strstr(comp_pos, "\"player\"");
        if (pc_pos) {
            auto& pc = reg.emplace<PlayerController>(e);
            float ms = 5.f; int acd = 800;
            SsParsFloat(pc_pos, "\"move_speed\"",        ms);
            SsParsInt  (pc_pos, "\"attack_cooldown_ms\"",acd);
            pc.move_speed         = ms;
            pc.attack_cooldown_ms = (uint32_t)acd;
            pc.last_attack_ms     = 0.f;
            pc.rng_state          = 1u;
        }

        // AIScript
        const char* sc_pos = strstr(comp_pos, "\"aiscript\"");
        if (sc_pos) {
            auto& sc = reg.emplace<AIScript>(e);
            SsParsStr(sc_pos, "\"func\"", sc.script_func, sizeof(sc.script_func));
        }

        cursor = comp_pos + 1;
        count++;
    }

    // Post-import: rebuild systems
    BuildSystem::Get().RebuildGridFromEntities();


    MD_LOG(MD_LOG_INFO, "[SceneSerializer] Imported %d entities from %s", count, path);
    return true;
}

} // namespace SceneSerializer
#endif // MONKEY_DUST_EDITOR
