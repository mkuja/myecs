# 06 — Asset System

## Goals

- Gameplay code refers to assets by **handle**, never raw pointers.
- Assets load synchronously first (M2), asynchronously later (M4) without
  changing call sites.
- Scene unload releases exactly the assets that scene brought in
  (ref-counting), with leak reporting in debug.

## Handles

Generational-index handles, one type per asset kind:

```c
typedef struct { uint32_t index; uint32_t gen; } mye_texture;
typedef struct { uint32_t index; uint32_t gen; } mye_model;
typedef struct { uint32_t index; uint32_t gen; } mye_sound;
typedef struct { uint32_t index; uint32_t gen; } mye_font;
```

- `index` points into the registry's slot array; `gen` must match the slot's
  generation or the handle is **stale** (slot was reused after unload).
- Distinct struct types (not one generic handle) so the compiler catches
  passing a sound where a texture goes.
- `mye_texture_get(db, h)` returns the raylib `Texture2D*` or `NULL` for
  stale/unloaded handles; a built-in 1×1 magenta placeholder is returned by
  the render path so a missing texture is visible, not a crash.

## Registry

One registry per asset kind, held in the `MyeAssetDb` singleton:

```text
slot: { generation, state, refcount, path-hash, payload }
state: EMPTY → LOADING → LOADED → FAILED
```

- **Load** (`mye_texture_load(db, "sprites/hero.png")`): hash the path; if a
  live slot has it, bump refcount and return the existing handle (dedupe).
  Otherwise claim a slot and load.
- **Release** (`mye_texture_release(db, h)`): decrement; at zero, call the
  raylib `Unload*`, bump the slot generation (invalidating all outstanding
  handles), free CPU-side memory.
- Payload = raylib object (`Texture2D`, `Model`, `Sound`, `Font`) plus
  bookkeeping. Slot storage is a `mye_pool` ([04-memory.md](04-memory.md)).

## Scene integration

Scenes own their assets: `mye_scene_load` records every handle acquired
during scene setup (a per-scene handle list in the scene arena); scene unload
releases them all. Shared assets survive via refcount if another scene also
holds them. Debug builds report any handle still live at shutdown with the
path that loaded it.

## Sync now, async later

- **M2 (sync)**: `mye_texture_load` does raylib `LoadTexture` inline on the
  main thread. State goes straight to `LOADED`. Fine for small demos.
- **M4 (async)**: same call returns immediately with state `LOADING`; a job
  reads + decodes the file (`LoadImage` works off-thread — it's CPU/stb-based),
  sends `{handle, Image}` over the channel; the `AssetUploadSystem`
  (`EcsPreStore`, main thread) performs `LoadTextureFromImage` (GPU upload
  must be main-thread — see [05-concurrency.md](05-concurrency.md)) and flips
  the state to `LOADED`. Renderers draw the placeholder until then.
- `mye_assets_ready(db)` / per-handle state queries let a loading screen wait
  for a scene's asset set.

## Paths & formats

- Asset root: `assets/` next to the executable (raylib `SearchAndSetResourceDir`
  pattern); paths are engine-relative, forward slashes.
- Formats (all raylib-native, no extra deps): PNG for textures/atlases,
  glTF (.glb) + OBJ for models, WAV (sfx) + OGG (music), TTF for fonts.
- Atlas metadata (frame rects for sprites/animation): start with a trivial
  JSON or generated C header; tooling is Tier 3.

## Testing

- **Unit** (headless): handle generation/staleness, dedupe by path, refcount
  release order, placeholder on stale get, pool slot reuse — the registry is
  testable with a fake payload type, no raylib init needed.
- **Integration**: M4's definition of done includes a headless async-load test
  (fake decoder job + channel + drain loop → state transitions
  EMPTY→LOADING→LOADED) and a `render`-labeled smoke test loading a real PNG
  into a hidden-window world. See [09-testing.md](09-testing.md).
