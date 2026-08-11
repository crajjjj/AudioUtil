#!/usr/bin/env python3
"""
lip_m4_decoder.py — the "M4" candidate grammar for Skyrim .lip payloads,
reverse-engineered 2026-08-11 from 161 Bella-pack lips + 1 LipGenerator lip.
Best known model: 128/162 files land within one frame of frames*33 with zero
errors (46 exact), vs ~0 under the shipping (dup + %4-marker) grammar.

GRAMMAR (per token, at token boundary):
  00 01 <b> ...      escape: <b> is a literal float byte; float continues
                     escape-aware (covers 00-leading floats after plateaus AND
                     00 bytes inside floats:  93 e7 00 01 00 3d -> 93 e7 00 3d)
  00 <t odd>         2-byte marker: skip = t>>2 resting cells; the NEXT float
                     is read RAW (4 blind bytes — it may contain bare 00s)
  00 <lo even> <hi>  3-byte marker: tag = lo|hi<<8 (u16), skip = tag>>2
                     (hi is almost always 00; hi!=0 seen in Bella pack:
                      00 44 02 = skip 145; Rajdazan tail 00 20 03 = skip 200)
  <float>            4 bytes, escape-aware, one grid cell; NEGATIVE values are
                     REAL cells (smooth negative curves on modifier slots) —
                     do not range-filter. Sentinels (f9e88a26 / d47b2e28,
                     ~1e-15 band) = explicit rest (0.0).

HEADERS:
  const14 == 3 -> 24-byte header (classic; preroll i32 at 16, vocab at 20)
  const14 == 7 -> 20-byte header (NO preroll field; vocab at 16, u22 at 18,
                  payload at 20) — the former "variant C", fully parseable
  variant B    -> extra byte at 14; drop it, then const14==3 applies

KNOWN REMAINING UNKNOWNS (see tools/lipresearch/RLE Hypothesis.md):
  - bare `00` immediately followed by a sane float (LipGenerator output):
    semantics unresolved (rest-cell? separator?); causes overshoot when
    misread as a u16 marker. ~30/162 files affected.
  - u16-vs-bare-00 disambiguation rule the engine actually uses.
  - engine confirmation of RAW-flag and escape semantics (in-game probe).

Usage: python3 lip_m4_decoder.py <file.lip|.fuz> [...]   # report per file
"""
import struct
import sys

SENTINELS = {b"\xf9\xe8\x8a\x26", b"\xd4\x7b\x2e\x28"}


def lip_block(path):
    d = open(path, "rb").read()
    if path.lower().endswith(".fuz"):
        if d[:4] != b"FUZE":
            return None
        size = struct.unpack_from("<I", d, 8)[0]
        return d[12:12 + size]
    return d


def parse_header(d):
    """-> (frames, num_curves, preroll, payload_off, variant) or None"""
    if len(d) < 20:
        return None
    num_curves = struct.unpack_from("<I", d, 8)[0]
    frames = struct.unpack_from("<H", d, 12)[0]
    c14 = struct.unpack_from("<H", d, 14)[0]
    if c14 == 3 and len(d) >= 24 and struct.unpack_from("<H", d, 20)[0] == 16:
        return frames, num_curves, struct.unpack_from("<i", d, 16)[0], 24, "A"
    if c14 == 7 and struct.unpack_from("<H", d, 16)[0] == 16:
        return frames, num_curves, 0, 20, "C20"
    d2 = d[:14] + d[15:]
    if (len(d2) >= 24 and struct.unpack_from("<H", d2, 14)[0] == 3
            and struct.unpack_from("<H", d2, 20)[0] == 16):
        h = parse_header(d2)
        if h:
            return h[0], h[1], h[2], None, "B"   # caller re-parses shifted copy
    return None


def decode(d, off, frames):
    """M4 decode -> (cells [(pos, value)], final_pos, errors [(kind, byteoff)])"""
    n = len(d)
    i = off
    pos = 0
    raw_next = False
    cells = []
    errs = []
    target = frames * 33
    while i < n:
        if n - i <= 3 and d[i] == 0:
            break                                   # tail padding
        if not raw_next and d[i] == 0 and i + 1 < n and d[i + 1] != 1:
            t = d[i + 1]
            if t & 1:                               # short marker + RAW flag
                pos += t >> 2
                i += 2
                raw_next = True
                continue
            hi = d[i + 2] if i + 2 < n else 0
            tag = t | (hi << 8)
            if tag == 0:
                break
            if pos + (tag >> 2) > target:
                errs.append(("skip-overshoot(bare-00?)", i))
            pos += tag >> 2
            i += 3
            continue
        data = bytearray()
        if raw_next:
            if i + 4 > n:
                break
            data = bytearray(d[i:i + 4])
            i += 4
        else:
            while len(data) < 4 and i < n:
                if d[i] == 0:
                    if i + 1 < n and d[i + 1] == 1:      # escape
                        if i + 2 < n:
                            data.append(d[i + 2])
                            i += 3
                        else:
                            i = n
                    elif data:
                        errs.append(("bare00-in-float", i))
                        data.append(0)
                        i += 1
                    else:
                        break
                else:
                    data.append(d[i])
                    i += 1
        raw_next = False
        if len(data) < 4:
            break
        raw = bytes(data)
        v = struct.unpack("<f", raw)[0]
        if raw in SENTINELS or 1e-17 < abs(v) < 1e-13:
            v = 0.0
        if abs(v) > 1.0001:
            errs.append(("garbage-value", i - 4))
        cells.append((pos, v))
        pos += 1
    return cells, pos, errs


def report(path):
    d = lip_block(path)
    if d is None:
        print(f"{path}: not a lip/fuz")
        return
    h = parse_header(d)
    if not h:
        print(f"{path}: unknown header")
        return
    frames, nc, preroll, off, variant = h
    if off is None:                                  # variant B: reparse shifted
        d = d[:14] + d[15:]
        frames, nc, preroll, off, _ = parse_header(d)
    cells, pos, errs = decode(d, off, frames)
    delta = frames * 33 - pos
    slots = sorted({p % 33 for p, v in cells if abs(v) > 1e-6})
    verdict = ("EXACT" if delta == 0 else
               "OK(<1 frame)" if 0 <= delta < 33 else
               "UNDERSHOOT" if delta > 0 else "OVERSHOOT")
    print(f"{path}\n  variant={variant} frames={frames} preroll={preroll} "
          f"num_curves={nc}\n  landing={verdict} (delta={delta} cells) "
          f"cells={len(cells)} errors={len(errs)}")
    if errs:
        print(f"  errors: {errs[:6]}")
    print(f"  active slots ({len(slots)}): {slots}")


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    for p in sys.argv[1:]:
        report(p)
