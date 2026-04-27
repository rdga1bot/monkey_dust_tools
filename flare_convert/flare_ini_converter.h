#pragma once
// Offline FLARE INI → monkey_dust JSON converter (Phase 35).
// NOT included in the game loop. Invoked once via --flare-convert CLI arg
// or the monkey_dust_flare_convert CMake target.
//
// Rules:
//   - FILE* + strstr + strtol/strtof only. No external JSON/INI libs.
//   - static arrays inside each function. No heap alloc.
//   - On invalid/unknown record: TraceLog(LOG_WARNING, ...) + skip. Never crash.
//   - Converter caps: MAX_ITEMS=1024, MAX_ENEMIES=256, MAX_POWERS=256.
//   - Game runtime limit (items the engine loads): 512.
//
// Attribution: source data from flare-game Empyrean Campaign (CC BY-SA 3.0).
// https://github.com/flareteam/flare-game

// Convert all items from a FLARE items directory.
// Reads every *.txt file in flare_items_dir (and sub-dirs via INCLUDE).
// Writes a JSON array to out_json_path.
// Returns true on success (≥1 item written).
bool FlareConvertItems(const char* flare_items_dir, const char* out_json_path);

// Convert all enemy definitions from a FLARE enemies directory.
// Reads every *.txt file in flare_enemies_dir (skips base/ sub-dir).
// Writes a JSON array to out_json_path.
// Returns true on success (≥1 enemy written).
bool FlareConvertEnemies(const char* flare_enemies_dir, const char* out_json_path);

// Convert all combat powers from a FLARE powers/categories directory.
// Pass the categories sub-dir directly (powers.txt itself is INCLUDE-only).
// Writes a JSON array to out_json_path.
// Returns true on success (≥1 power written).
bool FlareConvertPowers(const char* flare_powers_categories_dir, const char* out_json_path);
