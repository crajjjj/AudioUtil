#!/usr/bin/env python3
"""
make_grammar_probe.py — the construct-alphabet probes (RLE Hypothesis.md §10
follow-up): four synthetic lips, each isolating grammar constructs in separate
time windows, so an `autest lipcap` capture reads the ENGINE's interpretation
of every construct directly off the mouth.

Design (engine-verified identity slot map does the talking):
  - ANCHOR: slot 2 (BMP) carries 0.10 on EVERY frame. If a construct makes the
    engine consume a different number of grid cells than we predicted, all
    later cells shift and the anchor visibly jumps channels:
      shift -2 -> Aah, -1 -> BigAah, +1 -> ChjSh, +2 -> DST
    (also keeps every marker gap < 1 frame — the legacy-parser-safe layout
    proven by make_probe.py's staircase files)
  - each construct window plants a 0.9 signature pulse whose landing channel
    differs between interpretations of the construct.

Probes (hum pitch identifies them by EAR, frame count identifies them in the
capture):
  A  220 Hz, 240 frames — controls + sentinel rests + THE DUP PAIR
  B  330 Hz, 270 frames — marker zoo: odd tags (00 05 / 00 29), u16 hi!=0
  C  300 frames, 440 Hz — 00 01 escapes, bare-00 + float, negative cell
  D  180 frames, 550 Hz — variant-C 20-byte header, plain control payload

Subcommands:
  build       write probe_A..D.fuz + grammar_probe_manifest.json next to this
              script (probes/), print both grammars' decode of each probe
  deploy X    copy probe X onto the Carlotta market-call-out override
              (E:\\nefaram\\overwrite\\...\\dialoguewhiterun__0008f148_1.fuz)

In-game: `autest lipcap start` -> stand at Carlotta's stall through 1-2
call-outs -> `autest lipcap stop` -> read_grammar_probe.py <csv>.
Swap probes between call-outs with `deploy` (the hum pitch confirms which one
actually played; if the game holds the old file, relaunch per probe).
"""
import json
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
OUTDIR = os.path.join(HERE, "probes")
SEP = chr(92)
OVERRIDE = SEP.join(["E:", "nefaram", "overwrite", "sound", "voice",
                     "skyrim.esm", "femaleeventoned",
                     "dialoguewhiterun__0008f148_1.fuz"])
XWMAENCODE = os.path.join(os.environ.get("SKYRIM_GAME_PATH",
    r"C:\SteamLibrary\steamapps\common\Skyrim Special Edition"),
    "Tools", "Audio", "xwmaencode.exe")

STRIDE = 33
ANCHOR_SLOT = 2      # BMP — shifts of ±1/±2 land on visible neighbors
ANCHOR = 0.10
PULSE = 0.9
MFG = ["Aah", "BigAah", "BMP", "ChjSh", "DST", "Eee", "Eh", "FV",
       "I", "K", "N", "Oh", "OohQ", "R", "Th", "W"]

F = lambda v: struct.pack("<f", v)
# safe float constants whose byte patterns contain no 0x00 and are distinct
V_ANCHOR = F(ANCHOR)          # 0.10 -> cd cc cc 3d
V_PULSE = F(PULSE)            # 0.90 -> 66 66 66 3f
V_HALF = F(0.45)              # 66 66 e6 3e
V_NEG = F(-0.3)               # 9a 99 99 be
V_KEEP = F(0.01)              # slot-0 keepalive, make_probe's proven pattern
V_SENTINEL = bytes.fromhex("f9e88a26")   # canonical rest sentinel
assert 0 not in V_ANCHOR + V_PULSE + V_HALF + V_NEG + V_KEEP


