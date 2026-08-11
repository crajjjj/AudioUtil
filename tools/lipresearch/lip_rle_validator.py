#!/usr/bin/env python3
"""
lip_rle_validator.py — test the pure-RLE grammar hypothesis for Skyrim .lip files
against a corpus, using the structural invariant: a correct parse must land on
exactly frameCount * 33 grid cells at end-of-payload.

Grammars compared per file:
  RLE      pure run-length model: every f32 token is one grid cell; a 3-byte
           marker 00 <tag> 00 (tag != 0, tag % 4 == 0) is a run of tag/4
           resting cells; markers may chain; no dup rule, no range filter.
  DUP      the current AudioUtil model: f32 cell, an exact 4-byte repeat is a
           tangent consuming one extra cell, optional marker suffix.

Per-file signals, per grammar:
  delta      frames*33 - finalPos  (0 = exact land; <0 overshoot = misparse;
             >0 undershoot = trailing rest possibly omitted by encoder)
  leftover   payload bytes left when a token no longer fits (0 = clean EOF)
  oor        cells outside [0,1] that are not rest sentinels (~1e-15) — should
             be ~0 if the grammar is right and cells are weights
  slot32     non-zero cells landing on the unused slot 32 — drift indicator
  curves     channels with signal vs header num_curves — secondary validator

Usage:
  python3 lip_rle_validator.py <corpus_dir> [--csv out.csv] [--limit N] [--worst N]

Reads .lip files and the embedded LIP block of .fuz files, recursively.
Stdlib only.
"""

import argparse
import csv
import math
import os
import struct
import sys
from collections import Counter

STRIDE = 33
CHANNELS = 32
SENTINEL_EPS = 1.0e-6          # |v| below this = explicit rest key
OOR_LO, OOR_HI = -1.0e-4, 1.0001
MAX_FRAMES = 60 * 30 * 30


# ---------------------------------------------------------------- containers

def read_lip_block(path):
    """Return raw LIP bytes from a .lip or .fuz file, or None."""
    with open(path, "rb") as fh:
        data = fh.read()
    if path.lower().endswith(".fuz"):
        if len(data) < 12 or data[:4] != b"FUZE":
            return None
        lip_size = struct.unpack_from("<I", data, 8)[0]
        if lip_size == 0 or 12 + lip_size > len(data):
            return None
        return data[12:12 + lip_size]
    return data


# ------------------------------------------------------------------- header

def parse_header(data):
    """
    Returns (variant, frames, num_curves, preroll, payload) or (None, reason).
    variant: 'A' or 'B' (B = extra byte at offset 14 dropped).
    """
    if len(data) < 24:
        return None, "short"
    vocab = struct.unpack_from("<H", data, 20)[0]
    variant = "A"
    if vocab != 16:
        data = data[:14] + data[15:]
        variant = "B"
        if len(data) < 24:
            return None, "short"
        vocab = struct.unpack_from("<H", data, 20)[0]
        if vocab != 16:
            return None, "variantC"
    version, duration, num_curves, frames, _c14, preroll, _vocab, _u22 = \
        struct.unpack_from("<IIIHHiHH", data, 0)
    if version != 1 or frames == 0 or frames > MAX_FRAMES:
        return None, "badheader"
    return (variant, frames, num_curves, preroll, data[24:]), None


# ----------------------------------------------------------------- grammars

def is_marker(data, i):
    return (i + 3 <= len(data) and data[i] == 0 and data[i + 2] == 0
            and data[i + 1] != 0 and data[i + 1] % 4 == 0)


def parse_rle(payload):
    """Pure RLE: f32 = one cell; marker = run of resting cells; markers chain.
    Marker is tried FIRST at each token boundary (standalone token model)."""
    cells = []          # (pos, value)
    pos = 0
    i = 0
    n = len(payload)
    while i < n:
        if is_marker(payload, i):
            pos += payload[i + 1] // 4
            i += 3
            continue
        if i + 4 > n:
            break
        value = struct.unpack_from("<f", payload, i)[0]
        cells.append((pos, value))
        pos += 1
        i += 4
    return cells, pos, n - i


def parse_dup(payload):
    """Current AudioUtil model: cell float, exact-repeat tangent (+1 cell),
    optional marker suffix."""
    cells = []
    pos = 0
    i = 0
    n = len(payload)
    while i + 4 <= n:
        value = struct.unpack_from("<f", payload, i)[0]
        step = 1
        i += 4
        if i + 4 <= n and payload[i:i + 4] == payload[i - 4:i]:
            step = 2
            i += 4
        skip = 0
        if is_marker(payload, i):
            skip = payload[i + 1] // 4
            i += 3
        cells.append((pos, value))
        pos += step + skip
    return cells, pos, n - i


GRAMMARS = {"RLE": parse_rle, "DUP": parse_dup}


# ------------------------------------------------------------------ signals

def analyze(cells, final_pos, leftover, frames, num_curves):
    total = frames * STRIDE
    oor = 0
    slot32 = 0
    active = set()
    for pos, v in cells:
        if not math.isfinite(v) or v < OOR_LO or v > OOR_HI:
            if abs(v) > SENTINEL_EPS or not math.isfinite(v):
                oor += 1
            continue
        slot = pos % STRIDE
        if slot == 32:
            if v > SENTINEL_EPS:
                slot32 += 1
        elif slot < CHANNELS and v > SENTINEL_EPS:
            active.add(slot)
    return {
        "delta": total - final_pos,      # >0 undershoot, <0 overshoot
        "leftover": leftover,
        "oor": oor,
        "slot32": slot32,
        "curves_seen": len(active),
        "curves_hdr": num_curves,
    }


