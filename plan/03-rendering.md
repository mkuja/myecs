# 03 — Rendering (2D & 3D on raylib)

## The one hard rule

**All rendering happens on the main thread.** raylib's OpenGL context is bound
to the thread that called `InitWindow()`; GL calls from other threads are
undefined behavior. Therefore:

- Every draw call lives in systems registered in the `EcsOnStore` phase, which
  the engine guarantees runs single-threaded on the main thread
  (flecs runs non-`multi_threaded` systems on the main thread).
- Asset GPU uploads (`LoadTextureFromImage`, `UploadMesh`) are main-thread
  only as well — the async loader ships decoded bytes to the main thread over
  a channel ([05-concurrency.md](05-concurrency.md), [06-assets.md](06-assets.md)).

## Frame structure

One flecs frame renders like this (all in `EcsPreStore`/`EcsOnStore`):

```text
EcsPreStore   RenderPrepare      collect visible sprites/meshes into frame
                                 arrays (frame allocator), sort them
EcsOnStore    RenderBegin        BeginDrawing(); ClearBackground(scene color)
              Render3DPass       if a MyeCamera3D is active:
                                   BeginMode3D … draw MyeMeshInstance … EndMode3D
              Render2DPass       if a MyeCamera2D is active:
                                   BeginMode2D … draw sorted MyeSprite … EndMode2D
              RenderUIPass       screen-space 2D (no camera): UI, text
              RenderDebugPass    debug overlay, gizmos (debug builds)
              RenderEnd          EndDrawing()   ← swaps buffers, pumps input
```

System ordering within a phase is controlled by registration order (and can be
pinned with explicit `DependsOn` edges if needed). `RenderBegin`/`RenderEnd`
are engine systems; passes in between are engine modules the game can extend
by registering its own draw systems ordered between them.

This layout gives **mixed scenes for free**: a 3D world with a 2D HUD is just
both passes active in the same frame.

## 2D pipeline

- **Sprite drawing**: `RenderPrepare` runs a query
  `[in] MyeSprite, [in] MyeWorldTransform, !MyeHidden`, writes compact
  draw records into a frame-allocator array, then sorts by
  `(layer, y, texture)` — layer for explicit ordering, y for top-down depth
  illusion, texture last to help raylib's internal batching.
- **Batching**: raylib batches consecutive draws with the same texture
  automatically (rlgl). Using **texture atlases** (one texture, many
  `source` rects) keeps the whole sprite pass in a handful of draw calls.
- **Camera2D**: the active `MyeCamera2D` entity provides
  offset/target/rotation/zoom; camera-follow logic is a `EcsPostUpdate`
  system writing `cam.target` from a followed entity's transform.
- **Text**: raylib `DrawTextEx` with loaded `Font` assets; UI text renders in
  `RenderUIPass` (screen space).
- **Interpolation** (built; see `MyeInterpolate`): with a fixed timestep the
  simulation advances in 1/60 s jumps while the display may refresh at 144 Hz,
  so most drawn frames fall *between* two simulated states. Drawing the most
  recent one makes motion judder. `MyeTime.alpha` -- already computed and
  tested -- is the blend factor between the previous and current step, and the
  sprite pass would lerp with it while building the draw list.

  **Opt-in, per entity, by decision:**

  ```c
  ecs_set(world, ship, MyeInterpolate, { 0 });   /* this entity is smoothed */
  ```

  Rationale: with interpolation on by default, an entity's rendered position
  would stop matching its `MyePosition2D` value, and someone comparing the
  component in the flecs Explorer against the screen would find a discrepancy
  with no visible cause. An engine that is worth reading should not diverge
  data from pixels behind the developer's back. This is the same call made in
  M6, where flecs' implicit `ecs_set_with` capture was dropped for explicit
  tagging in `mye_entity_new`.

  Consequences:

  - No world-level flag and no engine-wide mode. A game that never opts in
    pays nothing: the driver only stores previous transforms for entities
    that asked.
  - The interpolated value is **renderer-internal** -- computed into the draw
    list, never stored as a component -- so nothing else can read it by
    accident.
  - `mye_transform_snap(world, entity)` suppresses the blend for one frame.
    Needed for teleports: interpolating a screen wrap from x=1270 to x=10
    smears the sprite across the whole screen. Asteroids' `wrap_position()`
    would call it. A permanent `MyeNoInterpolate` is the wrong tool there --
    the ship should be smoothed on every frame except the one it wraps.
  - Discoverability is the **debug overlay's** job, not the engine's:
    reporting `alpha 0.37 - interpolated 0/412 entities` (which it now does) shows the feature
    exists without the engine deciding to use it for you.

  Interpolation also renders one step in the past, trading ~16 ms of latency
  for smoothness. Extrapolating forward instead avoids the latency but
  overshoots and snaps back whenever something changes direction.

