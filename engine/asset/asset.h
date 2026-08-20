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
 * frames rather than reach for a thread.
 *
 * FAILED LOADS. A load that fails does not leave the slot empty: it leaves a
 * *failure record* behind, holding the key it was asked for. An empty slot
 * can only say "nothing here", which leaves "why did this asset not resolve"
 * with no answer at all.
 *
 *   - A failure record is not an asset: no handle is ever handed out for one,
 *     mye_*_valid() is false, and mye_*_get() returns NULL.
 *   - Loading the same key again RETRIES, reusing the record's own slot. The
 *     usual cause of a failure is a file that was not there yet, so retrying
 *     is the useful default; reusing the slot keeps a per-frame retry from
 *     filling the registry with copies of one failure. A retry that succeeds
 *     turns the record into the loaded asset, in place.
 *   - A failure record is evictable: a load with no empty slot left claims a
 *     failed one before giving up. Bookkeeping must never be the reason a
 *     real asset cannot load.
 *   - mye_asset_stats.assets_failed counts them, and each new record is
 *     logged once at warning level with the key that failed.
 *
 * SHUTDOWN. Every asset still loaded when the registry is torn down is
 * reported at warning level with the key that loaded it and its refcount
 * (plan/06-assets.md). It is a report, not an error: holding a handle for as
 * long as the world lives is legitimate. What it catches is the scene that
 * forgot to release. */
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

typedef struct mye_music {
    uint32_t index;
    uint32_t generation;
} mye_music;

typedef struct mye_model {
    uint32_t index;
    uint32_t generation;
} mye_model;

typedef struct mye_font {
    uint32_t index;
    uint32_t generation;
} mye_font;

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

/* ----------------------------------------------------------------- music -- */

/* A streaming track: decoded a buffer at a time while it plays, rather than
 * held in memory whole like a sound. Deduped by path and refcounted like the
 * rest; playback lives in audio/audio.h.
 *
 * Whatever raylib can stream is accepted -- OGG and WAV in practice, and the
 * extension is never inspected here: raylib sniffs the file, and gating on
 * ".ogg" would only teach the engine a shorter list of formats than the
 * decoder actually has.
 *
 * Headless (or with no audio device) the slot is recorded without a stream,
 * so scene bookkeeping and the playback state machine stay testable without
 * a sound card. The file still has to exist: a typo is a bug whether or not
 * anyone can hear it. mye_music_get then hands back a zeroed Music, which
 * every raylib music call ignores. */
mye_music mye_music_load(ecs_world_t *world, const char *path);
const Music *mye_music_get(const ecs_world_t *world, mye_music handle);
void mye_music_release(ecs_world_t *world, mye_music handle);
bool mye_music_valid(const ecs_world_t *world, mye_music handle);
/* ----------------------------------------------------------------- fonts -- */

/* Loads a TTF (or OTF) and rasterises it at `size` pixels.
 *
 * THE SIZE IS PART OF THE KEY. raylib bakes a glyph atlas at load time, so a
 * font at 16px and the same file at 48px are two atlases, two GPU textures,
 * and therefore two slots: the dedupe key is "<path>@<size>", not the path.
 * Deduping on the path alone would hand the 16px atlas to whoever asked for
 * 48px, and the text would come out blurry with no visible cause. Truncation
 * of an over-long key keeps the tail, so the "@<size>" that distinguishes the
 * two slots always survives.
 *
 * `size` of 0 or less is taken as MYE_FONT_DEFAULT_SIZE. Loads raylib's
 * default 95-codepoint ASCII set; other scripts need a codepoint list, which
 * is not built yet.
 *
 * Returns an invalid handle on failure, leaving a failure record (see the
 * note at the top of this header). A headless world validates and measures
 * the font on the CPU but uploads no atlas, exactly as textures do. */
#define MYE_FONT_DEFAULT_SIZE 32

mye_font mye_font_load(ecs_world_t *world, const char *path, int size);

/* NULL for an invalid or stale handle. */
const Font *mye_font_get(const ecs_world_t *world, mye_font handle);

/* Falls back to raylib's built-in default font, so text with a missing font
 * is still readable on screen rather than silently absent -- and so a zeroed
 * mye_font means "the default font" rather than "no text". Still NULL in a
 * headless world: the default font is a GPU atlas and there is no context
 * that holds one. */
const Font *mye_font_get_or_placeholder(const ecs_world_t *world,
                                        mye_font handle);

/* Drops one reference; unloads at zero and invalidates every handle to it. */
void mye_font_release(ecs_world_t *world, mye_font handle);

bool mye_font_valid(const ecs_world_t *world, mye_font handle);

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
    uint32_t music_live;
    uint32_t fonts_live;
    uint32_t textures_loaded_total;
    /* Slots holding a failure record, across every kind: loads that failed
     * and kept the key they were asked for. Non-zero after a level loads is
     * the signal to go and read the log for the paths. */
    uint32_t assets_failed;
} mye_asset_stats;

mye_asset_stats mye_asset_stats_get(const ecs_world_t *world);

#endif /* MYE_ASSET_ASSET_H */