# --------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("corpus", help="directory scanned recursively for .lip/.fuz")
    ap.add_argument("--csv", help="write per-file results to this CSV")
    ap.add_argument("--limit", type=int, default=0, help="stop after N files")
    ap.add_argument("--worst", type=int, default=20,
                    help="how many worst RLE offenders to list (default 20)")
    args = ap.parse_args()

    files = []
    for root, _dirs, names in os.walk(args.corpus):
        for name in names:
            if name.lower().endswith((".lip", ".fuz")):
                files.append(os.path.join(root, name))
    files.sort()
    if args.limit:
        files = files[:args.limit]
    if not files:
        sys.exit(f"no .lip/.fuz files under {args.corpus}")

    stats = {g: Counter() for g in GRAMMARS}
    deltas = {g: Counter() for g in GRAMMARS}   # per-frame-distance histogram
    header_fail = Counter()
    variants = Counter()
    rows = []
    worst = []

    for idx, path in enumerate(files):
        try:
            block = read_lip_block(path)
        except OSError as exc:
            header_fail["ioerror"] += 1
            continue
        if not block:
            header_fail["nolip"] += 1
            continue
        parsed, reason = parse_header(block)
        if not parsed:
            header_fail[reason] += 1
            continue
        variant, frames, num_curves, preroll, payload = parsed
        variants[variant] += 1

        row = {"file": os.path.relpath(path, args.corpus),
               "variant": variant, "frames": frames, "preroll": preroll}
        for gname, gfunc in GRAMMARS.items():
            cells, final_pos, leftover = gfunc(payload)
            sig = analyze(cells, final_pos, leftover, frames, num_curves)
            s = stats[gname]
            s["files"] += 1
            if sig["delta"] == 0:
                s["exact"] += 1
            elif sig["delta"] < 0:
                s["overshoot"] += 1
            else:
                s["undershoot"] += 1
            if sig["delta"] >= 0 and sig["delta"] < STRIDE:
                s["within1frame"] += 1
            if sig["leftover"] == 0:
                s["cleaneof"] += 1
            if sig["oor"] == 0:
                s["no_oor"] += 1
            s["oor_cells"] += sig["oor"]
            s["slot32_cells"] += sig["slot32"]
            if sig["curves_seen"] == sig["curves_hdr"]:
                s["curves_match"] += 1
            frame_delta = sig["delta"] // STRIDE if sig["delta"] >= 0 else -1
            deltas[gname][max(-1, min(frame_delta, 10))] += 1
            for key, val in sig.items():
                row[f"{gname}_{key}"] = val
        rows.append(row)
        badness = abs(row["RLE_delta"]) + row["RLE_oor"] * STRIDE
        if badness:
            worst.append((badness, row["file"], row["RLE_delta"],
                          row["RLE_oor"], row["RLE_leftover"]))
        if (idx + 1) % 2000 == 0:
            print(f"  ... {idx + 1}/{len(files)}", file=sys.stderr)

    # ------------------------------------------------------------- report
    print(f"\ncorpus: {len(files)} files | parsed: {len(rows)} | "
          f"variants: {dict(variants)} | header failures: {dict(header_fail)}\n")

    def pct(s, key):
        return f"{100.0 * s[key] / s['files']:6.2f}%" if s["files"] else "   n/a"

    print(f"{'':14}{'exact-land':>12}{'<1 frame':>10}{'overshoot':>11}"
          f"{'clean EOF':>11}{'no OOR':>9}{'slot32':>8}{'curves=':>9}")
    for gname in GRAMMARS:
        s = stats[gname]
        print(f"{gname:14}{pct(s, 'exact'):>12}{pct(s, 'within1frame'):>10}"
              f"{pct(s, 'overshoot'):>11}{pct(s, 'cleaneof'):>11}"
              f"{pct(s, 'no_oor'):>9}{s['slot32_cells']:>8}"
              f"{pct(s, 'curves_match'):>9}")

    print("\nundershoot distance histogram (frames short of frameCount*33;"
          " -1 = overshoot, 10 = 10+):")
    for gname in GRAMMARS:
        line = "  ".join(f"{k:>3}:{deltas[gname][k]:<6}"
                         for k in sorted(deltas[gname]))
        print(f"  {gname:5} {line}")

    if worst:
        worst.sort(reverse=True)
        print(f"\nworst {min(args.worst, len(worst))} files under RLE "
              f"(delta cells, OOR cells, leftover bytes):")
        for badness, name, delta, oor, leftover in worst[:args.worst]:
            print(f"  {name}  delta={delta} oor={oor} leftover={leftover}")

    if args.csv and rows:
        with open(args.csv, "w", newline="") as fh:
            writer = csv.DictWriter(fh, fieldnames=list(rows[0]))
            writer.writeheader()
            writer.writerows(rows)
        print(f"\nper-file results -> {args.csv}")

    print("\nreading the result:")
    print("  RLE exact-land ~100%, no-OOR ~100%  -> RLE is the grammar; drop the"
          " dup rule and range filter")
    print("  RLE undershoot small & consistent   -> encoder omits the trailing"
          " rest run; invariant is 'delta >= 0 and < 1 frame'")
    print("  RLE overshoot on a cluster          -> a token is still missing;"
          " inspect those files' bytes at the desync point")


if __name__ == "__main__":
    main()