## 3D pipeline

- **Meshes/models**: `MyeMeshInstance` references a model asset handle; the
  pass draws with `DrawModel`/`DrawMesh` using `MyeWorldTransform.m` as the
  transform matrix.
- **Camera3D**: active `MyeCamera3D` entity; fly/orbit camera controllers are
  ordinary `EcsOnUpdate` systems the engine ships as optional module systems.
- **Lighting**: start with raylib's example lighting shader (`rlights.h`
  pattern): a small forward shader with N point/directional lights read from
  `MyeLight` components. Good enough through M5; custom shaders are Tier 3.
- **Sorting/culling**: opaque meshes drawn front-to-back is a non-goal early
  (raylib does depth testing); transparent meshes sorted back-to-front when
  the need arises. Frustum culling deferred until profiling says so.

## Lighting, and the trap in it

The engine carries its own shader (raylib ships `rlights.h` under `examples/`,
not in the library). It is Blinn-Phong: three terms added together.

```
final = albedo x (ambient + SUM over lights of (lightColour x max(dot(N, L), 0)))
```

- **albedo** -- a property of the *surface*: what fraction of each colour
  channel it reflects. Red brick is about (0.6, 0.1, 0.1). Unchanged whether
  the scene is dark or blazing. Called "diffuse colour" in older engines and
  "base colour" in glTF.
- **irradiance** (`ambient + lightAccum`) -- all light *arriving*. Changes as
  lights move.
- Lights only ever **add**. There is no subtraction, which is why a shadow is
  not a lighting term at all but the separate problem of deciding whether the
  path to a light is blocked.
- Because albedo multiplies per channel, a red object under blue light is
  black. Physically right, and a frequent source of "my model looks broken".

**The trap: light maths is only correct in linear space.** Colours are stored
in sRGB, where 128 is not half as bright as 255. The first version of this
shader lit sRGB values and then applied the sRGB conversion -- the output half
only -- which brightens everything and flattens the contrast. Convert in,
light, convert out:

```glsl
vec3 albedo = pow(base.rgb, vec3(2.2));      // sRGB -> linear
vec3 lit = albedo*(ambientLin + lightAccum); // maths in linear
finalColor = vec4(pow(lit, vec3(1.0/2.2)), base.a);  // linear -> sRGB
```

**PBR is now implemented** alongside Blinn-Phong, selected with
`MyeRender3dConfig.use_pbr` (default on). Cook-Torrance: GGX distribution,
Smith geometry, Schlick Fresnel, plus Reinhard tone mapping -- because lights
only add, and without it a bright scene clips to flat white. It reads the
metallic-roughness, normal and emissive maps that glTF actually carries, so a
downloaded model looks as its author intended.

raylib binds material map N to texture slot N and writes that slot into
`shader.locs[SHADER_LOC_MAP_*]`, so the sampler names are wired up explicitly
rather than by convention (see `resolve_pbr_uniforms`). glTF packs roughness
in the green channel and metallic in blue of one texture.

**Skeletal animation is implemented** via `MyeModelAnimator`. raylib 6.0 takes
a *fractional* frame and interpolates between keyframes. One limitation worth
knowing: raylib stores the pose **in the Model**, not per instance, so two
entities sharing a model handle cannot hold different poses.

