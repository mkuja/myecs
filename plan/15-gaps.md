# 15 — Gaps: what remains

The original audit (2026-08-20, commit `69ebcdf`) checked every promise in
plan/00–14 against the code and found ~30 unmet ones. Later the same day,
nine parallel implementation branches closed most of them: text and font
rendering, streaming music, engine collision + gameplay events +
`MyeVelocity2D/3D`, camera milestones C2–C3, networking N1 and the engine
net module (plus backoff, fuzz, and volume tests), the tier-2
state-preserving web reload, the overlay's entity/system counts and
per-system profile with `FLECS_STATS` gated out of Release, the `FAILED`
asset state and shutdown leak report, the Asteroids menu→game→menu join, the
named missing tests (sprite smoke, high-water bound, draw-order unit,
module-registration), the CTest sanitizer environment, and every false
statement in §5 of the original audit, the two code comments included. The
full original enumeration is in git history; this file now lists only what
is still open.

Each remaining item is open for a reason, and the reason is the sorting key:
**decisions** wait for the user, **designs** need an API sketch before code,
**limits** are shipped behaviour documented as imperfect.


## 1. Decisions — not build tasks

- **`mye_channel` / `mye_jobs` / `mye_pool` have no consumer.** Still true
  after the wave: the asset workers that consumed them were removed with
  async loading, the net transport is deliberately single-threaded, and
  asset slots are plain arrays. They are tested, documented (TUTORIAL §17
  teaches channels/jobs as the pattern for a game's own threads), and dead.
  Keep as supported library surface, or delete. Recorded in
  plan/05-concurrency.md §3.
- **`multi_threaded` on engine systems.** The mechanism is proven (M7,
  bit-identical determinism test) but no engine system is marked. Note the
  overlay's per-system profile reads flecs' `time_spent`, which workers
  write unsynchronised — enabling both together is a TSan finding waiting
  to happen; the overlay documents it. Decide the rollout deliberately.
- **TLS / `wss://`** (N2's last bullet): mbedTLS vs system OpenSSL is a
  dependency decision. A `wss://` URL is refused loudly today, and a test
  asserts that.
- **`core/math` + `core/containers` relayering** (01-architecture): the
  matrix helpers live in the transform module, one layer above where the
  plan drew them. Moving them is churn with no behaviour change; striking
  the layer-diagram line is also fine. Either way it is a choice, not a bug.


## 2. Designs — need an API sketch first

- **Gizmos** (00 Tier 1, `engine/debug/`): what a first-time game developer
  wants drawn (axes? collider outlines? velocity arrows?) shapes the API.
  Collider-outline drawing would pair naturally with the new collision
  module.
- **Loading-screen support** (07 M4): a hook to draw a frame around a scene
  load. Small, but the hook's shape (callback? a scene the engine shows?)
  should be chosen, not defaulted.
- **A second fixed sub-phase** (new, found by the collision work): systems a
  game registers in `MyeOnFixedUpdate` run after the engine's detection
  system, so a game moving entities from its own fixed system sees
  collisions one step late. Using `MyeVelocity2D` avoids it; fixing it
  generally means splitting the fixed pipeline. Recorded in
  engine/collision/collision.h.


## 3. Limits shipped with the wave — documented behaviour, not bugs

- **Web reload restores what reflection can see**: an unreflected component
  comes back present-but-uninitialized, and anonymous `ChildOf` children do
  not survive (flecs serializes a raw parent id that means nothing to a
  fresh module). Both in plan/11-web-dev-loop.md.
- **World-space text draws over all sprites, and only for window cameras** —
  a canvas gets sprites, not text (render/text.h).
- **A canvas on a mesh is mirrored** unless the mesh's UVs were authored for
  it (render3d.h, unchanged from C4).
- **N1's browser-to-native path is unverified** — the native proof exists
  (relay + two clients, movement seen both ways); the web side needs a
  browser harness and is recorded as unverified in plan/12-networking.md.
- **`FIND_PACKAGE_ARGS` covers flecs only**: raylib cannot come from a
  system package (the allocator force-include cannot be applied to an
  IMPORTED target, and tracking would silently vanish), libwebsockets needs
  `LWS_WITH_EXTERNAL_POLL` no distro build has. Documented in
  cmake/MyeDependencies.cmake.
- **The 09-testing "every non-rendering module has a unit test" rule is
  still aspirational**: the wave added unit tests for collision, backoff,
  draw order, and module registration, but asset, scene, serialize,
  transform, log, audio, and camera are covered by integration tests only.
  Either soften the policy line or add the files.


## 4. Deliberately deferred — unchanged from the original audit

**Rendering** (03): shadows · point/spot lights · IBL · custom
shader/material system · post-processing · instanced drawing · frustum
culling · transparent-mesh sorting. **Cameras** (13): SubViewport node type
· separate projection component · follow dead zones and bounds ·
`is_position_in_frustum`. **Canvases** (14): nesting beyond one level ·
resize-in-place · UV flipping. **Memory** (04): TLSF · mimalloc ·
per-allocation file/line. **Assets** (06): async/streaming (**dropped**, by
decision) · atlas tooling. **Web** (10): WebGL 1 · pthreads. **Networking**
(12): WebRTC/WebTransport · state-sync/rollback · interest management · NAT
traversal · matchmaking. **Testing/build** (08, 09): CI (by decision) ·
golden-image pixel diff · clang-format/tidy. **Tier 3** (00, 07): particles
· physics · Lua · editor tooling.


## Also worth knowing

Not a plan item, but surfaced by the audit: the repository has
THIRD-PARTY-NOTICES.md and no LICENSE of its own.