class Enc:
    """Sequential payload encoder. Tracks the grid position we INTEND the
    engine to be at; every gap is bridged with legacy-safe 3-byte markers
    (00 4k 00, k<=63, always directly after a float)."""

    def __init__(self):
        self.buf = bytearray()
        self.pos = 0           # intended next grid position
        self.after_float = False

    def skip(self, n):
        assert n >= 0
        while n > 0:
            k = min(n, 63)
            assert self.after_float, "marker must follow a float (legacy-safe layout)"
            self.buf += bytes([0, 4 * k, 0])
            self.pos += k
            n -= k
            # legacy reads ONE marker per float; chaining breaks it
            assert n == 0, f"gap needed chaining ({n} left) — add an anchor cell"

    def cell(self, pos, raw4):
        assert pos >= self.pos, (pos, self.pos)
        self.skip(pos - self.pos)
        self.buf += raw4
        self.pos = pos + 1
        self.after_float = True

    def raw(self, data, consumed_cells, note=""):
        """Emit arbitrary bytes we PREDICT consume `consumed_cells` grid
        cells. If the engine disagrees, the anchor jumps channels."""
        self.buf += data
        self.pos += consumed_cells
        self.after_float = True   # constructs under test end in float bytes


def frames_payload(frames, frame_hook):
    """Walk frames; per frame place the anchor + whatever frame_hook returns:
    either [(slot, raw4bytes), ...] extra cells (slot > ANCHOR_SLOT), or a
    callable(enc, base_pos) that emits custom bytes for the frame."""
    enc = Enc()
    for f in range(frames):
        base = f * STRIDE
        if base < enc.pos:
            continue   # frame swallowed by a long rest under test (u16 marker)
        hook = frame_hook(f)
        enc.cell(base, V_KEEP)            # slot-0 keepalive (proven shape)
        enc.cell(base + ANCHOR_SLOT, V_ANCHOR)
        if callable(hook):
            hook(enc, base)
        else:
            for slot, raw4 in sorted(hook):
                enc.cell(base + slot, raw4)
    return bytes(enc.buf)


def header_a(frames):
    # preroll -2 mirrors the engine-accepted make_probe staircase headers
    return struct.pack("<IIIHHiHH", 1, 132 * frames + 28, 13, frames, 3, -2, 16, 3)


def header_c20(frames):
    return struct.pack("<IIIHHHH", 1, 132 * frames + 28, 13, frames, 7, 16, 0)


# ---------------------------------------------------------------- constructs
# Each returns (hook_callable, prediction_note) and is placed on one frame.

def dup_pair(slot):
    def hook(enc, base):
        enc.cell(base + slot, V_PULSE)
        enc.raw(V_PULSE, 1, "dup")     # identical float, adjacent, no marker
    note = (f"dup pair on slot {slot}: tangent-reading -> only {MFG[slot]} fires, "
            f"pos +2; two-real-cells -> {MFG[slot]}+{MFG[slot+1]} both fire, pos +2")
    return hook, note


def odd_marker(tag, slot):
    def hook(enc, base):
        enc.cell(base + slot, V_PULSE)
        # construct: 00 <odd tag> then a plain float. M4 reading: skip tag>>2
        # cells + next float RAW (one cell). We PREDICT tag>>2 + 1 cells.
        enc.raw(bytes([0, tag]) + V_HALF, (tag >> 2) + 1)
    note = (f"00 {tag:02x} after a {MFG[slot]} pulse: M4 -> skip {tag>>2} + RAW 0.45 cell; "
            f"legacy -> reads '00 {tag:02x} ..' as float garbage (cell count differs -> anchor jumps)")
    return hook, note


def u16_marker(lo, hi, slot):
    skip = (lo | (hi << 8)) >> 2
    def hook(enc, enc_base):
        enc.cell(enc_base + slot, V_PULSE)
        enc.raw(bytes([0, lo, hi]), skip)
    note = (f"u16 marker 00 {lo:02x} {hi:02x} after {MFG[slot]} pulse: M4 -> skip {skip} cells "
            f"({skip/33:.1f} frames of rest); legacy -> not a marker, desync")
    return hook, note


def escape_leading_zero(slot):
    # encode float 00 07 0c 3e (0.1368, byte0=00) as: 00 01 00 | 07 0c 3e
    target = bytes.fromhex("00070c3e")
    def hook(enc, base):
        enc.skip(base + slot - enc.pos)
        enc.raw(bytes([0, 1]) + target, 1)
    note = (f"00 01 escape + 00-leading float on slot {slot}: M4 -> one 0.137 cell on "
            f"{MFG[slot]}; legacy -> different byte walk (anchor jump / wrong channel)")
    return hook, note


