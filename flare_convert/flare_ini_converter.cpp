// Phase 35.B-D: FLARE INI → monkey_dust JSON converter implementation.
// Offline tool only — never linked into the game binary.
// Attribution: source data from flare-game Empyrean Campaign (CC BY-SA 3.0).
// https://github.com/flareteam/flare-game

#include "flare_ini_converter.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <dirent.h>
#include <sys/stat.h>

// ─── internal helpers ────────────────────────────────────────────────────────

static void str_trim(char* s) {
    int n = (int)strlen(s);
    while (n > 0 && (unsigned char)s[n-1] <= 32) s[--n] = '\0';
    char* p = s;
    while ((unsigned char)*p <= 32 && *p) ++p;
    if (p != s) memmove(s, p, strlen(p) + 1);
}

static void path_dirname(const char* path, char* out, int sz) {
    strncpy(out, path, sz - 1); out[sz - 1] = '\0';
    int n = (int)strlen(out);
    while (n > 1 && out[n-1] == '/') out[--n] = '\0';
    char* sep = strrchr(out, '/');
    if (sep) { *sep = '\0'; }
    else     { out[0] = '.'; out[1] = '\0'; }
}

static void path_join(char* out, int sz, const char* a, const char* b) {
    snprintf(out, sz, "%s/%s", a, b);
}

static bool has_suffix(const char* s, const char* suf) {
    int sl = (int)strlen(s), fl = (int)strlen(suf);
    return sl >= fl && strcmp(s + sl - fl, suf) == 0;
}

static void json_str(FILE* f, const char* s) {
    fputc('"', f);
    for (const char* p = s; *p; ++p) {
        if (*p == '"' || *p == '\\') fputc('\\', f);
        if ((unsigned char)*p >= 32) fputc(*p, f);
    }
    fputc('"', f);
}

static void strip_trailing_slash(char* s) {
    int n = (int)strlen(s);
    while (n > 1 && s[n-1] == '/') s[--n] = '\0';
}

// ─── Items ───────────────────────────────────────────────────────────────────

struct ItemRec {
    int  id;
    char name[64];
    int  level;
    int  price;
    char dmg_type[8];   // "melee" / "ranged" / "ment" / ""
    int  dmg_min, dmg_max;
    int  abs_min, abs_max;
    char req_stat[16];
    int  req_val;
};

static bool item_valid(const ItemRec& r) { return r.id > 0 && r.name[0]; }
static void item_reset(ItemRec& r)       { memset(&r, 0, sizeof(r)); }

// Depth 0 = items.txt: INCLUDEs are always cross-file (between items) → flush+recurse.
// Depth > 0 = categories/*.txt: INCLUDEs are always within-[item] base templates
//             (icon/gfx/soundfx only) → skip.
static void parse_items_file(
    const char* fpath, const char* mod_root,
    ItemRec* out, int* cnt, int max_cnt, int depth)
{
    if (depth > 4) return;
    FILE* f = fopen(fpath, "r");
    if (!f) return;  // missing base/ templates from other mods — expected, skip quietly

    char line[512];
    ItemRec cur; item_reset(cur);
    bool in_item = false;

    while (fgets(line, sizeof(line), f)) {
        str_trim(line);
        if (!line[0] || line[0] == '#') continue;

        if (strncmp(line, "INCLUDE ", 8) == 0) {
            if (depth == 0) {
                // items.txt: INCLUDEs appear between [item] blocks → flush and recurse
                if (item_valid(cur) && *cnt < max_cnt) out[(*cnt)++] = cur;
                item_reset(cur); in_item = false;
                char sub[512]; path_join(sub, 512, mod_root, line + 8);
                parse_items_file(sub, mod_root, out, cnt, max_cnt, depth + 1);
            }
            // depth > 0: within-[item] base template → skip
            continue;
        }

        if (strcmp(line, "[item]") == 0) {
            if (item_valid(cur) && *cnt < max_cnt) out[(*cnt)++] = cur;
            item_reset(cur); in_item = true;
            continue;
        }

        if (!in_item) continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0'; char* key = line; char* val = eq + 1;
        str_trim(key); str_trim(val);

        if (!strcmp(key, "id")) {
            cur.id = (int)strtol(val, nullptr, 10);
        } else if (!strcmp(key, "name")) {
            strncpy(cur.name, val, 63);
        } else if (!strcmp(key, "level")) {
            cur.level = (int)strtol(val, nullptr, 10);
        } else if (!strcmp(key, "price")) {
            cur.price = (int)strtol(val, nullptr, 10);  // ,player_level:X suffix ignored by strtol
        } else if (!strcmp(key, "dmg")) {
            // dmg=melee,MIN,MAX  (or ranged / ment)
            char tmp[64]; strncpy(tmp, val, 63);
            char* c1 = strchr(tmp, ',');
            if (c1) {
                *c1 = '\0'; strncpy(cur.dmg_type, tmp, 7);
                char* c2 = strchr(c1 + 1, ',');
                if (c2) {
                    *c2 = '\0';
                    cur.dmg_min = (int)strtol(c1 + 1, nullptr, 10);
                    cur.dmg_max = (int)strtol(c2 + 1, nullptr, 10);
                }
            }
        } else if (!strcmp(key, "abs")) {
            // abs=MIN,MAX
            char* c = strchr(val, ',');
            if (c) {
                *c = '\0';
                cur.abs_min = (int)strtol(val,   nullptr, 10);
                cur.abs_max = (int)strtol(c + 1, nullptr, 10);
            }
        } else if (!strcmp(key, "requires_stat")) {
            // requires_stat=STAT_NAME,VALUE
            char* c = strchr(val, ',');
            if (c) {
                *c = '\0'; strncpy(cur.req_stat, val, 15);
                cur.req_val = (int)strtol(c + 1, nullptr, 10);
            }
        }
    }
    if (item_valid(cur) && *cnt < max_cnt) out[(*cnt)++] = cur;
    fclose(f);
}

