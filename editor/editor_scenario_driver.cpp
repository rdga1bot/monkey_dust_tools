#include "editor_scenario_driver.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

bool ParseEditorScenarioArgs(int argc, char** argv, EditorScenarioConfig& cfg) {
    for (int i = 1; i < argc; ++i) {
        const char* a = argv[i];
        if (strcmp(a, "--exec") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--exec requires a script path\n"); return false; }
            cfg.active = true;
            strncpy(cfg.script_path, argv[++i], sizeof(cfg.script_path) - 1);
        } else if (strcmp(a, "--seed") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--seed requires a value\n"); return false; }
            cfg.seed = (unsigned)strtoul(argv[++i], nullptr, 10);
        } else if (strcmp(a, "--max-frames") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--max-frames requires a value\n"); return false; }
            cfg.max_frames = strtol(argv[++i], nullptr, 10);
        } else if (strcmp(a, "--max-seconds") == 0) {
            if (i + 1 >= argc) { fprintf(stderr, "--max-seconds requires a value\n"); return false; }
            cfg.max_seconds = strtod(argv[++i], nullptr);
        } else if (strcmp(a, "--fast") == 0) {
            cfg.fast = true;
        } else if (strcmp(a, "--headless") == 0) {
            cfg.headless = true;
        }
        // Unrecognized args ignored — keeps the door open for engine/SDL flags.
    }
    return true;
}
