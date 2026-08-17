# RESOLVED: the web loop never stalled; the measurement did

**There was no engine bug.** The web build runs indefinitely at 60fps. The
stall was an artifact of how it was being observed: a one-shot
`chromium --headless=old --screenshot --timeout=N` freezes the page's timer
task queue right after the load event, so the wake-up timer that rewinds
`emscripten_sleep` is never delivered. The wasm side is left in the healthy
"suspended, waiting for rewind" state -- which is why there was never an
error. One to three frames fit in the window between `main()` starting and
the freeze.

Proved by running the identical binary with no `--screenshot`: 1481
unwind/rewind cycles in 25 seconds (~59/s), and separately by
`examples/07_net` logging `frame 2340  sent 77  received 77`.

**The lesson is about the harness, not the engine: pixels are not liveness.**
Assert a web build is alive from console output in a long-lived session, or
from an independent observer such as a relay counting messages. Every web
screenshot taken before this was of a page that had run about three frames --
which is also why the showcase looked black.

The original note follows, unchanged, because the reasoning that got here is
worth keeping.

---

# Original note: the game loop stops after 1-3 frames

Working note, not a design document. Facts and their provenance, so the
investigation starts from evidence rather than from my earlier guesses --
several of which were wrong.

## Expected behaviour

`main()` runs a blocking loop:

```c
while (mye_running(world)) {
    mye_progress(world, GetFrameTime());
}
```

The web build links `-sASYNCIFY`, whose purpose is to let exactly this shape
work in a browser: the stack unwinds at a yield point, the browser services
its event loop, and the stack rewinds and continues. Expected result is the
same as desktop -- the loop runs indefinitely at display rate, frame and
draw counters climbing, input and socket events delivered between frames.

## Observed behaviour

`examples/07_net` draws three numbers: `draws` (incremented by the draw
system itself), `frame` (`MyeTime.frame`, engine-maintained), and `GetFPS()`.

Headless chromium, `--headless=old --disable-gpu --enable-unsafe-swiftshader`,
page served by `tools/web_dev.py` (sends `Cache-Control: no-store`), captured
after **18 seconds** of wall clock:

```
sent 0  received 0  frame 3  draws 3  fps 384
```

Three frames in eighteen seconds, at a reported 384 FPS. The loop is not
slow; it stops.

## What has been established

1. **Not networking.** Removing the socket from the example entirely --
   `mye_net_connect` never called -- gives the same stall (`frame 2, draws 2,
   fps 503`).
2. **No error is reported.** The page console shows normal raylib init,
   then `net: connecting`, `net: connected`, then nothing. No exception, no
   `unreachable`, no abort, no emscripten assertion.
3. **ASYNCIFY is genuinely linked.** The link line carries `-sASYNCIFY`; the
   generated `.js` contains 78 references to `Asyncify` and exports
   `_emscripten_sleep`.
4. **Adding a yield did not help.** `emscripten_sleep(0)` at the end of
   `mye_progress` changed nothing (still 3 frames). That change was reverted
   rather than kept.
5. **Stale caching was a real confounder earlier.** `python3 -m http.server`
   sends no cache headers and chromium served stale wasm across rebuilds.
   The numbers above are from the `no-store` dev server and are trustworthy;
   older observations in git history may not be.
6. **The JS side appears to keep running.** During an earlier hot-reload
   test on a different example, the shell's `/build-id` poller (a JS
   `setTimeout` loop) produced ~40 polls in ~20 seconds, and the page
   reloaded itself when a rebuild finished. **Verify this independently** --
   if the JS event loop really is alive while the wasm loop is stopped, then
   the main thread is *not* blocked, and the failure is "unwound and never
   rewound" rather than "spinning without yielding". Those have different
   causes and the distinction matters.

## Two candidate explanations, undistinguished

- **Engine/toolchain bug.** The loop unwinds at some yield point and never
  resumes. Suspects: what raylib's `PLATFORM_WEB` `EndDrawing`/`WaitTime`
  does under ASYNCIFY given `target_fps = 60`; whether any yield happens
  inside a call reached from a JS callback (ASYNCIFY cannot unwind through
  a JS frame); whether ASYNCIFY's instrumentation is missing a function on
  the unwind path.
- **Headless artifact.** `requestAnimationFrame` throttled or not driven for
  a page that is never visible, in which case a real browser is unaffected
  and there is no engine bug at all.

## Reproducing

```sh
./build/debug/examples/example_07_net --serve          # optional; not required
python3 tools/web_dev.py --target example_07_net --port 8086
timeout 90 chromium --headless=old --disable-gpu --enable-unsafe-swiftshader \
    --window-size=720,300 --timeout=18000 --screenshot=/tmp/shot.png \
    "http://localhost:8086/example_07_net.html?autostart"
```

`?autostart` skips the click-to-start veil. Any web example reproduces it;
`07_net` is the one with counters on screen.

Relevant files: `engine/core/engine.c` (`mye_progress`, the loop contract),
`cmake/MyeWeb.cmake` (link options), `web/shell.html` (page + poller),
`examples/07_net/main.c` (the counters). raylib's web backend is in
`build/web/_deps/raylib-src/src/platforms/rcore_web.c`.
