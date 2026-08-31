#!/usr/bin/env python3
"""
verify_engine.py — the grammar engine-verification loop (RLE Hypothesis.md §4,
repurposed for the legacy-vs-m4dp grammar verdict).

Idea: override a vanilla dialogue line's .fuz with one whose LIP block is the
committed Rajdazan regression fixture — the file on which the legacy and m4dp
grammars decode WILDLY different mouths (legacy: 32 active slots, desynced;
m4dp: 23 slots, near-exact landing). Trigger the line in dialogue, capture the
engine's own decode of that lip off the speaker's phoneme1 facegen block
(`autest lipcap`), then score both candidate decodes against the engine track.
Whichever grammar the engine's mouth matches is the real one.

Subcommands:

  build   synth a 4.2 s hum wav -> xwmaencode -> pack FUZE(lip=fixture) and
          deploy to the MO2 overwrite folder as the Carlotta favor line
          (sound/voice/skyrim.esm/femaleeventoned/favor013_favor013questgive_000ca1f7_1.fuz
          — same vehicle line as the slot-map staircase probes)

  list    show which dialogue infos a capture CSV recorded (id, speaker,
          samples, mouth activity) — use it to pick a repeatable line to probe:
          python verify_engine.py list <lipcap_*.csv>

  check   score a lipcap capture CSV against both grammars:
          python verify_engine.py check <lipcap_*.csv> [infoID]
          (infoID defaults to 000CA1F7 — pass the 8-hex id of the line the
          probe fuz is deployed on)

In-game procedure (after build):
  1. launch via MO2 (overwrite wins all conflicts, no mod/order changes needed)
  2. console: `autest lipcap start`
  3. talk to Carlotta Valentia (Whiterun market) and trigger the favor line
     (info 000CA1F7) — same trigger as the slot-map probe sessions
  4. console: `autest lipcap stop`
  5. python verify_engine.py check <Data|overwrite>/SKSE/Plugins/AudioUtil/lipcap_*.csv

Cleanup: delete the fuz from overwrite when done (build prints the path).
"""
import math
import os
import struct
import subprocess
import sys
import tempfile
import wave

HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "fixtures", "RajdazanTG_RajdazanTG03Let_00000BE2_1.lip")
OVERRIDE = r"E:\nefaram\overwrite\sound\voice\skyrim.esm\femaleeventoned\favor013_favor013questgive_000ca1f7_1.fuz"
XWMAENCODE = os.path.join(os.environ.get("SKYRIM_GAME_PATH",
    r"C:\SteamLibrary\steamapps\common\Skyrim Special Edition"),
    "Tools", "Audio", "xwmaencode.exe")
INFO = "000CA1F7"
STRIDE = 33
MFG = ["Aah", "BigAah", "BMP", "ChjSh", "DST", "Eee", "Eh", "FV",
       "I", "K", "N", "Oh", "OohQ", "R", "Th", "W"]


# ------------------------------------------------------------------ decoders
# Both mirror src/LipData.cpp (0.9.15) exactly: same header handling, same
# cells -> dense 30 fps series build (lead-skipped, no release fade — the fade
# is an AudioUtil playback nicety, not part of either grammar).

def parse_header(d):
    """-> (frames, num_curves, preroll, payload_off) — fixture is variant A."""
    num_curves = struct.unpack_from("<I", d, 8)[0]
    frames = struct.unpack_from("<H", d, 12)[0]
    c14 = struct.unpack_from("<H", d, 14)[0]
    if c14 == 3 and struct.unpack_from("<H", d, 20)[0] == 16:
        return frames, num_curves, struct.unpack_from("<i", d, 16)[0], 24
    if c14 == 7 and struct.unpack_from("<H", d, 16)[0] == 16:
        return frames, num_curves, 0, 20
    raise SystemExit("unsupported header variant for this probe")


def sane(v):
    a = abs(v)
    if 1e-17 < a < 1e-13:
        return 0.0
    if -1.0001 <= v <= 1.0001 and (v == 0.0 or a > 1e-30):
        return v
    return None


def cells_legacy(d, off):
    cells, pos, i, n = [], 0, off, len(d)
    while i + 4 <= n:
        v = struct.unpack_from("<f", d, i)[0]
        i += 4
        floats = 1
        if i + 4 <= n and d[i:i + 4] == d[i - 4:i]:
            floats = 2
            i += 4
        skip = 0
        if i + 3 <= n and d[i] == 0 and d[i + 2] == 0 and d[i + 1] and d[i + 1] % 4 == 0:
            skip = d[i + 1] // 4
            i += 3
        cells.append((pos, v))
        pos += floats + skip
    return cells


