#!/usr/bin/env python3
"""Batch-generate .lip files (and optionally .fuz) for a voicepack folder.

For every .wav under the given folder, generates a same-stem .lip with the
Creation Kit's LipGenerator.exe, taking the spoken text from the wav's
AudioUtil caption sidecar (same-stem .toml, per-language keys: en = "...").
AudioUtil then plays those lines with real FaceFX phoneme lipsync instead of
the amplitude envelope (see docs/lip-format-research.md).

Captions are NOT required: --default-text "ah mm ah" gives every uncaptioned
wav a generic lip (good enough for moan pools), and with --fuz, wavs that
have no text at all are still packed as audio-only fuz (lipSize 0), which
AudioUtil lipsyncs from the decoded audio (pseudo/envelope) at runtime.

With --fuz, each wav+lip pair is additionally packed into a same-stem .fuz:
the wav is encoded to xWMA with the CK's xwmaencode.exe and wrapped in the
FUZE container (magic + u32 version + u32 lipSize + LIP block + xWMA RIFF).
A .fuz is one file per line and much smaller than the wav, at the cost of a
first-play decode through AudioUtil's FuzCache. Captions still work (the
.toml sidecar keys off the .fuz path); keep the sidecar next to the fuz.

Requirements: Python 3.11+, Skyrim SE Creation Kit tools installed
(Tools\\LipGen\\LipGenerator\\LipGenerator.exe, and for --fuz
Tools\\Audio\\xwmaencode.exe). Override the game root with --game-path or
the SKYRIM_GAME_PATH env var.

Notes:
  - LipGenerator wants 16-bit PCM wav input (it resamples to 16 kHz
    internally); other formats are attempted with a warning.
  - LipGenerator output is mildly non-deterministic run-to-run; existing
    .lip files are kept unless --force.
"""

import argparse
import os
import struct
import subprocess
import sys
import tomllib
from pathlib import Path

DEFAULT_GAME_PATH = r"C:\SteamLibrary\steamapps\common\Skyrim Special Edition"


def find_tool(game_path: Path, rel: str, what: str) -> Path:
    exe = game_path / rel
    if not exe.is_file():
        sys.exit(f"error: {what} not found at {exe}\n"
                 f"       (install the CK tools, or point --game-path / SKYRIM_GAME_PATH at the game root)")
    return exe


def wav_format(path: Path) -> tuple[int, int] | None:
    """(format_tag, bits_per_sample) from the RIFF fmt chunk, or None."""
    try:
        with open(path, "rb") as f:
            if f.read(4) != b"RIFF":
                return None
            f.seek(8)
            if f.read(4) != b"WAVE":
                return None
            while True:
                header = f.read(8)
                if len(header) < 8:
                    return None
                chunk_id, size = header[:4], struct.unpack("<I", header[4:])[0]
                if chunk_id == b"fmt ":
                    fmt = f.read(min(size, 16))
                    if len(fmt) < 16:
                        return None
                    tag = struct.unpack_from("<H", fmt, 0)[0]
                    bits = struct.unpack_from("<H", fmt, 14)[0]
                    return tag, bits
                f.seek(size + (size & 1), os.SEEK_CUR)
    except OSError:
        return None


def sidecar_text(wav: Path, lang: str) -> str | None:
    """Caption text for the wav from its same-stem .toml sidecar."""
    sidecar = wav.with_suffix(".toml")
    if not sidecar.is_file():
        return None
    try:
        with open(sidecar, "rb") as f:
            data = tomllib.load(f)
    except (tomllib.TOMLDecodeError, OSError) as e:
        print(f"  warning: unreadable sidecar {sidecar.name}: {e}")
        return None
    for key in (lang, "en"):
        value = data.get(key)
        if isinstance(value, str) and value.strip():
            return value.strip()
    for value in data.values():  # any language beats nothing
        if isinstance(value, str) and value.strip():
            return value.strip()
    return None


def generate_lip(lipgen: Path, wav: Path, text: str, out_lip: Path) -> bool:
    # LipGenerator must run from its own directory (FonixData.cdf lives there).
    # Double quotes inside the text would break its command line parsing.
    text = text.replace('"', "'").replace("\r", " ").replace("\n", " ")
    result = subprocess.run(
        [str(lipgen), str(wav), text, f"-OutputFileName:{out_lip}"],
        cwd=lipgen.parent, capture_output=True, text=True)
    if result.returncode != 0 or not out_lip.is_file():
        message = (result.stderr or result.stdout or "").strip()
        print(f"  ERROR: LipGenerator failed on {wav.name}"
              + (f": {message.splitlines()[-1]}" if message else ""))
        out_lip.unlink(missing_ok=True)
        return False
    return True


