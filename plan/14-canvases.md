# 14 — Canvases (render targets)

**Status: planned. Milestone C4. Written for hand-off: an implementer with no
prior context should be able to build this from here.**

## What this is, in one paragraph

Today every camera renders onto one canvas — the window — and that canvas is
implicit. This adds *explicit* canvases: an entity wrapping a raylib
`RenderTexture2D` that cameras can render **into**, and whose result is an
ordinary texture the game can draw **with** — on a sprite, on a mesh, in the
HUD. A security monitor showing another room, a minimap on a car's dashboard
model, a portal, picture-in-picture: all of these are "camera renders into a
canvas; something else displays the canvas."

The window becomes just the last canvas in the chain. Nothing about how a
single-camera game works changes.

## The model, and where it comes from

Checked against current documentation, same as [13-cameras.md](13-cameras.md).

- **Bevy**: `Camera.target` is a `RenderTarget`, either the window or an
  `Image` handle. Cameras rendering to an image run *before* cameras rendering
  to the window that displays it — Bevy sorts by target then `order`. The
  image is a normal asset usable as a material texture or a UI image.
- **Godot**: a `SubViewport` node is an off-screen render target with its own
  size and clear behaviour; it has *its own* current camera; its output is a
  `ViewportTexture` usable anywhere a texture is. Godot renders SubViewports
  before their parent viewport, recursively.

Both agree: **a canvas is a texture with a size and a clear colour; cameras
target it; it renders before whatever displays it; its output is a plain
texture.** That is what to build. We take Bevy's shape (a `target` field on
the camera, not a viewport node type) because it fits the entity-component
design already in place and needs no tree.

What we deliberately do **not** take: nested canvases beyond one level in
C4 (a canvas displayed on a mesh that is itself rendered into another
canvas). The design allows it; the first milestone does not sort for it.
State the limitation, do not build the general dependency sort yet.

## What exists today (read these first)

| Thing | Where | Relevance |
|---|---|---|
| Cameras collected per dimensionality, sorted by `order`, each drawn into its `viewport` | `engine/render/camera.c` `collect()`, `mye_camera3d_collect`, `mye_camera_viewport` | canvases add a *grouping* above this: per target, then order |
| `mye_camera_begin_3d/2d(viewport, camera, clear)` — GL viewport + scissor + projection with the rect's aspect | `engine/render/camera.c` | reused unchanged inside a canvas; the rect is then canvas-relative |
| 3D pass driver | `engine/render/render3d.c` `MyeRender3dPass` → `draw_through()` | becomes: for each canvas (then window): for each camera targeting it |
| Sprite pass driver | `engine/render/render2d.c` `MyeRenderSprites` | same restructuring; sprite list is built once and drawn per camera already |
| Frame begin/end | `render2d.c` `MyeRenderBegin` (BeginDrawing + clear), `MyeRenderEnd` (screenshot capture then EndDrawing) | canvas rendering happens between these — inside BeginDrawing |
| Draw phases | `engine/core/engine.[ch]`: `EcsOnStore` → `MyeOnCamera` → `MyeOnDraw3D` → `MyeOnDraw2D` → `MyeOnDrawUI` → `MyeOnRenderEnd` | canvases need a phase before `MyeOnDraw3D` — see below |
| Texture registry | `engine/asset/asset.c` `claim_texture_slot`, `mye_texture_release` (calls `UnloadTexture` on release) | a canvas must expose its colour attachment as an `mye_texture` **without** the registry unloading it |
| `MyeSprite.texture` is an `mye_texture`; `MyeMeshInstance` has no per-instance texture | `render2d.h`, `render3d.h` | sprite display of a canvas is free; mesh display needs one small addition |
| raylib | `LoadRenderTexture(w,h)` attaches colour **and depth** (`rtextures.c`); `BeginTextureMode` sets viewport/projection to the RT and `CORE.Window.currentFbo`; `EndTextureMode` restores the window's | 3D into a canvas works; the aspect used by our `begin_3d` is the *viewport rect's*, so nothing else needed |
| Screenshot | `MyeRenderEnd` reads the framebuffer **before** the swap | must run while the window FBO is bound, i.e. after every canvas is ended — already true if canvases render in an earlier phase |

## Design

### The canvas entity