def cells_m4dp(d, off, frames):
    """The constrained-DP decode, mirroring LipData::DecodeM4DP."""
    n, target = len(d), frames * STRIDE
    reach = {off: {0: None}}

    def read_esc(i):
        bb, j = [], i
        while len(bb) < 4 and j < n:
            if d[j] == 0:
                if j + 2 < n and d[j + 1] == 1:
                    bb.append(d[j + 2]); j += 3
                else:
                    return None
            else:
                bb.append(d[j]); j += 1
        return (bytes(bb), j) if len(bb) == 4 else None

    def sane_at(i):
        return sane(struct.unpack_from("<f", d, i)[0]) if i + 4 <= n else None

    for i in range(off, n + 1):
        m = reach.get(i)
        if not m:
            continue
        for pos in list(m):
            if pos > target:
                continue
            r = read_esc(i)
            if r:
                v = sane(struct.unpack("<f", r[0])[0])
                if v is not None:
                    reach.setdefault(r[1], {}).setdefault(pos + 1, (i, pos, "F", v, 0))
            if i < n and d[i] == 0:
                t = d[i + 1] if i + 1 < n else -1
                if t > 0 and t & 1 and i + 6 <= n:
                    v = sane_at(i + 2)
                    if v is not None:
                        reach.setdefault(i + 6, {}).setdefault(
                            pos + (t >> 2) + 1, (i, pos, "R", v, t >> 2))
                if t >= 0 and not t & 1 and i + 3 <= n:
                    tag = t | (d[i + 2] << 8 if i + 2 < n else 0)
                    if tag:
                        reach.setdefault(i + 3, {}).setdefault(
                            pos + (tag >> 2), (i, pos, "M", 0.0, 0))
                v = sane_at(i + 1)
                if v is not None:
                    reach.setdefault(i + 5, {}).setdefault(pos + 1, (i, pos, "B", v, 0))

    best = None
    for i in range(n, max(off, n - 3) - 1, -1):
        for pos in reach.get(i, {}):
            bad = 10**9 + (pos - target) if pos > target else target - pos
            if best is None or bad < best[0]:
                best = (bad, i, pos)
    if best is None or best[0] >= STRIDE:
        raise SystemExit("m4dp abstained on this lip (unexpected for the fixture)")
    cells, (ci, cp) = [], (best[1], best[2])
    while True:
        rec = reach[ci][cp]
        if rec is None:
            break
        pi, pp, kind, v, skip = rec
        if kind in "FB":
            cells.append((pp, v))
        elif kind == "R":
            cells.append((pp + skip, v))
        ci, cp = pi, pp
    cells.reverse()
    return cells


def build_series(cells, frames, preroll, legacy_range):
    """16 phoneme channels x effFrames dense series, per LipData::BuildAnim."""
    lead = max(0, min(150, -preroll))
    if lead + 1 >= frames:
        lead = 0
    eff = frames - lead
    series = [[0.0] * eff for _ in range(16)]
    for pos, v in cells:
        frame, slot = pos // STRIDE, pos % STRIDE
        if frame < lead or frame >= frames or slot >= 16:
            continue
        f = frame - lead
        if abs(v) < 1e-6:
            series[slot][f] = 0.0
        elif 0.0 <= v <= 1.0001:
            series[slot][f] = min(v, 1.0)
        elif not legacy_range and -1.0001 <= v < 0.0:
            series[slot][f] = 0.0
    return series, eff


def decode_both():
    d = open(FIXTURE, "rb").read()
    frames, _nc, preroll, off = parse_header(d)
    leg, _ = build_series(cells_legacy(d, off), frames, preroll, True)
    m4, eff = build_series(cells_m4dp(d, off, frames), frames, preroll, False)
    return leg, m4, eff


# ------------------------------------------------------------------ build

