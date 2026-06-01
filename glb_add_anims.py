#!/usr/bin/env python3
"""
glb_add_anims.py — merge animations from a source GLB into a base GLB.
Uses bone NAME remapping so source and base can have different node orders.

Usage:
  python3 tools/glb_add_anims.py --base game/data/props/md_human_t.glb \
                                  --src  game/data/props/md_human.glb \
                                  --out  game/data/props/md_human_t.glb
"""
import struct, json, argparse

def load_glb(path):
    with open(path, 'rb') as f:
        assert f.read(4) == b'glTF'
        f.read(8)  # version + length
        jlen  = struct.unpack('<I', f.read(4))[0]
        f.read(4)  # JSON type
        gltf  = json.loads(f.read(jlen))
        bin_data = b''
        chunk_hdr = f.read(8)
        if len(chunk_hdr) == 8:
            blen = struct.unpack('<I', chunk_hdr[:4])[0]
            f.read(0)  # BIN type already consumed in chunk_hdr[4:]
            bin_data = f.read(blen)
    return gltf, bin_data

def save_glb(path, gltf, bin_data):
    pad = (4 - len(bin_data) % 4) % 4
    bin_data = bin_data + b'\x00' * pad
    json_bytes = json.dumps(gltf, separators=(',',':')).encode('utf-8')
    jpad = (4 - len(json_bytes) % 4) % 4
    json_bytes = json_bytes + b' ' * jpad
    total = 12 + 8 + len(json_bytes) + (8 + len(bin_data) if bin_data else 0)
    with open(path, 'wb') as f:
        f.write(b'glTF')
        f.write(struct.pack('<II', 2, total))
        f.write(struct.pack('<I', len(json_bytes)))
        f.write(b'JSON')
        f.write(json_bytes)
        if bin_data:
            f.write(struct.pack('<I', len(bin_data)))
            f.write(b'BIN\x00')
            f.write(bin_data)
    print(f"Written {path}  ({total/1024:.0f} KB)")

def main():
    p = argparse.ArgumentParser()
    p.add_argument('--base', required=True)
    p.add_argument('--src',  required=True)
    p.add_argument('--out',  required=True)
    args = p.parse_args()

    print(f"Loading base: {args.base}")
    base_gltf, base_bin = load_glb(args.base)
    print(f"Loading src:  {args.src}")
    src_gltf,  src_bin  = load_glb(args.src)

    # Build name→index maps
    base_name_to_idx = {n.get('name',''): i for i,n in enumerate(base_gltf.get('nodes',[]))}
    src_idx_to_name  = {i: n.get('name','') for i,n in enumerate(src_gltf.get('nodes',[]))}

    # Remap: src node index → base node index (by name)
    node_remap = {}
    for src_idx, name in src_idx_to_name.items():
        if name in base_name_to_idx:
            node_remap[src_idx] = base_name_to_idx[name]

    base_anim_names = {a['name'] for a in base_gltf.get('animations', [])}
    src_anims = [a for a in src_gltf.get('animations', []) if a['name'] not in base_anim_names]

    if not src_anims:
        print("No new animations to add.")
        save_glb(args.out, base_gltf, base_bin)
        return

    print(f"Adding {len(src_anims)} animations: {[a['name'] for a in src_anims]}")

    out_gltf = json.loads(json.dumps(base_gltf))
    out_bin  = bytearray(base_bin)

    acc_offset = len(out_gltf.get('accessors',   []))
    bv_offset  = len(out_gltf.get('bufferViews', []))

    new_accessors    = []
    new_buffer_views = []
    src_acc_map      = {}

    def add_src_accessor(src_acc_idx):
        if src_acc_idx in src_acc_map:
            return src_acc_map[src_acc_idx]
        acc = src_gltf['accessors'][src_acc_idx]
        bv  = src_gltf['bufferViews'][acc['bufferView']]
        raw = src_bin[bv.get('byteOffset',0) : bv.get('byteOffset',0)+bv['byteLength']]
        align = (4 - len(out_bin) % 4) % 4
        out_bin.extend(b'\x00' * align)
        start = len(out_bin)
        out_bin.extend(raw)
        new_bv = {'buffer':0,'byteOffset':start,'byteLength':bv['byteLength']}
        if 'byteStride' in bv: new_bv['byteStride'] = bv['byteStride']
        if 'target'     in bv: new_bv['target']     = bv['target']
        new_buffer_views.append(new_bv)
        new_bv_idx = bv_offset + len(new_buffer_views) - 1
        new_acc = dict(acc)
        new_acc['bufferView'] = new_bv_idx
        new_acc.pop('byteOffset', None)
        new_accessors.append(new_acc)
        new_acc_idx = acc_offset + len(new_accessors) - 1
        src_acc_map[src_acc_idx] = new_acc_idx
        return new_acc_idx

    new_anims = []
    skipped_channels = 0
    for anim in src_anims:
        new_samplers = []
        samp_remap = {}
        for si, samp in enumerate(anim['samplers']):
            new_si = len(new_samplers)
            samp_remap[si] = new_si
            new_samp = {'input': add_src_accessor(samp['input']),
                        'output': add_src_accessor(samp['output'])}
            if 'interpolation' in samp:
                new_samp['interpolation'] = samp['interpolation']
            new_samplers.append(new_samp)

        new_channels = []
        for ch in anim['channels']:
            src_node = ch['target'].get('node')
            if src_node is None:
                new_channels.append({'sampler': samp_remap[ch['sampler']], 'target': ch['target']})
                continue
            base_node = node_remap.get(src_node)
            if base_node is None:
                skipped_channels += 1
                continue
            new_channels.append({
                'sampler': samp_remap[ch['sampler']],
                'target': {'node': base_node, 'path': ch['target']['path']}
            })
        new_anims.append({'name': anim['name'], 'samplers': new_samplers, 'channels': new_channels})

    if skipped_channels:
        print(f"  Skipped {skipped_channels} channels (bones not in base skeleton)")

    for key in ('accessors','bufferViews','animations'):
        if key not in out_gltf: out_gltf[key] = []
    out_gltf['accessors']  .extend(new_accessors)
    out_gltf['bufferViews'].extend(new_buffer_views)
    out_gltf['animations'] .extend(new_anims)
    out_gltf['buffers'][0]['byteLength'] = len(out_bin)

    save_glb(args.out, out_gltf, bytes(out_bin))
    print(f"Total anims: {[a['name'] for a in out_gltf['animations']]}")

if __name__ == '__main__':
    main()
