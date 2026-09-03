# Tag-Scored Pools

A category folder plays one shuffle-picked file — but not every line in a pool
fits every moment. **Tags** let a voice pack refine *which* file plays without
splitting content across dozens of narrowly-named categories: files carry
**tags** (what a line commits to), a play call carries **facts** (what is true
right now), and the best-matching pool wins.

```papyrus
; plain call — plays only untagged files, exactly like PlayVoice
AudioUtil.PlayVoice(npc, "BattleCry")

; tagged call — same category, plus the facts of the moment
AudioUtil.PlayVoiceTagged(npc, "BattleCry", "angry intense undead")
```

The design has one safety rule at its core: **tags are constraints, facts are
freedoms.** A file tagged `afraid` can *never* play in a scene whose facts
don't include `afraid` — but an untagged file plays anywhere. Pack authors tag
only what they actually recorded; anything they omit safely falls back to the
untagged pool or the normal [category fallbacks](config/resolution.md).

!!! info "Off by default"
    The plugin ships **no tag vocabulary** (the neutral-default principle —
    same as the empty slot and SFX tables). Tags activate only when some
    config file defines a `[tags]` section; with none anywhere, the whole
    layer is dormant: category folders scan flat, subfolders and bracketed
    filenames mean nothing, `PlayVoiceTagged` behaves exactly like
    `PlayVoice`.

## 1. Defining the vocabulary — `[tags]`

The consumer mod that *calls* `PlayVoiceTagged` owns its vocabulary, shipped
in its base `AudioUtil.toml` or a `config\*.toml` overlay. The vocabulary is a
set of named **axes**; each axis is a list of mutually-exclusive **tokens**
plus a match **weight**:

```toml
[tags]
mood      = { tokens = ["calm", "angry", "afraid"],  weight = 8 }
intensity = { tokens = ["soft", "intense"],          weight = 4 }
target    = { tokens = ["human", "beast", "undead"], weight = 1 }
```

- **One fact per axis.** A moment is either `angry` or `afraid`, never both —
  and a file tagged with two tokens of one axis can never qualify (it is
  excluded at scan time, with a warning). A *request* that names two tokens of
  one axis keeps the first and drops the rest, warning once.
- **Weights rank axes**, not files: with the table above, matching the mood is
  worth more than matching the target — a file that nails the emotional tone
  beats one that merely names the right enemy type. Choose weights so the axes
  a listener *notices* dominate (a good rule: each tier's weight exceeds the
  sum of everything below it).
- Up to **64 tokens** total across all axes. Tokens are matched case- and
  punctuation-insensitively, like every other AudioUtil name.
- **Additive across config files.** `[tags]` merges from the base
  `AudioUtil.toml` plus every `config\*.toml` overlay (sorted filename
  order), like slots and sfx — so **several consumer mods can each ship their
  own axes** and coexist on one install. A new axis name adds; a same-named
  axis **unions its tokens** — no file can *remove* another mod's tokens, so
  nothing already tagged can be broken by installing another mod — and its
  weight is last-writer-wins (logged when it changes; numeric filename
  prefixes order deliberately).

!!! tip "Extending the vocabulary"
    Growing a vocabulary is always safe — `au reload` applies it live, and
    **adding** tokens or axes never breaks existing packs (old tags keep
    parsing; a new token is inert until packs tag with it *and* calls send it
    as a fact). That inertness is also why cross-mod interference stays nil:
    mod B's tokens on mod A's files only ever match facts mod B sends.
    **Removing or renaming** a token in your own file is the breaking
    direction — files tagged with it turn into `unknown tag token`
    exclusions. Reweighting re-ranks every pool on that axis. Pick reasonably
    distinctive token names (or prefix them, `myMod_close`) if you worry
    about colliding with another mod's generic vocabulary — a duplicate token
    across two *different* axes goes to whichever loaded first, with a
    warning.

## 2. Tagging content — the three carriers

Tags attach to files inside a normal category folder. A file's **effective tag
set is the union of all three carriers**; use whichever is convenient, mixing
freely.

### Tag subfolders (broad strokes)

One level of subfolders whose names are tag sets — token order irrelevant:

```
Sound\MyMod\Voice\PackA\
  BattleCry\
      cry_01.wav              ← untagged pool (plays for any facts)
      cry_02.wav
      angry\
          cry_10.wav          ← angry
          cry_11.wav
      afraid intense\
          cry_20.wav          ← afraid + intense
```

