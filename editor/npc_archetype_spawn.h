#pragma once
#ifdef MONKEY_DUST_EDITOR
// NpcArchetype spawning — only available in in-game editor (MONKEY_DUST_EDITOR).
// Include AFTER npc_archetype_editor.h.
#include "npc_archetype_editor.h"
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

inline entt::entity SpawnFromArchetype(int idx) {
    if (idx < 0 || idx >= NpcArchetypeEditor::g_count) return entt::null;
    const auto& d  = NpcArchetypeEditor::g_archs[idx];
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
    cmb.weapon.attack_ms    = 900u;

    reg.emplace<Renderable>(e);
    ec.Select(e);
    return e;
}
#endif // MONKEY_DUST_EDITOR
