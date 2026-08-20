# 13 — Cameras

How cameras should work, taking the parts of Bevy's and Godot's designs that
earn their keep. Supersedes the ad-hoc `MyeRenderConfig.camera` field added in
`6cff945`, which was a stopgap for a renderer that could only draw one view.

## What the two engines actually do

Checked against current documentation, not memory.

**Bevy.** A camera is an entity with a `Transform` and a `Camera` component
whose fields are `target` (window or image), `viewport` (optional sub-rect of
that target), `order`, and `is_active`. **There is no "the active camera".**
All active cameras render, sorted by `order`; higher `order` draws on top.
A minimap is a second camera with `viewport = Some(Viewport { physical_position,
physical_size })`. Projection is a separate component (`PerspectiveProjection`
with `fov`, or `OrthographicProjection` with `scale`). `RenderLayers` (0–31) is
a bitmask intersected between camera and entity to decide what each sees.

**Godot.** `Camera3D` is a node in the tree (so, parented) with `fov`,
`projection` (perspective / orthogonal / frustum), `near`, `far`, and
`cull_mask`. **Exactly one camera is current per `Viewport`** — "Only one
camera can be active per viewport", and setting one `current` unsets the
other. Several simultaneous views are several `SubViewport` nodes, each
compositing into the UI. Helpers: `unproject_position`, `project_ray_origin` /
`project_ray_normal`, `is_position_in_frustum`.

`Camera2D` additionally ships follow behaviour the engine performs:
`position_smoothing_enabled` / `_speed`, `drag_*_margin` dead zones, and
`limit_*` bounds. Notably the docs warn that with smoothing on, **"the
Camera2D node's position doesn't represent the actual position of the
screen"** — hence `get_screen_center_position()`.

### What to take, and what not to

