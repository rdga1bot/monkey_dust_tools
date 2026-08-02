#include "editor_reflect_inspector.h"
#ifdef MONKEY_DUST_EDITOR
#include "imgui.h"
#include "editor_core.h"
#include "editor_reflect_bridge.h"
#include <monkey_dust/ecs/md_entity.h>
#include <monkey_dust/ecs/md_registry.h>
#include <monkey_dust/editor/cmd_registry.h>
#include <flecs.h>
#include <cstdio>
#include <cstring>

namespace {

// Draws every field of a reflected component via the generic FieldType
// switch. Returns true if any widget reported an edit — caller is
// responsible for calling ecs_modified_id() exactly once afterward.
bool DrawReflectedFields(void* comp, const md::ComponentDesc& desc) {
    bool edited = false;
    uint8_t* base = static_cast<uint8_t*>(comp);

    for (int i = 0; i < desc.field_count; ++i) {
        const md::FieldDesc& f = desc.fields[i];
        void* p = base + f.offset;
        char label[48];
        snprintf(label, sizeof(label), "%s##rf%d", f.name, i);

        switch (f.type) {
        case md::FieldType::F32: {
            float* v = static_cast<float*>(p);
            if (f.ui_min < f.ui_max)
                edited |= ImGui::DragFloat(label, v, f.ui_speed, f.ui_min, f.ui_max);
            else
                edited |= ImGui::DragFloat(label, v, f.ui_speed);
            break;
        }
        case md::FieldType::F64: {
            double* v = static_cast<double*>(p);
            edited |= ImGui::InputDouble(label, v);
            break;
        }
        case md::FieldType::Vec3: {
            float* v = static_cast<float*>(p);
            edited |= ImGui::DragFloat3(label, v, f.ui_speed);
            break;
        }
        case md::FieldType::Bool: {
            bool* v = static_cast<bool*>(p);
            edited |= ImGui::Checkbox(label, v);
            break;
        }
        case md::FieldType::I8:
            edited |= ImGui::DragScalar(label, ImGuiDataType_S8, p, 1.f);
            break;
        case md::FieldType::I16:
            edited |= ImGui::DragScalar(label, ImGuiDataType_S16, p, 1.f);
            break;
        case md::FieldType::I32:
            edited |= ImGui::DragScalar(label, ImGuiDataType_S32, p, 1.f);
            break;
        case md::FieldType::I64:
            edited |= ImGui::DragScalar(label, ImGuiDataType_S64, p, 1.f);
            break;
        case md::FieldType::U8:
        case md::FieldType::Enum8:
            edited |= ImGui::DragScalar(label, ImGuiDataType_U8, p, 1.f);
            break;
        case md::FieldType::U16:
        case md::FieldType::Enum16:
            edited |= ImGui::DragScalar(label, ImGuiDataType_U16, p, 1.f);
            break;
        case md::FieldType::U32:
        case md::FieldType::Enum32:
            edited |= ImGui::DragScalar(label, ImGuiDataType_U32, p, 1.f);
            break;
        case md::FieldType::U64:
            edited |= ImGui::DragScalar(label, ImGuiDataType_U64, p, 1.f);
            break;
        }
    }
    return edited;
}

} // namespace

void EditorReflectInspector::DrawContent(ecs_world_t* world) {
    if (!world) { ImGui::TextDisabled("No ECS world bound"); return; }

    MdEntity sel = EditorCore::Get().GetPrimary();
    ecs_entity_t eid = sel.Raw();
    if (eid == 0 || !ecs_is_alive(world, eid)) {
        ImGui::TextDisabled("No entity selected");
        return;
    }

    ImGui::Text("Entity #%u", (unsigned)eid);
    ImGui::Separator();

    EcsReflectBridge& bridge = EcsReflectBridge::Get();

    static constexpr int MAX_STRUCT_OPS = 8;
    ecs_entity_t remove_ops[MAX_STRUCT_OPS]; int remove_count = 0;
    // Names, not ids: queued adds now go through the registered "Add
    // Component" command (EDITOR_AUTOMATION_PLAN_v1.md Phase 1.3.4), which
    // re-resolves the id from the name itself — see AddComponentCmd's doc
    // comment (editor_std_commands.cpp) for why it takes a name, not a raw
    // ecs_entity_t, across the command-args ABI. Pointers into
    // bridge.Desc(i).name are stable for the remainder of this frame.
    const char* add_names[MAX_STRUCT_OPS];   int add_count = 0;

    // ── Reflected components present on this entity ─────────────────────
    for (int i = 0; i < bridge.Count(); ++i) {
        ecs_entity_t cid = bridge.Id(i);
        if (cid == 0 || !ecs_has_id(world, eid, cid)) continue;
        const md::ComponentDesc& desc = bridge.Desc(i);

        ImGui::PushID(i);
        bool open = ImGui::CollapsingHeader(desc.name, ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::BeginPopupContextItem("##ctx")) {
            if (ImGui::MenuItem("Remove component")) {
                if (remove_count < MAX_STRUCT_OPS) remove_ops[remove_count++] = cid;
            }
            ImGui::EndPopup();
        }
        if (open) {
            // Re-fetched every frame — never held across a structural op
            // (see class header comment / md_registry.h B3.4).
            void* comp = ecs_get_mut_id(world, eid, cid);
            if (comp) {
                bool edited;
                EcsReflectBridge::DrawerFn custom = bridge.CustomFor(i);
                if (custom) edited = custom(world, eid, comp);
                else        edited = DrawReflectedFields(comp, desc);
                if (edited) ecs_modified_id(world, eid, cid);
            }
        }
        ImGui::PopID();
    }

    // ── Add component ─────────────────────────────────────────────────
    ImGui::Separator();
    if (ImGui::BeginCombo("##add_component", "+ Add component")) {
        for (int i = 0; i < bridge.Count(); ++i) {
            ecs_entity_t cid = bridge.Id(i);
            if (cid == 0 || ecs_has_id(world, eid, cid)) continue;
            if (ImGui::Selectable(bridge.Desc(i).name)) {
                if (add_count < MAX_STRUCT_OPS) add_names[add_count++] = bridge.Desc(i).name;
            }
        }
        ImGui::EndCombo();
    }

    // ── Apply structural ops AFTER the draw pass ─────────────────────────
    for (int i = 0; i < remove_count; ++i) ecs_remove_id(world, eid, remove_ops[i]);
    for (int i = 0; i < add_count; ++i) {
        CmdArgs args; args.count = 2;
        args.values[0].type = CmdArgType::Entity; args.values[0].entity_id = eid;
        args.values[1].type = CmdArgType::Str;
        snprintf(args.values[1].str, sizeof(args.values[1].str), "%s", add_names[i]);
        EditorCmdRegistry::Get().Dispatch("Add Component", args, MdRegistry::Get());
    }

    // ── Unreflected components (read-only) ───────────────────────────────
    ImGui::Separator();
    ImGui::TextDisabled("Unreflected");
    const ecs_type_t* type = ecs_get_type(world, eid);
    if (type) {
        for (int i = 0; i < type->count; ++i) {
            ecs_id_t id = type->array[i];
            if (ECS_IS_PAIR(id)) continue;

            bool covered = false;
            for (int j = 0; j < bridge.Count(); ++j) {
                if (bridge.Id(j) == id) { covered = true; break; }
            }
            if (covered) continue;

            const char* nm = ecs_get_name(world, id);
            if (!nm || strcmp(nm, "MdManagedTag") == 0) continue;
            ImGui::TextDisabled("%s", nm);
        }
    }
}
#endif
