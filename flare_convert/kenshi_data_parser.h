#pragma once
// kenshi_data_parser.h — Kenshi FCS binary format parser (gamedata.base, *.mod)
// Phase 4, Task 4.1
//
// Binary format (reconstructed from hex analysis of gamedata.base):
//
//   File header (variable):
//     [uint32 tag=0x10][uint32 flags][uint32 pad][uint32 pad]
//     [uint32 dep_pad][uint32 dep_len][char dep[dep_len]][uint64 dep_hash]
//     ... variable bytes before first record
//
//   Per record:
//     [uint32 type_id_len][char type_id[N]]       // e.g. "DIALOGUE_LINE6071"
//     [uint32 ref_id_len][char ref_id[M]]          // e.g. "6071-Dialogue.mod"
//     [uint32 flags][uint32 pad]
//     [uint32 float_count]
//       × [uint32 key_len][char key[key_len]][float32 value]
//     [uint32 int_count]
//       × [uint32 key_len][char key[key_len]][int32 value]
//     [uint32 typeA_count=0][uint32 typeB_count=0]  // reserved, skip
//     [uint32 string_count]
//       × [uint32 key_len][char key[key_len]][uint32 str_len][char str[str_len]]
//     [uint32 ref_count]
//       × [uint32 key_len][char key[key_len]][uint32 ref_len][char ref[ref_len]]
//     [uint32 child_count]
//       × [nested record...]

#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>

static constexpr int KEN_MAX_KEY     = 64;
static constexpr int KEN_MAX_STR     = 256;
static constexpr int KEN_MAX_FIELDS  = 48;
static constexpr int KEN_MAX_REFS    = 16;
static constexpr int KEN_MAX_RECORDS = 4096;
static constexpr int KEN_TYPE_LEN    = 48;
static constexpr int KEN_MAX_DEPTH   = 4;

// ── Field value (tagged union) ────────────────────────────────────────────────
enum class KenFT : uint8_t { None=0, Float=1, Int=2, Str=3, Ref=4 };

struct KenField {
    char  key[KEN_MAX_KEY] = {};
    KenFT type = KenFT::None;
    union { float f; int32_t i; };
    char  s[KEN_MAX_STR] = {};
};

// ── Parsed record ─────────────────────────────────────────────────────────────
struct KenRecord {
    char     type_class[KEN_TYPE_LEN] = {};  // e.g. "DIALOGUE_LINE"
    char     num_id[16]               = {};  // e.g. "6071"
    char     ref_id[KEN_MAX_STR]     = {};  // e.g. "6071-Dialogue.mod"
    KenField fields[KEN_MAX_FIELDS];
    int      field_count  = 0;
    int      child_start  = 0;  // index into KenParser::records_[]
    int      child_count  = 0;
};

// ── Low-level byte reader ─────────────────────────────────────────────────────
struct KenReader {
    const uint8_t* data;
    size_t         size;
    size_t         pos;

    bool ok() const { return pos < size; }

    uint32_t u32() {
        if (pos + 4 > size) { pos = size; return 0; }
        uint32_t v = (uint32_t)data[pos]
                   | ((uint32_t)data[pos+1] << 8)
                   | ((uint32_t)data[pos+2] << 16)
                   | ((uint32_t)data[pos+3] << 24);
        pos += 4;
        return v;
    }
    int32_t i32() { return (int32_t)u32(); }
    float   f32() { uint32_t v = u32(); float f; memcpy(&f, &v, 4); return f; }

    bool str(char* out, int cap) {
        uint32_t len = u32();
        if (len == 0) { if (out) out[0] = 0; return true; }
        if (len > 4096 || pos + len > size) { pos = size; return false; }
        if (out) {
            uint32_t copy = len < (uint32_t)(cap - 1) ? len : (uint32_t)(cap - 1);
            memcpy(out, data + pos, copy);
            out[copy] = 0;
        }
        pos += len;
        return true;
    }

    void skip(size_t n) { pos = (pos + n <= size) ? pos + n : size; }
};

// ── Parser ────────────────────────────────────────────────────────────────────
class KenParser {
public:
    KenRecord records_[KEN_MAX_RECORDS];
    int       record_count_ = 0;
    char      error_[128]   = {};

