#!/usr/bin/env python3
"""Serve lipsim.html over localhost with native xWMA decoding.

    python lipsim_server.py            # opens http://127.0.0.1:8737/lipsim.html

Why: double-clicking lipsim.html works for wav/lip, but a file:// page cannot
spawn the web worker ffmpeg.wasm needs, so fuz xWMA audio stays silent. Served
over localhost the page is fully unrestricted — and this server also exposes
POST /decode, which lipsim uses to decode a fuz's xWMA audio through the CK's
own xwmaencode.exe (instant, offline, bit-exact with the game's pipeline;
ffmpeg.wasm remains the page's fallback when /decode is unavailable).

Requires the CK audio tools for /decode (Tools\\Audio\\xwmaencode.exe);
override the game root with --game-path or SKYRIM_GAME_PATH.
"""

import argparse
import os
import subprocess
import tempfile
import threading
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

DEFAULT_GAME_PATH = r"C:\SteamLibrary\steamapps\common\Skyrim Special Edition"
ROOT = Path(__file__).resolve().parent
XWMAENCODE: Path | None = None

MIME = { ".html": "text/html; charset=utf-8", ".js": "text/javascript",
         ".css": "text/css", ".wasm": "application/wasm" }


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):  # quiet
        pass

    def _send(self, code, body, ctype="text/plain; charset=utf-8"):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        name = self.path.split("?")[0].lstrip("/") or "lipsim.html"
        file = (ROOT / name).resolve()
        # serve only files directly inside the lipsim folder
        if file.parent != ROOT or not file.is_file():
            self._send(404, b"not found")
            return
        self._send(200, file.read_bytes(), MIME.get(file.suffix.lower(), "application/octet-stream"))

    def do_POST(self):
        if self.path.rstrip("/") != "/decode":
            self._send(404, b"not found")
            return
        if XWMAENCODE is None:
            self._send(500, b"xwmaencode.exe not found on this machine (set SKYRIM_GAME_PATH)")
            return
        length = int(self.headers.get("Content-Length", 0))
        if not 0 < length < 256 * 1024 * 1024:
            self._send(400, b"bad length")
            return
        data = self.rfile.read(length)
        tmp = Path(tempfile.mkdtemp(prefix="lipsim_"))
        try:
            xwm, wav = tmp / "in.xwm", tmp / "out.wav"
            xwm.write_bytes(data)
            result = subprocess.run([str(XWMAENCODE), str(xwm), str(wav)],
                                    capture_output=True, text=True, timeout=60)
            if result.returncode != 0 or not wav.is_file():
                message = (result.stderr or result.stdout or "xwmaencode failed").strip()
                self._send(500, message.encode())
                return
            self._send(200, wav.read_bytes(), "audio/wav")
        finally:
            for f in tmp.iterdir():
                f.unlink(missing_ok=True)
            tmp.rmdir()


def main() -> None:
    global XWMAENCODE
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--port", type=int, default=8737)
    parser.add_argument("--game-path", type=Path,
                        default=Path(os.environ.get("SKYRIM_GAME_PATH", DEFAULT_GAME_PATH)))
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()

    exe = args.game_path / r"Tools\Audio\xwmaencode.exe"
    if exe.is_file():
        XWMAENCODE = exe
    else:
        print(f"warning: {exe} not found — /decode disabled, the page falls back to ffmpeg.wasm")

    url = f"http://127.0.0.1:{args.port}/lipsim.html"
    server = ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print(f"lipsim at {url}  (Ctrl+C to stop)")
    if not args.no_browser:
        threading.Timer(0.3, webbrowser.open, [url]).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