def cmd_build():
    lip = open(FIXTURE, "rb").read()
    frames = struct.unpack_from("<H", lip, 12)[0]
    dur = frames / 30.0 + 0.4
    # synth a soft 220 Hz hum (audibly a probe; the engine only needs audio to
    # run at least as long as the lip — the mouth comes from the LIP block)
    sr = 44100
    nsamp = int(sr * dur)
    with tempfile.TemporaryDirectory() as td:
        wav_path = os.path.join(td, "probe.wav")
        with wave.open(wav_path, "wb") as w:
            w.setnchannels(1)
            w.setsampwidth(2)
            w.setframerate(sr)
            frames_pcm = bytearray()
            for i in range(nsamp):
                t = i / sr
                env = min(1.0, t / 0.05, max(0.0, (dur - t) / 0.2))
                s = 0.18 * env * (math.sin(2 * math.pi * 220 * t) +
                                  0.3 * math.sin(2 * math.pi * 440 * t))
                frames_pcm += struct.pack("<h", int(s * 32767))
            w.writeframes(bytes(frames_pcm))
        xwm_path = os.path.join(td, "probe.xwm")
        if not os.path.isfile(XWMAENCODE):
            raise SystemExit(f"xwmaencode not found: {XWMAENCODE} (set SKYRIM_GAME_PATH)")
        subprocess.run([XWMAENCODE, wav_path, xwm_path], check=True,
                       capture_output=True)
        audio = open(xwm_path, "rb").read()
    os.makedirs(os.path.dirname(OVERRIDE), exist_ok=True)
    with open(OVERRIDE, "wb") as fh:
        fh.write(b"FUZE")
        fh.write(struct.pack("<II", 1, len(lip)))
        fh.write(lip)
        fh.write(audio)
    print(f"probe fuz deployed -> {OVERRIDE}")
    print(f"  lip: fixture ({frames} frames, {frames/30.0:.2f}s), audio: {dur:.1f}s hum")
    print("  in-game: autest lipcap start -> Carlotta favor line -> autest lipcap stop")


# ------------------------------------------------------------------ check

def load_capture(path):
    rows, hdr, cols, it = [], None, None, None
    for line in open(path):
        if line.startswith("#"):
            continue
        p = line.strip().split(",")
        if hdr is None:
            hdr = p
            it = hdr.index("info")
            key = "phoneme1" if "phoneme1_0" in hdr else "unk120"
            cols = [hdr.index(f"{key}_{i}") for i in range(16)]
            continue
        if len(p) != len(hdr):
            continue
        rows.append((int(p[0]), p[it], [float(p[c]) for c in cols]))
    return rows


def runs_of(rows, info):
    runs, cur = [], None
    for t, i, p in rows:
        if i == info:
            if cur and t - cur[-1][0] > 2500:
                runs.append(cur)
                cur = None
            cur = cur or []
            cur.append((t, p))
        elif cur and t - cur[-1][0] > 2500:
            runs.append(cur)
            cur = None
    if cur:
        runs.append(cur)
    return runs


def sample_series(series, tsec):
    """linear-interp a 30 fps series at time tsec (clamped)."""
    pos = tsec * 30.0
    i = int(pos)
    if i < 0 or i >= len(series):
        return 0.0
    if i + 1 >= len(series):
        return series[i]
    return series[i] + (series[i + 1] - series[i]) * (pos - i)


def score_run(run, grammars):
    """Find the time offset aligning each grammar to the capture, return
    per-grammar (best offset, per-channel rmse/corr, totals)."""
    t0 = run[0][0]
    cap = [((t - t0) / 1000.0, p) for t, p in run]
    out = {}
    for name, (series, eff) in grammars.items():
        best = None
        for off_ms in range(-500, 1500, 33):
            off = off_ms / 1000.0
            se = sse = n = 0.0
            dot = ca = cb = 0.0
            for tsec, p in cap:
                for ch in range(16):
                    a = sample_series(series[ch], tsec - off)
                    b = p[ch]
                    se += (a - b) ** 2
                    dot += a * b
                    ca += a * a
                    cb += b * b
                    n += 1
            rmse = math.sqrt(se / max(n, 1))
            corr = dot / math.sqrt(ca * cb) if ca > 0 and cb > 0 else 0.0
            if best is None or corr > best[1]:
                best = (off, corr, rmse)
        off = best[0]
        perch = []
        for ch in range(16):
            se = n = dot = ca = cb = 0.0
            for tsec, p in cap:
                a = sample_series(series[ch], tsec - off)
                b = p[ch]
                se += (a - b) ** 2
                dot += a * b
                ca += a * a
                cb += b * b
                n += 1
            corr = dot / math.sqrt(ca * cb) if ca > 1e-9 and cb > 1e-9 else float("nan")
            perch.append((math.sqrt(se / max(n, 1)), corr, math.sqrt(ca / n), math.sqrt(cb / n)))
        # channel-energy profile match: cosine similarity of the 16-dim
        # per-channel RMS vectors (ours vs engine). Alignment-free and immune
        # to engine-side smoothing/gain — it asks only "does the engine put
        # energy on the channels this grammar decodes, and none where it
        # decodes silence?" (the fixture's two decodes differ exactly there)
        pa = [x[2] for x in perch]
        pb = [x[3] for x in perch]
        dotp = sum(a * b for a, b in zip(pa, pb))
        na = math.sqrt(sum(a * a for a in pa))
        nb = math.sqrt(sum(b * b for b in pb))
        profile = dotp / (na * nb) if na > 1e-9 and nb > 1e-9 else 0.0
        out[name] = (off, best[1], best[2], perch, profile)
    return out


