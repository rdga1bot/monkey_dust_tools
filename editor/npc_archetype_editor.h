#pragma once
#include "imgui.h"
#include "editor_core.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/components/renderable.h>
#include <monkey_dust/components/stat_sheet.h>
#include <monkey_dust/components/nav_agent.h>
#include <monkey_dust/platform/md_log.h>
#include <cstdio>
#include <cstring>
#include <cstdlib>

// ── NpcArchetypeDef ────────────────────────────────────────────────────────────
struct NpcArchetypeDef {
    char     id[32]          = {};
    char     name[48]        = {};
    char     bt_template[48] = {};
    uint32_t faction_id      = 1;
    float    walk_speed      = 1.7f;
    float    run_speed       = 4.5f;
    float    health          = 80.f;
    uint8_t  toughness       = 15;
    uint8_t  strength        = 20;
    uint8_t  dexterity       = 12;
    uint8_t  _pad            = 0;
    int      weapon_type     = 0;    // DamageType int
    float    weapon_damage   = 25.f;
    float    weapon_range    = 1.4f;
    int      armor_type      = 0;    // 0=None 1=Leather 2=Chain 3=Plate
    uint8_t  lod_level       = 0;
};

namespace NpcArchetypeEditor {

static constexpr int MAX_ARCHETYPES = 64;
static NpcArchetypeDef g_archs[MAX_ARCHETYPES];
static int             g_count = 0;
static int             g_sel   = -1;
static bool            g_dirty = false;
static char            g_path[256] = "game/data/defs/npc_archetypes.json";

// ── strstr helpers ──────────────────────────────────────────────────────────
static float na_getf(const char* p, const char* key, float def = 0.f) {
    const char* f = strstr(p, key);
    if (!f) return def;
    f += strlen(key);
    while (*f == '"' || *f == ':' || *f == ' ') ++f;
    return strtof(f, nullptr);
}
static int na_geti(const char* p, const char* key, int def = 0) {
    const char* f = strstr(p, key);
    if (!f) return def;
    f += strlen(key);
    while (*f == '"' || *f == ':' || *f == ' ') ++f;
    return (int)strtol(f, nullptr, 10);
}
static void na_gets(const char* p, const char* key, char* out, int maxlen) {
    const char* f = strstr(p, key);
    if (!f) { out[0] = '\0'; return; }
    f += strlen(key);
    while (*f == '"' || *f == ':' || *f == ' ') ++f;
    if (*f == '"') ++f;
    int i = 0;
    while (*f && *f != '"' && i < maxlen - 1) out[i++] = *f++;
    out[i] = '\0';
}

// ── Load ───────────────────────────────────────────────────────────────────
inline bool Load(const char* path) {
    static constexpr int BUF = 64 * 1024;
    static char buf[BUF];
    FILE* f = fopen(path, "r");
    if (!f) return false;
    int len = (int)fread(buf, 1, BUF - 1, f);
    fclose(f);
    buf[len] = '\0';

    g_count = 0;
    const char* cur = buf;
    while (g_count < MAX_ARCHETYPES) {
        const char* ob = strstr(cur, "{");
        if (!ob) break;
        // skip outer array braces — only parse items that have "id"
        const char* id_pos = strstr(ob, "\"id\"");
        const char* next   = strstr(ob + 1, "{");
        if (!id_pos || (next && next < id_pos)) { cur = ob + 1; continue; }

        auto& d = g_archs[g_count++];
        na_gets(ob, "\"id\"",          d.id,          sizeof(d.id));
        na_gets(ob, "\"name\"",        d.name,        sizeof(d.name));
        na_gets(ob, "\"bt_template\"", d.bt_template, sizeof(d.bt_template));
        d.faction_id    = (uint32_t)na_geti(ob, "\"faction_id\"",  1);
        d.walk_speed    = na_getf(ob, "\"walk_speed\"",  1.7f);
        d.run_speed     = na_getf(ob, "\"run_speed\"",   4.5f);
        d.health        = na_getf(ob, "\"health\"",     80.f);
        d.toughness     = (uint8_t)na_geti(ob, "\"toughness\"", 15);
        d.strength      = (uint8_t)na_geti(ob, "\"strength\"",  20);
        d.dexterity     = (uint8_t)na_geti(ob, "\"dexterity\"", 12);
        d.weapon_type   = na_geti(ob, "\"weapon_type\"",   0);
        d.weapon_damage = na_getf(ob, "\"weapon_damage\"", 25.f);
        d.weapon_range  = na_getf(ob, "\"weapon_range\"",  1.4f);
        d.armor_type    = na_geti(ob, "\"armor_type\"",    0);
        d.lod_level     = (uint8_t)na_geti(ob, "\"lod_level\"", 0);

        cur = id_pos + 4;
    }
    MD_LOG(MD_LOG_INFO, "[NpcArchEditor] Loaded %d archetypes from %s", g_count, path);
    return true;
}

// ── Save ───────────────────────────────────────────────────────────────────
inline bool Save(const char* path) {
    char tmp[260]; snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE* f = fopen(tmp, "w");
    if (!f) return false;
    fprintf(f, "{\n  \"archetypes\": [\n");
    for (int i = 0; i < g_count; ++i) {
        const auto& d = g_archs[i];
        fprintf(f, "    {\n");
        fprintf(f, "      \"id\": \"%s\",\n",          d.id);
        fprintf(f, "      \"name\": \"%s\",\n",        d.name);
        fprintf(f, "      \"faction_id\": %u,\n",      d.faction_id);
        fprintf(f, "      \"bt_template\": \"%s\",\n", d.bt_template);
        fprintf(f, "      \"walk_speed\": %.2f,\n",    d.walk_speed);
        fprintf(f, "      \"run_speed\": %.2f,\n",     d.run_speed);
        fprintf(f, "      \"health\": %.1f,\n",        d.health);
        fprintf(f, "      \"toughness\": %d,\n",       (int)d.toughness);
        fprintf(f, "      \"strength\": %d,\n",        (int)d.strength);
        fprintf(f, "      \"dexterity\": %d,\n",       (int)d.dexterity);
        fprintf(f, "      \"weapon_type\": %d,\n",     d.weapon_type);
        fprintf(f, "      \"weapon_damage\": %.2f,\n", d.weapon_damage);
        fprintf(f, "      \"weapon_range\": %.2f,\n",  d.weapon_range);
        fprintf(f, "      \"armor_type\": %d,\n",      d.armor_type);
        fprintf(f, "      \"lod_level\": %d\n",        (int)d.lod_level);
        fprintf(f, "    }%s\n", (i < g_count - 1) ? "," : "");
    }
    fprintf(f, "  ]\n}\n");
    fclose(f);
    if (rename(tmp, path) != 0) return false;
    g_dirty = false;
    MD_LOG(MD_LOG_INFO, "[NpcArchEditor] Saved %d archetypes to %s", g_count, path);
    return true;
}

// ── Spawn entity from archetype ─────────────────────────────────────────────
inline void SpawnFromArch(int idx) {
    if (idx < 0 || idx >= g_count) return;
    const auto& d = g_archs[idx];
    auto& reg = Registry::Get();
    auto& ec  = EditorCore::Get();
    auto  e   = reg.create();

    auto& tr  = reg.emplace<WorldTransform>(e);
    tr.x = ec.cam_target.x; tr.y = 0.f; tr.z = ec.cam_target.z; tr.rot_y = 0.f;
    tr.slot = TransformSoA::Get().Alloc(e, tr.x, tr.z, (uint8_t)d.faction_id);

    auto& ai = reg.emplace<AIAgent>(e);
    ai.faction_id = d.faction_id;
    ai.lod_level  = d.lod_level;

    reg.emplace<Health>(e) = LimbHealth::Make(d.health);

    auto& nav = reg.emplace<NavAgent>(e);
    nav.walk_speed = d.walk_speed;
    nav.run_speed  = d.run_speed;

    auto& ss = reg.emplace<StatSheet>(e);
    ss[Skill::Toughness]  = d.toughness;
    ss[Skill::Strength]   = d.strength;
    ss[Skill::Dexterity]  = d.dexterity;

    auto& cmb = reg.emplace<Combat>(e);
    cmb.weapon.type         = (DamageType)d.weapon_type;
    cmb.weapon.damage       = d.weapon_damage;
    cmb.weapon.attack_range = d.weapon_range;
    cmb.weapon.attack_ms    = 900;

    reg.emplace<Renderable>(e);
    ec.Select(e);
    MD_LOG(MD_LOG_INFO, "[NpcArchEditor] Spawned '%s' at (%.1f,%.1f)", d.name, tr.x, tr.z);
}

// ── Draw ───────────────────────────────────────────────────────────────────
inline void Draw() {
    static const char* weapon_names[] = { "Blunt", "Cut", "Pierce" };
    static const char* armor_names[]  = { "None", "Leather", "Chain", "Plate" };

    ImGui::SetNextWindowSize(ImVec2(640, 520), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80, 80),    ImGuiCond_FirstUseEver);
    ImGui::Begin("NPC Archetypes (FCS)##npcarch");