def pack_fuz(xwmaencode: Path, wav: Path, lip: Path | None, out_fuz: Path) -> bool:
    tmp_xwm = out_fuz.with_suffix(".xwm.tmp")
    try:
        result = subprocess.run(
            [str(xwmaencode), str(wav), str(tmp_xwm)],
            capture_output=True, text=True)
        if result.returncode != 0 or not tmp_xwm.is_file():
            message = (result.stderr or result.stdout or "").strip()
            print(f"  ERROR: xwmaencode failed on {wav.name}"
                  + (f": {message.splitlines()[-1]}" if message else ""))
            return False
        # no lip -> audio-only fuz (lipSize 0): plays fine, AudioUtil lipsyncs
        # it from the decoded audio (pseudo/envelope) at runtime
        lip_bytes = lip.read_bytes() if lip else b""
        xwm_bytes = tmp_xwm.read_bytes()
        # FUZE container: magic, u32 version (1), u32 lip size, LIP block, audio
        with open(out_fuz, "wb") as f:
            f.write(b"FUZE")
            f.write(struct.pack("<II", 1, len(lip_bytes)))
            f.write(lip_bytes)
            f.write(xwm_bytes)
        return True
    finally:
        tmp_xwm.unlink(missing_ok=True)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("folder", type=Path,
                        help="voicepack folder to process (recursive)")
    parser.add_argument("--lang", default="en",
                        help="caption sidecar language key to use as spoken text (default: en)")
    parser.add_argument("--default-text", default=None, metavar="TEXT",
                        help="text for wavs without a caption sidecar (e.g. \"ah mm ah\" for "
                             "moan pools); without this, such wavs are skipped")
    parser.add_argument("--fuz", action="store_true",
                        help="also pack each wav+lip into a same-stem .fuz (needs xwmaencode.exe)")
    parser.add_argument("--force", action="store_true",
                        help="regenerate .lip/.fuz files that already exist")
    parser.add_argument("--game-path", type=Path,
                        default=Path(os.environ.get("SKYRIM_GAME_PATH", DEFAULT_GAME_PATH)),
                        help="Skyrim SE root containing Tools\\ (default: SKYRIM_GAME_PATH env or Steam path)")
    args = parser.parse_args()

    if not args.folder.is_dir():
        sys.exit(f"error: not a folder: {args.folder}")

    lipgen = find_tool(args.game_path, r"Tools\LipGen\LipGenerator\LipGenerator.exe", "LipGenerator")
    xwmaencode = None
    if args.fuz:
        xwmaencode = find_tool(args.game_path, r"Tools\Audio\xwmaencode.exe", "xwmaencode")

    wavs = sorted(args.folder.rglob("*.wav"))
    if not wavs:
        sys.exit(f"no .wav files under {args.folder}")

    generated = fuzed = kept = skipped_text = failed = 0
    for wav in wavs:
        rel = wav.relative_to(args.folder)
        lip = wav.with_suffix(".lip")

        if lip.is_file() and not args.force:
            kept += 1
        else:
            text = sidecar_text(wav, args.lang) or args.default_text
            if not text:
                skipped_text += 1
                if not args.fuz:
                    print(f"skip (no caption text): {rel}")
                    continue
                # still worth a fuz: audio-only, lipsynced from the decoded
                # audio (pseudo/envelope) at runtime
                print(f"no text: {rel} — audio-only fuz")
            else:
                fmt = wav_format(wav)
                if fmt is None or fmt != (1, 16):
                    print(f"  warning: {rel} is not 16-bit PCM {fmt or '(unreadable header)'} — "
                          f"LipGenerator may reject it")
                print(f"lip: {rel}  \"{text[:60]}{'…' if len(text) > 60 else ''}\"")
                if generate_lip(lipgen, wav, text, lip):
                    generated += 1
                else:
                    failed += 1  # fall through: an audio-only fuz still beats nothing

        if args.fuz:
            fuz = wav.with_suffix(".fuz")
            if fuz.is_file() and not args.force:
                continue
            have_lip = lip.is_file()
            print(f"fuz: {rel.with_suffix('.fuz')}{'' if have_lip else '  (no lip)'}")
            if pack_fuz(xwmaencode, wav, lip if have_lip else None, fuz):
                fuzed += 1
            else:
                failed += 1

    print(f"\ndone: {generated} lip generated, {kept} kept, {fuzed} fuz packed, "
          f"{skipped_text} without text, {failed} failed")
    if args.fuz and fuzed:
        print("note: if you ship the .fuz files, the .wav/.lip pairs beside them are "
              "redundant (AudioUtil scans would register both); move or delete them, "
              "and keep the .toml caption sidecars next to the .fuz.")


if __name__ == "__main__":
    main()
