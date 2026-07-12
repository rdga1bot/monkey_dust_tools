#pragma once
#ifdef MONKEY_DUST_EDITOR
#include <monkey_dust/ecs/md_registry.h>
#include <cstdint>
#include <cstring>

struct Command {
    void (*execute)(void* data, MdRegistry& reg) = nullptr;
    void (*undo)(void* data, MdRegistry& reg)    = nullptr;
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
    void Undo(MdRegistry& reg) {
        if (top <= 0) return;
        --top;
        auto& cmd = stack[top % MAX_UNDO];
        if (cmd.valid && cmd.undo) cmd.undo(cmd.data, reg);
    }
    void Redo(MdRegistry& reg) {
        if (top >= redo_top) return;
        auto& cmd = stack[top % MAX_UNDO];
        if (cmd.valid && cmd.execute) cmd.execute(cmd.data, reg);
        ++top;
    }
    void Clear() { top = 0; redo_top = 0; }
};
#endif
