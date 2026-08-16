#!/usr/bin/env python3
"""Serve the WebAssembly build, rebuild on change, reload the browser.

    tools/web_dev.py                        # asteroids on :8080
    tools/web_dev.py --example 05_showcase --port 9000

Cross-platform by construction: nothing here uses inotify, fswatch or any
other platform API. Python ships with emscripten, so it costs no dependency.

See plan/11-web-dev-loop.md.
"""

import argparse
import http.server
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD_DIR = ROOT / "build" / "web"

# Directories whose C sources trigger a rebuild.
WATCHED = ["engine", "examples", "web"]
WATCHED_SUFFIXES = {".c", ".h", ".html", ".glsl"}

# Incremented after every successful rebuild; the page polls it and reloads.
build_id = 0
build_lock = threading.Lock()
last_error = ""


def log(message: str) -> None:
    print(f"[web_dev] {message}", flush=True)


def emsdk_env() -> dict:
    """Environment with emscripten on PATH.

    emcmake/emcc live in the SDK rather than on the system PATH, so the
    child processes need it explicitly. EMSDK is set once emsdk_env.sh has
    been sourced; otherwise fall back to the conventional location.
    """
    env = os.environ.copy()
    if "EMSDK" in env:
        return env

    emsdk = Path.home() / "emsdk"
    upstream = emsdk / "upstream" / "emscripten"
    if upstream.is_dir():
        env["PATH"] = f"{upstream}{os.pathsep}{emsdk}{os.pathsep}" + env["PATH"]
        env["EMSDK"] = str(emsdk)
    return env


def source_fingerprint() -> dict:
    """Modification times of every watched source file.

    Polling rather than filesystem events: a few hundred stat() calls every
    300 ms is nothing next to a rebuild, and it behaves identically on every
    platform.
    """
    stamps = {}
    for directory in WATCHED:
        base = ROOT / directory
        if not base.is_dir():
            continue
        for path in base.rglob("*"):
            if path.suffix in WATCHED_SUFFIXES:
                try:
                    stamps[str(path)] = path.stat().st_mtime
                except OSError:
                    pass
    return stamps


def configure(example: str) -> bool:
    log("configuring web build (first run takes a minute)")
    result = subprocess.run(
        ["emcmake", "cmake", "-S", str(ROOT), "-B", str(BUILD_DIR),
         "-DCMAKE_BUILD_TYPE=Release"],
        env=emsdk_env(), capture_output=True, text=True)
    if result.returncode != 0:
        log("configure failed:")
        print(result.stderr[-3000:], file=sys.stderr)
        return False
    return True


def build(example: str) -> bool:
    global build_id, last_error

    target = f"example_{example}"
    started = time.monotonic()
    result = subprocess.run(
        ["cmake", "--build", str(BUILD_DIR), "--target", target,
         "-j", str(os.cpu_count() or 4)],
        env=emsdk_env(), capture_output=True, text=True)

    if result.returncode != 0:
        # Compiler diagnostics only: the rest is build-system chatter.
        lines = [line for line in result.stderr.splitlines()
                 if "error" in line.lower()]
        last_error = "\n".join(lines[:20])
        log(f"build FAILED\n{last_error}")
        return False

    elapsed = time.monotonic() - started
    with build_lock:
        build_id += 1
        current = build_id
    last_error = ""
    log(f"rebuilt in {elapsed:.1f}s (build {current}) -- browser will reload")
    return True


def watch(example: str) -> None:
    known = source_fingerprint()
    while True:
        time.sleep(0.3)
        current = source_fingerprint()
        if current != known:
            changed = [Path(p).name for p in current
                       if known.get(p) != current.get(p)]
            log(f"changed: {', '.join(sorted(set(changed))[:4])}")
            known = current
            build(example)


class Handler(http.server.SimpleHTTPRequestHandler):
    """Static files, plus the build-id endpoint the page polls."""

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(BUILD_DIR / "examples"), **kwargs)

    def do_GET(self):  # noqa: N802 - name fixed by the base class
        if self.path.startswith("/build-id"):
            with build_lock:
                body = str(build_id).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/plain")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def end_headers(self):
        # Cross-origin isolation: harmless now, and required the day the
        # build enables pthreads (SharedArrayBuffer is gated behind it).
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        # Never cache the module during development.
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_message(self, fmt, *args):
        pass  # the access log drowns out the build output


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--example", default="02_asteroids",
                        help="example directory name, e.g. 05_showcase")
    parser.add_argument("--port", type=int, default=8080)
    parser.add_argument("--no-watch", action="store_true",
                        help="serve without rebuilding on change")
    args = parser.parse_args()

    if not configure(args.example):
        return 1
    if not build(args.example):
        log("initial build failed; fix the errors and save to retry")

    if not args.no_watch:
        threading.Thread(target=watch, args=(args.example,),
                         daemon=True).start()

    url = f"http://localhost:{args.port}/example_{args.example}.html"
    log(f"serving {url}")
    log("edit any .c or .h file to rebuild and reload; Ctrl-C to stop")

    server = http.server.ThreadingHTTPServer(("", args.port), Handler)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        log("stopped")
    return 0


if __name__ == "__main__":
    sys.exit(main())
