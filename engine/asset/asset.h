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
 * M3 loads synchronously. The async path (worker decodes, main thread
 * uploads) arrives in M4 behind these same functions. */
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
mye_texture mye_texture_from_image(ecs_world_t *world, const char *name,
                                   Image image);

/* Queues a load on a worker thread and returns immediately. The handle is
 * valid straight away but resolves to the placeholder until the pixels have
 * been uploaded, so callers never wait and never branch on readiness.
 *
 * Split of responsibilities (see plan/05-concurrency.md):
 *   worker thread : read the file, decode it (CPU only, no GL)
 *   main thread   : upload to the GPU, in MyeAssetUpload during EcsPreStore
 *
 * Falls back to a synchronous load if no worker pool is available. */
mye_texture mye_texture_load_async(ecs_world_t *world, const char *path);

typedef enum mye_asset_status {
    MYE_ASSET_MISSING = 0, /* no such handle, or it went stale */
    MYE_ASSET_LOADING,     /* queued or decoding on a worker */
    MYE_ASSET_READY,       /* usable */
    MYE_ASSET_FAILED,      /* the file could not be read or decoded */
} mye_asset_status;

mye_asset_status mye_texture_status(const ecs_world_t *world,
                                    mye_texture handle);

/* True when nothing is still loading -- what a loading screen waits on. */
bool mye_assets_ready(const ecs_world_t *world);
/* How many loads are still in flight. */
size_t mye_assets_pending(const ecs_world_t *world);

/* NULL for an invalid or stale handle, and NULL while a load is in flight. */
const Texture2D *mye_texture_get(const ecs_world_t *world, mye_texture handle);

/* Never NULL: falls back to a magenta placeholder so a missing asset is
 * visible on screen rather than a crash. */
const Texture2D *mye_texture_get_or_placeholder(const ecs_world_t *world,
                                                mye_texture handle);

/* Drops one reference; unloads at zero and invalidates every handle to it. */
void mye_texture_release(ecs_world_t *world, mye_texture handle);

bool mye_texture_valid(const ecs_world_t *world, mye_texture handle);

/* --------------------------------------------------------------- models -- */

/* Loads a 3D model. raylib 6.0 handles glTF/GLB, OBJ, IQM, M3D and VOX;
 * glTF is the format to target (see plan/03-rendering.md). Deduped by path
 * and refcounted like textures. */
mye_model mye_model_load(ecs_world_t *world, const char *path);

/* Registers a mesh the caller generated (GenMeshCube, GenMeshSphere, ...) as
 * a single-mesh model with a default material. Takes ownership of the mesh.
 * `name` is the dedupe key. */
mye_model mye_model_from_mesh(ecs_world_t *world, const char *name, Mesh mesh,
                              Color tint);

const Model *mye_model_get(const ecs_world_t *world, mye_model handle);
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