bool FlareConvertItems(const char* flare_items_dir, const char* out_json_path) {
    static ItemRec items[1024];
    int count = 0;

    char dir[512]; strncpy(dir, flare_items_dir, 511); strip_trailing_slash(dir);
    char mod_root[512]; path_dirname(dir, mod_root, 512);
    char items_txt[512]; path_join(items_txt, 512, dir, "items.txt");

    parse_items_file(items_txt, mod_root, items, &count, 1024, 0);

    if (count == 0) {
        fprintf(stderr, "[WARNING] FlareConvertItems: no items from %s", flare_items_dir);
        return false;
    }

    FILE* out = fopen(out_json_path, "w");
    if (!out) {
        fprintf(stderr, "[WARNING] FlareConvertItems: cannot write %s", out_json_path);
        return false;
    }
    fprintf(out, "[\n");
    for (int i = 0; i < count; ++i) {
        const ItemRec& r = items[i];
        fprintf(out, "  {\"id\":%d,\"name\":", r.id);
        json_str(out, r.name);
        fprintf(out, ",\"level\":%d,\"price\":%d", r.level, r.price);
        if (r.dmg_type[0])
            fprintf(out, ",\"dmg_type\":\"%s\",\"dmg_min\":%d,\"dmg_max\":%d",
                    r.dmg_type, r.dmg_min, r.dmg_max);
        if (r.abs_min || r.abs_max)
            fprintf(out, ",\"abs_min\":%d,\"abs_max\":%d", r.abs_min, r.abs_max);
        if (r.req_stat[0])
            fprintf(out, ",\"req_stat\":\"%s\",\"req_val\":%d", r.req_stat, r.req_val);
        fprintf(out, "}%s\n", (i + 1 < count) ? "," : "");
    }
    fprintf(out, "]\n");
    fclose(out);
    fprintf(stderr, "[INFO] FlareConvertItems: %d items → %s", count, out_json_path);
    return true;
}

// ─── Enemies ─────────────────────────────────────────────────────────────────

struct EnemyRec {
    char  name[64];
    char  category[64];  // filename without .txt
    int   level;
    int   hp;
    int   dmg_melee_min, dmg_melee_max;
    int   absorb_min, absorb_max;
    float speed;         // m/s (FLARE speed field is already in tiles/s ≈ m/s)
};

static bool enemy_valid(const EnemyRec& r) { return r.name[0] && r.level > 0; }

