#pragma once
#ifdef MONKEY_DUST_EDITOR
// EcsReflectBridge — resolves md::ComponentReflect entries against a live
// ecs world, entirely through each backend's untyped/runtime-id API
// (flecs: the C API; gaia: World::resolve()+runtime Entity ids, just as
// boundary-safe as flecs's ecs_entity_t -- both are POD 64-bit handles, no
// template instantiation of the component type itself).
//
// Why untyped/runtime-id, not compile-time flecs::_::type<T>::id() / gaia
// w.add<T>(): libeditor_panels.so does not link monkey_dust::engine
// statically (dlopen-resolved symbols only), so caching a component id via
// a compile-time-typed registration call inside this .so would instantiate
// the type's registration independently of the host binary's copy — the
// exact vague-linkage risk editor_module.h's Config::ecs_world now avoids
// for the world pointer itself. Runtime by-name resolution
// (ecs_lookup()/World::resolve()) is the boundary-safe path either way.
//
// Ids are NOT stable across a dlopen/dlclose cycle for this .so's own
// statics (they reset), so Init() must be called fresh from
// editor_panels_init() on every load/reload — never memoize ids past that.
#include <monkey_dust/ecs/component_reflect.h>
#include <gaia.h>
#include <cstring>
#include <cctype>

// gaia-ecs migration (Phase 5, PROMPT_GAIA_MIGRATION.md §7 p.2): thin,
// backend-conditional aliases + free-function wrappers so EcsReflectBridge
// and its consumers (editor_reflect_inspector.cpp, editor_std_commands.cpp)
// stay branch-free at the call site — same shape as flecs's own
// free-function C API either way, only the implementation differs.
using EcsBridgeWorldT = gaia::ecs::World;
using EcsBridgeIdT    = gaia::ecs::Entity;

inline bool EcsBridgeIdValid(EcsBridgeIdT id) {
    return id != gaia::ecs::EntityBad;
}

inline EcsBridgeIdT EcsBridgeResolve(EcsBridgeWorldT* world, const char* name) {
    if (world == nullptr) return gaia::ecs::EntityBad;
    return world->resolve(name);
}

// Raw 64-bit round-trip for the command-args ABI (CmdArgs::entity_id) --
// both backends' entity handles are 64-bit POD, so this is a lossless
// reinterpretation, not a lookup. Reconstruct with EcsBridgeIdFromRaw().
inline uint64_t EcsBridgeIdRaw(EcsBridgeIdT id) {
    return id.val;
}
inline EcsBridgeIdT EcsBridgeIdFromRaw(uint64_t raw) {
    return gaia::ecs::Entity((gaia::ecs::Identifier)raw);
}

inline bool EcsBridgeIsAlive(EcsBridgeWorldT* world, EcsBridgeIdT e) {
    return world->valid(e);
}

// entity has component/tag id `c` (plain id, not a wildcard/pair query).
inline bool EcsBridgeHas(EcsBridgeWorldT* world, EcsBridgeIdT e, EcsBridgeIdT c) {
    return world->has(e, c);
}

// Read-only pointer to component `c`'s payload on entity `e`, or nullptr
// if absent. Same re-fetch-every-frame caveat as EcsBridgeGetMut().
inline const void* EcsBridgeGet(EcsBridgeWorldT* world, EcsBridgeIdT e, EcsBridgeIdT c) {
    auto view = world->get_raw(e, c);
    return view.valid() ? view.data : nullptr;
}

// Mutable pointer to component `c`'s payload on entity `e`, or nullptr if
// absent. Re-fetch every frame -- never hold across a structural change
// (both backends invalidate on archetype/table move).
inline void* EcsBridgeGetMut(EcsBridgeWorldT* world, EcsBridgeIdT e, EcsBridgeIdT c) {
    auto view = world->mut_raw(e, c);
    return view.valid() ? view.data : nullptr;
}

// Marks component `c`'s payload on entity `e` modified after a direct
// write through EcsBridgeGetMut()'s pointer -- runs OnSet observers/hooks.
inline void EcsBridgeModified(EcsBridgeWorldT* world, EcsBridgeIdT e, EcsBridgeIdT c) {
    world->modify_raw(e, c);
}

inline void EcsBridgeRemove(EcsBridgeWorldT* world, EcsBridgeIdT e, EcsBridgeIdT c) {
    world->del(e, c);
}

// Adds component `c` to entity `e` with a zero-initialized payload of
// `size` bytes (size comes from ComponentDesc::component_size -- the
// reflected struct's real size, so the zero payload is a valid default
// value for every field type this project's reflection covers: numeric
// fields read as 0/0.f/false, which matches each field's own declared
// in-struct default for every reflected component checked against
// component_reflect.cpp's RegisterCoreComponents()).
// gaia: World::add_raw() -- verified by reading its body -- correctly
// branches on Table vs Sparse storage internally (the exact distinction
// task #54's move_entity_data investigation was about), unlike the plain
// tag-only World::add(Entity,Entity) which asserts when target is a
// Sparse-storage component. size must stay within the stack buffer below;
// MAX_ADD_PAYLOAD comfortably covers every reflected component today
// (largest is well under 128 bytes) with headroom.
inline bool EcsBridgeAddDefault(EcsBridgeWorldT* world, EcsBridgeIdT e, EcsBridgeIdT c, uint16_t size) {
    static constexpr uint16_t MAX_ADD_PAYLOAD = 256;
    if (size > MAX_ADD_PAYLOAD)
        return false;
    uint8_t zero[MAX_ADD_PAYLOAD] = {};
    return world->add_raw(e, c, zero, size);
}

