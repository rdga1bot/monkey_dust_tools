// Entry point for the monkey_dust_flare_convert offline tool (Phase 35).
// Usage: monkey_dust_flare_convert <items_dir> <enemies_dir> <powers_cat_dir> <out_dir>
//
// Example (from repo root):
//   ./build/monkey_dust_flare_convert \
//     third_party/flare-game/mods/empyrean_campaign/items \
//     third_party/flare-game/mods/empyrean_campaign/enemies \
//     third_party/flare-game/mods/empyrean_campaign/powers/categories \
//     data/flare

#include "flare_ini_converter.h"
#include <cstdio>
#include <sys/stat.h>

static void mkdir_p(const char* path) {
#ifdef _WIN32
    _mkdir(path);
#else
    mkdir(path, 0755);
#endif
}

int main(int argc, char* argv[]) {
    if (argc < 5) {
        printf("Usage: %s <items_dir> <enemies_dir> <powers_cat_dir> <out_dir>\n", argv[0]);
        return 1;
    }
    const char* items_dir   = argv[1];
    const char* enemies_dir = argv[2];
    const char* powers_dir  = argv[3];
    const char* out_dir     = argv[4];

    mkdir_p(out_dir);

    char buf[512];
    int ok = 0;

    snprintf(buf, 512, "%s/items.json", out_dir);
    if (FlareConvertItems(items_dir, buf))   ++ok;

    snprintf(buf, 512, "%s/enemies.json", out_dir);
    if (FlareConvertEnemies(enemies_dir, buf)) ++ok;

    snprintf(buf, 512, "%s/powers.json", out_dir);
    if (FlareConvertPowers(powers_dir, buf))  ++ok;

    printf("flare_convert: %d/3 outputs written to %s\n", ok, out_dir);
    return (ok == 3) ? 0 : 1;
}