```c
/* engine/render/canvas.h */

/* An off-screen surface that cameras render into and that anything can draw
 * with, as an ordinary texture. Sized in pixels; the size is fixed at
 * creation (resize = destroy + create). */
typedef struct MyeCanvas {
    int width, height;
    Color clear_color;   /* what the canvas is cleared to each frame */
    bool clear;          /* false: accumulate (trails, paint effects) */
    bool active;         /* false: not rendered this frame; texture keeps
                            its last contents */
    /* engine-maintained -- do not write */
    RenderTexture2D target;
    mye_texture texture; /* the colour attachment, as a registry handle */
} MyeCanvas;

ecs_entity_t mye_canvas_create(ecs_world_t *world, const char *name,
                               int width, int height, Color clear_color);
void mye_canvas_destroy(ecs_world_t *world, ecs_entity_t canvas);

/* The canvas's output as a texture handle, for MyeSprite.texture,
 * MyeMeshInstance.texture, or DrawTexture in a HUD system. Stable for the
 * canvas's lifetime. */
mye_texture mye_canvas_texture(const ecs_world_t *world, ecs_entity_t canvas);

/* raylib render textures are stored bottom-up: drawn as-is with DrawTexture
 * they appear upside down. This returns the source rect that draws it the
 * right way up -- (0, 0, w, -h) -- so callers never have to know. */
Rectangle mye_canvas_source_rect(const ecs_world_t *world, ecs_entity_t canvas);
```

The name is `MyeCanvas`, not `MyeRenderTarget`, because that is the word the
user reached for unprompted and it is the less jargon-laden of the two.
`mye_canvas_create` goes through `mye_entity_new`, so a canvas is scene-owned
and dies with its scene like every other entity; the observer below frees the
GPU object.

### Cameras gain a target

```c
/* MyeCamera3D and MyeCamera2D both gain: */
ecs_entity_t target;   /* a MyeCanvas entity, or 0 for the window */
```

`viewport` becomes **target-relative**: for a canvas, in canvas pixels; for the
window, in window pixels, as now. A zero-sized viewport still means "the whole
target". `mye_camera_viewport()` therefore needs the target's size — it takes
the camera entity already, so it looks up the target and uses `MyeCanvas`
width/height, else the window's.

### Rendering order

The rule: **every canvas renders before the window.** Within a canvas, cameras
targeting it render in `order`; then the window's cameras render in `order`,
exactly as today. This is what makes "camera A renders into canvas X; sprite
displays X in the window" show the *current* frame rather than last frame's.

Implementation: a new phase **`MyeOnDrawCanvases`**, inserted between
`MyeOnCamera` and `MyeOnDraw3D`:

```
EcsOnStore          BeginDrawing + clear (window)
MyeOnCamera         follow / orbit systems
MyeOnDrawCanvases   for each active canvas: BeginTextureMode, clear,
                    3D cameras targeting it, then 2D cameras targeting it,
                    EndTextureMode                              <-- NEW
MyeOnDraw3D         window: 3D cameras with target == 0
MyeOnDraw2D         window: 2D cameras with target == 0
MyeOnDrawUI         HUD
MyeOnRenderEnd      screenshot, EndDrawing
```

Doing canvases as one phase that internally runs 3D-then-2D per canvas keeps
each canvas self-contained (its own depth, its own clear) and means the
existing per-pass drivers only need one change: **filter cameras by target**.
Refactor `MyeRender3dPass` and `MyeRenderSprites` so their bodies become
`draw_3d_cameras_targeting(world, target)` / `draw_2d_cameras_targeting(world,
target)`; the phase systems call them with `0`; the canvas system calls them
with the canvas entity, per canvas, between `BeginTextureMode`/`EndTextureMode`.

