# 15 — Gaps: planned but not built

An audit of every promise in plan/00–14 against the code, 2026-08-20. Method:
each plan's stated deliverables — feature lists, milestone bullets, named
APIs, named tests — were checked by searching the source for the symbol, file,
or test, not by trusting the plan's own status markers. Line references are to
the tree at the time of the audit.

Three kinds of finding are separated below, because they call for different
responses. A **gap** is a promise with no implementation: something to build,
or to strike from the plan on purpose. **Unconnected infrastructure** exists
and passes tests but nothing uses it: something to wire up or delete. **Rot**
is a statement in a plan or a comment that is now false: something to fix in
an afternoon, before it misleads somebody. The last section lists what the
plans *deliberately* deferred — those are decisions, not omissions, and are
here only so they are not rediscovered as gaps.

## 1. Feature gaps

### Engine features promised in the overview's Tier 1/2 (00-overview.md)

| Promise | Where promised | State |
|---|---|---|
| **Text rendering** | 00 Tier 1 "2D rendering — … text"; 03 "2D pipeline": "raylib `DrawTextEx` with loaded `Font` assets" | Nothing. No engine text API; the only `DrawText` in the engine is the debug overlay's. Games call raylib directly. |
| **Font assets** | 00 Tier 1 "handle-based registry for textures, models, sounds, fonts"; 06 declares `mye_font` with the other handle types | No `mye_font` type, loader, or slot storage. The registry has textures, sounds, models only. |
| **Music playback** | 00 Tier 1 "sound effects and music playback"; 07 M3 "Audio module: sfx + music"; 06 "WAV (sfx) + OGG (music)" | Sfx only. `engine/audio` is a one-shot `Sound` queue; no `LoadMusicStream`/`UpdateMusicStream`, no streaming handle, no OGG anywhere. |
| **2D collision** | 00 Tier 2 "AABB + circle overlap tests and collision events (not a physics engine)"; 07 M3 bullet, ticked | No engine collision facility at all — no component, no overlap helpers, no events. Asteroids does its own circle test in game code. The M3 checkmark is wrong about this bullet. |
| **Gameplay events** | 00 Tier 2 "gameplay events via flecs observers / `ecs_emit`" | `ecs_emit` appears nowhere in the repo. Observers are used only for engine bookkeeping. No API, example, or test shows a game how to do events. |
| **Debug overlay: entity/system counts, gizmos** | 00 Tier 1; 01 even reserves `engine/debug/ # overlay, gizmos` | Overlay shows table/merge counts and scene-owned entities — no total entity count, no system count, and "gizmo" matches nothing in the repo. |
| **`MyeVelocity2D/3D` core components** | 02 "Core components (defined by the engine)" | Do not exist; every example defines its own `Velocity`. Either ship them or strike them — currently the plan promises a vocabulary the engine doesn't speak. |
| **`core: math` and `core: containers`** | 01 layer diagram and repo layout | Neither file exists. The matrix helpers live in `engine/scene/transform.c` — above the layer the plan puts them in. No container types were ever needed; the promise outlived the need. |

### Cameras C2 and C3 (13-cameras.md)

- **C2 — layer masks.** Nothing exists: no `MyeVisibilityLayers`, no `layers`
  field on either camera component, no intersection test in either pass, and
  neither of the two tests the milestone names. This is the item that blocks
  "the minimap shows blips, not the world".
- **C3 — per-camera near/far.** `MyeCamera3D` has no `near_plane`/`far_plane`;
  [camera.c](../engine/render/camera.c) builds every projection from raylib's
  global cull distances — still the hardcoded 0.01/1000 the plan set out to
  fix.
- **C3 — explicit-camera helpers.** `mye_world_to_screen_for` and
  `mye_screen_ray_for` do not exist.
