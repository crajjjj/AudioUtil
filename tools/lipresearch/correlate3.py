"""Exact-line calibration: v3 capture (with topic-info FormIDs) vs the .lip
files those exact lines came from. No candidate guessing — the INFO FormID in
each row names the fuz file (…_<formid8hex>_<n>.fuz).

Usage: python correlate3.py <capture.csv> <voices_root> [more_roots...]
       voices_root = folder tree containing fuz files (extracted BSA / loose)
"""
import sys, os, glob
import numpy as np
import correlate as C1  # decode_lip (33-slot weights), FPS constants

MFG_NAMES = C1.MFG_NAMES
NSLOTS = C1.NSLOTS
FPS = 30.0
MAXLAG = 15  # +-0.5 s

def load_v3(path):
    rows, hdr = [], None
    for line in open(path):
        if line.startswith("#"):
            continue
        p = line.strip().split(",")
        if hdr is None:
            hdr = p
            it = hdr.index("info")
            cols = [hdr.index(f"unk120_{i}") for i in range(16)]
            continue
        if len(p) != len(hdr):
            continue
        rows.append((int(p[0]), p[1], p[it], [float(p[c]) for c in cols]))
    return rows

def segments(rows, on=0.02, gap_ms=2500, min_ms=500):
    """One segment per continuous INFO-id run (the exact line), keeping quiet
    frames inside — a mid-line pause is still part of the line. A segment ends
    when the info changes or goes silent+unknown for > gap_ms."""
    segs, cur = [], None
    for t, fid, info, p in rows:
        known = info != "00000000"
        if known:
            if cur and (cur["info"] != info or t - cur["end"] > gap_ms):
                segs.append(cur); cur = None
            if not cur:
                cur = {"fid": fid, "info": info, "start": t, "end": t, "rows": []}
            cur["end"] = t
            cur["rows"].append((t, p))
        elif cur and t - cur["end"] > gap_ms:
            segs.append(cur); cur = None
    if cur:
        segs.append(cur)
    # trim leading/trailing dead air, keep interior pauses
    out = []
    for s in segs:
        rs = s["rows"]
        a = 0
        while a < len(rs) and max(rs[a][1]) <= on:
            a += 1
        b = len(rs) - 1
        while b > a and max(rs[b][1]) <= on:
            b -= 1
        if b > a:
            s["rows"] = rs[a:b+1]
            s["start"], s["end"] = s["rows"][0][0], s["rows"][-1][0]
            if s["end"] - s["start"] >= min_ms:
                out.append(s)
    return out

def resample(seg):
    t0, t1 = seg["rows"][0][0], seg["rows"][-1][0]
    n = max(2, int((t1 - t0) / 1000.0 * FPS) + 1)
    ts = [r[0] for r in seg["rows"]]
    out = np.zeros((16, n), dtype=np.float32)
    for j in range(n):
        t = t0 + j * 1000.0 / FPS
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

def find_fuz(roots, info_hex):
    hexl = info_hex.lower()
    hits = []
    for root in roots:
        hits += glob.glob(os.path.join(root, "**", f"*_{hexl}_*.fuz"), recursive=True)
        # ESL/ESP-flagged: filename keeps only lower 6 hex with 00 prefix
        low6 = "00" + hexl[2:]
        if low6 != hexl:
            hits += glob.glob(os.path.join(root, "**", f"*_{low6}_*.fuz"), recursive=True)
    return sorted(set(hits))

def zscore_rows(M):
    mu = M.mean(axis=1, keepdims=True)
    sd = M.std(axis=1, keepdims=True)
    sd[sd < 1e-6] = np.inf
    return (M - mu) / sd

def demean_common(M):
    return M - M.mean(axis=0, keepdims=True)

def corr_matrix(E, D):
    n = min(E.shape[1], D.shape[1])
    if n < 8:
        return None, 0
    return (zscore_rows(E[:, :n]) @ zscore_rows(D[:, :n]).T) / n, n

def best_lag(eA, dA):
    best, bl = -2.0, 0
    for lag in range(-MAXLAG, MAXLAG + 1):
        e = eA[lag:] if lag >= 0 else eA
        d = dA if lag >= 0 else dA[-lag:]
        n = min(len(e), len(d))
        if n < 8:
            continue
        ez, dz = e[:n] - e[:n].mean(), d[:n] - d[:n].mean()
        den = np.linalg.norm(ez) * np.linalg.norm(dz)
        c = float(ez @ dz) / den if den > 1e-9 else 0.0
        if c > best:
            best, bl = c, lag
    return best, bl

