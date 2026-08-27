#pragma once
#ifdef MONKEY_DUST_EDITOR
// NpcArchetype spawning — only available in in-game editor (MONKEY_DUST_EDITOR).
// Include AFTER npc_archetype_editor.h.
#include "npc_archetype_editor.h"
#include "editor_core.h"
#include <monkey_dust/ecs/registry.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/world/world_transform.h>
#include <monkey_dust/world/transform_soa.h>
#include <monkey_dust/components/health.h>
#include <monkey_dust/components/ai_agent.h>
#include <monkey_dust/components/combat.h>
#include <monkey_dust/components/renderable.h>
#include <monkey_dust/components/stat_sheet.h>
#include <monkey_dust/components/skill_xp_accum.h>
#include <monkey_dust/components/nav_agent.h>

inline MdEntity SpawnFromArchetype(int idx) {
    if (idx < 0 || idx >= NpcArchetypeEditor::g_count) return MdEntity::Null();
    const auto& d  = NpcArchetypeEditor::g_archs[idx];
    auto& reg = MdRegistry::Get();
    auto& ec  = EditorCore::Get();
    auto  e   = reg.Create();

    reg.Handle(e).emplace<WorldTransform>();
    auto& tr  = reg.Handle(e).get_mut<WorldTransform>();
    tr.x = ec.cam_target.x; tr.y = 0.f; tr.z = ec.cam_target.z; tr.rot_y = 0.f;
    tr.slot = TransformSoA::Get().Alloc(e, tr.x, tr.z, (uint8_t)d.faction_id);

    reg.Handle(e).emplace<AIAgent>();
    reg.Handle(e).emplace<AIAgentTickState>();
    auto& ai = reg.Handle(e).get_mut<AIAgent>();
    ai.faction_id = d.faction_id;
    reg.Handle(e).get_mut<AIAgentTickState>().lod_level = d.lod_level;

    reg.Handle(e).set<Health>(LimbHealth::Make(d.health));

    reg.Handle(e).emplace<NavAgent>();
    auto& nav = reg.Handle(e).get_mut<NavAgent>();
    nav.walk_speed = d.walk_speed;
    nav.run_speed  = d.run_speed;

    reg.Handle(e).emplace<StatSheet>();
    auto& ss = reg.Handle(e).get_mut<StatSheet>();
    ss[Skill::Toughness]  = d.toughness;
    ss[Skill::Strength]   = d.strength;
    ss[Skill::Dexterity]  = d.dexterity;

    // Pre-seed alongside StatSheet so ApplySkillXpGrant (game/src/combat/
    // skill_xp.h) never has to take its first-grant no-op safety-net path
    // for NPCs spawned here.
    reg.Handle(e).emplace<SkillXpAccum>();

    reg.Handle(e).emplace<Combat>();
    auto& cmb = reg.Handle(e).get_mut<Combat>();
    cmb.weapon.type         = (DamageType)d.weapon_type;
    cmb.weapon.damage       = d.weapon_damage;
    cmb.weapon.attack_range = d.weapon_range;
    cmb.weapon.attack_ms    = 900u;

    reg.Handle(e).emplace<Renderable>();
    ec.Select(e);
    return e;
}
#endif // MONKEY_DUST_EDITOR