static void parse_enemy_file(const char* fpath, const char* fname, EnemyRec& r) {
    memset(&r, 0, sizeof(r));
    strncpy(r.category, fname, 63);
    char* dot = strrchr(r.category, '.');
    if (dot) *dot = '\0';

    FILE* f = fopen(fpath, "r");
    if (!f) { fprintf(stderr, "[WARNING] FlareConvertEnemies: cannot open %s", fpath); return; }

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        str_trim(line);
        if (!line[0] || line[0] == '#') continue;
        if (strncmp(line, "INCLUDE ", 8) == 0) continue;  // base = visual/audio fields only

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0'; char* key = line; char* val = eq + 1;
        str_trim(key); str_trim(val);

        if (!strcmp(key, "name")) {
            strncpy(r.name, val, 63);
        } else if (!strcmp(key, "level")) {
            r.level = (int)strtol(val, nullptr, 10);
        } else if (!strcmp(key, "speed")) {
            r.speed = strtof(val, nullptr);
        } else if (!strcmp(key, "stat")) {
            // stat=FIELD,VALUE
            char tmp[64]; strncpy(tmp, val, 63);
            char* c = strchr(tmp, ',');
            if (!c) continue;
            *c = '\0';
            int v = (int)strtol(c + 1, nullptr, 10);
            if      (!strcmp(tmp, "hp"))            r.hp            = v;
            else if (!strcmp(tmp, "dmg_melee_min")) r.dmg_melee_min = v;
            else if (!strcmp(tmp, "dmg_melee_max")) r.dmg_melee_max = v;
            else if (!strcmp(tmp, "absorb_min"))    r.absorb_min    = v;
            else if (!strcmp(tmp, "absorb_max"))    r.absorb_max    = v;
        }
    }
    fclose(f);
}

bool FlareConvertEnemies(const char* flare_enemies_dir, const char* out_json_path) {
    static EnemyRec enemies[256];
    int count = 0;

    char dir[512]; strncpy(dir, flare_enemies_dir, 511); strip_trailing_slash(dir);

    DIR* d = opendir(dir);
    if (!d) {
        fprintf(stderr, "[WARNING] FlareConvertEnemies: cannot open %s", flare_enemies_dir);
        return false;
    }

    struct dirent* ent;
    while ((ent = readdir(d)) && count < 256) {
        if (ent->d_name[0] == '.') continue;
        if (!has_suffix(ent->d_name, ".txt")) continue;
        char fpath[512]; path_join(fpath, 512, dir, ent->d_name);
        struct stat st;
        if (stat(fpath, &st) != 0 || S_ISDIR(st.st_mode)) continue;

        EnemyRec r;
        parse_enemy_file(fpath, ent->d_name, r);
        if (enemy_valid(r)) {
            enemies[count++] = r;
        } else {
            fprintf(stderr, "[WARNING] FlareConvertEnemies: skipped %s (missing name/level)", ent->d_name);
        }
    }
    closedir(d);

    if (count == 0) {
        fprintf(stderr, "[WARNING] FlareConvertEnemies: no enemies from %s", flare_enemies_dir);
        return false;
    }

    FILE* out = fopen(out_json_path, "w");
    if (!out) {
        fprintf(stderr, "[WARNING] FlareConvertEnemies: cannot write %s", out_json_path);
        return false;
    }
    fprintf(out, "[\n");
    for (int i = 0; i < count; ++i) {
        const EnemyRec& r = enemies[i];
        fprintf(out, "  {\"category\":\"%s\",\"name\":", r.category);
        json_str(out, r.name);
        fprintf(out,
            ",\"level\":%d,\"hp\":%d"
            ",\"dmg_melee_min\":%d,\"dmg_melee_max\":%d"
            ",\"absorb_min\":%d,\"absorb_max\":%d"
            ",\"speed\":%.2f}%s\n",
            r.level, r.hp,
            r.dmg_melee_min, r.dmg_melee_max,
            r.absorb_min, r.absorb_max,
            r.speed,
            (i + 1 < count) ? "," : "");
    }
    fprintf(out, "]\n");
    fclose(out);
    fprintf(stderr, "[INFO] FlareConvertEnemies: %d enemies → %s", count, out_json_path);
    return true;
}

// ─── Powers ──────────────────────────────────────────────────────────────────

struct PowerRec {
    int   id;
    char  name[64];
    char  dmg_type[8];   // "melee" / "ranged" / ""
    float radius;
    int   cooldown_ms;
};

static bool power_valid(const PowerRec& r) { return r.id > 0 && r.name[0]; }
static void power_reset(PowerRec& r)       { memset(&r, 0, sizeof(r)); }

// Apply one base-template line into a PowerRec (called from inline INCLUDE expansion).
static void apply_power_field(PowerRec& cur, const char* key, const char* val) {
    if      (!strcmp(key, "name"))        strncpy(cur.name,     val, 63);
    else if (!strcmp(key, "base_damage")) strncpy(cur.dmg_type, val,  7);
    else if (!strcmp(key, "radius"))      cur.radius      = strtof(val, nullptr);
    else if (!strcmp(key, "cooldown")) {
        // "1500ms" → 1500 ms;  "40s" → 40000 ms
        if (strstr(val, "ms")) cur.cooldown_ms = (int)strtol(val, nullptr, 10);
        else                   cur.cooldown_ms = (int)(strtof(val, nullptr) * 1000.0f);
    }
}

