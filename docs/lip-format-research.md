# Skyrim `.lip` Format — Technical Summary

*Researched 2026-08-03/04.*

> **PARTIALLY SUPERSEDED 2026-08-31.** The **slot map** (§3: slots 0–15 = MFG
> phonemes, 16–31 = modifiers, identity order), the **header/preroll** analysis
> and the §5 probe methodology remain engine-verified and correct. The **payload
> token grammar** described in §2/§4 (floats + dup="tangent" + `00 4k 00`
> markers, range filter) is **refuted**: the payload is actually a plain
> zero-RLE compression (`00` + u16-LE count = that many zero bytes) over a dense
> `float32[frames*33]` grid — proven by decompiling the engine's own loader.
> See `tools/lipresearch/RLE Hypothesis.md` §11 for the proof and
> `src/LipData.cpp` for the shipping decoder. Read the token-grammar sections
> here as history, not as the spec.

*Config: `[lipsync] use_lip_files` / `drive_modifiers` / `pseudo_phonemes`;
runtime toggles: `autest lipfiles on|off`, `autest pseudolip on|off`.*

## 1. Overview

A `.lip` file is Skyrim's baked lipsync animation: per-channel weight curves for the
16 MFG phoneme and 16 MFG modifier channels on a uniform 30 fps grid. AudioUtil decodes
it — from a standalone `.lip` beside a wav or from the block embedded in a `.fuz` — and
drives the actor's face with the authored curves, replacing the amplitude-envelope
fallback (Aah/BigAah only) whenever a lip is available.

Key correction to public knowledge: OpenFaceFX's `SKYRIM_SLOT_MAP` (even slots, BMP=0,
Aah=22 …) is **wrong**. The true layout is identity (§2). No published decoder had it
right; the in-game probe methodology (§5) is what settled it.

## 2. File format (verified)

A `.fuz` = `FUZE` magic, u32 version, u32 lipSize, LIP block, RIFF audio (see `FuzCache`).
A standalone `.lip` is just the LIP block.

### Header — 24 bytes, little-endian `struct "<IIIHHiHH"`

| off | type | field | value / meaning |
|---|---|---|---|
| 0 | u32 | version | 1 |
| 4 | u32 | duration | `132 * frameCount + 28` (FaceFX ticks; 132/frame) |
| 8 | u32 | num_curves | active-curve count (13 in vanilla; advisory) |
| 12 | u16 | frameCount | frames on a uniform **30 fps** grid |
| 14 | u16 | const14 | 3 = variant A (normal); see variant B below |
| 16 | i32 | preroll | negative first-frame index (−9..0 vanilla; −22 seen from LipGenerator). The first \|preroll\| grid frames are **pre-audio lead-in**: grid frame \|preroll\| lands on audio t=0, so a player must skip the lead-in or the whole lip runs \|preroll\|/30 s late. Validated by duration fit: frames−\|preroll\| ≈ audio length on authored lips (raw frames overshoot). MfgFix's engine mirror agrees — its dialogue timer runs to `(frames+\|preroll\|)·0.033` |
| 20 | u16 | vocab | **16** = Skyrim (43 = Fallout 4) |
| 22 | u16 | u22 | varies; semantics unknown; irrelevant for playback |