def bare_zero_float(slot):
    def hook(enc, base):
        enc.cell(base + slot, V_PULSE)
        enc.raw(bytes([0]) + V_HALF, 1)   # PREDICTION: skip-0 + RAW float (m4dp's guess)
    note = (f"bare 00 + float 0.45 after {MFG[slot]} pulse: m4dp guess -> one cell "
            f"({MFG[slot+1]}=0.45); other consumptions show as anchor jumps — THE open question")
    return hook, note


def negative_cell(slot):
    def hook(enc, base):
        enc.skip(base + slot - enc.pos)
        enc.raw(V_NEG, 1)
        enc.cell(base + slot + 2, V_PULSE)
    note = (f"negative -0.3 on slot {slot} then {MFG[slot+2]} pulse: if negatives are real "
            f"cells pos +1 (anchor steady); if skipped/tangent the anchor jumps")
    return hook, note


def sentinel_cells(slot):
    def hook(enc, base):
        enc.cell(base + slot, V_SENTINEL)
        enc.cell(base + slot + 1, V_PULSE)
    note = f"rest sentinel then {MFG[slot+1]} pulse: both grammars agree (control)"
    return hook, note


# ---------------------------------------------------------------- probe defs

def staircase(f0, slots, width):
    """control: one clean 0.9 pulse per listed slot, `width` frames each"""
    def gen(f):
        i = (f - f0) // width
        if 0 <= i < len(slots) and (f - f0) % width not in (0, width - 1):
            return [(slots[i], V_PULSE)]
        return []
    return gen, f0 + len(slots) * width


def build_probe(frames, windows):
    """windows: list of (frame, hook_or_cells, name, note); everything else is
    anchor-only. Returns (payload, manifest_windows)."""
    by_frame = {f: h for f, h, _n, _note in windows}
    payload = frames_payload(frames, lambda f: by_frame.get(f, []))
    man = [{"frame": f, "t": round(f / 30.0, 2), "name": n, "note": note}
           for f, _h, n, note in windows]
    return payload, man


def probes():
    out = {}

    # ---- A: controls + dup (240 frames, 220 Hz)
    win = []
    stair, _ = staircase(12, [6, 9, 12], 12)   # Eh, K, OohQ control pulses
    for f in range(12, 48):
        c = stair(f)
        if c:
            win.append((f, c, f"control {MFG[c[0][0]]}", "clean staircase pulse (both grammars agree)"))
    h, note = sentinel_cells(8);  win.append((70, h, "sentinel", note))
    h, note = dup_pair(4);        win.append((110, h, "dup #1", note))
    h, note = dup_pair(4);        win.append((150, h, "dup #2", note))
    h, note = dup_pair(9);        win.append((190, h, "dup #3 (K/N)", note))
    out["A"] = (240, 220.0, win)

    # ---- B: marker zoo (270 frames, 330 Hz) — riskiest LAST
    win = []
    stair, _ = staircase(12, [6], 12)
    for f in range(12, 24):
        c = stair(f)
        if c:
            win.append((f, c, "control Eh", "baseline pulse"))
    h, note = odd_marker(0x05, 6);  win.append((60, h, "odd 00 05", note))
    h, note = odd_marker(0x29, 6);  win.append((120, h, "odd 00 29", note))
    h, note = u16_marker(0x44, 0x02, 6); win.append((180, h, "u16 00 44 02", note))
    out["B"] = (270, 330.0, win)

    # ---- C: escapes / bare-00 / negatives (300 frames, 440 Hz)
    win = []
    stair, _ = staircase(12, [6], 12)
    for f in range(12, 24):
        c = stair(f)
        if c:
            win.append((f, c, "control Eh", "baseline pulse"))
    h, note = escape_leading_zero(6);  win.append((60, h, "escape 00 01", note))
    h, note = negative_cell(9);        win.append((120, h, "negative cell", note))
    h, note = bare_zero_float(6);      win.append((180, h, "bare 00 #1", note))
    h, note = bare_zero_float(9);      win.append((240, h, "bare 00 #2", note))
    out["C"] = (300, 440.0, win)

    # ---- D: variant-C header acceptance (180 frames, 550 Hz), payload = clean
    win = []
    stair, _ = staircase(12, [6, 9, 12, 14], 24)
    for f in range(12, 108):
        c = stair(f)
        if c:
            win.append((f, c, f"C20 control {MFG[c[0][0]]}",
                        "any mouth movement at all = engine ACCEPTS the 20-byte header"))
    out["D"] = (180, 550.0, win)
    return out