| Idea | Verdict |
|---|---|
| Camera is an entity/node with a transform, parentable | **Already have it.** Both engines agree; keep. |
| **All active cameras render; `order` + `viewport`** (Bevy) | **Take.** This is the answer to "how many cameras may I have" — the engine never picks, so the question never arises. |
| One-current-per-viewport + SubViewports (Godot) | **Leave.** Same capability, more machinery: a viewport node type, compositing, a tree. Bevy's rect-on-the-camera gets a minimap in one component. |
| Follow smoothing in the engine (Godot 2D) | **Already have it** as `MyeCameraFollow`, which also works in 3D. Godot's own docs show the cost of doing it Godot's way: the node position stops meaning what it says. Ours keeps position honest and blends at draw time via `MyeRenderTransform` — the same split the fixed timestep already uses. |
| Dead zones (`drag_margin`) and bounds (`limit_*`) | **Take later**, as fields on `MyeCameraFollow` / a `MyeCameraBounds`. Both are real game needs; neither is needed to make multi-camera correct. |
| Layer masks (`RenderLayers` / `cull_mask`) | **Take**, minimally. A minimap that shows the same scene at a different zoom is fine without it, but "the minimap shows blips, not the world" needs it, and it is one `uint32_t` and one bit test. |
| Projection as a separate component (Bevy) | **Leave.** `MyeCamera3D` already carries `fov` + `projection`; splitting it buys nothing at this size. |
| `near` / `far` | **Take.** Godot exposes them, raylib hardcodes 0.01/1000 in `rlgl`, and a large 3D scene needs them. Cheap: two floats used when building the projection. |
| Render-to-texture targets (Bevy's `target`) | **Leave for now.** Needed for portals and mirrors, not for split screen or minimaps. The `viewport` design does not block it. |

## The design

**Every active camera draws, in `order`, into its `viewport`.** No camera is
special; nothing is chosen; there is nothing to warn about. One camera is the
degenerate case of that rule, and behaves exactly as it does today.

### Components

```c
typedef struct MyeCamera3D {
    float fov;          /* vertical degrees; perspective only. 0 -> 60 */
    int projection;     /* CAMERA_PERSPECTIVE | CAMERA_ORTHOGRAPHIC */
    float near_plane;   /* 0 -> raylib's default */
    float far_plane;
    bool active;

    /* Where on the window this camera draws, in pixels. A zero-width rect
     * means the whole window, which is what a single-camera game wants and
     * never has to think about. */
    Rectangle viewport;

    /* Draw order among cameras on the same window; higher draws later, so
     * a minimap with order 1 lands on top of a world view with order 0. */
    int order;

    /* Bitmask intersected with MyeVisibilityLayers on each entity: an entity
     * is drawn by this camera if any bit matches. 0 -> all layers. */
    uint32_t layers;
} MyeCamera3D;
```

`MyeCamera2D` gains the same `viewport`, `order`, `layers` beside its existing
`zoom` and `offset`.

```c
/* Optional, on any drawable entity. Absent means EVERY layer, so an entity
 * that says nothing about layers is drawn by every camera. */
typedef struct MyeVisibilityLayers { uint32_t mask; } MyeVisibilityLayers;
```

Both defaults are permissive, and deliberately: a camera that names no layers
sees everything, an entity that names none is on everything, so neither can be
made invisible by a field it never set. The alternative default — absent means
layer 0 — would make a camera with `layers = 2` render an empty screen in an
existing game, which reads as a broken renderer. The cost is that "the minimap
shows blips, not the world" needs the world labelled too; layers are a
labelling scheme, and the engine labels nothing on the game's behalf.

`MyeRenderConfig.camera` and `MyeRender3dConfig.camera` are **deleted**.

### The passes

Both `MyeRender3dPass` and `MyeRenderSprites` become:

```
collect active cameras (of this dimensionality) into the frame arena
sort by order                       /* stable; ties keep entity order */
for each camera:
    resolve it (existing mye_camera*_resolve)
    rlViewport + scissor to its viewport rect
    BeginMode2D/3D
      draw the entities whose layers intersect this camera's
    EndMode
restore the full-window viewport and scissor
```

Two things this must get right, both of which are how it can go subtly wrong:

- **Scissor as well as viewport.** `rlViewport` alone changes the projection
  but does not stop a clear or a large sprite bleeding outside the rect.
- **Restore afterwards**, or the HUD in `MyeOnDrawUI` inherits the last
  camera's viewport — a minimap-sized HUD in the corner, which looks like a
  layout bug rather than a camera bug.

`mye_camera3d_active()` / `mye_camera2d_active()` keep their meaning — the
lowest-`order` active camera on the window — and that is what world→screen
uses, since there is no point to ask. Screen→world is the other way round: the
POINT picks the camera, through `mye_camera_at_screen`, which answers with the
TOPMOST camera over that pixel (the one the player thinks they clicked on).
Mouse picking in a split screen must resolve against the viewport the cursor
is in, or clicks in player 2's half are interpreted in player 1's world. A
pixel inside no viewport falls back to the active camera, which is the only
answer available and is what a single-camera game has always got.

### Helpers

```c
/* Which camera's viewport a screen point falls in; 0 if none. Split-screen
 * picking needs this, and getting it wrong is invisible until two players
 * are on screen. */
ecs_entity_t mye_camera_at_screen(const ecs_world_t *, Vector2 screen);

/* Existing helpers gain an explicit-camera form, so a game with several
 * cameras is not forced through "the active one". */
Vector2 mye_world_to_screen_for(const ecs_world_t *, ecs_entity_t cam, Vector3);
Ray     mye_screen_ray_for(const ecs_world_t *, ecs_entity_t cam, Vector2);

/* The layer rule in one place, so a game drawing in a system of its own can
 * obey the same one the passes do -- and so it is checkable without a
 * window, which is where masks are easiest to get wrong. */
bool mye_camera_sees(const ecs_world_t *, ecs_entity_t cam, ecs_entity_t);
```

Godot's `is_position_in_frustum` is deliberately **not** added: it is the thin
end of frustum culling, which is out of scope by decision.

## Milestones

**C1 — multi-camera. BUILT** (`5829245`, `26673a2`). `viewport`, `order`, both passes looping, config field
deleted, scissor restored. *Done when:* a test asserts two cameras with
different viewports both draw, ordered; and the examples still look identical
because one camera is the degenerate case.

**C2 — layers. BUILT.** `MyeVisibilityLayers` (owned by the 2D module, like
`MyeHidden`, because both renderers draw), `layers` on both camera components,
the intersection test in both passes, and `mye_camera_sees` stating the rule
once. The sprite pass resolves each entity's mask into the draw list it
already builds per frame; the mesh pass asks per entity, and only when the
camera named layers at all, so a camera with `layers == 0` costs nothing.
*Done:* two pixel tests, one per pass, draw a single object through two
cameras that differ in nothing but their mask; only the sharing camera shows
it, and the other's half of the window has not one pixel of it.

**C3 — near/far and the ergonomics. BUILT.** `near_plane`/`far_plane` on
`MyeCamera3D`, used when `mye_camera_begin_3d` builds the projection (0 keeps
raylib's global cull distances, and an inverted pair falls back to them rather
than render nothing). `mye_camera_at_screen` came with C1; the explicit forms
`mye_world_to_screen_for` / `mye_screen_ray_for` are new, and — the part that
was a correctness trap rather than an ergonomic one — all four existing
screen↔world helpers now account for the camera's viewport rect, offset and
aspect both, and screen→world resolves the camera the pixel is actually in.
Not carried into the helpers: the clipping planes, because neither a screen
position nor a ray direction depends on where they sit.

**Deferred, with reasons:** render-to-texture targets -- now planned in full
as [14-canvases.md](14-canvases.md) (C4), dead zones and bounds on follow (real, but orthogonal), frustum
culling (out of scope by decision), a viewport/node abstraction (Godot's
answer to a question Bevy's design does not ask).

## Testing

Headless throughout, since none of this needs a window: viewport arithmetic,
ordering, and layer masks are all pure data. The one thing that does need GL —
that `rlViewport` and the scissor actually confine drawing — belongs in
`test_int_render_smoke.c` (labelled `render`), which now drives the 3D pass.

Specific cases worth naming because they are the ways this breaks:

- two cameras, different viewports, both draw; swapping `order` swaps which is
  on top
- a camera with a zero-size viewport means the whole window (the single-camera
  path)
- after a frame, the viewport and scissor are back to the full window, so the
  HUD is not clipped to the last camera's rect
- `mye_camera_at_screen` returns the right camera for a point in each half of
  a split screen, and 0 outside both
- an entity with no `MyeVisibilityLayers` is visible to a camera with the
  default mask (nothing existing changes)
- `mye_screen_ray_for` against a viewport-restricted camera accounts for the
  offset — a ray cast from a minimap pixel must not be interpreted as a
  main-view pixel

All six exist. The pure ones are in `tests/integration/test_int_camera.c`; the
ones that need a real framebuffer live in `test_int_render_cameras.c`
(labelled `render`, alongside the C1 and C4 pixel tests rather than in
`test_int_render_smoke.c` as first sketched): that each pass really applies
the mask, that the clipping planes really clip, and that the viewport and
scissor are back before the HUD draws.

## Sources

- Bevy `Camera`: <https://docs.rs/bevy/latest/bevy/camera/struct.Camera.html>
- Bevy cameras (cheatbook): <https://bevy-cheatbook.github.io/graphics/camera.html>
- Godot `Camera3D`: <https://docs.godotengine.org/en/stable/classes/class_camera3d.html>
- Godot `Camera2D`: <https://docs.godotengine.org/en/stable/classes/class_camera2d.html>
