# AudioUtil LipSim

A standalone lipsync simulator for Skyrim voice files. Drop a `.wav`, `.lip`,
or `.fuz` into the page and it plays the audio while animating a face and a
33-channel phoneme/modifier timeline, exactly the way the AudioUtil SKSE
plugin drives an actor's mouth in-game.

Part of [AudioUtil](https://crajjjj.github.io/AudioUtil/) (GPLv3).

## Quick start

- **Double-click `LipSim.exe`** — fully self-contained and fully offline, no
  Python, no internet, nothing else required. It starts a local server and
  opens the page, with sound for `.fuz` files (decoded via the CK's xwmaencode
  when installed, else the ffmpeg.wasm bundle baked into the exe). Any
  `.head.json` / preset `.json` placed next to the exe auto-loads on start.
- With Python 3.11+ you can instead run `LipSim.bat` / `lipsim_server.py`
  (same experience) — that's also what the `fuz2wav.py` / `tri2head.py`
  command-line tools need.
- Bare minimum: open `lipsim.html` directly in a browser. Everything works
  except `.fuz` **sound** (their lip curves still visualize).

## What you can do

- **Preview authored lipsync**: drop a wav + same-stem `.lip`, or a `.fuz`
  with an embedded lip. The mouth plays the authored FaceFX curves
  (identity slot map: 0-15 phonemes, 16-29 blink/brow/gaze/squint modifiers,
  30-32 the three head channels, see [What the modifier channels move](#what-the-modifier-channels-move)).
- **Preview AudioUtil's pseudo-phoneme synthesis**: drop a bare wav — the
  envelope is segmented into syllables with vowel variety and lip closures,
  mirroring the DLL's `SynthesizePseudoLip` line for line. At seed
  variant 0 the preview reproduces the in-game pick sequence (both sides
  seed from the lowercased file name). The tuning
  sliders re-synthesize live; **copy constants** exports your values for the
  `[lipsync]` toml / C++ constants.
- **Compare modes** (auto / authored lip / pseudo / plain envelope), scrub
  the timeline, loop, watch per-channel bars.
- **Authored .lip decoding**: lips are decoded with the engine-verified format
  (zero-RLE compression over a dense `float32[frames*33]` grid — the same
  `LipData::Parse` the DLL ships; see `tools/lipresearch/RLE Hypothesis.md` §11),
  so the preview mouth matches in-game (same decoder, same rejection rules). Handles 24- and 20-byte
  (`const14=7`) headers plus variant B.
- **Parse / edit / create MFG expression presets**: drop a PapyrusUtil
  StorageUtil preset json (e.g. SLO VE's `FemaleExpressions.json` — a
  `"string"` table of 33-value presets: 16 phonemes, 14 modifiers,
  expression id, strength, speed). Pick a preset (filterable) and the face
  wears it; playing a line shows lipsync over the expression exactly like
  in-game. Every value is editable (sliders + per-channel N/A), **new**
  clones the current preset under a new name, and **download json** saves
  the whole modified file (replace the original with it). With no json
  loaded, **new** starts a file instead: name it `Expression_<name>.json`
  for a ConditionalExpressionsExtended profile, anything else makes a
  StorageUtil preset table. In Chrome/Edge it asks where to put the file
  and **save** then writes it in place.

## 3D head face (the real thing)

The most faithful mode: renders an actual Skyrim head mesh with the game's
own morph data. One-time conversion from your install:

    python tri2head.py "<path>\femalehead.tri" --dds "<path>\femalehead.dds" -o female.head.json

The `.tri` is `meshes\actors\character\character assets\femalehead.tri`
(vanilla from the BSA, or your facial-animation mod's loose replacement —
e.g. Expressive Facial Animation); the `.dds` is your skin mod's
`textures\actors\character\female\femalehead.dds`. Drop the resulting
`.head.json` into lipsim: the head renders in WebGL and every phoneme,
modifier, and expression plays its **actual game morph**, not an
approximation. Drag the face to rotate; "flip tex" if the skin looks wrong.
(BC7 textures need `pip install texture2ddecoder`.)

The eyes and the mouth are separate head parts with their own `.tri`. Add
them to the same bundle and the head gets eyeballs that follow the gaze
channels, plus teeth and a tongue behind the lips:

    python tri2head.py "<path>\femalehead.tri" --dds "<path>\femalehead.dds" --eyes "<path>\eyesfemale.tri" --eyes-dds "<path>\eyeblue.dds" --mouth "<path>\mouthhumanf.tri" --mouth-dds "<path>\mouthhuman.dds" -o female.head.json

`eyesfemale.tri` / `eyesmale.tri` sit beside the head `.tri`, and
`mouthhumanf.tri` / `mouthhuman.tri` in the `mouth\` subfolder next to it.
The textures are
`textures\actors\character\eyes\eye*.dds` and
`textures\actors\character\mouth\mouthhuman.dds`. Both flags are optional and
independent. Without `--eyes` the eye sockets stay dark and **Look\*** shows
on the eyelids only. Remember to tick **drive modifiers on face**, or the
lip's blink/brow/gaze channels are not applied to the preview at all.

The view is a true front view: the character's left is on screen right, as
it is when you face them in game. `BlinkLeft`, `LookLeft` and the other Left
channels therefore act on the right half of the screen. (Up to 0.9.27 every
face style drew a mirror image, which only one-sided channels reveal.)

### What the modifier channels move

- **LookUp / LookDown / LookLeft / LookRight** turn the **eyeballs only**.
  They are morphs of the eyes mesh. The head mesh adds a small eyelid follow
  for up and down, and has nothing at all for left and right. The head itself
  never moves, so an eye roll or a downcast look is yours to author; a bowed
  head is not.
- **HeadPitch / HeadRoll / HeadYaw** (slots 30-32) are in the file format and
  lip generators fill them, but the game does not play them back. SkyrimSE 1.6.1170
  has exactly one routine that reads those three slots (as radians, clamped to
  +-90 degrees, into a quaternion) and nothing in the executable calls it, and
  no head part has a morph of that name. This is from disassembly, not from an
  in-game test. LipSim lists the rows in the timeline and leaves the head still.
- **Blink / Brow / Squint** are head-mesh morphs (the eyes mesh follows the
  blink and squint with its lash line).
- Holding a **Blink\*** or **Look\*** channel non-zero stops the actor's own
  blinking for as long as it is held.

## Photo face (Skyrim-realistic preview)

Drop a **screenshot of your character's face** (front-facing, `.png`/`.jpg`).
The first time, you click ~21 landmarks (brows, eye corners/lids, nose,
lips, jaw) as prompted — the rig is remembered per image. After that, the
photo itself is animated: the jaw drops, lips part over a painted mouth
cavity (teeth/tongue), brows and lids move — driven by exactly the same
channels as the vector face, in both tabs. "re-rig photo" redoes the
landmarks; "vector face" switches back. No game assets ship with the tool —
you bring your own screenshot.

## How fuz audio gets decoded

Fuz audio is xWMA, which no browser decodes natively. The page tries, in
order:

1. the local server's `/decode` endpoint — runs the Creation Kit's own
   `xwmaencode.exe` (instant, offline; needs the CK audio tools installed,
   game root via `SKYRIM_GAME_PATH` or `--game-path`),
2. ffmpeg.wasm — the copy vendored under `vendor/` (served locally by
   `lipsim_server.py`, and baked into `LipSim.exe`) so it works fully offline;
   only if that copy is missing does the page fall back to the jsdelivr CDN
   (~10 MB once, needs internet). ffmpeg.wasm needs the page served over http,
   so this path is unavailable when `lipsim.html` is opened as a bare `file://`.
3. failing both, the page tells you to run `fuz2wav.py`.

### Re-fetching the vendored ffmpeg.wasm

`vendor/` (~32 MB) is not committed — it's rebuilt into the release zip/exe
from the working tree. To repopulate it from the CDN:

    cd tools/lipsim && mkdir -p vendor && cd vendor
    base=https://cdn.jsdelivr.net/npm
    curl -fLO $base/@ffmpeg/ffmpeg@0.12.10/dist/umd/ffmpeg.js
    curl -fLO $base/@ffmpeg/ffmpeg@0.12.10/dist/umd/814.ffmpeg.js
    curl -fLO $base/@ffmpeg/core@0.12.6/dist/umd/ffmpeg-core.js
    curl -fLO $base/@ffmpeg/core@0.12.6/dist/umd/ffmpeg-core.wasm

The `xmake build lipsim` packaging step silently omits the bundle if `vendor/`
is absent (the page then uses the CDN as before).

## fuz2wav.py

Command-line fallback / batch tool: cracks `.fuz` files into `<stem>.wav`
(+ `<stem>.lip` when embedded) beside the source, using `xwmaencode.exe`.

    python fuz2wav.py somefile.fuz
    python fuz2wav.py somefolder --out decoded

## Files

| file | role |
|---|---|
| `lipsim.html` | the simulator — a single self-contained page |
| `lipsim_server.py` | localhost server + native xWMA decode endpoint |
| `LipSim.bat` | double-click launcher for the server |
| `fuz2wav.py` | CLI fuz → wav+lip extractor |
| `tri2head.py` | CLI head `.tri` (+ eyes, mouth, textures) → `.head.json` for the 3D head |