    // Load file into memory-owned buffer and parse.
    bool LoadFile(const char* path) {
        FILE* f = fopen(path, "rb");
        if (!f) { snprintf(error_, sizeof(error_), "cannot open: %s", path); return false; }
        fseek(f, 0, SEEK_END);
        long fsz = ftell(f);
        rewind(f);
        if (fsz <= 0 || fsz > 32*1024*1024) {
            fclose(f);
            snprintf(error_, sizeof(error_), "file size out of range: %ld", fsz);
            return false;
        }
        delete[] buf_;
        buf_     = new uint8_t[(size_t)fsz];
        buf_sz_  = (size_t)fsz;
        if (fread(buf_, 1, buf_sz_, f) != buf_sz_) {
            fclose(f); snprintf(error_, sizeof(error_), "read error"); return false;
        }
        fclose(f);
        return Parse();
    }

    ~KenParser() { delete[] buf_; }

    // Find first record of a given type_class (prefix match).
    const KenRecord* FindFirst(const char* type_class) const {
        for (int i = 0; i < record_count_; ++i)
            if (!strncmp(records_[i].type_class, type_class, strlen(type_class)))
                return &records_[i];
        return nullptr;
    }

    // Iterate all records of a given type_class via callback f(rec&).
    template<typename F>
    void ForEach(const char* type_class, F f) const {
        size_t n = strlen(type_class);
        for (int i = 0; i < record_count_; ++i)
            if (!strncmp(records_[i].type_class, type_class, n))
                f(records_[i]);
    }

    // Field accessors
    static float       GetFloat(const KenRecord& r, const char* key, float def = 0.f) {
        for (int i = 0; i < r.field_count; ++i)
            if (!strcmp(r.fields[i].key, key) && r.fields[i].type == KenFT::Float)
                return r.fields[i].f;
        return def;
    }
    static int32_t     GetInt  (const KenRecord& r, const char* key, int32_t def = 0) {
        for (int i = 0; i < r.field_count; ++i)
            if (!strcmp(r.fields[i].key, key) && r.fields[i].type == KenFT::Int)
                return r.fields[i].i;
        return def;
    }
    static const char* GetStr  (const KenRecord& r, const char* key, const char* def = "") {
        for (int i = 0; i < r.field_count; ++i)
            if (!strcmp(r.fields[i].key, key) && r.fields[i].type == KenFT::Str)
                return r.fields[i].s;
        return def;
    }
    static const char* GetRef  (const KenRecord& r, const char* key, const char* def = "") {
        for (int i = 0; i < r.field_count; ++i)
            if (!strcmp(r.fields[i].key, key) && r.fields[i].type == KenFT::Ref)
                return r.fields[i].s;
        return def;
    }

    // Dump all records as JSON to a file (for game/data/kenshi_import/ cache).
    bool DumpJson(const char* out_path) const {
        FILE* f = fopen(out_path, "w");
        if (!f) return false;
        fprintf(f, "[\n");
        for (int i = 0; i < record_count_; ++i) {
            const KenRecord& r = records_[i];
            fprintf(f, "  {\"type\":\"%s\",\"id\":\"%s\",\"ref\":\"%s\",\"fields\":[",
                    r.type_class, r.num_id, r.ref_id);
            for (int j = 0; j < r.field_count; ++j) {
                const KenField& fd = r.fields[j];
                if (j) fprintf(f, ",");
                fprintf(f, "{\"k\":\"%s\",", fd.key);
                if      (fd.type == KenFT::Float) fprintf(f, "\"v\":%.6g}", fd.f);
                else if (fd.type == KenFT::Int)   fprintf(f, "\"v\":%d}", fd.i);
                else if (fd.type == KenFT::Str)   fprintf(f, "\"s\":\"%s\"}", fd.s);
                else if (fd.type == KenFT::Ref)   fprintf(f, "\"r\":\"%s\"}", fd.s);
                else                              fprintf(f, "\"v\":null}");
            }
            fprintf(f, "]}%s\n", i < record_count_ - 1 ? "," : "");
        }
        fprintf(f, "]\n");
        fclose(f);
        return true;
    }

private:
    uint8_t* buf_    = nullptr;
    size_t   buf_sz_ = 0;