Not implemented, in rough order of value: **shadows** (shadow mapping, a
milestone of its own -- everything still floats), **point/spot lights**
(position plus distance falloff, about an hour), and **image-based lighting**
(an environment map, which is what makes PBR metals really sing).

## Cameras

A camera is an entity in the transform hierarchy. Its position and rotation
come from `MyePosition`/`MyeRotation` and its parents; `MyeCamera2D` and
`MyeCamera3D` carry only what a camera *is* -- field of view, projection,
zoom, and which one is active.

That single decision is what makes the two things games want fall out of
machinery that already exists: **parent a camera** and it is rigidly carried,
turning as well as moving (first-person, turret sight, rollercoaster);
**add `MyeCameraFollow`** and it chases a target with framerate-independent
lag.

**Orientation is a rotation, not a stored target point.** A target is a point
in some coordinate space, and once a camera is parented that space is
ambiguous -- the same target would mean two different directions depending on
where the parent is. `mye_camera_look_at()` provides the convenient form: it
takes a world point, converts it into the camera's own space, and writes the
rotation. Skip that conversion and a parented camera aims using two different
coordinate systems, which points it at nothing; there is a test for exactly
that (`look_at_is_correct_for_a_parented_camera`).

**The view is resolved on demand, never stored.** `mye_camera3d_resolve()` is
a pure function of the camera's components and its parent's render transform.
The renderer and the screen/world helpers both call it, so they cannot
disagree, and there is no cached matrix to go stale.

### Ordering: the MyeOnCamera phase

Camera logic runs in its own phase, between `EcsOnStore` and `MyeOnDraw3D`:

```text
EcsPostUpdate   transforms propagated
EcsPreStore     render transforms blended (interpolation)
MyeOnCamera     follow / orbit / shake systems  <-- here
MyeOnDraw3D     the 3D pass reads the resolved camera
```

flecs orders systems by phase, not by hierarchy -- parenting a camera to a
player does *not* make a follow system run after the player moves. The phase
is what makes "positions first, then the camera" a guarantee rather than an
accident of registration order.

It also means a follow camera reads its target's **drawn** position
(`mye_render_position`), not the simulated one. Mid-step those differ, and
following the simulated position makes the world shimmer against a player who
is perfectly smooth.

### The one caveat

A camera moved during `MyeOnCamera` is moved *after* propagation ran, so its
own world matrix is a frame stale until the next frame. Nothing that draws is
affected, because resolving recomputes from components -- but anything
parented *to* such a camera lags a frame. Parent cameras to things; do not
parent things to a following camera.

## Canvases

A camera does not have to draw on the window. `MyeCamera*.target` names a
**canvas** — an off-screen surface (`MyeCanvas`, `render/canvas.h`) whose
result is an ordinary texture, usable on a sprite, on a mesh
(`MyeMeshInstance.texture`), or in a HUD system. `target == 0` means the
window, so a game that never mentions canvases takes exactly the path it did
before.

The ordering is the substance of the feature. Canvases render in
`MyeOnDrawCanvases`, a phase between `MyeOnCamera` and `MyeOnDraw3D`, so
everything a canvas contains is finished before anything that displays it
starts drawing. Get that backwards and the picture still looks plausible --
it is simply one frame old, which is invisible until something moves fast.

Two details that are easy to get wrong and silent when wrong:

- Viewport flipping must use the **current** framebuffer's height
  (`rlGetFramebufferHeight`), not the window's. Inside a canvas they differ,
  and flipping against the window puts a camera's viewport off the edge of a
  smaller canvas -- it draws nothing, with no error anywhere.
- raylib is asymmetric about that height: `BeginTextureMode` records the
  canvas's size with `rlSetFramebufferHeight` and `EndTextureMode` never
  restores it. The canvas pass restores it, or every window camera for the
  rest of the frame is flipped against the canvas and the whole window's
  output slides down the screen.

