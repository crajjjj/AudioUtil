# lipresearch — .lip format research scripts

Research artifacts behind `docs/lip-format-research.md` (the full record: verified format
spec, engine internals, calibration methodology, LipDriver plan). Python 3.12 + numpy.

Dependency: clone https://github.com/OpenFaceFX/OpenFaceFX next to these scripts (they
import `OpenFaceFX/tools/lip_codec_research.py`, the byte-exact token parser; note that
repo's slot MAP is wrong — see the doc).

- `correlate.py` — reference decoder: `decode_lip(path)` handles .lip/.fuz, variant A+B
  headers, weights-only extraction. This is what the C++ LipDriver ports.
- `correlate3.py` — align in-game `autest lipcap` captures against decoded lips.
- `make_probe.py` / `read_probe.py` — build staircase probe fuz + read the engine's answer
  (the slot→channel verification loop).
- `probe_v2.py` — which facegen keyframe blocks are active in a capture CSV.
- `validate.py` — decode sample lips, per-phoneme peak sanity report.
