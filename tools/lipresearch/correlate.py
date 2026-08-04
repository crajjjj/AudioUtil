"""Map .lip grid slots -> MFG phoneme channels from an in-game engine capture.

Inputs:
  1. lipcap CSV (AudioUtil `autest lipcap`): t_ms,formid,p0..p15  — the ENGINE's
     own phoneme output while NPCs spoke (ground truth).
  2. A folder of candidate .fuz/.lip files (the voicetype folders of the NPCs
     talked to, extracted from the voices BSA or loose).

Pipeline: segment the capture into utterances -> decode every candidate lip ->
for each (utterance, candidate) find the best time alignment and a
permutation-invariant channel-correlation score -> best candidate per utterance
-> accumulate the 16x16 channel-correlation matrix across matches -> solve the
assignment -> print the verified slot->MFG map.

Usage: python correlate.py <capture.csv> <candidate_dir> [more_dirs...]
"""
import sys, os, csv, math, struct

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "OpenFaceFX", "tools"))
import lip_codec_research as L

MFG_NAMES = ["Aah","BigAah","BMP","ChjSh","DST","Eee","Eh","FV","i","k","N","Oh","OohQ","R","Th","W"]
NSLOTS = 33  # correlate against ALL grid slots — let the data say where curves live
FPS = 30.0

# ---------------- capture side ----------------

def load_capture(path):
    """v1 (p0..p15) or v2 (13-block probe; engine dialogue phonemes = unk120_*)."""
    rows, hdr = [], None
    with open(path, newline="") as f:
        for line in f:
            if line.startswith("#"):
                continue
            parts = line.strip().split(",")
            if hdr is None:
                hdr = parts
                if "unk120_0" in hdr:
                    cols = [hdr.index(f"unk120_{i}") for i in range(16)]
                else:
                    cols = [hdr.index(f"p{i}") for i in range(16)]
                continue
            if len(parts) != len(hdr):
                continue
            rows.append((int(parts[0]), parts[1], [float(parts[c]) for c in cols]))
    return rows

def segment(rows, on=0.02, gap_ms=350, min_ms=600):
    """Contiguous speech runs per speaker: activity above `on`, joining gaps."""
    segs = []
    cur = None
    for t, fid, p in rows:
        act = max(p)
        if act > on:
            if cur and (fid != cur["fid"] or t - cur["end"] > gap_ms):
                segs.append(cur); cur = None
            if not cur:
                cur = {"fid": fid, "start": t, "end": t, "rows": []}
            cur["end"] = t
            cur["rows"].append((t, p))
        elif cur and t - cur["end"] > gap_ms:
            segs.append(cur); cur = None
    if cur:
        segs.append(cur)
    return [s for s in segs if s["end"] - s["start"] >= min_ms]

def resample(seg):
    """Segment rows -> 16 x N matrix on the 30 fps grid (linear interp)."""
    t0, t1 = seg["rows"][0][0], seg["rows"][-1][0]
    n = max(2, int((t1 - t0) / 1000.0 * FPS) + 1)
    out = [[0.0]*n for _ in range(16)]
    ts = [r[0] for r in seg["rows"]]
    for j in range(n):
        t = t0 + j * 1000.0 / FPS
        # find bracketing rows
        lo = 0
        while lo + 1 < len(ts) and ts[lo+1] <= t:
            lo += 1
        hi = min(lo + 1, len(ts) - 1)
        span = ts[hi] - ts[lo]
        frac = (t - ts[lo]) / span if span > 0 else 0.0
        for i in range(16):
            a, b = seg["rows"][lo][1][i], seg["rows"][hi][1][i]
            out[i][j] = a + (b - a) * frac
    return out

# ---------------- lip side ----------------

def read_lip_bytes(path):
    data = open(path, "rb").read()
    if path.lower().endswith(".fuz"):
        if len(data) < 12 or data[:4] != b"FUZE":
            return None
        lip_size = struct.unpack_from("<I", data, 8)[0]
        if 12 + lip_size > len(data) or lip_size < 24:
            return None
        return data[12:12+lip_size]
    return data if len(data) >= 24 else None