The canvas's colour attachment is registered with the asset database as an
**unowned** texture, so a handle to it behaves like any other texture handle
while the canvas -- not the asset system -- remains responsible for freeing
the GPU surface. Releasing a canvas texture must not `UnloadTexture` it, or
the canvas is destroyed twice. Note this is one of the few things here with
no test that can fail: deleting a GL name twice is a no-op by specification
and invisible to the sanitizers, which see only CPU memory.

Adoption deliberately does **not** dedupe by name the way `mye_texture_load`
does. For a file, the same path really is the same pixels; for an adopted GPU
object it would hand the caller somebody else's texture -- the canvas
displays an unrelated picture, and two independent lifetimes end up behind
one refcount, leaving a handle the registry calls valid after the real owner
has freed it. A name collision is refused and logged.

A camera whose canvas has been **destroyed** is ordinary: the game's
component still names a dead entity, because the engine does not rewrite what
the game wrote. Every path that reads a camera's `target` therefore checks
`ecs_is_alive` before `ecs_has` -- flecs aborts on the latter for a dead
entity in debug builds and dereferences NULL in release, so the documented
"falls back to the window" behaviour is a crash the moment it is not
guarded.

**A surface cannot appear in its own feed.** The camera that fills a canvas
draws the whole scene, which includes any mesh or sprite textured with that
canvas -- sampling the framebuffer being written. GL calls this a feedback
loop and leaves the pixels undefined; WebGL logs `GL_INVALID_OPERATION` per
draw call (the showcase produced 504 of them in forty seconds before this was
handled). Both passes therefore skip a surface whose texture is the canvas
currently being drawn. It is the one place the engine overrides what the game
asked for, and it is justified because the alternative is not a different
picture but an undefined one. A layer mask (C2) would let a game exclude more
than this minimum; this rule is what makes the mesh path correct without it.

Deliberately not in scope: canvases that display other canvases (one level
only), resizing a canvas in place (destroy and recreate), and flipping a
mesh's UVs so a canvas on a mesh is not mirrored.

### The material-aliasing trap

`MyeMeshInstance.texture` exposed an older bug worth recording, because the
same shape will recur for any future per-instance material state. raylib's
`Material` holds `maps` as a **pointer**, so

```c
Material material = model->materials[model->meshMaterial[m]];  /* aliases! */
```

copies the struct but shares the map array with the model every other entity
draws with. The 3D pass wrote both the per-instance tint and (briefly) the
per-instance texture straight into it. Two consequences, neither of which
raises anything:

- the tint **compounded**, because the base colour was re-read from the
  already-modified shared value each frame -- a 50% grey tint halves the
  model's colour sixty times a second until it is black. A `WHITE` tint is
  idempotent, which is the only reason this survived unnoticed;
- a per-instance texture appeared on **every** entity sharing the model.

The pass now saves the diffuse map's colour and texture, draws, and puts them
back. A local copy of the array would be the tidier fix but is not available:
`DrawMesh` indexes every slot up to `MAX_MATERIAL_MAPS`, which is private to
raylib's `rmodels.c`, so the length would be a guess. Both halves are pinned
by pixel tests (`test_int_render_cameras.c`).

## What we deliberately postpone

- Custom shader/material system (Tier 3) — raylib `Material` + default shader
  until then.
- Post-processing — a canvas is the mechanism; a full-screen effect pass over
  one is not scheduled.
- Instanced drawing (`DrawMeshInstanced`) — only if a demo needs thousands of
  identical meshes.

## Testing

Render code is smoke-tested, not unit-tested: integration tests behind the
`render` CTest label open a hidden window (`SetConfigFlags(FLAG_WINDOW_HIDDEN)`),
render a few frames, and assert no crash; optional `TakeScreenshot()` goldens
for manual comparison. Sorting and visibility *logic* (pure functions over
arrays) is unit-testable headlessly and should be written as pure functions
for that reason. See [09-testing.md](09-testing.md).