# ---------------------------------------------------------------- audio + fuz

def make_hum(dur, freq, path):
    sr = 44100
    with wave.open(path, "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(sr)
        pcm = bytearray()
        for i in range(int(sr * dur)):
            t = i / sr
            env = min(1.0, t / 0.05, max(0.0, (dur - t) / 0.2))
            s = 0.16 * env * (math.sin(2*math.pi*freq*t) + 0.25*math.sin(2*math.pi*2*freq*t))
            pcm += struct.pack("<h", int(s * 32767))
        w.writeframes(bytes(pcm))


def cmd_build():
    os.makedirs(OUTDIR, exist_ok=True)
    manifest = {}
    # both-grammar self-decode via verify_engine's decoders
    import importlib.util
    spec = importlib.util.spec_from_file_location("ve", os.path.join(HERE, "verify_engine.py"))
    ve = importlib.util.module_from_spec(spec); spec.loader.exec_module(ve)

    for name, (frames, freq, windows) in probes().items():
        payload, man = build_probe(frames, windows)
        lip = (header_c20(frames) if name == "D" else header_a(frames)) + payload
        dur = frames / 30.0 + 0.5
        with tempfile.TemporaryDirectory() as td:
            wavp = os.path.join(td, "p.wav"); xwmp = os.path.join(td, "p.xwm")
            make_hum(dur, freq, wavp)
            subprocess.run([XWMAENCODE, wavp, xwmp], check=True, capture_output=True)
            audio = open(xwmp, "rb").read()
        fuz = b"FUZE" + struct.pack("<II", 1, len(lip)) + lip + audio
        fp = os.path.join(OUTDIR, f"probe_{name}.fuz")
        open(fp, "wb").write(fuz)
        manifest[name] = {"frames": frames, "hz": freq, "file": os.path.basename(fp),
                          "windows": man}
        # self-test: what does each grammar think this probe contains?
        try:
            fr, nc, pre, off = ve.parse_header(lip)
            for g, cells in (("legacy", ve.cells_legacy(lip, off)),
                             ("m4dp", ve.cells_m4dp(lip, off, fr))):
                ser, eff = ve.build_series(cells, fr, pre, g == "legacy")
                act = [MFG[ch] for ch in range(16) if any(v > 0.05 for v in ser[ch])]
                print(f"probe_{name} [{g:6s}] frames={fr} active: {act}")
        except SystemExit as e:
            print(f"probe_{name} decode: {e}")
        print(f"probe_{name}: {frames} frames ({frames/30.0:.1f}s) @ {freq:.0f} Hz, "
              f"{len(windows)} windows -> {fp}")
    open(os.path.join(OUTDIR, "grammar_probe_manifest.json"), "w").write(
        json.dumps(manifest, indent=1))
    print("manifest ->", os.path.join(OUTDIR, "grammar_probe_manifest.json"))


def cmd_deploy(which):
    src = os.path.join(OUTDIR, f"probe_{which}.fuz")
    if not os.path.isfile(src):
        raise SystemExit(f"{src} missing — run build first")
    os.makedirs(os.path.dirname(OVERRIDE), exist_ok=True)
    open(OVERRIDE, "wb").write(open(src, "rb").read())
    hz = {"A": 220, "B": 330, "C": 440, "D": 550}[which]
    print(f"probe {which} deployed -> {OVERRIDE}")
    print(f"you should hear a {hz} Hz hum when Carlotta calls out")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "build":
        cmd_build()
    elif len(sys.argv) >= 3 and sys.argv[1] == "deploy" and sys.argv[2].upper() in "ABCD":
        cmd_deploy(sys.argv[2].upper())
    else:
        sys.exit(__doc__)