Only one level: a folder inside `angry\` is ignored (warned). This is the
carrier for whole batches recorded in one tone.

### Filename brackets (per-line nuance)

A trailing `[...]` group in the stem: `cry_12 [angry undead].wav` — tokens
space- or comma-separated. Inside a tag subfolder the sets union:
`angry\cry_13 [intense].wav` is effectively `angry intense`.

The bracket group is part of the filename, so same-stem sidecars — a caption
`.toml` or a `.lip` file — must carry the **identical full stem**:
`cry_12 [angry undead].toml`.

A bracket group holding **no** vocabulary token at all is not read as a tag
set — it's just a filename. Packs bracket names for their own reasons
(`moan_04 [loud].wav`), and any mod's `[tags]` block turns the carrier on for
every pack on the install, so an unrelated convention must not mute content.
A group that names *some* known tokens **is** a tag set, so a typo inside one
(`[angry undad]`) still excludes the file with a warning. Category subfolders
follow the same rule: a subfolder whose name holds no vocabulary is ignored
silently, exactly as it was before tags existed.

### `_tags.toml` manifest (no renames)

An optional file per folder mapping filenames to tag strings — the carrier for
tagging an existing pack without renaming thousands of files and their
sidecars:

```toml
# Sound\MyMod\Voice\PackA\BattleCry\_tags.toml
"cry_05.wav" = "angry"
"cry_06.wav" = "angry intense"
```

`_tags.toml` is never scanned as audio. It works inside tag subfolders too
(again a union).

## 3. How a file is chosen

For `PlayVoiceTagged(actor, category, facts)`:

1. **The category resolves first**, exactly as always — aliases,
   `male_only_remap`, category fallbacks, then the slot's `fallback` chain
   (see [Voice & Category Resolution](config/resolution.md)). Tags work
   *inside* whichever category wins. A category whose only pools are tagged
   and non-matching counts as **missing** for this call, so it falls through
   the fallback chain like any absent category.
2. Within the folder, every distinct effective tag set is a **pool** (untagged
   files form the tagless pool). A pool **qualifies** iff *all* its tags
   appear among the facts.
3. The qualifying pool with the **highest weight sum** wins; ties go to the
   pool with more tokens, then deterministically by token order. The untagged
   pool scores 0 — the always-valid floor.
4. The winning pool's own **shuffle bag** picks the file (no repeats until
   that pool's deck empties).

Worked example, against the folder above:

| Call | Winner | Why |
|---|---|---|
| `...Tagged(npc, "BattleCry", "angry intense undead")` | `angry\` (8) | `afraid intense` disqualifies (`afraid` not a fact); `angry` beats the floor |
| `...Tagged(npc, "BattleCry", "afraid intense")` | `afraid intense\` (12) | full match outranks everything |
| `...Tagged(npc, "BattleCry", "calm")` | untagged pool (0) | every tagged pool disqualifies |
| `PlayVoice(npc, "BattleCry")` | untagged pool (0) | no facts = legacy behavior |

Two practical corollaries for callers:

- **Send every fact you know, omit what you don't.** A missing fact never
  causes a wrong line — it only keeps the most specific pools out of the
  running for that call.
- An **unknown fact token** (typo, or a vocabulary mismatch between mods) is
  ignored and warned once per distinct string in `AudioUtil.log` — the call
  still plays.

And one for pack authors: **keep pools chunky.** Ten files sharing one tag set
cycle nicely; ten files with ten unique sets are ten one-file pools and
audible repeats. Tag at the folder level first, reach for brackets sparingly.

## 4. The natives

```papyrus
int Function PlayVoiceTagged(Actor akActor, string category, string tags, \
    float volume = 1.0, string group = "", string channel = "", \
    bool blockLipSync = false, bool blockCaption = false) global native

int Function PlayVoiceFromSlotTagged(string slot, string category, string tags, \
    Actor akFollow = None, float volume = 1.0, string group = "", string channel = "", \
    bool blockLipSync = false, bool blockCaption = false) global native
```

API v6+. Both are their untagged counterpart plus the `tags` fact string and
a `blockCaption` opt-out (suppresses the line's caption sidecar — HUD subtitle
and `AudioUtil_Caption` event — for callers rendering their own text) — every
other argument, the gag routing, lipsync behavior, groups, channels, and the
returned handle are identical. Empty `tags` (or no `[tags]`
vocabulary) makes them byte-for-byte equivalent to `PlayVoice` /
`PlayVoiceFromSlot`, so they are always safe to call.

Introspection (`CategoryExists`, `GetCategoryFileCount`, `GetResolvingSlot`)
stays **tag-blind**: it answers "does this category hold any content at all",
counting every pool.

## 5. Exclusion rules and log lines

Nothing tags can do produces a hard error or plays a file in the wrong
context — every failure mode is *this file/folder doesn't play, one line in
`AudioUtil.log`*:

| Warning | Cause | Effect |
|---|---|---|
| `unknown tag token '…'` (scan) | a carrier uses a token outside the vocabulary | that file/folder never plays |
| `contradictory axis tokens` | one effective set holds two tokens of one axis (e.g. `soft` folder ∪ `[intense]` file) | that file never plays |
| `subfolder '…' is not a valid tag set` | a category subfolder that names *some* vocabulary but doesn't parse (a name with none is ignored silently) | folder ignored |
| `nested folder(s) under tag folder … ignored` | a second nesting level | inner folder invisible |
| `unknown fact token(s) '…' ignored` (play) | a `PlayVoiceTagged` fact outside the vocabulary | fact dropped, call proceeds |
| `fact token(s) '…' share an axis` (play) | a request names two tokens of one axis | later token dropped, call proceeds |
| `SFX '…': every pool is tagged beyond this request's facts` | an sfx category exists but holds only pools the call can't cover | nothing plays for that call |

The **registration roster** logged at load (and on `au reload`) shows what the
scan understood: each category lists its total file count, and categories with
tagged pools get a `tags` detail line — `[angry](2) [afraid intense](1)` —
so an author can verify their tagging in one glance.

## 6. Testing from the console

With [ConsoleUtil Extended](console.md) installed:

```
autest voicetag PackA BattleCry "angry intense"   ; explicit slot, quoted facts
autest voicetagpc BattleCry "afraid"              ; through the player's slot
au reload                                          ; re-scan after retagging
```

`GetHandlePath` (on the returned handle) tells you the exact file the pick
chose, which pool a given fact set lands in included.