- **C3 — viewport-aware picking. The one with a correctness trap.**
  `mye_screen_ray` and `mye_screen_to_world_2d` resolve the active camera and
  project against the **full window size**, ignoring the camera's viewport
  rect. The plan's own warning — "clicks in player 2's half are interpreted in
  player 1's world … invisible until two players are on screen" — describes
  the current code. `mye_camera_at_screen` exists and is tested, but nothing
  consumes it: it is half of a workflow whose other half is missing.

### Networking N1 and N2 (12-networking.md)

- **The flecs module.** The plan's "MyeNetModule" section promises a system
  pumping the connection in the input-polling slot and a `MyeNetStatus`
  singleton for the debug overlay. Neither exists —
  [examples/07_net/main.c](../examples/07_net/main.c) hand-rolls its own `Net`
  singleton and `NetPump` system, i.e. the thing the engine was supposed to
  provide.
- **N1 — presence example.** `examples/07_net` is the N0 echo vehicle, not the
  promised presence demo: no per-client entity, no position relay, no
  `MyeInterpolate` on remotes, no chat line, no 1-byte kind prefix (promised
  as "a pattern to copy"), no RTT display, and no two-clients-plus-relay
  integration test.
- **N2 — hardening.** Of its four bullets, only the THIRD-PARTY-NOTICES entry
  and the message size cap exist. Missing: TLS/`wss://` (tracked honestly —
  a test asserts `wss://` is refused), reconnect-with-backoff, and fuzzing the
  receive path.
- Smaller N0 shortfalls: the "10k messages round-trip" DoD is tested with 200;
  nothing asserts a graceful close reaching `MYE_NET_CLOSED`; the headless-
  chromium-against-native-relay verification exists only as a by-hand
  procedure in WEB-LOOP-STALL.md, scripted nowhere.

### Web (10-web.md, 11-web-dev-loop.md)

- **Tier-2 state-preserving reload.** The roadmap's own M8 entry says "still
  to do". Only the JS half exists: [web/shell.html](../web/shell.html) calls
  `window.myeSnapshot` if defined, and nothing ever defines it. No
  `mye_web_snapshot`/`mye_web_restore`, no `EMSCRIPTEN_KEEPALIVE` exports, no
  sessionStorage round-trip. The serializer it needs has existed since M6.
- **`mye_run(world, frame_fn)` loop inversion.** 10-web promises it
  unconditionally ("introduce `mye_run` and convert all examples");
  11-web-dev-loop demotes it to "only if ASYNCIFY misbehaves". ASYNCIFY has
  not misbehaved, so nothing was built — fine, but 10-web should be edited to
  match the decision 11 already made.
- **Web build in `tools/check.sh`.** Both web plans call for the web target in
  the routine check sequence; check.sh has no web step, so web breakage is
  invisible until someone builds by hand.
- **`emrun` target** (11: "keep an emrun target as well") — never added.

### Assets and scenes (06-assets.md, 07-roadmap.md)

- **`FAILED` slot state.** The registry has `EMPTY` and `LOADED` only. A
  failed load leaves `EMPTY`, so "why did this asset not resolve" cannot be
  answered from the registry. (`LOADING` died with async, correctly.)
- **Shutdown leak report.** 06 promises "debug builds report any handle still
  live at shutdown *with the path that loaded it*". `assets_fini` silently
  unloads everything; the stored key is never printed. The allocator reports
  leaked bytes, which says nothing about which asset.
- **Loading-screen support** (07 M4 bullet). No hook exists to draw a frame
  around a scene load; no example draws one.
- **Menu→game→menu in the M3 game** (07 M6 bullet). Asteroids never touches
  the scene system — it has an in-place game-over flag. The M3 game and the
  M6 scene system were never joined; only the tutorial example uses scenes.
- **M7 "profile first"**: the overlay has a frame-time ring, but flecs
  pipeline/system stats appear nowhere — the half that would say *which
  system* to shard is missing.

## 2. Built but never connected

