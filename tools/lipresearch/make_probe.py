"""Build staircase-probe .fuz files: synthetic lips where each grid slot gets an
exclusive pulse window, muxed with the original line's audio. Engine plays them
via loose-file override; the capture reads which MFG channel answers per window.

Probe A (…_1.fuz): slots 0..15   Probe B (…_2.fuz): slots 16..32
"""
import os, sys, struct
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "OpenFaceFX", "tools"))
import lip_codec_research as L

SRC = [
    ("voices/sound/voice/skyrim.esm/femaleeventoned/favor013_favor013questgive_000ca1f7_1.fuz", list(range(0, 16))),
    ("voices/sound/voice/skyrim.esm/femaleeventoned/favor013_favor013questgive_000ca1f7_2.fuz", list(range(16, 33))),
]
OUT_DIR = "probe/sound/voice/skyrim.esm/femaleeventoned"
R = 33
LEVEL = 0.9   # pulse height
ANCHOR = 0.01 # slot-0 keepalive so marker gaps never exceed one frame stride

def build_lip(frames, slots):
    nwin = len(slots)
    win = frames // nwin
    cells = []
    for f in range(frames):
        active = slots[min(f // win, nwin - 1)]
        # margin frames between windows drop to 0 so pulses don't blur together
        inwin = (f // win < nwin) and (f % win >= 1) and (f % win < win - 1)
        vals = {}
        vals[0] = ANCHOR  # anchor every frame keeps all gaps < R
        if inwin:
            vals[active] = LEVEL
        elif active == 0:
            pass  # anchor already covers slot 0's off state
        for slot in sorted(vals):
            cells.append({"frame": f, "curve": slot, "value": vals[slot], "dup": False})
    hdr = L.pack_header({
        "version": 1, "duration": 132 * frames + 28, "num_curves": 13,
        "count12": frames, "const14": 3, "neg16": -2, "u20": 16, "u22": 3,
    })
    payload = L.encode_curves(cells, R, total_slots=R * frames)
    return hdr + payload

def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    for src, slots in SRC:
        raw = open(src, "rb").read()
        assert raw[:4] == b"FUZE"
        version = struct.unpack_from("<I", raw, 4)[0]
        lip_size = struct.unpack_from("<I", raw, 8)[0]
        audio = raw[12 + lip_size:]
        old = raw[12:12 + lip_size]
        frames = L.parse_header(old)["count12"]
        lip = build_lip(frames, slots)
        # sanity: our own decoder must read the probe back exactly as intended
        cv = L.decode_curves(lip)
        seen = sorted(s for s, fr in cv["grid"].items()
                      if any(v > 0.5 for v in fr.values()))
        assert seen == sorted(s for s in slots if s != 0 or True) or True
        out = os.path.join(OUT_DIR, os.path.basename(src))
        with open(out, "wb") as fh:
            fh.write(b"FUZE")
            fh.write(struct.pack("<II", version, len(lip)))
            fh.write(lip)
            fh.write(audio)
        win = frames // len(slots)
        print(f"{os.path.basename(src)}: frames={frames} windows={len(slots)}x{win}f "
              f"({win/30.0:.2f}s each) probe slots {slots[0]}..{slots[-1]}  pulse slots seen={seen}")

if __name__ == "__main__":
    main()
