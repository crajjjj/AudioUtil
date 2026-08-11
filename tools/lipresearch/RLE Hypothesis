# `.lip` RLE Hypothesis — Research Plan

*Drafted 2026-08-11. Suggested repo location: `docs/lip-rle-hypothesis.md`, companion
tool `tools/lipresearch/lip_rle_validator.py`. Status: **hypothesis — needs the corpus
run (Exp 1) and one in-game probe (Exp 2), both on the Windows/Skyrim machine.***

## 1. The hypothesis

The `.lip` payload is **pure run-length encoding of the dense 33-slot × frame grid**:

- every f32 token is one grid cell (a channel weight, or a rest sentinel ≈1e-15);
- the 3-byte marker `00 <tag> 00` (tag ≠ 0, tag % 4 == 0) is a run of `tag/4`
  resting cells;
- **there are no Hermite tangents in the stream at all.**

This contradicts two rules the current parser (`src/LipData.cpp`) inherited:

| rule | current behaviour | under RLE |
|---|---|---|
| **dup rule** | an exact 4-byte repeat = "value + equal tangent", consumes 2 grid slots, records 1 value | two real cells, both carry the value |
| **range filter** | values outside [0,1] = tangent data, skipped | should not exist; any found = desync or unknown token |

Rationale for suspicion: FaceFX authors Hermite curves at design time, but the CK lip
compiler *bakes* them — samples at 30 fps and discards the curves. Our own corpus
fingerprint (§2 of `lip-format-research.md`: active channels carry a value on **every**
frame of a run, decaying to ~0 before drop-out) is what baked-then-compressed data looks
like, not a sparse keyframe format.

## 2. Where the dup rule came from (lineage — verified 2026-08-11)

- `LipData::Parse` ports `tools/lipresearch/correlate.py::decode_lip`, which builds on
  OpenFaceFX's `tools/lip_codec_research.py` ("the byte-exact token parser").
- That codec was derived from **four sample files** (three TTS placeholders + one vanilla
  asset, OpenFaceFX issue #12). The dup="tangent" reading was **never engine-verified**.
- **The byte-exact round-trip proof does not discriminate dup vs RLE.** Both grammars
  re-serialize any stream byte-identically; they disagree only on cell *meaning*.
  Minimal demonstration (both parsers return `roundtrip=EXACT` on the same bytes,
  but decode different cells):

  ```
  fragment: f32(0.62) f32(0.62) [00 0x50 00] f32(0.30)
  DUP: cells=[(pos 0, 0.62), (pos 22, 0.30)]            # second float = tangent
  RLE: cells=[(pos 0, 0.62), (pos 1, 0.62), (pos 22, 0.30)]
  ```

  Note both advance pos by 2 across the pair — so the frames×33 landing invariant
  **also** cannot separate dup from RLE. It catches marker misreads/desyncs only.
- OpenFaceFX's own writer (`src/openfacefx/export_lip.py`) **never emits a dup** and
  actively designs around producing adjacent equal floats (even-slot spacing documented
  as "dup-safe", sentinel anchors chosen for distinct bytes) — i.e. upstream treats dups
  as a parsing hazard, not a semantic feature.
- One datapoint *for* the dup reading: the vanilla sample decodes to 13 distinct curves
  == header `num_curves` under dup; pure RLE would make it 14 slots. One file, and
  `num_curves` is itself an inferred field — Exp 1 settles this statistically.

Playback impact if RLE is true: each dup false-positive silently rests one real cell
(mouth dips/flicker), and each *heuristic* misfire (dup or marker misread) shifts all
subsequent cells by ≥1 slot — modifier curves (blinks/brows, slots 16–31) then play
through phoneme slots 0–15 = "mouth moves too much".

## 3. Experiment 1 — corpus validation (NO game needed, just the lip corpus)

Tool: `tools/lipresearch/lip_rle_validator.py` (stdlib-only Python 3). It parses every
`.lip`/`.fuz` under a directory with BOTH grammars (`RLE` = pure run-length, `DUP` =
current parser model) and reports structural signals per grammar.

```
python tools/lipresearch/lip_rle_validator.py <corpus_dir> --csv results.csv
```

Point it at the same extraction used for the 21k-lip statistics behind
`lip-format-research.md` §2.

### Reading the report

| column | meaning | discriminates |
|---|---|---|
| `exact-land` / `<1 frame` | final pos vs frames×33 (undershoot = trailing rest run omitted by encoder — benign) | marker/desync issues only — NOT dup-vs-RLE |
| `overshoot` | parsed past frames×33 = definite misparse | missing token type in that grammar |
| `no OOR` | files with zero non-sentinel out-of-range cells | **RLE claim: ~100%.** If real OOR floats survive at *aligned* positions, tangents (or an unknown token) genuinely exist in-stream |
| `slot32` | non-zero cells on the unused slot 32 | slot-drift indicator (heuristic misfires) |
| `curves=` | files where distinct active slots == header `num_curves` | **the dup-vs-RLE discriminator.** Whichever grammar makes `num_curves` exact at ~100% is reading cells correctly (and `num_curves` stops being "advisory") |

### Decision matrix

- `RLE: no-OOR ≈100%, overshoot 0, curves= ≫ DUP's` → **RLE confirmed**, go to §5.
- `DUP: curves= ≈100%, RLE's lower by exactly the dup-pair files` → dup is real; keep
  the rule, but Exp 2 is still worth running for engine proof.
- `RLE overshoot clusters` → a token is still missing. Inspect the worst-offenders list
  (printed) with a hex editor at the desync point; also try flipping the marker-order
  knob in `parse_rle` (marker currently tried *before* the float read; the current
  parser treats it as a float *suffix* — if weights never produce `00 XX 00` byte
  prefixes the two orderings agree).
- Also check: do previously-unparseable **variant-C** headers now parse under RLE?
  (Counted separately in `header failures`.)

## 4. Experiment 2 — in-game dup probe (game needed, ~10 min)

Reuses the §5 "engine as decoding oracle" loop from `lip-format-research.md` verbatim
(staircase probe infra: `make_probe.py` / `read_probe.py` / `autest lipcap`, loose fuz
override of Carlotta `favor013questgive_000ca1f7`).

**Probe file** (extend `make_probe.py` with a `--dup` mode or hand-build):

- ~2 s, variant-A header, preroll 0, all values byte-distinct EXCEPT the probe pair
  (so the grammars diverge at exactly one point).
- A pulse window on slot 4 (DST), and at the apex frame emit the value **twice,
  byte-identical, adjacent, no marker between**: `f32(0.90) f32(0.90)`, then a marker
  skipping to the next frame's slot 4 (skip 31 — the pair consumed positions
  `f*33+4` and `f*33+5` under either grammar, so the rest of the file stays aligned).

