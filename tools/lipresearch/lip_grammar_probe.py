#!/usr/bin/env python3
"""
lip_grammar_probe.py — single-file .lip grammar analysis (companion to
lip_rle_validator.py; use this on files the validator flags).

Runs on one .lip (or .fuz), prints:
  1. header + duration/audio-fit sanity
  2. the shipping-grammar walk (dup rule + %4 markers) with desync diagnostics
  3. an escape-tolerant DP segmentation (floats + strict markers + 1-byte
     escapes) that catalogues every byte run the known grammar cannot cover —
     THE OUTPUT THAT MATTERS: the unknown-construct inventory with context.

Known unknown-constructs observed in LipGenerator output (2026-08-11, see
tools/lipresearch/RLE Hypothesis.md "real-file evidence" section):
  - markers with odd tags (0x05 0x09 0x29 0x3d — all == 1 mod 4)
  - the restated-float motif:  X  00 01  00-prefixed-X
  - interior  00 01 00  inside what should be one float
  - a 3-byte tail  00 20 03  (tag high byte? padding?)

Usage: python3 lip_grammar_probe.py <file.lip|file.fuz>
"""
import struct
import sys


def read_lip(path):
    data = open(path, "rb").read()
    if path.lower().endswith(".fuz"):
        assert data[:4] == b"FUZE", "not a FUZE container"
        lip_size = struct.unpack_from("<I", data, 8)[0]
        return data[12:12 + lip_size]
    return data


def sane_float(d, i, n):
    if i + 4 > n:
        return None
    v = struct.unpack_from("<f", d, i)[0]
    if -1e-4 <= v <= 1.0001 and (v == 0.0 or abs(v) > 1e-30):
        return v
    if 1e-17 < abs(v) < 1e-13:   # rest-sentinel band (f9e88a26 / d47b2e28)
        return 0.0
    return None


def main():
    d = read_lip(sys.argv[1])
    n = len(d)
    v, dur, nc, frames, c14, pre, vocab, u22 = struct.unpack_from("<IIIHHiHH", d, 0)
    print(f"header: version={v} duration={dur} num_curves={nc} frames={frames} "
          f"const14={c14} preroll={pre} vocab={vocab} u22={u22}")
    print(f"duration fit 132*frames+28: {'EXACT' if 132*frames+28 == dur else 'MISMATCH'}"
          f"   grid target = frames*33 = {frames*33}")

    # ---- shipping grammar walk (dup + %4 marker suffix) with OOR diagnostics
    pos, i, cells, oor = 0, 24, [], []
    while i + 4 <= n:
        b = d[i:i+4]
        val = struct.unpack("<f", b)[0]
        i += 4
        nf = 1
        if i + 4 <= n and d[i:i+4] == b:
            nf = 2
            i += 4
        skip = 0
        if i + 3 <= n and d[i] == 0 and d[i+2] == 0 and d[i+1] and d[i+1] % 4 == 0:
            skip = d[i+1] // 4
            i += 3
        cells.append((pos, val))
        if not (-1e-4 <= val <= 1.0001) and abs(val) > 1e-6:
            oor.append((pos, val))
        pos += nf + skip
    print(f"\nshipping grammar: landing={pos} delta={frames*33-pos} "
          f"cells={len(cells)} OOR={len(oor)} leftover={n-i}")
    slots = sorted({p % 33 for p, val in cells if val > 1e-6})
    print(f"  active slots ({len(slots)}, header says {nc}): {slots}")
    for p, val in oor[:6]:
        print(f"  OOR pos={p} frame={p//33} slot={p%33} v={val:.6g}")

    # ---- escape-DP: what can't the known grammar cover?
    INF = 1 << 30
    best = [INF] * (n + 1)
    best[n] = 0
    choice = [None] * (n + 1)
    for i in range(n - 1, 23, -1):
        if best[i+1] + 1 < best[i]:
            best[i] = best[i+1] + 1
            choice[i] = ("E", 1)
        if (i + 3 <= n and d[i] == 0 and d[i+2] == 0 and d[i+1]
                and d[i+1] % 4 == 0 and best[i+3] < best[i]):
            best[i] = best[i+3]
            choice[i] = ("M", 3)
        if sane_float(d, i, n) is not None and i + 4 <= n and best[i+4] < best[i]:
            best[i] = best[i+4]
            choice[i] = ("F", 4)
    print(f"\nescape-DP: {best[24]} bytes uncoverable by the known grammar")
    i, runs, cur = 24, [], None
    while i < n:
        kind, ln = choice[i]
        if kind == "E":
            cur = [i, i + 1] if cur is None else [cur[0], i + 1]
        elif cur:
            runs.append(cur)
            cur = None
        i += ln
    if cur:
        runs.append(cur)
    for a, b in runs:
        print(f"  @{a:5} len={b-a:2}  {d[a:b].hex(' ')}   "
              f"before: {d[max(24,a-8):a].hex(' ')}   after: {d[b:b+8].hex(' ')}")


if __name__ == "__main__":
    main()
