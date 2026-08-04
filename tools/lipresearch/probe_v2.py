import sys

path = sys.argv[1]
hdr = None
rows = []
for line in open(path):
    if line.startswith("#"):
        continue
    if hdr is None:
        hdr = line.strip().split(",")
        continue
    p = line.strip().split(",")
    if len(p) == len(hdr):
        rows.append(p)

print(f"rows={len(rows)} cols={len(hdr)}")
# per column: max, nonzero count (value cols); counts (for _n cols)
blocks = {}
for ci, name in enumerate(hdr):
    if ci < 2:
        continue
    base = name.rsplit("_", 1)[0]
    blocks.setdefault(base, {"n": 0, "act": {}})
    if name.endswith("_n"):
        mx = max(int(r[ci]) for r in rows)
        blocks[base]["n"] = mx
    else:
        idx = int(name.rsplit("_", 1)[1])
        vals = [float(r[ci]) for r in rows]
        mx = max(vals)
        nz = sum(1 for v in vals if abs(v) > 0.005)
        if nz:
            blocks[base]["act"][idx] = (nz, mx)

for b, info in blocks.items():
    if info["n"] == 0 and not info["act"]:
        continue
    line = f"{b:12s} count={info['n']:3d}  "
    if info["act"]:
        det = " ".join(f"[{i}]nz={n},mx={m:.2f}" for i, (n, m) in sorted(info["act"].items()))
        line += "ACTIVE: " + det
    else:
        line += "(all zero)"
    print(line)