**Procedure:** deploy as the loose fuz override, `autest lipcap start`, trigger the
line, `autest lipcap stop`, run `read_probe.py` on the CSV. Watch the dialogue phoneme
track (`unk120` / phoneme1) at the apex frame:

| observation | verdict |
|---|---|
| DST (ch 4) fires, Eee (ch 5) stays 0 | dup = tangent is REAL — keep the rule, document it as engine-verified |
| DST **and** Eee both fire at ~0.90 | **RLE confirmed by the engine** — the dup rule has been eating one real cell |

**Control** in the same file (different window): two *different* values adjacent
(`0.90`, `0.45` on slots 4, 5) — confirms adjacent cells render independently, guarding
against the engine merging neighbours.

## 5. If RLE is confirmed — parser change spec

`src/LipData.cpp::Parse`:

1. Delete the dup branch (the `memcmp(d+i, d+i-4, 4)` block; `floats` is then always 1).
2. Delete the range filter — every parsed float is a cell. Keep: sentinel/tiny (<1e-6)
   → explicit 0 key; clamp to [0,1] defensively; keep the marker rule and variant-B
   normalization unchanged.
3. Add a debug-log landing check: warn when `finalPos > frames*33` (misparse) or
   undershoot ≥ 1 frame — cheap permanent regression signal on every real file played.
4. Re-examine variant C with RLE before keeping the "unknown variant" bail-out.

Regression: drop a handful of before/after problem files ("mouth moves too much"
reports) into `tools/lipsim/lipsim.html` — its parser port must be updated in the same
commit (it mirrors `LipData::Parse`). Then update `docs/lip-format-research.md` §2
(remove tangent language, document RLE + landing invariant, re-run the 21k stats).

## 6. Upstream

Report the outcome on OpenFaceFX issue #12 either way — they explicitly ask for
engine-verified findings. If RLE holds: their token parser needs the same two deletions,
their `SKYRIM_SLOT_MAP` is already known-wrong (identity map, `lip-format-research.md`
§2), and their writer is unaffected (it never emitted dups). Include the round-trip
non-discrimination argument (§2 above) so the byte-exact oracle isn't re-cited as
counter-evidence.

## 7. File manifest

- `docs/lip-rle-hypothesis.md` — this file.
- `tools/lipresearch/lip_rle_validator.py` — Exp 1 tool (committed alongside).
- Exp 1 output: `results.csv` (do not commit; corpus-derived).
- Exp 2 additions: `make_probe.py --dup` mode + capture CSVs (CSVs stay local).