static void parse_powers_file(
    const char* fpath, const char* mod_root,
    PowerRec* out, int* cnt, int max_cnt)
{
    FILE* f = fopen(fpath, "r");
    if (!f) { fprintf(stderr, "[WARNING] FlareConvertPowers: cannot open %s", fpath); return; }

    char line[512];
    PowerRec cur; power_reset(cur);
    bool in_power = false;

    while (fgets(line, sizeof(line), f)) {
        str_trim(line);
        if (!line[0] || line[0] == '#') continue;

        if (strncmp(line, "INCLUDE ", 8) == 0) {
            if (in_power) {
                // Inline-expand base template: read its fields directly into cur.
                // Main-file fields that follow will override these.
                char sub[512]; path_join(sub, 512, mod_root, line + 8);
                FILE* bf = fopen(sub, "r");
                if (bf) {
                    char bline[512];
                    while (fgets(bline, sizeof(bline), bf)) {
                        str_trim(bline);
                        if (!bline[0] || bline[0] == '#') continue;
                        if (strncmp(bline, "INCLUDE ", 8) == 0) continue;
                        char* beq = strchr(bline, '=');
                        if (!beq) continue;
                        *beq = '\0'; char* bk = bline; char* bv = beq + 1;
                        str_trim(bk); str_trim(bv);
                        apply_power_field(cur, bk, bv);
                    }
                    fclose(bf);
                }
            }
            continue;
        }

        if (strcmp(line, "[power]") == 0) {
            if (power_valid(cur) && *cnt < max_cnt) out[(*cnt)++] = cur;
            power_reset(cur); in_power = true;
            continue;
        }

        if (!in_power) continue;

        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0'; char* key = line; char* val = eq + 1;
        str_trim(key); str_trim(val);
        apply_power_field(cur, key, val);

        // id is only in the main file, not base templates
        if (!strcmp(key, "id")) cur.id = (int)strtol(val, nullptr, 10);
    }
    if (power_valid(cur) && *cnt < max_cnt) out[(*cnt)++] = cur;
    fclose(f);
}

bool FlareConvertPowers(const char* flare_powers_categories_dir, const char* out_json_path) {
    static PowerRec powers[256];
    int count = 0;

    char dir[512]; strncpy(dir, flare_powers_categories_dir, 511); strip_trailing_slash(dir);

    // mod root is two levels up: categories/ → powers/ → mod root
    char mod_root[512];
    { char tmp[512]; path_dirname(dir, tmp, 512); path_dirname(tmp, mod_root, 512); }

    DIR* d = opendir(dir);
    if (!d) {
        fprintf(stderr, "[WARNING] FlareConvertPowers: cannot open %s", flare_powers_categories_dir);
        return false;
    }

    struct dirent* ent;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        if (!has_suffix(ent->d_name, ".txt")) continue;
        char fpath[512]; path_join(fpath, 512, dir, ent->d_name);
        struct stat st;
        if (stat(fpath, &st) != 0 || S_ISDIR(st.st_mode)) continue;
        parse_powers_file(fpath, mod_root, powers, &count, 256);
    }
    closedir(d);

    if (count == 0) {
        fprintf(stderr, "[WARNING] FlareConvertPowers: no powers from %s", flare_powers_categories_dir);
        return false;
    }

    FILE* out = fopen(out_json_path, "w");
    if (!out) {
        fprintf(stderr, "[WARNING] FlareConvertPowers: cannot write %s", out_json_path);
        return false;
    }
    fprintf(out, "[\n");
    for (int i = 0; i < count; ++i) {
        const PowerRec& r = powers[i];
        fprintf(out, "  {\"id\":%d,\"name\":", r.id);
        json_str(out, r.name);
        fprintf(out, ",\"dmg_type\":\"%s\",\"radius\":%.2f,\"cooldown_ms\":%d}%s\n",
                r.dmg_type, r.radius, r.cooldown_ms,
                (i + 1 < count) ? "," : "");
    }
    fprintf(out, "]\n");
    fclose(out);
    fprintf(stderr, "[INFO] FlareConvertPowers: %d powers → %s", count, out_json_path);
    return true;
}
