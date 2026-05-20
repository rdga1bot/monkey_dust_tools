#!/usr/bin/env python3
"""
md_mod_import.py — Proper binary parser for Kenshi .mod/.base files.
Based on FCS (Forgotten Construction Set) IL decompilation (method-level RE).

Binary format summary:
  int32 fileVersion (1–17)
  [if fileVersion > 15]: Header block (author, deps, save counter, merge/delete tables)
  int32 lastID
  int32 numItems
  for each item:
    int32  <discard>    (old state/padding field)
    int32  itemType
    int32  oldNumericId (used as fallback ID for fileVersion < 7)
    string itemName     (display name)
    [if fileVersion >= 7]: string itemId (string GUID)
    Item property blocks (bool, float, int, vec3, quat, string, file, refs, instances)

readString: int32 length + UTF-8 bytes (length 0 → empty string)

Outputs:
  game/data/terrain_config.txt   — zone definitions (appends/merges)
  game/data/md_world.json    — factions + towns for the world panel editor

Usage: python3 tools/md_mod_import.py
"""
import struct, os, re, json

KENSHI_DATA = '/run/media/rdga1/win/SteamLibrary/steamapps/common/Kenshi/data'
MOD_FILES   = ['gamedata.base', 'Newwworld.mod', 'rebirth.mod']
OUT_CFG     = 'game/data/terrain_config.txt'
OUT_JSON    = 'game/data/md_world.json'

# itemType enum values from FCS IL (GameData/itemType)
ITEM_FACTION  = 10
ITEM_ZONE_MAP = 12
ITEM_TOWN     = 13
ITEM_BIOMES   = 28
WANTED_TYPES  = {ITEM_FACTION, ITEM_ZONE_MAP, ITEM_TOWN, ITEM_BIOMES}

BIOME_KEYWORDS = {
    'swamp': 'swamp', 'marsh': 'swamp', 'bog': 'swamp',
    'desert': 'desert', 'dunes': 'desert', 'sandy': 'desert',
    'canyon': 'canyon', 'gorge': 'canyon', 'ravine': 'canyon',
    'volcanic': 'volcanic', 'ashland': 'volcanic', 'deadland': 'volcanic',
    'acid': 'volcanic', 'venge': 'volcanic',
    'forest': 'forest', 'jungle': 'forest', 'green': 'forest',
    'coast': 'coast', 'shore': 'coast', 'island': 'coast', 'sea': 'coast',
    'highland': 'highlands', 'plateau': 'highlands', 'mountain': 'highlands',
    'plain': 'scrubland', 'savanna': 'scrubland', 'scrub': 'scrubland',
}


# ── Low-level reader ───────────────────────────────────────────────────────────

def _rs(f):
    """readString: int32 length-prefixed UTF-8."""
    raw = f.read(4)
    if len(raw) < 4:
        raise EOFError('EOF in string length')
    length = struct.unpack('<i', raw)[0]
    if length <= 0:
        return ''
    return f.read(length).decode('utf-8', errors='replace')


def _ri(f):  return struct.unpack('<i', f.read(4))[0]
def _ru(f):  return struct.unpack('<I', f.read(4))[0]
def _rf(f):  return struct.unpack('<f', f.read(4))[0]
def _rb(f):  return struct.unpack('<?', f.read(1))[0]
def _rby(f): return struct.unpack('<B', f.read(1))[0]


# ── Header ─────────────────────────────────────────────────────────────────────

def _read_header(f, fv):
    """Skip/consume the file header block (fileVersion > 15)."""
    block_end = 0
    if fv >= 17:
        block_end = _ru(f) + f.tell()

    _ri(f)     # Version int
    _rs(f)     # Author
    _rs(f)     # Description
    _rs(f)     # Dependencies (comma-sep)
    _rs(f)     # Referenced (comma-sep)

    pos = f.tell()
    if block_end > 0 and pos < block_end:
        _ru(f)          # SaveCounter
        _ru(f)          # LastMergeResolve
        merge_count = _rby(f)
        for _ in range(merge_count):
            peek = _ri(f)
            if peek > 256:
                f.seek(-4, 1)
                break
            f.seek(-4, 1)
            _rs(f)      # mod name
            f.read(8)   # two uint32s

    pos = f.tell()
    if block_end > 0 and pos < block_end:
        del_count = _rby(f)
        for _ in range(del_count):
            _rs(f)      # mod name
            _ru(f)      # version
            _rs(f)      # colon-separated item ids

    if block_end > 0:
        f.seek(block_end)