**Variant B** (~10% of vanilla files): one **extra byte at offset 14** and `const14 = 2`.
Normalize by deleting byte 14, then parse as variant A. Its rest-sentinel differs
(`0x282E7BD4` vs A's `0x268AE8F9`); both are ≈1e-15 floats, so a "keep [0,1], treat tiny
as 0" rule handles both without caring.
A third, rarer variant exists (u20 garbage even after the shift — e.g. 0x2059); it is
unhandled — such files fail parsing and playback falls back to the envelope.

### Payload — frame-major positional token grid

Stride **R = 33 slots per frame**; flattened position `pos = frame*33 + slot`.
Token stream to EOF, each token:

```
<f32 value>                      4 B   — cell at current pos; advances pos by 1
[<f32 value>]                    4 B   — optional exact duplicate (value + equal
                                          Hermite tangent); advances pos by 1 more
[00 <u8 tag> 00]                 3 B   — optional marker, tag = 4 * skip;
                                          advances pos by `skip` (resting slots)
```

Marker is a suffix — always read the float first (a float whose bytes match `00 XX 00`
would otherwise desync). Values in **[0,1] are channel weights** (keep 0.0 — closures are
distinctive); values outside [0,1] are Hermite tangent data — ignore for playback. The
engine plays weights **verbatim** (probe: 0.90 in → 0.90 out, no scaling, ~1-frame latency).

**Cells are dense samples, not sparse keyframes.** The marker's `skip` counts *resting*
slots, and a slot the stream skips is at **rest (0)** — it does **not** hold the channel's
previous value, and there is nothing to interpolate across. An active channel carries a
value on **every** frame of its run and decays to ~0 on its own before dropping out of the
stream. Measured over 21 478 vanilla/mod lips: mean run 11.2 frames, 55% of runs end below
0.05, 77% below 0.15, and the last frame of a file is at 0 on essentially all of them.
Decoding the cells as keyframes to interpolate/hold instead leaves the mouth **frozen in
each channel's last shape for the rest of the line** — under that rule 70% of the corpus
ends with the jaw stuck open at 0.25–0.99. Where a run does stop abruptly (>0.3 → absent),
another phoneme takes over the mouth shape 65% of the time; AudioUtil fades the remaining
third out over 2 frames so the drop-out doesn't pop.

### Slot map — ENGINE-VERIFIED, identity

| grid slots | drives | order |
|---|---|---|
| 0–15 | MFG **phonemes** 0–15 | Aah, BigAah, BMP, ChjSh, DST, Eee, Eh, FV, i, k, N, Oh, OohQ, R, Th, W |
| 16–31 | MFG **modifiers** 0–15 | BlinkL, BlinkR, BrowDownL, BrowDownR, BrowInL, BrowInR, BrowUpL, BrowUpR, LookDown, LookLeft, LookRight, LookUp, SquintL, SquintR, +2 (head) |
| 32 | nothing | padding/anchor |

So a `.lip` animates the whole face: mouth + blinks + brows + gaze.

## 3. Engine internals discovered (runtime 1.6.1170, CommonLibSSE-NG)

`RE::BSFaceGenAnimationData` (see `include/RE/B/BSFaceGenAnimationData.h`) has 13 keyframe
blocks; the dialogue/voice system does **not** write the scripted `phenomeKeyFrame` (0x080,
which MFG console and AudioUtil's LipSync drive). Verified by capture, and independently
confirmed by MfgFix NG's reversed struct (`Mfg-Fix-NG/src/mfgfix/BSFaceGenAnimationData.h`),
whose layer naming is: `*1` = dialogue-driven, `*2` = scripted (console/Papyrus), `*3` =
final blended output:

| block (CommonLib) | MfgFix name | offset | role (verified) |
|---|---|---|---|
| `phenomeKeyFrame` | `phoneme2` | 0x080 | scripted phonemes (MFG / AudioUtil) — stays 0 during dialogue |
| `unk0E0` | `modifier1` | 0x0E0 | dialogue lip **modifier** track (pure lip data) |
| `unk100` | `modifier3` | 0x100 | final modifiers = lip + procedural blinks |
| `unk120` | `phoneme1` | 0x120 | dialogue lip **phoneme** track (count=16) |
| `unk140` | `phoneme3` | 0x140 | final phonemes = dialogue + scripted merged (≈unk120 when nothing scripted) |

Scripted `phenomeKeyFrame` and the dialogue tracks coexist — the engine's keyframe update
(hooked by MfgFix when installed) merges `phoneme1`+`phoneme2` into `phoneme3` — which is
why AudioUtil's envelope lipsync works during non-dialogue playback. AudioUtil's lip mode
therefore keeps writing `phenomeKeyFrame` — proven to render, and doesn't fight dialogue.
Note that with MfgFix installed and a **dialogue line active**, scripted phoneme values
below its `fDialoguePhonemeThreshold` INI setting are zeroed rather than merged (per-channel
override above threshold) — one more reason `[lipsync] block_in_dialogue` hands the mouth
over. MfgFix's `DialoguePhonemesUpdate` also confirms the 30 fps grid: it advances the
dialogue phoneme track with a `0.033f`/frame clock via the engine's lip-sampling call.

## 4. Toolchain on this machine

- **CK LipGenerator**: `C:\SteamLibrary\steamapps\common\Skyrim Special Edition\Tools\LipGen\LipGenerator\`
  — `LipGenerator.exe <wav> "<text>" -OutputFileName:<out.lip>` (run from its dir;
  16-bit PCM wav input, resamples to 16 kHz internally; FaceFX license valid; output is
  mildly non-deterministic run-to-run). `FonixData.cdf` beside it.
- **LIPFuzer**: `..\LipFuzer\LIPFuzer.exe` — fuz/unfuz (`-u` extracts lip+audio).
- Voicepack authors generate `.lip` offline with either tool (or FaceFXWrapper — same CK
  code, Compress flag hardcoded true; "compressed" IS this grid format, no zlib anywhere).
- **`tools/lipsim/lipsim.html`** — standalone browser simulator (no server, no deps): drop a
  `.wav`/`.lip`/`.fuz`, it plays the audio and animates a mouth + 32-channel timeline using
  the same decoding/synthesis as the DLL (LipData parser port, pseudo-phoneme synthesis with
  live tuning sliders, envelope mode). The place to calibrate `SynthesizePseudoLip` constants
  without launching the game ("copy constants" exports the tuned values). Fuz plays fully:
  the embedded lip visualizes and the xWMA audio is decoded in-page by a lazily
  CDN-loaded ffmpeg.wasm (one-time ~10 MB download; offline fallback:
  `tools/lipsim/fuz2wav.py` cracks a fuz into wav+lip via the CK's xwmaencode).
- **`tools/lipsim/tri2head.py`** — converts a head `.tri` (FaceGen FRTRI003: base mesh +
  UVs + the 45 named MFG morph deltas the engine animates; format verified byte-exact)
  plus a skin `.dds` (BC1/3/7) into a `.head.json` that lipsim renders in WebGL — real
  game morphs, no approximation. The NIF turned out unnecessary: the tri carries the
  full renderable geometry.
- **`tools/lipgen/make_lips.py`** wraps the whole pipeline for a voicepack folder: every
  wav gets a same-stem `.lip` via LipGenerator, taking the spoken text from the wav's
  AudioUtil caption sidecar (`--lang`, `--default-text` for uncaptioned pools); `--fuz`
  additionally encodes the wav to xWMA (`Tools\Audio\xwmaencode.exe`) and packs the FUZE
  container itself (magic + version + lipSize + LIP + RIFF).

## 5. Calibration methodology (reusable; how the map was verified)

Two instruments, both kept in-tree:

1. **`autest lipcap start|stop`** (console, via CUE): `src/LipCapture.cpp` samples the
   dialogue speaker's **all 13 facegen keyframe blocks** at ~30 Hz plus the playing
   `TESTopicInfo` FormID (`MenuTopicManager::currentTopicInfo` — fuz filenames embed that
   FormID, giving exact line identity), CSV to `Data\SKSE\Plugins\AudioUtil\lipcap_*.csv`
   (lands in MO2's `SKSE Output` mod). Registered on the `AudioUtilTest` script only —
   not public API.
2. **Staircase probe lips** (`tools/lipresearch/make_probe.py`): synthetic lips giving each
   grid slot an exclusive 0.9 pulse window, muxed with a vanilla line's audio and deployed
   as a **loose fuz override** of a repeatable dialogue line (used Carlotta
   `favor013questgive_000ca1f7`; loose beats BSA). Play line in-game while capturing →
   `read_probe.py` prints which engine channel fired per window = direct slot→channel map.

The engine accepted hand-built lips without issue and echoed values exactly. This
"engine as decoding oracle" loop can settle any future format question (e.g. the third
variant) in one capture.

`tools/lipresearch/` also keeps `correlate.py` (the Python reference decoder — variant A+B
normalization, weights-only extraction — `decode_lip()` is what `LipData::Parse` ports)
and `correlate3.py` (capture↔lip alignment analysis). They import OpenFaceFX's
`tools/lip_codec_research.py` (clone https://github.com/OpenFaceFX/OpenFaceFX — MIT;
its token parser is correct, only its slot MAP is wrong).

Interactive visualization from the research: https://claude.ai/code/artifact/59bb837f-2a07-4b5d-bc36-3bd3d3559b65
(decodes clips to a 16-channel MFG timeline + animated mouth; built pre-verification, so
its slot map is the OLD wrong assumption — the timelines/format part is still right).

## 6. Implementation in AudioUtil

**`src/LipData.{h,cpp}`** — parser, no game deps beyond the resource reader:
- `Parse(bytes)`: normalizes variant B, parses header + token grid, produces dense
  per-channel timelines (`values[32][frame]`, 30 fps) for slots 0–15 (phonemes) and
  16–31 (modifiers). Values kept in [0,1] at their slot; sentinel/tiny → 0;
  out-of-range → skipped as tangent data. Frames a channel doesn't appear on are
  **rest (0)** with a 2-frame release, per §2 — not held or interpolated.
  `Anim::Sample(channel, t)` linearly interpolates between frames (sub-frame smoothing
  only); `HasMouthData()` rejects lips with no phoneme signal.
- `GetFor(path)`: resolves the lip for a played path — a `.fuz`'s embedded LIP block
  (via `FuzCache`), else a same-stem `.lip` beside the file (loose or BSA) — with
  session caching (misses cached too; `ClearCache()` on config reload).

**`src/LipSync.cpp`** — per-entry mode, chosen at `Start`:
- When `GetFor` returns curves and lip mode is on, the entry holds a
  `shared_ptr<const LipData::Anim>`; `ApplyAll` writes all 16 phoneme channels from
  `lip->Sample(ch, t)` at `t = now − audibleAt`, scaled by `gain` and a master fade
  (the envelope attack/release smoothing repurposed as start/stop fade — during
  fade-out the last shape is held and scaled down). No lip → the amplitude envelope
  drives Aah/BigAah as before.
- Entry removal zeroes every channel that was driven (all 16 phonemes, and modifiers
  if driven), not just Aah/BigAah.
- Modifiers (blink/brow/gaze, slots 16–31) are written only with
  `[lipsync] drive_modifiers = true` (default false — SLO VE and expression mods own
  brows/eyes).
- All suppression logic (gag, tongue, dialogue handover, `blockLipSync`,
  `block_categories`) is mode-independent — it gates entry creation/retention.

**Pseudo-phoneme synthesis** (`[lipsync] pseudo_phonemes`, default false) covers lines
with NO lip data: `SynthesizePseudoLip` (LipSync.cpp) segments the envelope into
syllables (voiced runs above `min_level`, split at local minima under 60% of the running
peak), gives each syllable one vowel — weighted pick `Aah`4/`Oh`3/`OohQ`2/`Eh`1, seeded
by FNV-1a of the file path so a given wav always mouths the same way, never the same
vowel twice in a row — and places a 2-frame `BMP` lip closure before any syllable
following ≥5 frames of silence (closures render distinctly; the engine plays 0 frames
verbatim, §2). The attack/release shaping and `min_level` clamp the live envelope mode
applies are baked into the synthesized curves, because the output is a normal
`LipData::Anim` played through the verbatim lip-mode path.

Config (`[lipsync]`, base-only): `use_lip_files` (default true), `drive_modifiers`
(default false), `pseudo_phonemes` (default false), plus the pseudo tuning keys
`pseudo_voiced_floor` / `pseudo_valley_ratio` / `pseudo_min_syl_frames` /
`pseudo_gap_frames` / `pseudo_closure` (calibrate in `tools/lipsim`, paste, `au
reload` — no rebuild). Runtime A/B toggles
`autest lipfiles on|off|status` and `autest pseudolip on|off|status` (affect new lines;
backed by test-script-only natives `Set/GetLipFilesMode`, `Set/GetPseudoLipMode`). No
public Papyrus additions — lip detection is automatic per played path.

Verification: LipGenerator-made lips beside loose wavs play with visibly-correct phoneme
shapes via `autest play`; fuz-embedded lips (any vanilla dialogue line by path) drive the
same curves the engine's own dialogue playback shows in `lipcap` captures.
