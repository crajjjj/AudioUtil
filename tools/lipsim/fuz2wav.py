#!/usr/bin/env python3
"""Crack .fuz files into lipsim-ready pieces: <stem>.wav (+ <stem>.lip if embedded).

A browser can't decode a fuz's xWMA audio, so tools/lipsim/lipsim.html simulates
such files silently. This helper extracts the embedded LIP block (if any) and
decodes the audio to PCM wav with the CK's xwmaencode.exe — drop the resulting
wav (+ lip, or the original fuz) into lipsim together and it plays with sound.

Usage:
  python fuz2wav.py <file.fuz | folder> [more...] [--out DIR] [--game-path PATH]

Files are written beside each fuz unless --out is given. Requires the Skyrim SE
Creation Kit audio tools (Tools\\Audio\\xwmaencode.exe); override the game root
with --game-path or the SKYRIM_GAME_PATH env var.
"""

import argparse
import os
import struct
import subprocess
import sys
from pathlib import Path

DEFAULT_GAME_PATH = r"C:\SteamLibrary\steamapps\common\Skyrim Special Edition"


def crack(fuz: Path, out_dir: Path | None, xwmaencode: Path) -> bool:
    data = fuz.read_bytes()
    if len(data) < 12 or data[:4] != b"FUZE":
        print(f"skip (not a FUZE container): {fuz}")
        return False
    lip_size = struct.unpack_from("<I", data, 8)[0]
    if 12 + lip_size > len(data):
        print(f"skip (corrupt lip size): {fuz}")
        return False
    dest = (out_dir or fuz.parent) / fuz.stem
    audio = data[12 + lip_size:]

    made = []
    if lip_size >= 24:
        Path(f"{dest}.lip").write_bytes(data[12:12 + lip_size])
        made.append("lip")

    out_wav = Path(f"{dest}.wav")
    form = audio[8:12] if len(audio) > 12 else b""
    if audio[:4] == b"RIFF" and form in (b"WAVE", b"XWMA"):
        fmt_tag = struct.unpack_from("<H", audio, 20)[0] if len(audio) > 22 else 0
        if form == b"WAVE" and fmt_tag == 1:  # already plain PCM — just copy it out
            out_wav.write_bytes(audio)
            made.append("wav (pcm copy)")
        else:  # xWMA (RIFF form "XWMA") — decode via the CK tool
            tmp_xwm = Path(f"{dest}.fuz2wav.tmp.xwm")
            try:
                tmp_xwm.write_bytes(audio)
                result = subprocess.run([str(xwmaencode), str(tmp_xwm), str(out_wav)],
                                        capture_output=True, text=True)
                if result.returncode != 0 or not out_wav.is_file():
                    message = (result.stderr or result.stdout or "").strip()
                    print(f"  ERROR: xwmaencode failed on {fuz.name}"
                          + (f": {message.splitlines()[-1]}" if message else ""))
                    return bool(made)
                made.append("wav (xwma decoded)")
            finally:
                tmp_xwm.unlink(missing_ok=True)
    else:
        print(f"  warning: {fuz.name} audio block is not RIFF — skipped audio")

    print(f"{fuz.name} -> {', '.join(made) if made else 'nothing (no lip, no audio)'}")
    return bool(made)


def main() -> None:
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("inputs", nargs="+", type=Path, help=".fuz files and/or folders (recursive)")
    parser.add_argument("--out", type=Path, default=None, help="output directory (default: beside each fuz)")
    parser.add_argument("--game-path", type=Path,
                        default=Path(os.environ.get("SKYRIM_GAME_PATH", DEFAULT_GAME_PATH)))
    args = parser.parse_args()

    xwmaencode = args.game_path / r"Tools\Audio\xwmaencode.exe"
    if not xwmaencode.is_file():
        sys.exit(f"error: xwmaencode not found at {xwmaencode} (set --game-path / SKYRIM_GAME_PATH)")
    if args.out:
        args.out.mkdir(parents=True, exist_ok=True)

    fuzes = []
    for item in args.inputs:
        if item.is_dir():
            fuzes.extend(sorted(item.rglob("*.fuz")))
        elif item.is_file():
            fuzes.append(item)
        else:
            print(f"skip (not found): {item}")
    if not fuzes:
        sys.exit("no .fuz files to process")

    ok = sum(crack(f, args.out, xwmaencode) for f in fuzes)
    print(f"done: {ok}/{len(fuzes)} cracked")


if __name__ == "__main__":
    main()
