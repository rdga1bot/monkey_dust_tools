#pragma once
#ifdef MONKEY_DUST_EDITOR
// EcsReflectBridge — resolves md::ComponentReflect entries against a live
// ecs_world_t*, entirely through the untyped flecs C API.
//
// Why untyped: libeditor_panels.so does not link monkey_dust::engine
// statically (dlopen-resolved symbols only), so caching a flecs component id
// via flecs::_::type<T>::id() inside this .so would instantiate the type's
// registration independently of the host binary's copy — the exact
// vague-linkage risk editor_module.h's Config::ecs_world now avoids for the
// world pointer itself. ecs_lookup() by name is the boundary-safe path.
//
// Ids are NOT stable across a dlopen/dlclose cycle for this .so's own
// statics (they reset), so Init() must be called fresh from
// editor_panels_init() on every load/reload — never memoize ids past that.
#include <monkey_dust/ecs/component_reflect.h>
#include <flecs.h>
#include <cstring>
#include <cctype>

class EcsReflectBridge {
public:
    static EcsReflectBridge& Get() { static EcsReflectBridge inst; return inst; }

    static constexpr int MAX_COMPONENTS = md::ComponentReflect::MAX_COMPONENTS;
    using DrawerFn = bool (*)(ecs_world_t*, ecs_entity_t, void*);

    // Re-resolves every reflected component's flecs id against `world` and
    // rebinds any previously-registered custom drawers. Call on every
    // editor_panels_init() (initial load AND every F5/auto reload).
    void Init(ecs_world_t* world) {
        count_ = 0;
        const md::ComponentReflect& reg = md::ComponentReflect::Get();
        int n = reg.Count();
        for (int i = 0; i < n && count_ < MAX_COMPONENTS; ++i) {
            const md::ComponentDesc& desc = reg.GetDesc(i);
            char pascal[40];
            ToPascalCase(desc.name, pascal, sizeof(pascal));
            ecs_entity_t id = world ? ecs_lookup(world, pascal) : 0;
            ids_[count_]   = id;
            descs_[count_] = &desc;
            customs_[count_] = nullptr;
            ++count_;
        }
        // Rebind persistent custom-drawer bindings against the freshly
        // resolved descriptor list (names, not ids, are the stable key).
        for (int b = 0; b < custom_binding_count_; ++b) RebindOne(custom_binding_[b]);
    }

    int Count() const { return count_; }
    ecs_entity_t              Id(int i)   const { return ids_[i]; }
    const md::ComponentDesc&  Desc(int i) const { return *descs_[i]; }
    DrawerFn                  CustomFor(int i) const { return customs_[i]; }

    // Register (or replace) a custom drawer for a reflected component name.
    // Persists across Init() calls — rebound against the new id/desc list
    // each time. fn returns true if it edited the component (caller must
    // then call ecs_modified_id).
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

    ecs_entity_t             ids_[MAX_COMPONENTS]     = {};
    const md::ComponentDesc* descs_[MAX_COMPONENTS]   = {};
    DrawerFn                 customs_[MAX_COMPONENTS] = {};
    int                      count_ = 0;

    CustomBinding custom_binding_[MAX_CUSTOM_BINDINGS] = {};
    int           custom_binding_count_ = 0;
};
#endif