- **`mye_channel` and `mye_jobs` have zero consumers.** Grep finds no caller
  outside their own unit tests. Their one real consumer (asset workers) was
  removed with async loading, and the promised second consumer (05: "the
  WebSocket transport uses the same pattern") never materialised — the
  transport is deliberately single-threaded. They are kept alive by 13 unit
  tests and nothing else. Decide: keep as the documented pattern for game
  threads (TUTORIAL §17 teaches them), or delete. If kept, say so in 05.
- **`mye_pool` is used by nobody.** `mye_pool_init` is called only in
  alloc tests. Every stated use case fell through: asset slots are plain
  arrays (despite 06 *and* the roadmap's M4 DoD claiming "slot storage uses
  `mye_pool`" — both false), particles and audio voices don't exist.
- **`multi_threaded` was never applied to an engine system.** 05 §1 promises
  "turn it on per-system in M7, starting with pure data-transform systems".
  Only the stress example and the workers test mark systems; every engine
  system runs single-threaded. The mechanism is proven; the promised
  application never happened.
- **Ownership table arrangements** (04): "scene entities → scene arena" and
  "asset CPU data → asset arena" — neither arena exists. Scene ownership went
  a different, working way (scope tags); the plan still sells the arena story.
- **Stale test binary**: `build/tests/test_int_async_assets` survives from
  before the async removal with no source file. Harmless, confusing; a clean
  build directory removes it.

## 3. Testing promises unmet (09-testing.md)

The policy's hard rule — "every non-rendering engine module has
`tests/unit/test_<module>.c`" — is met by 6 modules and unmet for **asset,
scene, serialize, transform, log, audio, net, camera** (integration tests
cover most behaviour, but the rule as stated is false; either soften the rule
or add the files). Specific named-test gaps:

- 09's worked example `int_render_sprites` — no sprite render smoke test
  exists (3D smoke and camera tests do).
- 09 M3 row "bounded high-water mark" for the full game run — asteroids
  asserts zero leaks but no high-water bound.
- 13's named test "viewport and scissor restored after a frame, HUD not
  clipped" — the code restores; the test doesn't exist.
- 14's test 9 `the_screenshot_is_of_the_window_not_the_last_canvas` — the
  only C4 test not implemented (its target mutation is caught by another
  test, but nothing asserts the screenshot's identity directly).
- Render-logic unit tests ("draw-list sorting order … pure functions, no
  GL") — the comparator is `static`; not unit-tested.
- ECS-glue test ("module import registers expected components/systems") —
  nothing asserts module registration.

## 4. Build promises unmet (08-build.md)

- `FLECS_REST`/`FLECS_STATS` were to be Debug-only; neither option is set
  anywhere, so Release still compiles them in (only runtime start is gated).
- `ASAN_OPTIONS=halt_on_error=1` in the CTest environment — no `ENVIRONMENT`
  property exists on any test.
- `FIND_PACKAGE_ARGS` (the stated reason for the CMake ≥ 3.24 floor) is used
  nowhere; dependencies are always fetched.

## 5. Rot: statements that are now false

Worth an afternoon, before they mislead. The two in **code comments** are the
priority; plan prose ages more gracefully.

| Where | Says | Truth |
|---|---|---|
| [engine/core/engine.c:35](../engine/core/engine.c#L35) | tracking counters "are not atomic … must be made thread-safe before workers are enabled" | They have been atomic since M4; workers are enabled and TSan-clean. Actively wrong, in the exact place someone would look before enabling workers. |
| [engine/core/channel.h:1](../engine/core/channel.h#L1) | "Bounded lock-free queue" | It is a mutex-protected ring — deliberately, per the primitive policy. The file's own body says so; its first line disagrees. |
| [engine/core/engine.h](../engine/core/engine.h) phase comment | advertises "follow / orbit systems" in `MyeOnCamera` | Only follow exists; orbit/fly controllers were never shipped as engine systems (03 also promises them "as optional module systems"). |
| 05 §3 | channels are Concurrency Kit `ck_ring` | ck was never a dependency; the mutex ring shipped instead. The roadmap's M4 notes record the real decision; 05 §3 was never updated. |
| 05 §2 | jobs built on C11 `<threads.h>`, pthreads as fallback | Inverted: pthreads on POSIX (TSan cannot instrument glibc's C11 threads), C11 threads on Windows. |
| 05 §3 | "the WebSocket transport uses the same [channel] pattern" | The transport is single-threaded and uses no channel. |
| 07 M4 DoD / 06 | "Slot storage uses `mye_pool`" | Plain arrays; the pool is unused (§2 above). |
| 07 Tier 3 / 00 Tier 3 | skeletal animation listed as unscheduled | `MyeModelAnimator` is implemented, tested, and demoed by the showcase. |
| 10-web:3, 11-web-dev-loop:3, plan/README rows | "Status: planned, not started" / "(planned, M8)" / 14 "planned … hand-off ready" | M8 shipped (tier-1 reload); C4 shipped. |
| 03 interpolation section | "(designed, not yet built)" parenthetical; overlay reports "interpolated 0/412 entities" | Interpolation is built and tested; the overlay reports alpha but no interpolated-entity count. |
| 01/02/06/08 API names | `MyeRender2dImport`, `MyeAssetDb`, `MyeScale`, `mye_scene_load/unload`, `mye_fixed_system`, `SearchAndSetResourceDir`, `examples/02_game2d` | Actual: `MyeRender2dModuleImport`, `MyeAssets`, `MyeScale2D/3D`, `mye_scene_register/switch/reload`, the `MyeOnFixedUpdate` phase, `mye_asset_path()`, `examples/02_asteroids`. The features exist under different names; the plans teach the wrong ones. |
| 01 main-loop sketch | `while (!WindowShouldClose()) ecs_progress(...)` | Unusable — input polling and the fixed pipeline live in `mye_progress`/`mye_running`. |
| 08 targets table | `example_00_hello` links `engine` | Deliberately links raylib only, to stay a toolchain proof. |

## 6. Explicitly deferred — decisions, not gaps

Listed so nobody re-reports them. Sources in parentheses.

**Rendering** (03): shadows · point/spot lights (`MyeLight` is directional
only) · image-based lighting · custom shader/material system · post-processing
· instanced drawing · frustum culling (out of scope by decision, 13) ·
transparent-mesh back-to-front sorting ("when the need arises").
**Cameras** (13): SubViewport-style node type · projection as a separate
component · follow dead zones and `MyeCameraBounds` ("take later") ·
`is_position_in_frustum`.
**Canvases** (14): canvas-in-canvas beyond one level · resize-in-place ·
mesh UV flipping (documented mirroring instead).
**Memory** (04): TLSF · mimalloc · per-allocation file/line tracking.
**Assets** (06): async/streaming loading (**dropped**, not deferred — the
plan records the removal) · atlas metadata tooling (Tier 3).
**Web** (10): WebGL 1 · pthreads on web ("later opt-in").
**Networking** (12): WebRTC/WebTransport · state-sync/rollback · interest
management · NAT traversal · matchmaking.
**Testing/build** (08, 09): CI ("don't do CI for now" — user decision;
`tools/check.sh` is the stand-in) · automated golden-image pixel diff ·
`.clang-format`/`clang-tidy`.
**Tier 3, unscheduled** (00, 07): particles · physics · Lua scripting ·
editor tooling. (Skeletal animation escaped this list — see §5.)

## Reading the list

If the question is "what was *forgotten*", the sharpest candidates are the
ones where half the work shipped and the finishing move didn't: the net
module + N1 presence example (the transport is done and tested; nothing shows
a game using it), C2/C3 (multi-camera works; the picking helpers that make
split screen *correct* don't exist, and the plan itself calls that failure
invisible), the tier-2 reload (one JS line has been waiting for its C
counterpart since M8), text/music/collision (promised in Tier 1/2, ticked off
inside milestone bullets, never built), and the two false code comments in §5
(five minutes each, and they sit exactly where a future decision would read
them).
