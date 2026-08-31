#!/usr/bin/env python3
"""
read_grammar_probe.py — read an `autest lipcap` capture of the construct-
alphabet probes (make_grammar_probe.py) and report, per construct window, what
the ENGINE did vs what each grammar predicted.

Usage: python read_grammar_probe.py <lipcap_*.csv> [infoID=0008F148]

Output per identified probe run:
  - the ANCHOR TIMELINE: which channel carries the constant ~0.10 anchor in
    each half-second — the anchor lives on BMP; a jump to a neighbor channel
    means a construct changed the engine's cell consumption at that moment
    (BigAah/Aah = engine consumed FEWER cells than predicted, ChjSh/DST = MORE)
  - per manifest window: engine's active channels (mean level) next to the
    legacy and m4dp predictions for those frames
Probes are identified by run duration (A 8s / B 9s / C 10s / D 6s).
"""
import importlib.util
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("ve", os.path.join(HERE, "verify_engine.py"))
ve = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ve)
MFG = ve.MFG

MANIFEST = os.path.join(HERE, "probes", "grammar_probe_manifest.json")


def decode_probe(name):
    fuz = open(os.path.join(HERE, "probes", f"probe_{name}.fuz"), "rb").read()
    import struct
    lipsize = struct.unpack_from("<I", fuz, 8)[0]
    lip = fuz[12:12 + lipsize]
    frames, _nc, preroll, off = ve.parse_header(lip)
    leg, _ = ve.build_series(ve.cells_legacy(lip, off), frames, preroll, True)
    try:
        m4, eff = ve.build_series(ve.cells_m4dp(lip, off, frames), frames, preroll, False)
    except SystemExit:
        m4, eff = None, frames
    return leg, m4, frames


def window_channels(series, f0, f1):
    out = []
    for ch in range(16):
        vals = series[ch][f0:f1]
        m = max(vals) if vals else 0.0
        if m > 0.05:
            out.append((ch, m))
    return out


def fmt(chs):
    return " ".join(f"{MFG[c]}={v:.2f}" for c, v in sorted(chs, key=lambda x: -x[1])) or "-"


def engine_window(cap, t0, off, ta, tb):
    """per-channel mean over capture samples with ta <= t-t0-off < tb"""
    acc = [0.0] * 16
    n = 0
    for t, p in cap:
        rel = (t - t0) / 1000.0 - off
        if ta <= rel < tb:
            for ch in range(16):
                acc[ch] += p[ch]
            n += 1
    if not n:
        return [], 0
    return [(ch, acc[ch] / n) for ch in range(16) if acc[ch] / n > 0.04], n


def main():
    path = sys.argv[1]
    info = sys.argv[2].upper() if len(sys.argv) > 2 else "0008F148"
    man = json.load(open(MANIFEST))
    rows = ve.load_capture(path)
    runs = [r for r in ve.runs_of(rows, info) if len(r) >= 30]
    if not runs:
        raise SystemExit(f"no runs of info {info} in {path}")
    print(f"{len(runs)} run(s) of info {info}\n")

    for ri, run in enumerate(runs):
        dur = (run[-1][0] - run[0][0]) / 1000.0
        # identify probe by duration (lip frames/30, capture may trail a bit)
        probe = min(man, key=lambda k: abs(man[k]["frames"] / 30.0 - dur))
        mismatch = abs(man[probe]["frames"] / 30.0 - dur)
        print(f"=== run {ri}: {dur:.1f}s -> probe {probe} "
              f"({man[probe]['frames']} frames, {man[probe]['hz']:.0f} Hz)"
              + (f"  [WEAK MATCH Δ{mismatch:.1f}s — check the hum pitch you heard]" if mismatch > 1.0 else ""))
        leg, m4, frames = decode_probe(probe)
        t0 = run[0][0]
        cap = run

        # time offset: align on the legacy-agnostic control pulses (both
        # grammars agree there) — search offset maximizing correlation of the
        # engine's summed track against the legacy decode's summed track
        best_off, best_c = 0.0, -1.0
        legsum = [sum(leg[ch][f] for ch in range(16)) for f in range(frames)]
        for oms in range(-700, 1200, 33):
            off = oms / 1000.0
            dot = aa = bb = 0.0
            for t, p in cap:
                rel = (t - t0) / 1000.0 - off
                fr = int(rel * 30.0)
                a = legsum[fr] if 0 <= fr < frames else 0.0
                b = sum(p)
                dot += a * b
                aa += a * a
                bb += b * b
            c = dot / (aa * bb) ** 0.5 if aa > 0 and bb > 0 else 0
            if c > best_c:
                best_c, best_off = c, off
        print(f"  aligned at offset {best_off*1000:+.0f}ms (control corr {best_c:.2f})")

        # anchor timeline: dominant low-level channel per 0.5 s bucket
        print("  anchor timeline (channel holding ~0.1 per 0.5s; BMP = in sync):")
        line = []
        for b in range(int(frames / 30.0 * 2) + 1):
            ta, tb = b * 0.5, b * 0.5 + 0.5
            chs, n = engine_window(cap, t0, best_off, ta, tb)
            low = [(c, v) for c, v in chs if 0.04 <= v <= 0.5]
            line.append(MFG[max(low, key=lambda x: x[1])[0]][:3] if low else "---")
        print("    " + " ".join(line))

        # windows
        print(f"  {'t':>5}  {'window':<18} {'ENGINE':<34} {'legacy-pred':<26} m4dp-pred")
        seen = set()
        for w in man[probe]["windows"]:
            key = (w["name"].split("#")[0], w["frame"] // 12)
            if w["name"].startswith("control") and key in seen:
                continue
            seen.add(key)
            f0, f1 = w["frame"], w["frame"] + 3
            eng, n = engine_window(cap, t0, best_off, f0 / 30.0, f1 / 30.0 + 0.1)
            lp = window_channels(leg, f0, f1)
            mp = window_channels(m4, f0, f1) if m4 else [("?", 0)]
            print(f"  {w['t']:5.1f}s {w['name']:<18} {fmt(eng):<34} {fmt(lp):<26} {fmt(mp)}")
        print()
        for w in man[probe]["windows"]:
            if not w["name"].startswith("control"):
                print(f"  [{w['name']}] {w['note']}")
        print()


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    main()