def decode_lip(path):
    """-> (frames, NSLOTS x frames matrix) or None.
    A cell's value is kept iff it's a plausible WEIGHT: sentinel -> 0.0 (rest),
    v in [0,1] kept INCLUDING 0 (closures are the most distinctive events);
    anything else (negative / >1) is a Hermite tangent -> ignored."""
    d = read_lip_bytes(path)
    if d is None:
        return None
    try:
        h = L.parse_header(d)
        if h["u20"] != 16:
            # variant B: one extra header byte at offset 14, const14=2
            # (2-component keys); dropping the byte yields the standard layout
            d = d[:14] + d[15:]
            h = L.parse_header(d)
            if h["u20"] != 16:
                return None
        cells, R, _ = L.decode_cells(d)
    except Exception:
        return None
    frames = h["count12"]
    if frames < 8:
        return None
    sparse = [{} for _ in range(NSLOTS)]
    for c in cells:
        f, slot = c["frame"], c["curve"]
        if not (0 <= f < frames and 0 <= slot < NSLOTS):
            continue
        if c["is_sentinel"]:
            sparse[slot][f] = 0.0
        elif 0.0 <= c["value"] <= 1.0001:
            sparse[slot][f] = min(1.0, c["value"])
    mat = [[0.0]*frames for _ in range(NSLOTS)]
    for k in range(NSLOTS):
        pts = sorted(sparse[k].items())
        if not pts:
            continue
        for f in range(frames):
            if f <= pts[0][0]:
                mat[k][f] = pts[0][1]
            elif f >= pts[-1][0]:
                mat[k][f] = pts[-1][1]
            else:
                for a in range(len(pts)-1):
                    (f0, v0), (f1, v1) = pts[a], pts[a+1]
                    if f0 <= f <= f1:
                        tt = (f - f0) / (f1 - f0) if f1 > f0 else 0
                        mat[k][f] = v0 + (v1 - v0) * tt
                        break
    return frames, mat

# ---------------- correlation ----------------

def corr(a, b):
    n = min(len(a), len(b))
    if n < 4:
        return 0.0
    a, b = a[:n], b[:n]
    ma, mb = sum(a)/n, sum(b)/n
    va = math.sqrt(sum((x-ma)**2 for x in a))
    vb = math.sqrt(sum((x-mb)**2 for x in b))
    if va < 1e-6 or vb < 1e-6:
        return 0.0
    return sum((a[i]-ma)*(b[i]-mb) for i in range(n)) / (va*vb)

def act_sig(mat):
    return [max(mat[i][j] for i in range(len(mat))) for j in range(len(mat[0]))]

def shift(sig, lag):
    if lag >= 0:
        return sig[lag:]
    return [0.0]*(-lag) + sig

def demean_common(M):
    """Remove the per-frame cross-channel mean: keeps each channel's DISTINCTIVE
    signal, killing the shared talk/silence envelope that correlates everything
    with everything."""
    rows, n = len(M), len(M[0])
    out = [[0.0]*n for _ in range(rows)]
    for j in range(n):
        mu = sum(M[i][j] for i in range(rows)) / rows
        for i in range(rows):
            out[i][j] = M[i][j] - mu
    return out

def channel_matrix(E, D, lag, decommon=False):
    """16x16 corr matrix between engine channels (E) and lip slots (D) at lag."""
    if decommon:
        E = demean_common(E)
        D = demean_common(D)
    C = [[0.0]*len(D) for _ in range(len(E))]
    for i in range(len(E)):
        e = shift(E[i], lag) if lag >= 0 else E[i]
        for k in range(len(D)):
            d = D[k] if lag >= 0 else shift(D[k], lag)
            C[i][k] = corr(e, d)
    return C