def main():
    cap, roots = sys.argv[1], sys.argv[2:]
    segs = []
    for c in cap.split(";"):
        rows = load_v3(c)
        s = segments(rows)
        print(f"capture {os.path.basename(c)}: {len(rows)} rows -> {len(s)} utterance(s)")
        segs += s

    total = np.zeros((16, NSLOTS))
    weight = 0.0
    used = 0
    for si, seg in enumerate(segs):
        E = resample(seg)
        durE = E.shape[1] / FPS
        hits = find_fuz(roots, seg["info"])
        if not hits:
            print(f"  seg{si} info={seg['info']} {durE:.1f}s -> no fuz found (mod line?)")
            continue
        # an info's responses (_1.._N) play back-to-back as one line: try each
        # response alone AND the full concatenation, take the best alignment
        mats = []
        for p in hits:
            dec = C1.decode_lip(p)
            if dec:
                mats.append((os.path.basename(p), np.array(dec[1], dtype=np.float32)))
        if len(mats) > 1:
            mats.append(("+".join(m[0].rsplit("_", 1)[-1][:-4] for m in mats),
                         np.concatenate([m[1] for m in mats], axis=1)))
        bestScore, bestPath, bestC = -1.0, None, None
        for stretch in (0.90, 0.92, 0.94, 0.96, 0.98, 1.0, 1.02, 1.04, 1.06, 1.08, 1.10, 1.12, 1.15):
            # wall-clock capture vs game-time playback can drift a few percent;
            # resample the engine matrix along time and take the best fit
            n0 = E.shape[1]
            n1 = max(8, int(round(n0 * stretch)))
            x1 = np.linspace(0, n0 - 1, n1)
            Es = np.stack([np.interp(x1, np.arange(n0), E[i]) for i in range(16)]).astype(np.float32)
            eA = Es.max(axis=0)
            for name, D in mats:
                c0, lag = best_lag(eA, D.max(axis=0))
                e = Es[:, lag:] if lag >= 0 else Es
                d = D if lag >= 0 else D[:, -lag:]
                C, n = corr_matrix(demean_common(e), demean_common(d))
                if C is None:
                    continue
                if c0 > bestScore:
                    bestScore, bestPath, bestC = c0, f"{name} x{stretch:.2f}", C
        if bestC is None:
            print(f"  seg{si} info={seg['info']} {durE:.1f}s -> {len(hits)} fuz, none aligned")
            continue
        print(f"  seg{si} [{seg['fid']}] {durE:.1f}s -> {bestPath}  align={bestScore:.3f}")
        if bestScore > 0.4:
            w = bestScore * durE
            total += bestC * w
            weight += w
            used += 1

    if weight == 0:
        print("nothing usable")
        return
    T = total / weight
    print(f"\n=== {used} exact line(s) accumulated ===")
    print("=== per-MFG top-3 lip slots (common-mode removed) ===")
    for i in range(16):
        ranked = np.argsort(-T[i])[:3]
        det = "  ".join(f"slot{int(k)}:{T[i][k]:+.3f}" for k in ranked)
        print(f"  MFG {i:2d} {MFG_NAMES[i]:7s} <- {det}")
    print("\n=== slot -> MFG greedy 1:1 ===")
    A = np.abs(T.copy())
    out = []
    for _ in range(16):
        i, k = np.unravel_index(np.argmax(A), A.shape)
        out.append((int(i), int(k), float(T[i, k]))); A[i, :] = -1; A[:, k] = -1
    for i, k, c in sorted(out):
        print(f"  grid slot {k:2d}  ->  MFG {i:2d} {MFG_NAMES[i]:7s}  (corr {c:+.3f})")
    print("\n(OpenFaceFX assumption: BMP=0 ChjSh=2 DST=4 Eee=6 Eh=8 FV=10 i=12 k=14 "
          "N=16 Oh=18 OohQ=20 Aah=22 BigAah=24 R=26 Th=28 W=30)")
    np.save(os.path.join(os.path.dirname(os.path.abspath(cap)) if False else ".", "corr_matrix.npy"), T)
    print("saved corr_matrix.npy")

if __name__ == "__main__":
    main()