Depth per camera: the existing rule (first camera on a target composites onto
the target's clear; later cameras clear their own viewport) applies per
target. Inside a canvas, "first" means first camera targeting *that canvas*.

**One-level only in C4.** If a canvas contains a sprite/mesh that displays
another canvas, whichever renders first sees the other's *previous* frame.
Document it in `canvas.h`; a later milestone can topologically sort canvases
by "displays" edges. Do not build that now — nothing needs it, and getting
the simple case airtight matters more.

### The canvas as a texture

`mye_canvas_create` claims a slot in the texture registry via a new internal
`mye_texture_adopt(world, name, Texture2D, bool owned)` in `asset.c`, with
`owned = false` meaning **the registry must not `UnloadTexture` it** — the
render texture owns it and `UnloadRenderTexture` frees both attachments. Add
an `owned` flag to `texture_slot`; `mye_texture_release` and `assets_fini`
skip `UnloadTexture` when it is false. This is the one change to the asset
module, and it is the kind that is easy to get wrong silently (a double free
of a GL texture is a "the other texture went black" bug), so it gets its own
test.

Displaying a canvas:

- **On a sprite**: `MyeSprite.texture = mye_canvas_texture(...)` and
  `MyeSprite.source = mye_canvas_source_rect(...)` (the vertical flip). Nothing
  else — the sprite pass already draws any `mye_texture`.
- **On a mesh**: `MyeMeshInstance` gains `mye_texture texture;` (0 = use the
  model's own material). In `draw_through`, if set, `material.maps[MATERIAL_MAP_ALBEDO].texture = *mye_texture_get(...)`.
  This is a small, general feature (per-instance albedo override) that the
  canvas happens to need. Note that mesh UVs are not flipped by a source rect;
  document that a canvas on a mesh appears flipped unless the mesh's UVs are
  authored for it, and provide `mye_canvas_flip_uvs(Mesh*)`? **No** — keep C4
  small: document the flip, do not fix it. Sprites and HUD are the first-class
  display paths; meshes are "works, mind the flip".
- **In the HUD**: `DrawTextureRec(*mye_texture_get(world, tex), mye_canvas_source_rect(...), pos, WHITE)`.

### Screen/world helpers

`mye_world_to_screen`, `mye_screen_ray` etc. use the *lowest-order active
camera*. With canvases, that should mean the lowest-order active camera
**targeting the window** — a minimap camera rendering into a canvas must not
become "the main view" for picking. `mye_camera3d_active` therefore filters
`target == 0`. Explicit-camera forms (`mye_world_to_screen_for(world, camera, point)`)
from C3 use whichever camera they are given, and for a canvas-targeting
camera the result is in **canvas pixels** — say so.

`mye_camera_at_screen` considers window-targeting cameras only.

### Lifecycle

- `mye_canvas_create`: `LoadRenderTexture`; if `.id == 0` (headless, or GL
  failure) still create the entity with `target.id = 0` and a zero handle, so
  code paths stay testable headless — the same rule the asset registry
  follows.
- `EcsOnRemove MyeCanvas` observer: `UnloadRenderTexture` if `.id != 0`, and
  release the registry slot (which, being un-owned, does not double-free).
- Cameras whose `target` is a dead or non-canvas entity: treated as
  window-targeting, **with a one-shot warning** — this *is* malformed data
  (the target does not exist), unlike "several cameras" which is a choice.
- Resize: not supported in C4. `mye_canvas_destroy` + `mye_canvas_create`.

### Web

`LoadRenderTexture` works under GLES 3.0 / WebGL 2 (`rlLoadTextureDepth`
uses a renderbuffer). Nothing platform-specific to do; verify with the
console-log method, never a screenshot (see `plan/WEB-LOOP-STALL.md`).

## Milestone C4 — deliverables, in build order

1. **`engine/render/canvas.[ch]`** — `MyeCanvas`, `MyeCanvasModuleImport`,
   create/destroy/texture/source_rect, the remove observer, the
   `MyeOnDrawCanvases` system. Import in `engine.c` **after** the camera
   module (its system calls both renderers' pass functions and the camera
   module's collect). Add to `engine/CMakeLists.txt`.
2. **`engine/asset/asset.[ch]`** — `owned` flag on slots; internal
   `mye_texture_adopt`; release/fini skip unload when un-owned.
3. **`engine/core/engine.[ch]`** — `MyeOnDrawCanvases` phase in the chain,
   exported, documented in the phase comment.
4. **`camera.[ch]`** — `target` on both camera components; `collect()` gains a
   `target` filter parameter (public: `mye_camera3d_collect_for(world, target,
   out, max)`, with the existing `_collect` meaning `_collect_for(…, 0, …)`);
   `mye_camera_viewport` resolves size from the target; `_active` and
   `_at_screen` filter to window cameras.
5. **`render3d.c` / `render2d.c`** — pass bodies refactored into
   `draw_*_cameras_targeting(world, target)`; phase systems call with 0.
   `MyeMeshInstance.texture` override in `draw_through`.
6. **Example**: `examples/05_showcase` — replace the corner-viewport minimap
   with a canvas minimap displayed as a **sprite in the HUD corner with a
   border**, and add a small "monitor" quad in the 3D scene displaying the
   same canvas (the mesh path, flip acknowledged). Two display paths, one
   canvas, one camera.
7. **Tests** (below), **docs**: `TUTORIAL.md` §8 gains "Canvases"; `plan/03-rendering.md`
   cameras section gains a paragraph; `README.md` module table.

## Tests

### Headless — `tests/integration/test_int_canvas.c` (new)

Everything below runs with `.headless = true`; `LoadRenderTexture` returns id 0
there and the entity still exists.

1. `a_canvas_is_an_entity_with_a_texture_handle` — create; `mye_canvas_texture`
   returns a valid handle (headless: valid but `id 0`, like generated textures);
   `mye_canvas_source_rect` is `(0,0,w,-h)`.
2. `a_camera_can_target_a_canvas_and_is_collected_under_it` — two cameras, one
   with `target = canvas`; `mye_camera3d_collect_for(world, canvas, …)` returns
   exactly that one; `_collect_for(world, 0, …)` returns exactly the other.
3. `a_canvas_targeting_camera_s_viewport_is_in_canvas_pixels` — 256×256 canvas,
   camera with zero viewport → `mye_camera_viewport` is `(0,0,256,256)`, not the
   window.
4. `the_active_camera_for_picking_ignores_canvas_cameras` — a canvas camera
   with `order = -1` (lowest) must NOT be what `mye_camera3d_active` returns.
5. `destroying_a_canvas_does_not_double_free_its_texture` — create, take the
   handle, destroy, then `mye_shutdown` returns 0 (leak/double-free clean under
   ASan). This is the `owned=false` guard.
6. `a_camera_whose_target_dies_falls_back_to_the_window_and_warns_once` —
   delete the canvas; the camera is collected under target 0; exactly one
   warning across five frames.
7. `an_inactive_canvas_is_skipped_but_its_camera_still_resolves`.

### Render-labelled — extend `tests/integration/test_int_render_cameras.c`

8. `a_canvas_shows_what_its_camera_saw_and_the_window_shows_the_canvas` — the
   red-ball pattern again: ball at +100 X visible only to a camera that
   targets a 128×128 canvas; a sprite in the window at a known rect displays
   the canvas; the window's own camera looks at nothing. Screenshot (via the
   engine's before-swap capture): red **inside the sprite's rect**, none
   elsewhere. Mutation checks to run and record: (a) canvases rendered
   *after* the window → sprite shows last frame → on frame 1 the rect is
   empty (make the test's frame count 1 to expose this); (b) drop
   `EndTextureMode` restore → HUD/sprite pass draws into the canvas → window
   rect empty; (c) drop the vertical flip in `source_rect` → red lands
   mirrored; assert red is in the *top* half of the rect where the ball is
   placed high in the canvas camera's view.
9. `the_screenshot_is_of_the_window_not_the_last_canvas` — with a canvas
   present, `MYE_SCREENSHOT` output has the window's dimensions and content.

## Traps for the implementer (each has bitten this codebase already)

- **Read pixels before the swap.** `LoadImageFromScreen` after `EndDrawing`
  returns the previous frame. The engine's capture in `MyeRenderEnd` is the
  only correct place; tests read via `MYE_SCREENSHOT`/`engine->screenshot_path`.
- **A test that passes on the first try is suspect.** Break the thing and
  watch the test fail before trusting it. The three mutations above are the
  minimum.
- **Cameras are ordinary entities.** Any system querying `MyeRotation3D` /
  `MyePosition3D` without a tag now touches cameras. The showcase's spin
  system did exactly this and aimed the minimap at the horizon; if a canvas
  camera "sees nothing", check what else writes its transform before
  suspecting the render path.
- **Generated meshes are black under PBR** (`fragColor` multiplies albedo and
  `GenMesh*` has no vertex colours). Test scenes should set `use_pbr = false`
  or use a textured/coloured model, or you will conclude "nothing renders".
- **Import order.** A module that builds queries over another module's
  components must import that module first (`ECS_IMPORT` inside its own
  import function is the robust way — see `MyeCameraModuleImport`).
- **Scissor and viewport must be restored** after each camera; canvases add
  `EndTextureMode` on top, and `EndTextureMode` resets the viewport to the
  *window* — so the sequence per canvas is: `BeginTextureMode` → cameras
  (each `mye_camera_begin/end`) → `EndTextureMode`. Never leave the window's
  passes running with a canvas FBO bound; the symptom is "the HUD vanished".
- **raylib render textures are upside down** when drawn as textures. Use
  `mye_canvas_source_rect`; the test in (8c) exists so nobody "fixes" it by
  flipping the camera instead.
- **Warn on malformed data, never on a game's choice** (from `13-cameras.md`
  and the follow-target discussion). A dead canvas target is malformed;
  several canvases, or a canvas nobody displays, are not.

## Definition of done

- Every test above green under Debug (ASan/UBSan), Release, and TSan
  (`tools/check.sh`), and the render-labelled ones green with a display.
- The three mutations in test 8 each fail exactly as described, and this is
  recorded in the commit message.
- Showcase: minimap as a HUD sprite (bordered) *and* on a monitor mesh, one
  canvas, one camera; screenshot confirms both.
- Web build of the showcase renders the canvas minimap — verified by a
  console-logged pixel probe or by `mye_texture_valid` on the canvas texture
  in a long-lived headless session, **not** by screenshot mode.
- `TUTORIAL.md` §8 explains canvases in the same register as the rest:
  what, why, one working example (`check_tutorial.py` compiles it).
- Fable review before merge, with tests 5 and 8 named as the things to attack.