class EcsReflectBridge {
public:
    static EcsReflectBridge& Get() { static EcsReflectBridge inst; return inst; }

    static constexpr int MAX_COMPONENTS = md::ComponentReflect::MAX_COMPONENTS;
    using DrawerFn = bool (*)(EcsBridgeWorldT*, EcsBridgeIdT, void*);

    // Re-resolves every reflected component's id against `world` and
    // rebinds any previously-registered custom drawers. Call on every
    // editor_panels_init() (initial load AND every F5/auto reload).
    void Init(EcsBridgeWorldT* world) {
        count_ = 0;
        const md::ComponentReflect& reg = md::ComponentReflect::Get();
        int n = reg.Count();
        for (int i = 0; i < n && count_ < MAX_COMPONENTS; ++i) {
            const md::ComponentDesc& desc = reg.GetDesc(i);
            char pascal[40];
            ToPascalCase(desc.name, pascal, sizeof(pascal));
            ids_[count_]   = EcsBridgeResolve(world, pascal);
            descs_[count_] = &desc;
            customs_[count_] = nullptr;
            ++count_;
        }
        // Rebind persistent custom-drawer bindings against the freshly
        // resolved descriptor list (names, not ids, are the stable key).
        for (int b = 0; b < custom_binding_count_; ++b) RebindOne(custom_binding_[b]);
    }

    int Count() const { return count_; }
    EcsBridgeIdT              Id(int i)   const { return ids_[i]; }
    const md::ComponentDesc&  Desc(int i) const { return *descs_[i]; }
    DrawerFn                  CustomFor(int i) const { return customs_[i]; }

    // Register (or replace) a custom drawer for a reflected component name.
    // Persists across Init() calls — rebound against the new id/desc list
    // each time. fn returns true if it edited the component (caller must
    // then call EcsBridgeModified(), defined in editor_reflect_inspector.cpp).
    void BindCustom(const char* reflect_name, DrawerFn fn) {
        for (int b = 0; b < custom_binding_count_; ++b) {
            if (strncmp(custom_binding_[b].name, reflect_name, sizeof(custom_binding_[b].name)) == 0) {
                custom_binding_[b].fn = fn;
                RebindOne(custom_binding_[b]);
                return;
            }
        }
        if (custom_binding_count_ >= MAX_CUSTOM_BINDINGS) return;
        CustomBinding& cb = custom_binding_[custom_binding_count_++];
        strncpy(cb.name, reflect_name, sizeof(cb.name) - 1);
        cb.name[sizeof(cb.name) - 1] = '\0';
        cb.fn = fn;
        RebindOne(cb);
    }

private:
    EcsReflectBridge() = default;

    // snake_case -> PascalCase, matching flecs's auto-derived (RTTI-based)
    // component names. Special-cases the "ai" segment -> "AI": codebase
    // convention keeps two-letter acronyms fully capitalized (AIAgent), which
    // plain per-segment capitalization ("Ai") would not reproduce.
    static void ToPascalCase(const char* snake, char* out, int out_size) {
        int o = 0;
        char buf[64];
        int bi = 0;
        auto flush_segment = [&]() {
            if (bi == 0) return;
            buf[bi] = '\0';
            if (bi == 2 && (buf[0] == 'a' || buf[0] == 'A') && (buf[1] == 'i' || buf[1] == 'I')) {
                if (o < out_size - 2) { out[o++] = 'A'; out[o++] = 'I'; }
            } else {
                for (int k = 0; k < bi && o < out_size - 1; ++k)
                    out[o++] = (k == 0) ? (char)toupper((unsigned char)buf[k]) : buf[k];
            }
            bi = 0;
        };
        for (int i = 0; ; ++i) {
            char c = snake[i];
            if (c == '_' || c == '\0') {
                flush_segment();
                if (c == '\0') break;
                continue;
            }
            if (bi < (int)sizeof(buf) - 1) buf[bi++] = c;
        }
        out[o] = '\0';
    }

    static constexpr int MAX_CUSTOM_BINDINGS = 8;
    struct CustomBinding { char name[32] = {}; DrawerFn fn = nullptr; };

    void RebindOne(const CustomBinding& cb) {
        for (int i = 0; i < count_; ++i) {
            if (strncmp(descs_[i]->name, cb.name, sizeof(descs_[i]->name)) == 0) {
                customs_[i] = cb.fn;
                return;
            }
        }
    }

    EcsBridgeIdT             ids_[MAX_COMPONENTS]     = {};
    const md::ComponentDesc* descs_[MAX_COMPONENTS]   = {};
    DrawerFn                 customs_[MAX_COMPONENTS] = {};
    int                      count_ = 0;

    CustomBinding custom_binding_[MAX_CUSTOM_BINDINGS] = {};
    int           custom_binding_count_ = 0;
};
#endif