def cmd_list(path):
    rows = load_capture(path)
    seen = {}
    for t, info, p in rows:
        if info == "00000000":
            continue
        e = seen.setdefault(info, [0, 0.0, t, t])
        e[0] += 1
        e[1] = max(e[1], max(p))
        e[3] = t
    if not seen:
        print("no dialogue infos in this capture")
        return
    print(f"{'info':>8}  {'samples':>7}  {'peak mouth':>10}  span")
    for info, (n, pk, t0, t1) in sorted(seen.items(), key=lambda kv: -kv[1][0]):
        print(f"{info:>8}  {n:>7}  {pk:>10.2f}  {t0/1000.0:.1f}s..{t1/1000.0:.1f}s")
    print()
    print("pick a line you can re-trigger at will; deploy the probe onto that")
    print("info's voice file, re-capture, then: check <csv> <infoID>")


def cmd_check(path, info=INFO):
    leg, m4, eff = decode_both()
    grammars = {"legacy": (leg, eff), "m4dp": (m4, eff)}
    rows = load_capture(path)
    runs = [r for r in runs_of(rows, info) if len(r) >= 20]
    if not runs:
        raise SystemExit(f"no runs of info {info} in {path} — was the line triggered "
                         "while lipcap was recording?")
    print(f"{len(runs)} capture run(s) of info {info}; lip = {eff} frames ({eff/30.0:.2f}s)")
    print()
    for ri, run in enumerate(runs):
        res = score_run(run, grammars)
        print(f"run {ri}: {len(run)} samples over "
              f"{(run[-1][0]-run[0][0])/1000.0:.1f}s")
        for name in ("legacy", "m4dp"):
            off, corr, rmse, _perch, profile = res[name]
            print(f"  {name:6s}: profile={profile:+.3f}  corr={corr:+.3f}  "
                  f"rmse={rmse:.3f}  (offset {off*1000:+.0f}ms)")
        # verdict: channel profile first (structural, post-processing-proof),
        # time-series corr as tiebreak when profiles are within 0.02
        pl, pm = res["legacy"][4], res["m4dp"][4]
        if abs(pl - pm) > 0.02:
            winner = "legacy" if pl > pm else "m4dp"
            margin = f"profile margin {abs(pl-pm):.3f}"
        else:
            winner = max(("legacy", "m4dp"), key=lambda g: res[g][1])
            margin = f"profiles tied; corr margin {abs(res['legacy'][1]-res['m4dp'][1]):.3f}"
        print(f"  --> engine matches: {winner.upper()}  ({margin})")
        _, _, _, perch, _ = res[winner]
        print(f"  per-channel ({winner}):  ch name    rmse  corr  ours  engine")
        for ch in range(16):
            rm, co, ra, rb = perch[ch]
            if ra < 0.01 and rb < 0.01:
                continue  # both silent
            print(f"    {ch:2d} {MFG[ch]:7s} {rm:.3f}  "
                  f"{'  n/a' if math.isnan(co) else f'{co:+.2f}'}  {ra:.3f}  {rb:.3f}")
        print()


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "build":
        cmd_build()
    elif len(sys.argv) >= 3 and sys.argv[1] == "list":
        cmd_list(sys.argv[2])
    elif len(sys.argv) >= 3 and sys.argv[1] == "check":
        cmd_check(sys.argv[2], *([sys.argv[3].upper()] if len(sys.argv) > 3 else []))
    else:
        sys.exit(__doc__)
