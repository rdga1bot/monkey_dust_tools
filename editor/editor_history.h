#pragma once
#ifdef MONKEY_DUST_EDITOR
#include <entt/entt.hpp>
#include <cstdint>
#include <cstring>

struct Command {
    void (*execute)(void* data, entt::registry& reg) = nullptr;
    void (*undo)(void* data, entt::registry& reg)    = nullptr;
    uint8_t data[64] = {};
    bool valid = false;
};

struct CommandStack {
    static constexpr int MAX_UNDO = 64;
    Command stack[MAX_UNDO] = {};
    int top      = 0;
    int redo_top = 0;

    void Push(Command c) {
        c.valid = true;
        stack[top % MAX_UNDO] = c;
        ++top;
        redo_top = top;
    }
    void Undo(entt::registry& reg) {
        if (top <= 0) return;
        --top;
        auto& cmd = stack[top % MAX_UNDO];
        if (cmd.valid && cmd.undo) cmd.undo(cmd.data, reg);
    }
    void Redo(entt::registry& reg) {
        if (top >= redo_top) return;
        auto& cmd = stack[top % MAX_UNDO];
        if (cmd.valid && cmd.execute) cmd.execute(cmd.data, reg);
        ++top;
    }
    void Clear() { top = 0; redo_top = 0; }
};
#endif
