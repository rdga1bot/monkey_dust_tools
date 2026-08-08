#pragma once
// JSON save/load section of character_editor.h's split. Included ONLY from
// character_editor.h, inside namespace CharacterEditor { ... }.

// ── JSON save/load ────────────────────────────────────────────────────────────
static float s_parse_f(const char* b, const char* k) {
    const char* p = strstr(b, k); if (!p) return 0.f;
    p = strchr(p, ':'); return p ? (float)atof(p + 1) : 0.f;
}
static int s_parse_i(const char* b, const char* k) {
    const char* p = strstr(b, k); if (!p) return 0;
    p = strchr(p, ':'); return p ? atoi(p + 1) : 0;
}
static void s_parse_str(const char* b, const char* k, char* out, int sz) {
    const char* p = strstr(b, k); if (!p) { out[0]='\0'; return; }
    p = strchr(p, ':'); if (!p) { out[0]='\0'; return; }
    while (*p && (*p==':'||*p==' '||*p=='"')) ++p;
    int n=0; while (*p && *p!='"' && n<sz-1) out[n++]=*p++;
    out[n]='\0';
}
static void LoadMorphNames(const char* /*path*/) {}  // names are hardcoded; file ignored
static bool LoadJSON(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) return false;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz <= 0 || sz > 16384) { fclose(f); return false; }
    char* buf = (char*)malloc((size_t)sz + 1);
    fread(buf, 1, (size_t)sz, f); buf[sz] = '\0'; fclose(f);

    s_parse_str(buf, "\"name\"", s_def.name, 32);
    s_def.sex      = (uint8_t)s_parse_i(buf, "\"sex\"");
    s_def.race_row = (uint8_t)s_parse_i(buf, "\"race_row\"");
    if (s_def.race_row >= RACE_COUNT) s_def.race_row = 0;
    s_def.skin_rgb[0]    = s_parse_f(buf, "\"skin_r\"");
    s_def.skin_rgb[1]    = s_parse_f(buf, "\"skin_g\"");
    s_def.skin_rgb[2]    = s_parse_f(buf, "\"skin_b\"");
    s_def.hair_rgb[0]    = s_parse_f(buf, "\"hair_r\"");
    s_def.hair_rgb[1]    = s_parse_f(buf, "\"hair_g\"");
    s_def.hair_rgb[2]    = s_parse_f(buf, "\"hair_b\"");
    s_def.color_strength = s_parse_f(buf, "\"color_str\"");
    if (s_def.color_strength < 0.01f) s_def.color_strength = 0.55f;

    auto load_arr = [&](const char* key, float* arr, int n, const float* defs) {
        const char* p = strstr(buf, key);
        if (p) { p = strchr(p, '['); if (p) { ++p;
            for (int i = 0; i < n; ++i) {
                while (*p && (*p==' '||*p==',')) ++p;
                if (!*p || *p == ']') break;
                arr[i] = (float)atof(p);
                while (*p && *p!=',' && *p!=']') ++p;
            }
        }} else { for (int i=0;i<n;++i) arr[i]=defs[i]; }
    };
    load_arr("\"body\"",   s_def.body,   BODY_N, kBodyDef);
    load_arr("\"face\"",   s_def.face,   FACE_N, kFaceDef);
    load_arr("\"hair_f\"", s_def.hair_f, HAIR_N, kHairDef);

    // Clamp loaded values to valid Kenshi ranges; resets stale saves to neutral
    for (int i = 0; i < BODY_N; ++i)
        if (s_def.body[i] < kBodyLo[i] || s_def.body[i] > kBodyHi[i])
            s_def.body[i] = kBodyDef[i];
    for (int i = 0; i < FACE_N; ++i)
        if (s_def.face[i] < kFaceLo[i] || s_def.face[i] > kFaceHi[i])
            s_def.face[i] = kFaceDef[i];

    // KEN-ATTR-1: load attr_spent[]
    {
        const char* p = strstr(buf, "\"attr_spent\"");
        if (p) { p = strchr(p, '['); if (p) { ++p;
            for (int i = 0; i < 7; ++i) {
                while (*p && (*p == ' ' || *p == ',')) ++p;
                if (!*p || *p == ']') break;
                int v = atoi(p);
                s_def.attr_spent[i] = (int8_t)(v < 0 ? 0 : (v > ATTR_FREE_TOTAL ? ATTR_FREE_TOTAL : v));
                while (*p && *p != ',' && *p != ']') ++p;
            }
        }} else { for (int i = 0; i < 7; ++i) s_def.attr_spent[i] = 0; }
    }

    free(buf); return true;
}
static bool SaveJSON(const char* path) {
    FILE* f = fopen(path, "w"); if (!f) return false;
    fprintf(f, "{\n  \"name\": \"%s\",\n  \"sex\": %d,\n  \"race_row\": %d,\n",
            s_def.name, s_def.sex, s_def.race_row);
    fprintf(f, "  \"skin_r\": %.4f, \"skin_g\": %.4f, \"skin_b\": %.4f,\n",
            s_def.skin_rgb[0], s_def.skin_rgb[1], s_def.skin_rgb[2]);
    fprintf(f, "  \"hair_r\": %.4f, \"hair_g\": %.4f, \"hair_b\": %.4f,\n",
            s_def.hair_rgb[0], s_def.hair_rgb[1], s_def.hair_rgb[2]);
    fprintf(f, "  \"color_str\": %.4f,\n", s_def.color_strength);
    auto save_arr = [&](const char* key, const float* arr, int n, bool comma) {
        fprintf(f, "  \"%s\": [", key);
        for (int i=0;i<n;++i) fprintf(f, "%s%.2f", i?",":"", arr[i]);
        fprintf(f, "]%s\n", comma ? "," : "");
    };
    save_arr("body",   s_def.body,   BODY_N, true);
    save_arr("face",   s_def.face,   FACE_N, true);
    save_arr("hair_f", s_def.hair_f, HAIR_N, true);
    // KEN-ATTR-1: save attr_spent[]
    fprintf(f, "  \"attr_spent\": [");
    for (int i = 0; i < 7; ++i) fprintf(f, "%s%d", i ? "," : "", (int)s_def.attr_spent[i]);
    fprintf(f, "]\n");
    fprintf(f, "}\n"); fclose(f); return true;
}
