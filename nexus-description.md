[center][size=6][b]AudioUtil[/b][/size]
[size=4]Native, folder-path audio playback for Skyrim SE/AE[/size]

[i]A framework for mod authors — play custom voice lines and sound effects from ordinary audio files, no plugins or Creation Kit required.[/i][/center]

[line]

[size=4][b]Description[/b][/size]
[b]AudioUtil[/b] is a lightweight SKSE framework that lets mods play custom voice lines and sound effects straight from loose audio files, wired up through simple [b]TOML[/b] config files. There are [b]no sound-descriptor records, no dialogue records, and no ESP edits[/b] — you drop WAV files in a folder, map them in a text file, and call one function from Papyrus.

[b]Installed on its own, AudioUtil does nothing.[/b] It ships completely neutral and SFW — no voices, no sounds. It's a dependency: content mods use it to bring [i]their[/i] audio to life.

The whole API is a handful of Papyrus functions — a typical call is just:
[code]int handle = AudioUtil.PlayVoice(akActor, "BattleCry")[/code]
You define your voices, sound effects, and routing in a TOML overlay that drops alongside the plugin, and multiple content mods compose without conflicts. Full documentation, the config reference, and the resolution rules:
[center][url=https://crajjjj.github.io/AudioUtil/][b]» AudioUtil Documentation «[/b][/url][/center]

[size=4][b]Installation instructions[/b][/size]
Install with a mod manager (Mod Organizer 2 / Vortex) like any SKSE plugin, or extract into your [font=Courier New]Data[/font] folder. Then install the content mods that depend on it. AudioUtil alone adds no voices or sounds.

[size=4][b]Main features[/b][/size]
[list]
[*][b]Play any WAV by path[/b] — loose files or audio packed inside BSA archives, positioned in 3D on an actor or played flat.[/*]
[*][b]Per-NPC voice assignment[/b] — one call picks the right voice for an actor automatically, resolved by NPC pin, voice type, race, or sex, then shuffle-picks a clip so lines don't repeat.[/*]
[*][b]Automatic lip movement[/b] — mouths move in sync with the audio, read from the clip's own loudness. No [font=Courier New].lip[/font] baking, works on any loose voice file.[/*]
[*][b]Vanilla voice lines[/b] — play any game [font=Courier New].fuz[/font] voice file straight by path (loose or inside a BSA). It's decoded and cached once, and lip movement works on it too.[/*]
[*][b]On-screen captions[/b] — a voice clip can carry a small text sidecar, shown as a game subtitle on the speaker while it plays, with per-language text and an event for custom UI.[/*]
[*][b]Mixing control[/b] — per-sound volume and stop, volume groups, ducking, and exclusive channels so a new line cleanly replaces the last.[/*]
[*][b]Gag support[/b] — when a speaker wears a mouth-owning device, their voice automatically switches to a muffled set and lip movement hands off to the device.[/*]
[*][b]TomlUtil[/b] — a bundled, general-purpose TOML config reader that [i]any[/i] Papyrus mod can use to load its own settings, whether or not it uses the audio side.[/*]
[/list]

[size=4][b]Requirements[/b][/size]
[list]
[*]Skyrim Special Edition / AE (all runtimes supported)[/*]
[*][url=https://skse.silverlock.org/]SKSE64[/url][/*]
[*][url=https://www.nexusmods.com/skyrimspecialedition/mods/32444]Address Library for SKSE Plugins[/url][/*]
[/list]

[size=4][b]AI disclosure[/b][/size]
This mod's code and documentation were written with help from an AI coding assistant ([b]Claude[/b]), directed, reviewed, and tested by me. AudioUtil ships [b]no AI-generated assets[/b] — no voice audio, no sound effects, no images, no text content of any kind. It is a code framework: it ships a DLL, Papyrus scripts, and an empty config. Any audio you hear comes from the content mods that use it, not from here.

[size=4][b]Shout outs[/b][/size]
[list]
[*]Built on [url=https://github.com/CharmedBaryon/CommonLibSSE-NG]CommonLibSSE-NG[/url] by CharmedBaryon and contributors.[/*]
[*]Licensed under [b]GPLv3[/b] — source is available; see the documentation and repository.[/*]
[/list]