    bool Parse() {
        record_count_ = 0;
        KenReader rd { buf_, buf_sz_, 0 };

        // Skip file header: scan forward until we find what looks like a valid
        // type_id string (length 4..80, all printable ASCII, contains '_').
        // The header is typically 20-40 bytes + deps.
        if (!SkipToFirstRecord(rd)) {
            snprintf(error_, sizeof(error_), "no records found");
            return false;
        }

        // Parse records sequentially until EOF or buffer exhausted.
        while (rd.ok() && record_count_ < KEN_MAX_RECORDS) {
            size_t save = rd.pos;
            if (!ParseRecord(rd, 0)) {
                // Non-fatal: try to re-sync 4 bytes forward.
                rd.pos = save + 4;
                if (!rd.ok()) break;
                // Skip until next plausible record boundary.
                if (!SkipToNextRecord(rd)) break;
            }
        }
        return record_count_ > 0;
    }

    // Advance rd to the first byte that looks like a valid record type_id length.
    bool SkipToFirstRecord(KenReader& rd) {
        // Read fixed 16-byte header.
        if (buf_sz_ < 16) return false;
        rd.pos = 16;
        // Skip dependency block: scan for first valid record boundary
        // by looking for plausible type_id_len (8..80) followed by printable string
        // ending at or containing '_'.
        for (size_t limit = 0; limit < 512 && rd.pos + 8 < buf_sz_; limit++) {
            if (PeekRecordValid(rd)) return true;
            rd.pos++;
        }
        return false;
    }

    bool SkipToNextRecord(KenReader& rd) {
        for (size_t limit = 0; limit < 256 && rd.pos + 8 < buf_sz_; limit++) {
            if (PeekRecordValid(rd)) return true;
            rd.pos++;
        }
        return false;
    }

    // Cheap heuristic: does rd.pos look like the start of a valid type_id?
    bool PeekRecordValid(const KenReader& rd) const {
        if (rd.pos + 8 >= buf_sz_) return false;
        uint32_t len = (uint32_t)buf_[rd.pos]
                     | ((uint32_t)buf_[rd.pos+1] << 8)
                     | ((uint32_t)buf_[rd.pos+2] << 16)
                     | ((uint32_t)buf_[rd.pos+3] << 24);
        if (len < 4 || len > 80) return false;
        if (rd.pos + 4 + len >= buf_sz_) return false;
        bool has_upper = false, has_underscore = false;
        for (uint32_t i = 0; i < len; ++i) {
            uint8_t c = buf_[rd.pos + 4 + i];
            if (c >= 'A' && c <= 'Z') has_upper = true;
            if (c == '_') has_underscore = true;
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_')) return false;
        }
        return has_upper && has_underscore;
    }