    // ── Toolbar ──────────────────────────────────────────────────────────
    if (ImGui::Button("+ New")) {
        if (g_count < MAX_ARCHETYPES) {
            auto& d = g_archs[g_count];
            snprintf(d.id,   sizeof(d.id),   "npc_%d", g_count);
            snprintf(d.name, sizeof(d.name), "New NPC %d", g_count);
            snprintf(d.bt_template, sizeof(d.bt_template), "bandit_patrol");
            d.walk_speed = 1.7f; d.run_speed = 4.5f; d.health = 80.f;
            d.toughness = 15; d.strength = 20; d.weapon_damage = 25.f;
            g_sel = g_count++;
            g_dirty = true;
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Duplicate") && g_sel >= 0 && g_count < MAX_ARCHETYPES) {
        g_archs[g_count] = g_archs[g_sel];
        snprintf(g_archs[g_count].id, sizeof(g_archs[0].id), "%s_copy", g_archs[g_sel].id);
        g_sel = g_count++;
        g_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete") && g_sel >= 0) {
        for (int i = g_sel; i < g_count - 1; ++i) g_archs[i] = g_archs[i + 1];
        --g_count;
        g_sel = (g_sel >= g_count) ? g_count - 1 : g_sel;
        g_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Spawn at Camera") && g_sel >= 0)
        SpawnFromArch(g_sel);
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, g_dirty ? ImVec4(0.7f,0.3f,0.1f,1.f)
                                                   : ImVec4(0.15f,0.45f,0.2f,1.f));
    if (ImGui::Button(g_dirty ? "Save*##arc" : "Save##arc"))
        Save(g_path);
    ImGui::PopStyleColor();

    ImGui::Separator();

    // ── List + Inspector side-by-side ─────────────────────────────────────
    ImGui::Columns(2, "npcarch_cols");
    ImGui::SetColumnWidth(0, 200);

    // Left: list
    ImGui::BeginChild("##arch_list", ImVec2(0, -4), false);
    for (int i = 0; i < g_count; ++i) {
        bool sel = (i == g_sel);
        ImGui::PushStyleColor(ImGuiCol_Text, sel
            ? ImVec4(1.f,0.85f,0.3f,1.f) : ImVec4(0.85f,0.85f,0.85f,1.f));
        char label[64];
        snprintf(label, sizeof(label), "[f%u] %s##%d", g_archs[i].faction_id, g_archs[i].name, i);
        if (ImGui::Selectable(label, sel))
            g_sel = i;
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::NextColumn();

    // Right: inspector
    if (g_sel >= 0 && g_sel < g_count) {
        auto& d = g_archs[g_sel];
        ImGui::BeginChild("##arch_insp", ImVec2(0, -4), false);

        ImGui::TextColored({1.f,0.85f,0.3f,1.f}, "▶ %s", d.name);
        ImGui::Separator();

        if (ImGui::InputText("ID##arc",   d.id,   sizeof(d.id)))   g_dirty = true;
        if (ImGui::InputText("Name##arc", d.name, sizeof(d.name))) g_dirty = true;

        ImGui::Spacing();
        ImGui::TextDisabled("── Faction & AI ──");
        int fi = (int)d.faction_id;
        if (ImGui::InputInt("Faction ID##arc", &fi)) { d.faction_id = (uint32_t)(fi<0?0:fi); g_dirty=true; }
        if (ImGui::InputText("BT Template##arc", d.bt_template, sizeof(d.bt_template))) g_dirty=true;
        int li = (int)d.lod_level;
        if (ImGui::SliderInt("LOD Level##arc", &li, 0, 2)) { d.lod_level=(uint8_t)li; g_dirty=true; }

        ImGui::Spacing();
        ImGui::TextDisabled("── Locomotion ──");
        if (ImGui::DragFloat("Walk Speed (m/s)##arc", &d.walk_speed, 0.05f, 0.5f, 5.f)) g_dirty=true;
        if (ImGui::DragFloat("Run Speed (m/s)##arc",  &d.run_speed,  0.1f,  1.f, 12.f)) g_dirty=true;

        ImGui::Spacing();
        ImGui::TextDisabled("── Stats ──");
        if (ImGui::DragFloat("Health##arc",       &d.health,    1.f, 10.f, 500.f)) g_dirty=true;
        int t=(int)d.toughness, s=(int)d.strength, dx=(int)d.dexterity;
        if (ImGui::SliderInt("Toughness##arc",  &t,  1, 99)) { d.toughness=(uint8_t)t;  g_dirty=true; }
        if (ImGui::SliderInt("Strength##arc",   &s,  1, 99)) { d.strength=(uint8_t)s;   g_dirty=true; }
        if (ImGui::SliderInt("Dexterity##arc",  &dx, 1, 99)) { d.dexterity=(uint8_t)dx; g_dirty=true; }

        ImGui::Spacing();
        ImGui::TextDisabled("── Combat ──");
        if (ImGui::Combo("Weapon Type##arc",  &d.weapon_type, weapon_names, 3)) g_dirty=true;
        if (ImGui::DragFloat("Weapon Dmg##arc",   &d.weapon_damage, 0.5f, 1.f, 200.f)) g_dirty=true;
        if (ImGui::DragFloat("Weapon Range##arc",  &d.weapon_range,  0.1f, 0.5f, 5.f)) g_dirty=true;
        if (ImGui::Combo("Armor##arc", &d.armor_type, armor_names, 4)) g_dirty=true;

        ImGui::Spacing();
        ImGui::Separator();
        if (ImGui::Button("Spawn This NPC##arc"))
            SpawnFromArch(g_sel);
        ImGui::EndChild();
    } else {
        ImGui::TextDisabled("Select an archetype");
    }

    ImGui::Columns(1);
    ImGui::End();
}

} // namespace NpcArchetypeEditor