# ── Item property blocks ───────────────────────────────────────────────────────

def _read_item_props(f, fv, is_base=False):
    """Read all property blocks for one item. Returns flat dict."""
    props = {}

    if fv >= 15:
        f.read(4)   # uint32 block_flags — skip

    # Tagged section: present in .mod files only (not gamedata.base).
    # IL: tagged() returns true when dict==null (base file) → all props stored normally.
    # For .mod files the tagged section stores per-property merge flags; values are real props too.
    if fv >= 11 and not is_base:
        n = _ri(f)
        for _ in range(n):
            k = _rs(f); v = _rb(f)
            props[k] = v  # tagged bools are actual property values

    # Bool
    n = _ri(f)
    for _ in range(n):
        k = _rs(f); props[k] = _rb(f)

    # Float
    n = _ri(f)
    for _ in range(n):
        k = _rs(f); props[k] = _rf(f)

    # Int
    n = _ri(f)
    for _ in range(n):
        k = _rs(f); props[k] = _ri(f)

    if fv > 8:
        # Vec3
        n = _ri(f)
        for _ in range(n):
            k = _rs(f)
            props[k] = (_rf(f), _rf(f), _rf(f))
        # Quat
        n = _ri(f)
        for _ in range(n):
            k = _rs(f)
            props[k] = (_rf(f), _rf(f), _rf(f), _rf(f))

    # String
    n = _ri(f)
    for _ in range(n):
        k = _rs(f); props[k] = _rs(f)

    # File
    n = _ri(f)
    for _ in range(n):
        k = _rs(f); props[k] = _rs(f)

    # Reference groups
    refs = {}
    n = _ri(f)
    for _ in range(n):
        group = _rs(f)
        nr    = _ri(f)
        grefs = []
        for _ in range(nr):
            if fv < 9:
                f.read(8); continue
            tid = _rs(f)
            v0  = _ri(f)
            v1 = v2 = 0
            if fv >= 10:
                v1 = _ri(f); v2 = _ri(f)
            grefs.append(tid)
        if grefs:
            refs[group] = grefs
    props['_refs'] = refs

    # Instances
    n = _ri(f)
    for _ in range(n):
        if fv >= 15:
            _rs(f)
        else:
            f.read(4)
        if fv >= 9:
            _rs(f)      # "ref" string
        f.read(4 * 7)   # x y z qw qx qy qz
        if fv > 6:
            ns = _ri(f)
            for _ in range(ns):
                if fv >= 15: _rs(f)
                else:        f.read(4)

    return props


# ── File parser ────────────────────────────────────────────────────────────────

def parse_mod(path):
    """
    Parse one Kenshi .mod or .base file.
    Returns list of (item_type, item_id, item_name, props_dict).
    """
    results = []
    fname = os.path.basename(path)

    with open(path, 'rb') as f:
        fv = _ri(f)
        if fv < 1 or fv > 17:
            print(f'  [skip] bad version {fv}')
            return results

        if fv > 15:
            _read_header(f, fv)

        last_id   = _ri(f)
        num_items = _ri(f)
        print(f'  fileVersion={fv}  lastID={last_id}  items={num_items}')

        for i in range(num_items):
            item_start = f.tell()
            sz = 0
            try:
                sz    = _ri(f)                  # total item bytes from this field (0 = unknown)
                itype = _ri(f)                  # itemType enum

                if itype not in WANTED_TYPES and sz > 0:
                    f.seek(item_start + sz)
                    continue

                old_nid = _ri(f)                # old numeric id
                iname   = _rs(f)                # display name
                iid     = _rs(f) if fv >= 7 else f'{old_nid}-{fname}'
                props   = _read_item_props(f, fv, is_base=(fname == 'gamedata.base'))

                if itype in WANTED_TYPES:
                    results.append((itype, iid, iname, props))
                elif sz == 0:
                    pass  # sequential parse succeeded for unwanted item

            except Exception as e:
                print(f'  [warn] item {i}/{num_items} @{item_start}: {e}')
                if sz > 0:
                    try:
                        f.seek(item_start + sz)
                        continue
                    except Exception:
                        pass
                break

    return results


