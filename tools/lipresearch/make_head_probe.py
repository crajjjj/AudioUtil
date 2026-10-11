"""Build the head-channel probe: a wav + same-stem .lip that answers, in game,
whether Skyrim plays the three head channels of a lip (HeadPitch / HeadRoll /
HeadYaw, grid slots 30-32). See "RLE Hypothesis.md" section 13: static analysis
says it does not, this is the test that settles it.

    python make_head_probe.py <output folder under Data\\Sound>

The line has four phases, each announced by that many beeps:

    1 beep   brows up              the CONTROL: proves modifiers are being driven
    2 beeps  HeadPitch +0.8, -0.8
    3 beeps  HeadYaw   +0.8, -0.8
    4 beeps  HeadRoll  +0.8, -0.8

Play it with `autest playlip Sound\\<folder>\\headtilt.wav` in third person, with
`[lipsync] use_lip_files = true` and `drive_modifiers = true` in the base
AudioUtil.toml that wins on the install. Brows move and the head never does =
the head channels are not played. No brow movement = the test did not run (the
lip was not used or modifiers are off), which says nothing about the head.
"""
import math
import struct
import sys
import wave
from pathlib import Path

FPS, STRIDE = 30, 33
DURATION = 8.5
RATE = 44100
SLOT_AAH, SLOT_BROW_UP_L, SLOT_BROW_UP_R = 0, 22, 23
SLOT_PITCH, SLOT_ROLL, SLOT_YAW = 30, 31, 32
LEVEL = 0.8   # the engine routine reads these as radians: 0.8 = 46 degrees
# (start second, beeps, [(slot, value at first half, value at second half)])
PHASES = [
    (0.5, 1, [(SLOT_BROW_UP_L, 1.0, 1.0), (SLOT_BROW_UP_R, 1.0, 1.0)]),
    (2.5, 2, [(SLOT_PITCH, LEVEL, -LEVEL)]),
    (4.5, 3, [(SLOT_YAW, LEVEL, -LEVEL)]),
    (6.5, 4, [(SLOT_ROLL, LEVEL, -LEVEL)]),
]
PHASE_LEN = 1.4


def build_grid():
    frames = round(DURATION * FPS)
    grid = [0.0] * (frames * STRIDE)
    for f in range(frames):
        t = f / FPS
        # a lip with no mouth data is treated as empty and dropped: keep the
        # lips slightly parted for the whole line
        grid[f * STRIDE + SLOT_AAH] = 0.08
        for start, _, channels in PHASES:
            if start <= t < start + PHASE_LEN:
                first_half = t < start + PHASE_LEN / 2
                for slot, a, b in channels:
                    grid[f * STRIDE + slot] = a if first_half else b
    return frames, grid


def zero_rle(data: bytes) -> bytes:
    """A nonzero byte is a literal; 0x00 + u16 count stands for that many zeros."""
    data = data.rstrip(b"\0")  # the encoder omits the trailing rest run
    out = bytearray()
    i = 0
    while i < len(data):
        if data[i]:
            out.append(data[i])
            i += 1
            continue
        run = 0
        while i < len(data) and data[i] == 0 and run < 0xFFFF:
            run += 1
            i += 1
        out += b"\0" + struct.pack("<H", run)
    return bytes(out)


def build_lip() -> bytes:
    frames, grid = build_grid()
    # 24-byte classic header: version, duration, curve count, frames, const14=3,
    # preroll 0 (no lead-in), phoneme-track count 16, start token 3 (no hoisted
    # leading zero cells)
    header = struct.pack("<IIIHHiHH", 1, 132 * frames + 28, 13, frames, 3, 0, 16, 3)
    return header + zero_rle(struct.pack(f"<{len(grid)}f", *grid))


def build_wav(path: Path):
    n = round(DURATION * RATE)
    samples = [0.0] * n
    for start, beeps, _ in PHASES:
        for b in range(beeps):
            t0 = start + b * 0.2
            for i in range(round(0.08 * RATE)):
                k = round(t0 * RATE) + i
                # 5 ms fade at both ends, so the beep does not click
                edge = min(i, round(0.08 * RATE) - 1 - i) / (0.005 * RATE)
                samples[k] = 0.4 * min(1.0, edge) * math.sin(2 * math.pi * 880 * i / RATE)
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(struct.pack(f"<{n}h", *(round(s * 32767) for s in samples)))


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    out = Path(sys.argv[1])
    out.mkdir(parents=True, exist_ok=True)
    lip = build_lip()
    (out / "headtilt.lip").write_bytes(lip)
    build_wav(out / "headtilt.wav")
    print(f"wrote {out / 'headtilt.wav'} and headtilt.lip ({len(lip)} bytes, {round(DURATION * FPS)} frames)")


if __name__ == "__main__":
    main()
