"""Read the staircase-probe capture: which engine MFG channel fires in each
probe window -> direct grid-slot -> MFG map.
Usage: python read_probe.py <capture.csv>"""
import sys
import numpy as np

MFG = ["Aah","BigAah","BMP","ChjSh","DST","Eee","Eh","FV","i","k","N","Oh","OohQ","R","Th","W"]
INFO = "000CA1F7"
# probe layout (from make_probe.py output)
WINA = [(s, 172, 16, 10) for s in range(16)]   # slots 0..15, 172f, 16 windows x 10f
WINB = [(s, 223, 17, 13) for s in range(16, 33)]

def load(path):
    rows, hdr = [], None
    for line in open(path):
        if line.startswith("#"):
            continue
        p = line.strip().split(",")
        if hdr is None:
            hdr = p
            it = hdr.index("info")
            key = "phoneme1" if "phoneme1_0" in hdr else "unk120"  # pre/post 0.9.15 capture CSVs
            cols = [hdr.index(f"{key}_{i}") for i in range(16)]
            continue
        if len(p) != len(hdr):
            continue
        rows.append((int(p[0]), p[it], [float(p[c]) for c in cols]))
    return rows

rows = load(sys.argv[1])
# contiguous runs of the probe info
runs, cur = [], None
for t, info, p in rows:
    if info == INFO:
        if cur and t - cur[-1][0] > 2500:
            runs.append(cur); cur = None
        if cur is None:
            cur = []
        cur.append((t, p))
    elif cur and t - cur[-1][0] > 2500:
        runs.append(cur); cur = None
if cur:
    runs.append(cur)
print(f"{len(runs)} probe run(s) of info {INFO}")

for ri, run in enumerate(runs):
    ts = np.array([r[0] for r in run], dtype=np.float64)
    P = np.array([r[1] for r in run], dtype=np.float32).T  # 16 x N
    act = P.max(axis=0)
    on = np.where(act > 0.15)[0]
    if len(on) == 0:
        print(f" run{ri}: no activity"); continue
    t0 = ts[on[0]]
    dur = (ts[on[-1]] - t0) / 1000.0
    print(f"\n run{ri}: {len(run)} samples, active {dur:.1f}s from t={t0:.0f}ms")
    # chronological pulse events: channel crossings above 0.3
    events = []
    for ch in range(16):
        v = P[ch]
        above = v > 0.3
        i = 0
        while i < len(v):
            if above[i]:
                j = i
                while j < len(v) and above[j]:
                    j += 1
                pk = i + int(np.argmax(v[i:j]))
                events.append(((ts[pk] - t0) / 1000.0, ch, float(v[pk]), (ts[j-1]-ts[i])/1000.0))
                i = j
            else:
                i += 1
    events.sort()
    print("  t(s)   ch  name     peak  width")
    for t, ch, pk, w in events:
        print(f"  {t:6.2f}  {ch:2d}  {MFG[ch]:7s} {pk:.2f}  {w:.2f}s")