    bool ParseRecord(KenReader& rd, int depth) {
        if (depth > KEN_MAX_DEPTH) return false;
        if (record_count_ >= KEN_MAX_RECORDS) return false;

        int idx = record_count_++;
        KenRecord& r = records_[idx];

        // type_id: e.g. "GENERATION_TEMPLATE_FACTION123"
        char combined[KEN_MAX_STR];
        if (!rd.str(combined, sizeof(combined))) { record_count_--; return false; }
        SplitTypeId(combined, r.type_class, r.num_id);

        // ref_id
        if (!rd.str(r.ref_id, sizeof(r.ref_id))) { record_count_--; return false; }

        // 8-byte record header (flags + pad)
        if (rd.pos + 8 > rd.size) { record_count_--; return false; }
        rd.skip(8);

        // Float fields
        uint32_t fc = rd.u32();
        if (fc > 512) { record_count_--; return false; }
        for (uint32_t i = 0; i < fc; ++i) {
            char key[KEN_MAX_KEY]; rd.str(key, sizeof(key));
            float val = rd.f32();
            if (!rd.ok()) { record_count_--; return false; }
            AddField(r, key, val);
        }

        // Int fields
        uint32_t ic = rd.u32();
        if (ic > 512) { record_count_--; return false; }
        for (uint32_t i = 0; i < ic; ++i) {
            char key[KEN_MAX_KEY]; rd.str(key, sizeof(key));
            int32_t val = rd.i32();
            if (!rd.ok()) { record_count_--; return false; }
            AddField(r, key, val);
        }

        // Reserved group A and B (skip)
        for (int g = 0; g < 2; ++g) {
            uint32_t nc = rd.u32();
            if (nc > 512) { record_count_--; return false; }
            for (uint32_t i = 0; i < nc; ++i) {
                rd.str(nullptr, 0);  // key
                rd.skip(4);          // value (assume 4 bytes)
                if (!rd.ok()) { record_count_--; return false; }
            }
        }

        // String fields
        uint32_t sc = rd.u32();
        if (sc > 512) { record_count_--; return false; }
        for (uint32_t i = 0; i < sc; ++i) {
            char key[KEN_MAX_KEY]; rd.str(key, sizeof(key));
            char val[KEN_MAX_STR]; rd.str(val, sizeof(val));
            if (!rd.ok()) { record_count_--; return false; }
            AddFieldStr(r, key, val);
        }

        // Ref fields
        uint32_t rc = rd.u32();
        if (rc > 512) { record_count_--; return false; }
        for (uint32_t i = 0; i < rc; ++i) {
            char key[KEN_MAX_KEY]; rd.str(key, sizeof(key));
            char val[KEN_MAX_STR]; rd.str(val, sizeof(val));
            if (!rd.ok()) { record_count_--; return false; }
            AddFieldRef(r, key, val);
        }

        // Children
        uint32_t cc = rd.u32();
        if (cc > 1024) { record_count_--; return false; }
        r.child_start = record_count_;
        r.child_count = 0;
        for (uint32_t i = 0; i < cc; ++i) {
            if (!ParseRecord(rd, depth + 1)) break;
            r.child_count++;
        }

        return true;
    }

    // "GENERATION_TEMPLATE_FACTION123" → type_class="GENERATION_TEMPLATE_FACTION", num_id="123"
    static void SplitTypeId(const char* src, char* tc, char* ni) {
        int len = (int)strlen(src);
        // Find where trailing digits begin.
        int split = len;
        while (split > 0 && src[split-1] >= '0' && src[split-1] <= '9') split--;
        int tcopy = split < KEN_TYPE_LEN - 1 ? split : KEN_TYPE_LEN - 1;
        memcpy(tc, src, tcopy);
        tc[tcopy] = 0;
        int nlen = len - split;
        if (nlen > 15) nlen = 15;
        memcpy(ni, src + split, nlen);
        ni[nlen] = 0;
    }

    static void AddField(KenRecord& r, const char* key, float v) {
        if (r.field_count >= KEN_MAX_FIELDS) return;
        KenField& f = r.fields[r.field_count++];
        strncpy(f.key, key, KEN_MAX_KEY - 1);
        f.type = KenFT::Float; f.f = v;
    }
    static void AddField(KenRecord& r, const char* key, int32_t v) {
        if (r.field_count >= KEN_MAX_FIELDS) return;
        KenField& f = r.fields[r.field_count++];
        strncpy(f.key, key, KEN_MAX_KEY - 1);
        f.type = KenFT::Int; f.i = v;
    }
    static void AddFieldStr(KenRecord& r, const char* key, const char* v) {
        if (r.field_count >= KEN_MAX_FIELDS) return;
        KenField& f = r.fields[r.field_count++];
        strncpy(f.key, key, KEN_MAX_KEY - 1);
        f.type = KenFT::Str;
        strncpy(f.s, v, KEN_MAX_STR - 1);
    }
    static void AddFieldRef(KenRecord& r, const char* key, const char* v) {
        if (r.field_count >= KEN_MAX_FIELDS) return;
        KenField& f = r.fields[r.field_count++];
        strncpy(f.key, key, KEN_MAX_KEY - 1);
        f.type = KenFT::Ref;
        strncpy(f.s, v, KEN_MAX_STR - 1);
    }
};
