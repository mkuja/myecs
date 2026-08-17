/* Handle-based asset registry. See plan/06-assets.md.
 *
 * Gameplay code holds handles, never pointers. A handle stays *checkable*
 * after the asset is unloaded: the slot's generation counter changes, so an
 * old handle resolves to NULL (or to the placeholder texture) instead of to
 * whatever was loaded into that slot next.
 *
 *   mye_texture tex = mye_texture_load(world, "sprites/ship.png");
 *   const Texture2D *t = mye_texture_get(world, tex);   // NULL if stale
 *
 * Loading is synchronous, and deliberately so: assets are loaded at scene
 * boundaries (see scene/scene.h), where a pause is what a loading screen is
 * for, and the GPU upload has to happen on the main thread regardless. A
 * game that later needs to stream during play should slice loads across
 * frames rather than reach for a thread. */
#ifndef MYE_ASSET_ASSET_H
#define MYE_ASSET_ASSET_H

#include "core/engine.h"

#include <raylib.h>

typedef struct mye_texture {
    uint32_t index;
    uint32_t generation;
} mye_texture;

typedef struct mye_sound {
    uint32_t index;
    uint32_t generation;
} mye_sound;

typedef struct mye_model {
    uint32_t index;
    uint32_t generation;
} mye_model;

typedef struct mye_asset_db mye_asset_db;

typedef struct MyeAssets {
    mye_asset_db *db;
} MyeAssets;

extern ECS_COMPONENT_DECLARE(MyeAssets);

void MyeAssetsModuleImport(ecs_world_t *world);

/* ------------------------------------------------------------- textures -- */

/* Loads from disk, or returns the existing handle and bumps its refcount if
 * the same path is already loaded. Returns an invalid handle on failure. */
mye_texture mye_texture_load(ecs_world_t *world, const char *path);

/* Registers an image the caller generated (raylib GenImage*, procedural art,
 * an atlas built at runtime). `name` is the dedupe key, so it must be unique.
 * The image is uploaded to the GPU and unloaded from CPU memory here. */
/* Registers a texture the registry does NOT own: it hands out a handle and
 * will never unload it. For a GPU object whose lifetime belongs to something
 * else -- a canvas's colour attachment, freed by UnloadRenderTexture -- so
 * that it can be referred to by handle like any other texture.
 *
 * The caller must release the handle before destroying the real owner. */
mye_texture mye_texture_adopt(ecs_world_t *world, const char *name,
                              Texture2D texture);

/* Uploads an Image and registers it under `name`.
 *
 * TAKES OWNERSHIP of the image: it is unloaded here, on every path including
 * failure and headless. Do not UnloadImage it yourself -- that is a double
 * free, and the sanitizers will say so only if you happen to run them. */
mye_texture mye_texture_from_image(ecs_world_t *world, const char *name,
                                   Image image);

/* NULL for an invalid or stale handle. */
const Texture2D *mye_texture_get(const ecs_world_t *world, mye_texture handle);

/* Falls back to a magenta placeholder, so a missing asset is visible on
 * screen rather than a crash. Still NULL in a headless world: the
 * placeholder is a GPU texture and there is no context to upload it to. */
const Texture2D *mye_texture_get_or_placeholder(const ecs_world_t *world,
                                                mye_texture handle);

/* Drops one reference; unloads at zero and invalidates every handle to it. */
void mye_texture_release(ecs_world_t *world, mye_texture handle);

bool mye_texture_valid(const ecs_world_t *world, mye_texture handle);

/* --------------------------------------------------------------- models -- */

/* Loads a 3D model. raylib 6.0 handles glTF/GLB, OBJ, IQM, M3D and VOX;
 * glTF is the format to target (see plan/03-rendering.md). Deduped by path
 * and refcounted like textures. */
/* Resolves a path given relative to the project root, so a program finds its
 * assets whatever directory it was started from.
 *
 * Tries the working directory first, then walks up from the executable's own
 * directory. Writes the resolved path to `out` and returns true; on failure
 * copies `relative` through unchanged and returns false, so the caller can
 * still report the path it wanted.
 *
 * Explicit rather than automatic: the engine does not quietly change the
 * working directory underneath you. */
bool mye_asset_path(const char *relative, char *out, size_t out_size);

mye_model mye_model_load(ecs_world_t *world, const char *path);

/* Registers a mesh the caller generated (GenMeshCube, GenMeshSphere, ...) as
 * a single-mesh model with a default material. Takes ownership of the mesh.
 * `name` is the dedupe key. */
mye_model mye_model_from_mesh(ecs_world_t *world, const char *name, Mesh mesh,
                              Color tint);

const Model *mye_model_get(const ecs_world_t *world, mye_model handle);

/* Skeletal animations that came with the model, if any. glTF and IQM carry
 * them; OBJ does not. Returns NULL and sets count to 0 when there are none. */
const ModelAnimation *mye_model_animations(const ecs_world_t *world,
                                           mye_model handle, int *out_count);
void mye_model_release(ecs_world_t *world, mye_model handle);
bool mye_model_valid(const ecs_world_t *world, mye_model handle);

/* --------------------------------------------------------------- sounds -- */

mye_sound mye_sound_load(ecs_world_t *world, const char *path);

/* Registers a Wave the caller synthesized (see the Asteroids example). The
 * wave is uploaded to the audio device and freed here. `name` is the dedupe
 * key, so it must be unique. */
mye_sound mye_sound_from_wave(ecs_world_t *world, const char *name, Wave wave);
const Sound *mye_sound_get(const ecs_world_t *world, mye_sound handle);
void mye_sound_release(ecs_world_t *world, mye_sound handle);
bool mye_sound_valid(const ecs_world_t *world, mye_sound handle);

/* ---------------------------------------------------------------- scopes -- */

/* Assets remember which scope loaded them, so a scene can release everything
 * it brought in without tracking handles by hand (see scene/scene.h).
 *
 * Release is refcounted: an asset two scopes both loaded survives the first
 * release. Scope 0 means "unscoped" and is never released automatically. */
void mye_assets_set_scope(ecs_world_t *world, uint32_t scope);
uint32_t mye_assets_current_scope(const ecs_world_t *world);
void mye_assets_release_scope(ecs_world_t *world, uint32_t scope);

/* ----------------------------------------------------------------- stats -- */

typedef struct mye_asset_stats {
    uint32_t textures_live;
    uint32_t sounds_live;
    uint32_t models_live;
    uint32_t textures_loaded_total;
} mye_asset_stats;

mye_asset_stats mye_asset_stats_get(const ecs_world_t *world);

#endif /* MYE_ASSET_ASSET_H */
