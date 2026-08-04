#!/usr/bin/env python3
"""Convert a Skyrim head .tri (+ diffuse .dds) into a .head.json for lipsim.

The FaceGen FRTRI003 .tri (e.g. meshes\\actors\\character\\character assets\\
femalehead.tri, or a mod's replacement like Expressive Facial Animation)
carries everything lipsim needs: the base head geometry, UVs, and the named
difference morphs for every MFG phoneme, modifier, and expression — the exact
per-vertex data the game engine animates. This script bundles it (plus an
optional skin texture) into one .head.json that lipsim renders in WebGL.

Usage:
  python tri2head.py femalehead.tri --dds femalehead.dds -o mychar.head.json

The dds is typically textures\\actors\\character\\female\\femalehead.dds from
your skin mod. BC1/BC3/BC7 supported (BC7 needs `pip install texture2ddecoder`).
No Bethesda assets ship with lipsim — you generate this from your own install.
"""

import argparse
import base64
import io
import json
import struct
import sys
from pathlib import Path


def parse_tri(path: Path):
    d = path.read_bytes()
    if d[:8] != b"FRTRI003":
        sys.exit(f"error: {path} is not an FRTRI003 tri file")
    V, T, Q, LV, LS, X, ext, Md, Ms, MsV = struct.unpack_from("<10i", d, 8)
    off = 8 + 40 + 16
    verts = struct.unpack_from(f"<{V * 3}f", d, off); off += V * 12
    off += MsV * 12
    tris = struct.unpack_from(f"<{T * 3}i", d, off); off += T * 12
    off += Q * 16
    uvs = struct.unpack_from(f"<{X * 2}f", d, off); off += X * 8
    uv_tris = tris
    if ext & 1:
        uv_tris = struct.unpack_from(f"<{T * 3}i", d, off); off += T * 12
    morphs = {}
    for _ in range(Md):
        nlen = struct.unpack_from("<I", d, off)[0]; off += 4
        name = d[off:off + nlen].split(b"\0")[0].decode(); off += nlen
        scale = struct.unpack_from("<f", d, off)[0]; off += 4
        deltas = struct.unpack_from(f"<{V * 3}h", d, off); off += V * 6
        morphs[name] = (scale, deltas)
    if off != len(d):
        print(f"warning: {len(d) - off} trailing bytes not parsed")
    return V, T, verts, tris, uvs, uv_tris, morphs


def decode_dds(path: Path, out_size: int):
    from PIL import Image
    d = path.read_bytes()
    if d[:4] != b"DDS ":
        sys.exit(f"error: {path} is not a dds")
    h, w = struct.unpack_from("<II", d, 12)
    fourcc = d[84:88]
    data_off = 128
    fmt = fourcc
    if fourcc == b"DX10":
        dxgi = struct.unpack_from("<I", d, 128)[0]
        data_off = 148
        fmt = { 71: b"DXT1", 74: b"DXT3", 77: b"DXT5", 98: b"BC7" }.get(dxgi)
        if fmt is None:
            sys.exit(f"error: unsupported DXGI format {dxgi}")
    try:
        import texture2ddecoder as t2d
    except ImportError:
        sys.exit("error: pip install texture2ddecoder (needed for dds decoding)")
    blob = d[data_off:]
    if fmt == b"DXT1":   bgra = t2d.decode_bc1(blob, w, h)
    elif fmt == b"DXT3": bgra = t2d.decode_bc2(blob, w, h)
    elif fmt == b"DXT5": bgra = t2d.decode_bc3(blob, w, h)
    elif fmt == b"BC7":  bgra = t2d.decode_bc7(blob, w, h)
    else: sys.exit(f"error: unsupported dds fourCC {fmt}")
    img = Image.frombytes("RGBA", (w, h), bgra, "raw", "BGRA")
    if max(w, h) > out_size:
        img = img.resize((out_size, out_size), Image.LANCZOS)
    img = img.convert("RGB")
    buf = io.BytesIO()
    img.save(buf, "JPEG", quality=88)
    return base64.b64encode(buf.getvalue()).decode()


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tri", type=Path, help="head .tri (FRTRI003)")
    ap.add_argument("--dds", type=Path, default=None, help="diffuse skin texture (.dds)")
    ap.add_argument("--tex-size", type=int, default=1024, help="texture size in the bundle (default 1024)")
    ap.add_argument("-o", "--out", type=Path, default=None, help="output (default <tri>.head.json)")
    args = ap.parse_args()

    V, T, verts, tris, uvs, uv_tris, morphs = parse_tri(args.tri)
    print(f"tri: {V} verts, {T} tris, {len(uvs)//2} uvs, {len(morphs)} morphs")

    # split vertices by (position index, uv index) so the mesh is renderable
    # with one uv per vertex; morph deltas stay keyed by ORIGINAL index
    remap = {}
    positions, uv_out, orig = [], [], []
    indices = []
    for c in range(T * 3):
        key = (tris[c], uv_tris[c])
        idx = remap.get(key)
        if idx is None:
            idx = len(orig)
            remap[key] = idx
            vi, ti = key
            positions.extend(round(v, 4) for v in verts[vi * 3: vi * 3 + 3])
            uv_out.extend(round(u, 5) for u in uvs[ti * 2: ti * 2 + 2])
            orig.append(vi)
        indices.append(idx)
    print(f"render mesh: {len(orig)} split verts")

    # morphs: sparse deltas over original vertex indices, scale baked in
    morph_out = {}
    for name, (scale, deltas) in morphs.items():
        idx_list, d_list = [], []
        for vi in range(V):
            dx, dy, dz = deltas[vi * 3: vi * 3 + 3]
            if dx or dy or dz:
                idx_list.append(vi)
                d_list.extend((round(dx * scale, 5), round(dy * scale, 5), round(dz * scale, 5)))
        morph_out[name] = { "i": idx_list, "d": d_list }
    active = {n: len(m["i"]) for n, m in morph_out.items()}
    print("busiest morphs:", sorted(active.items(), key=lambda kv: -kv[1])[:5])

    xs, ys, zs = verts[0::3], verts[1::3], verts[2::3]
    print("bbox: x", round(min(xs), 1), round(max(xs), 1),
          "y", round(min(ys), 1), round(max(ys), 1),
          "z", round(min(zs), 1), round(max(zs), 1))

    bundle = {
        "format": "lipsim-head-1",
        "source": args.tri.name,
        "positions": positions, "uvs": uv_out, "indices": indices, "orig": orig,
        "numOrig": V,
        "morphs": morph_out,
        "texture": decode_dds(args.dds, args.tex_size) if args.dds else None,
    }
    out = args.out or args.tri.with_suffix(".head.json")
    out.write_text(json.dumps(bundle, separators=(",", ":")))
    print(f"wrote {out} ({out.stat().st_size // 1024} KB)")


if __name__ == "__main__":
    main()
