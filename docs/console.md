# Console Commands

AudioUtil's API is Papyrus, but a few functions are useful to trigger by hand while
developing or troubleshooting — reloading config after editing a TOML, or playing a
file to check a path resolves. Skyrim's console has **no built-in way to call a
Papyrus global function** (the `cgf` command you may have seen is Fallout 4 only), so
AudioUtil ships small **[ConsoleUtil Extended][cue]** config files that expose those
functions as real console commands.

!!! info "ConsoleUtil Extended is an optional dependency"
    The commands on this page only work with **[ConsoleUtil Extended][cue]** (CUE)
    installed. It's a modders/debug convenience — **AudioUtil itself runs fine without
    it**; nothing in normal playback depends on it.

The config files ship inside the mod at `Data\SKSE\CustomConsole\` (`AudioUtil.yaml`,
`TomlUtil.yaml`, `AudioUtilTest.yaml`). CUE reads them at load; there is nothing to
configure.

## Syntax

```
<command|alias> <subcommand> [args...]
```

- Arguments are space-separated. Wrap an argument that contains **spaces** in
  double quotes: `autest play "Sound\fx\some folder\clip.wav"`.
- **Backslashes are literal** — no escaping needed. Forward slashes also work
  (`Sound/fx/foo.wav`), which is handy if your keyboard layout can't type `\` in the
  console.
- A command's return value is printed back into the console. The `autest play/voice/…`
  helpers also show a `Debug.Notification` (top-left) with the instance handle.

## Commands

### Config reload

| Command | Alias | Does |
|---|---|---|
| `AudioUtil ReloadConfig` | `au reload` | Re-parse `AudioUtil.toml` + all `config\*.toml` overlays and rescan every slot folder — live, no restart. Prints `true`, or `false` if a file failed to parse (the previous config stays active). |
| `TomlUtil Reload "<file>"` | `toml reload "<file>"` | Re-parse one Data-relative TOML file (any `TomlUtil` consumer). Prints `true`, or `false` (and keeps the cached contents) on parse failure. |
| `TomlUtil SetInt "<file>" <key> <value>` | `toml setint …` | Write an int to a dotted key, **preserving comments/formatting** (TomlUtil API v2+). Prints `true` when written; `false` = nothing touched. Same for `setfloat` / `setstring` / `setbool` (`true`/`false` or `1`/`0`). |

```
au reload
toml reload "SKSE\Plugins\MyMod\MyMod.toml"
toml setint "SKSE\Plugins\MyMod\MyMod.toml" voice.pcvolume 80
```

### Test harness (`autest`)

Content-agnostic probes — they take *what to play* as arguments, so they work on any
install. Each plays at the player and reports the resulting instance handle
(`handle=0` means nothing resolved).

| Command | Args | Does |
|---|---|---|
| `autest play <path>` | Data-relative wav path | Play any loose wav via `PlayFile`. |
| `autest voice <slot> <category>` | slot id, category | Play a category from an explicit slot (`PlayVoiceFromSlot`); also prints the category's file count. |
| `autest voicepc <category>` | category | Resolve the player's slot and play the category through it (`PlayVoice`); prints the resolved slot. |
| `autest sfx <name>` | sfx/category name | Play an SFX by name (`PlaySFX`). |

```
autest play Sound\fx\MyMod\Moans\01.wav
autest voice F1 Orgasm
autest voicepc BattleCry
autest sfx MediumClap
```

## Exposing your own commands

CUE maps a console subcommand to a `global` Papyrus function via a `.yaml` in
`Data\SKSE\CustomConsole\`. A mod built on AudioUtil can ship its own config to expose
its own diagnostics the same way — see AudioUtil's `AudioUtilTest.yaml` as a template
and the [ConsoleUtil Extended wiki][cue-wiki] for the full schema.

[cue]: https://www.nexusmods.com/skyrimspecialedition/mods/133569
[cue-wiki]: https://github.com/KrisV-777/ConsoleUtil-Extended/wiki
