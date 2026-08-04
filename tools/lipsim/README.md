# AudioUtil LipSim

A standalone lipsync simulator for Skyrim voice files. Drop a `.wav`, `.lip`,
or `.fuz` into the page and it plays the audio while animating a face and a
32-channel phoneme/modifier timeline — exactly the way the AudioUtil SKSE
plugin drives an actor's mouth in-game.

Part of [AudioUtil](https://crajjjj.github.io/AudioUtil/) (GPLv3).

## Quick start

- **Double-click `LipSim.exe`** — fully self-contained, no Python or anything
  else required. It starts a local server and opens the page, with sound for
  `.fuz` files (decoded via the CK's xwmaencode when installed, else
  ffmpeg.wasm from a CDN). Any `.head.json` / preset `.json` placed next to
  the exe auto-loads on start.
- With Python 3.11+ you can instead run `LipSim.bat` / `lipsim_server.py`
  (same experience) — that's also what the `fuz2wav.py` / `tri2head.py`
  command-line tools need.
- Bare minimum: open `lipsim.html` directly in a browser. Everything works
  except `.fuz` **sound** (their lip curves still visualize).

## What you can do

- **Preview authored lipsync**: drop a wav + same-stem `.lip`, or a `.fuz`
  with an embedded lip — the mouth plays the authored FaceFX curves
  (identity slot map: 0–15 phonemes, 16–31 blink/brow/gaze modifiers).
- **Preview AudioUtil's pseudo-phoneme synthesis**: drop a bare wav — the
  envelope is segmented into syllables with vowel variety and lip closures,
  mirroring the DLL's `SynthesizePseudoLip` line for line. The tuning
  sliders re-synthesize live; **copy constants** exports your values for the
  `[lipsync]` toml / C++ constants.
- **Compare modes** (auto / authored lip / pseudo / plain envelope), scrub
  the timeline, loop, watch per-channel bars.
- **Parse / edit / create MFG expression presets**: drop a PapyrusUtil
  StorageUtil preset json (e.g. SLO VE's `FemaleExpressions.json` — a
  `"string"` table of 33-value presets: 16 phonemes, 14 modifiers,
  expression id, strength, speed). Pick a preset (filterable) and the face
  wears it; playing a line shows lipsync over the expression exactly like
  in-game. Every value is editable (sliders + per-channel N/A), **new**
  clones the current preset under a new name, and **download json** saves
  the whole modified file — replace the original with it.

## 3D head face (the real thing)

The most faithful mode: renders an actual Skyrim head mesh with the game's
own morph data. One-time conversion from your install:

    python tri2head.py "<path>\femalehead.tri" --dds "<path>\femalehead.dds" -o female.head.json

The `.tri` is `meshes\actors\character\character assets\femalehead.tri`
(vanilla from the BSA, or your facial-animation mod's loose replacement —
e.g. Expressive Facial Animation); the `.dds` is your skin mod's
`textures\actors\character\female\femalehead.dds`. Drop the resulting
`.head.json` into lipsim: the head renders in WebGL and every phoneme,
modifier, and expression plays its **actual game morph** — not an
approximation. Drag the face to rotate; "flip tex" if the skin looks wrong.
(BC7 textures need `pip install texture2ddecoder`. Eyes/teeth are separate
meshes and not included in v1 — the mouth opens onto a dark cavity.)

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
2. ffmpeg.wasm fetched from a CDN (~10 MB once, then cached; works even
   without the CK tools, needs internet),
3. failing both, the page tells you to run `fuz2wav.py`.

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