def greedy_assign(C):
    """Best one-to-one pairing (greedy on |corr|); returns list of (mfg, slotIdx, corr)."""
    pairs, usedE, usedD = [], set(), set()
    flat = [(abs(C[i][k]), i, k, C[i][k]) for i in range(len(C)) for k in range(len(C[0]))]
    flat.sort(reverse=True)
    for _, i, k, c in flat:
        if i in usedE or k in usedD:
            continue
        usedE.add(i); usedD.add(k)
        pairs.append((i, k, c))
    return pairs

def match(E, D):
    """Try lags; return (score, lag, C). Score = mean of top-6 assigned |corr|."""
    eA, dA = act_sig(E), act_sig(D)
    best = (0.0, 0, None)
    maxlag = 12  # +-0.4 s
    for lag in range(-maxlag, maxlag+1):
        c0 = corr(shift(eA, max(lag,0)), shift(dA, max(-lag,0)))
        if c0 < best[0] * 0.5:  # cheap prefilter on total activity
            continue
        C = channel_matrix(E, D, lag)
        pairs = greedy_assign(C)
        score = sum(abs(c) for _,_,c in pairs[:6]) / 6.0
        if score > best[0]:
            best = (score, lag, C)
    return best

# ---------------- main ----------------

def main():
    cap_path, dirs = sys.argv[1], sys.argv[2:]
    rows = load_capture(cap_path)
    segs = segment(rows)
    print(f"capture: {len(rows)} rows -> {len(segs)} utterance(s)")
    cands = []
    for d in dirs:
        for root, _, files in os.walk(d):
            for fn in files:
                if fn.lower().endswith((".fuz", ".lip")):
                    p = os.path.join(root, fn)
                    dec = decode_lip(p)
                    if dec:
                        cands.append((p, dec[0], dec[1]))
    print(f"candidates: {len(cands)} decodable lip(s)")
    if not segs or not cands:
        return

    total = [[0.0]*NSLOTS for _ in range(16)]
    weight = 0.0
    for si, seg in enumerate(segs):
        E = resample(seg)
        durE = len(E[0]) / FPS
        best = (0.0, None, None, None)
        for path, frames, D in cands:
            durD = frames / FPS
            if abs(durD - durE) > max(1.0, 0.35 * durE):
                continue
            score, lag, C = match(E, D)
            if score > best[0]:
                best = (score, path, lag, C)
        score, path, lag, C = best
        name = os.path.basename(path) if path else "-"
        print(f"  seg{si} [{seg['fid']}] {durE:.1f}s -> {name}  score={score:.3f}")
        if path and score > 0.55:
            # re-correlate at the found lag with the common mode removed: the
            # residual per-channel signal is what identifies WHICH phoneme
            frames, D = decode_lip(path)
            Cd = channel_matrix(E, D, lag, decommon=True)
            w = score * durE
            for i in range(16):
                for k in range(NSLOTS):
                    total[i][k] += Cd[i][k] * w
            weight += w

    if weight == 0:
        print("no confident matches — capture more lines or widen candidates")
        return
    print("\n=== per-MFG top-3 lip slots (accumulated, common-mode removed) ===")
    for i in range(16):
        ranked = sorted(range(NSLOTS), key=lambda k: -total[i][k])
        det = "  ".join(f"slot{k}:{total[i][k]/weight:+.3f}" for k in ranked[:3])
        print(f"  MFG {i:2d} {MFG_NAMES[i]:7s} <- {det}")
    print("\n=== slot -> MFG assignment (accumulated) ===")
    pairs = greedy_assign(total)
    pairs.sort(key=lambda x: x[0])
    for i, k, c in pairs:
        print(f"  grid slot {k:2d}  ->  MFG {i:2d} {MFG_NAMES[i]:7s}  (corr {c/weight:+.3f})")
    print("\n(OpenFaceFX assumption for comparison: "
          "BMP=0 ChjSh=2 DST=4 Eee=6 Eh=8 FV=10 i=12 k=14 N=16 Oh=18 OohQ=20 Aah=22 BigAah=24 R=26 Th=28 W=30)")

if __name__ == "__main__":
    main()