# ── Helpers ────────────────────────────────────────────────────────────────────

def to_slug(name):
    return re.sub(r'[^a-z0-9]+', '_', name.lower()).strip('_')


def biome_from_name(name):
    nl = name.lower()
    for kw, b in BIOME_KEYWORDS.items():
        if kw in nl:
            return b
    return 'scrubland'


def load_existing_cfg_slugs():
    slugs = set()
    if not os.path.exists(OUT_CFG):
        return slugs
    with open(OUT_CFG) as f:
        for line in f:
            m = re.match(r'\s*zone=(\S+)', line)
            if m:
                slugs.add(m.group(1))
    return slugs


def load_existing_json():
    if not os.path.exists(OUT_JSON):
        return {'factions': [], 'towns': []}
    with open(OUT_JSON) as f:
        try:    return json.load(f)
        except: return {'factions': [], 'towns': []}


# ── Main ───────────────────────────────────────────────────────────────────────

def main():
    all_items = []
    for mf in MOD_FILES:
        path = os.path.join(KENSHI_DATA, mf)
        if not os.path.exists(path):
            print(f'[skip] {path} not found')
            continue
        sz = os.path.getsize(path)
        print(f'[parse] {path}  ({sz//1024} KB)')
        try:
            items = parse_mod(path)
        except Exception as e:
            print(f'  [error] {e}')
            items = []
        all_items.extend(items)
        type_counts = {}
        for t, _, _, _ in items:
            type_counts[t] = type_counts.get(t, 0) + 1
        for t, c in sorted(type_counts.items()):
            from_enum = {10:'FACTION',12:'ZONE_MAP',13:'TOWN',28:'BIOMES'}.get(t, str(t))
            print(f'    {from_enum}: {c}')

    # ── FACTION ───────────────────────────────────────────────────────────────
    factions = []
    for itype, iid, iname, props in all_items:
        if itype != ITEM_FACTION:
            continue
        # Use string id slug as faction id
        slug = to_slug(iid) or to_slug(iname)
        fac = {
            'id':          slug,
            'name':        iname,
            'md_id':   iid,
            'ideology':    props.get('tech level', 'unknown'),
            'color_rgb':   [0.6, 0.6, 0.6],
            'slaving':     bool(props.get('slaves', False)),
            'patrols':     True,
            'attitude':    'neutral',
        }
        factions.append(fac)

    # ── TOWN ──────────────────────────────────────────────────────────────────
    towns = []
    for itype, iid, iname, props in all_items:
        if itype != ITEM_TOWN:
            continue
        # Extract grid position
        gx = int(props.get('x', 0))
        gz = int(props.get('y', 0))
        # faction reference: first entry in '_refs' (any group)
        fac_ref = ''
        for grp, refs in props.get('_refs', {}).items():
            if refs:
                fac_ref = refs[0]
                break
        town = {
            'id':        to_slug(iid) or to_slug(iname),
            'name':      iname,
            'md_id': iid,
            'grid_x':    gx,
            'grid_z':    gz,
            'faction':   fac_ref,
            'biome':     biome_from_name(iname),
        }
        towns.append(town)

    # ── ZONE_MAP → terrain_config.txt ─────────────────────────────────────────
    existing_slugs = load_existing_cfg_slugs()
    new_zones = []
    for itype, iid, iname, props in all_items:
        if itype != ITEM_ZONE_MAP:
            continue
        slug = to_slug(iname)
        if not slug or slug in existing_slugs:
            continue
        existing_slugs.add(slug)
        gx = int(props.get('x', 0))
        gz = int(props.get('y', 0))
        new_zones.append({
            'slug':    slug,
            'name':    iname,
            'biome':   biome_from_name(iname),
            'grid_x':  gx,
            'grid_z':  gz,
            'danger':  int(props.get('danger', 1)),
            'amplitude': float(props.get('height modifier', 30.0)),
        })

    added = 0
    with open(OUT_CFG, 'a') as f:
        for z in new_zones[:200]:
            f.write(f'\nzone={z["slug"]}\n')
            f.write(f'  name={z["name"]}\n')
            f.write(f'  biome={z["biome"]}\n')
            f.write(f'  amplitude={z["amplitude"]:.1f}\n')
            f.write(f'  danger={z["danger"]}\n')
            f.write(f'  grid_x={z["grid_x"]}\n')
            f.write(f'  grid_z={z["grid_z"]}\n')
            added += 1
    print(f'\nNew zones → terrain_config.txt: {added}')

    # ── JSON output ───────────────────────────────────────────────────────────
    world = load_existing_json()

    # Merge factions (by md_id to avoid duplicates on re-run)
    existing_kids = {f.get('md_id', '') for f in world.get('factions', [])}
    added_f = 0
    for fac in factions:
        if fac['md_id'] not in existing_kids:
            world.setdefault('factions', []).append(fac)
            added_f += 1

    # Fallback: if 0 factions parsed (old-format .base), keep hardcoded core set
    if not world.get('factions'):
        world['factions'] = [
            {'id':'united_cities', 'name':'United Cities',   'ideology':'empire',   'color_rgb':[0.9,0.7,0.1],'slaving':True,  'patrols':True, 'attitude':'hostile_player'},
            {'id':'shek_kingdom',  'name':'Shek Kingdom',    'ideology':'warrior',  'color_rgb':[0.8,0.4,0.2],'slaving':False, 'patrols':True, 'attitude':'neutral'},
            {'id':'holy_nation',   'name':'Holy Nation',     'ideology':'theocracy','color_rgb':[1.0,0.9,0.6],'slaving':True,  'patrols':True, 'attitude':'hostile_nonhuman'},
            {'id':'anti_slavers',  'name':'Anti-Slavers',    'ideology':'rebel',    'color_rgb':[0.4,0.7,0.4],'slaving':False, 'patrols':False,'attitude':'friendly'},
            {'id':'tech_hunters',  'name':'Tech Hunters',    'ideology':'scavenger','color_rgb':[0.5,0.5,0.8],'slaving':False, 'patrols':True, 'attitude':'neutral'},
            {'id':'swampers',      'name':'Swampers',        'ideology':'tribal',   'color_rgb':[0.3,0.6,0.3],'slaving':False, 'patrols':False,'attitude':'neutral'},
            {'id':'cannibals',     'name':'Cannibals',       'ideology':'feral',    'color_rgb':[0.7,0.2,0.2],'slaving':False, 'patrols':True, 'attitude':'hostile_all'},
            {'id':'dust_bandits',  'name':'Dust Bandits',    'ideology':'bandit',   'color_rgb':[0.6,0.5,0.3],'slaving':False, 'patrols':True, 'attitude':'hostile_all'},
        ]

    # Merge towns (by md_id)
    existing_tids = {t.get('md_id', '') for t in world.get('towns', [])}
    added_t = 0
    for town in towns:
        if town['md_id'] not in existing_tids:
            world.setdefault('towns', []).append(town)
            added_t += 1

    with open(OUT_JSON, 'w') as f:
        json.dump(world, f, indent=2)
    print(f'md_world.json: {len(world["factions"])} factions (+{added_f} new), '
          f'{len(world["towns"])} towns (+{added_t} new)')


if __name__ == '__main__':
    main()
