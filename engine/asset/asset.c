#include "asset/asset.h"

#include "core/rl_alloc.h"

#include <stdio.h>
#include <string.h>

ECS_COMPONENT_DECLARE(MyeAssets);

#define MYE_MAX_TEXTURES 256
#define MYE_MAX_SOUNDS 128
#define MYE_MAX_MODELS 128
#define MYE_ASSET_KEY_MAX 128

typedef enum asset_state {
    ASSET_EMPTY = 0,
    ASSET_LOADED,
} asset_state;

typedef struct texture_slot {
    uint32_t generation;
    asset_state state;
    uint32_t refcount;
    uint32_t scope; /* which scene loaded it; 0 = unscoped */
    char key[MYE_ASSET_KEY_MAX];
    Texture2D texture;
} texture_slot;

typedef struct model_slot {
    uint32_t generation;
    asset_state state;
    uint32_t refcount;
    uint32_t scope; /* which scene loaded it; 0 = unscoped */
    char key[MYE_ASSET_KEY_MAX];
    Model model;
    /* Loaded alongside the model when the format carries them. */
    ModelAnimation *animations;
    int animation_count;
} model_slot;

typedef struct sound_slot {
    uint32_t generation;
    asset_state state;
    uint32_t refcount;
    uint32_t scope; /* which scene loaded it; 0 = unscoped */
    char key[MYE_ASSET_KEY_MAX];
    Sound sound;
} sound_slot;

struct mye_asset_db {
    mye_allocator allocator;

    texture_slot *textures;
    sound_slot *sounds;
    model_slot *models;

    Texture2D placeholder;
    bool placeholder_ready;
    bool audio_ready;
    /* No window/GL: textures are recorded with their dimensions but never
     * uploaded, so registry behaviour stays testable headlessly. */
    bool headless;

    uint32_t textures_loaded_total;
    uint32_t scope; /* scope new assets are attributed to */
};

/* Generation 0 is never handed out, so a zeroed handle is always invalid. */
#define GENERATION_START 1

/* ---------------------------------------------------------------- lookup -- */

static mye_asset_db *db_get(const ecs_world_t *world)
{
    const MyeAssets *assets = ecs_singleton_get(world, MyeAssets);
    return assets != NULL ? assets->db : NULL;
}

static void copy_key(char *dst, const char *src)
{
    size_t n = strlen(src);
    if (n >= MYE_ASSET_KEY_MAX) {
        /* Keep the tail: the filename is more distinctive than the prefix. */
        src += n - (MYE_ASSET_KEY_MAX - 1);
        n = MYE_ASSET_KEY_MAX - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* Dedupe: a second request for a path already loaded shares that slot and
 * takes a reference, rather than loading and uploading it twice. */
static int find_texture_by_key(const mye_asset_db *db, const char *key)
{
    for (int i = 0; i < MYE_MAX_TEXTURES; ++i) {
        if (db->textures[i].state == ASSET_LOADED &&
            strcmp(db->textures[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_texture_slot(const mye_asset_db *db)
{
    for (int i = 0; i < MYE_MAX_TEXTURES; ++i) {
        if (db->textures[i].state == ASSET_EMPTY) {
            return i;
        }
    }
    return -1;
}

static texture_slot *resolve_texture(const mye_asset_db *db,
                                     mye_texture handle)
{
    if (db == NULL || handle.generation == 0 ||
        handle.index >= MYE_MAX_TEXTURES) {
        return NULL;
    }
    texture_slot *slot = &db->textures[handle.index];
    if (slot->state != ASSET_LOADED || slot->generation != handle.generation) {
        return NULL; /* stale handle: the slot was reused or freed */
    }
    return slot;
}

/* -------------------------------------------------------------- textures -- */

static mye_texture claim_texture_slot(mye_asset_db *db, const char *key,
                                      Texture2D texture)
{
    int index = find_free_texture_slot(db);
    if (index < 0) {
        if (texture.id != 0) {
            UnloadTexture(texture); /* registry full: do not leak the upload */
        }
        return (mye_texture){ 0 };
    }

    texture_slot *slot = &db->textures[index];
    if (slot->generation == 0) {
        slot->generation = GENERATION_START;
    }
    slot->state = ASSET_LOADED;
    slot->refcount = 1;
    slot->scope = db->scope;
    slot->texture = texture;
    copy_key(slot->key, key);
    ++db->textures_loaded_total;

    return (mye_texture){ .index = (uint32_t)index,
                          .generation = slot->generation };
}

mye_texture mye_texture_load(ecs_world_t *world, const char *path)
{
    mye_asset_db *db = db_get(world);
    if (db == NULL || path == NULL) {
        return (mye_texture){ 0 };
    }

    char key[MYE_ASSET_KEY_MAX];
    copy_key(key, path);

    int existing = find_texture_by_key(db, key);
    if (existing >= 0) {
        ++db->textures[existing].refcount; /* dedupe: share the same upload */
        return (mye_texture){ .index = (uint32_t)existing,
                              .generation = db->textures[existing].generation };
    }

    if (db->headless) {
        /* Decode on the CPU only -- enough to know the dimensions. */
        Image image = LoadImage(path);
        if (image.data == NULL) {
            return (mye_texture){ 0 };
        }
        Texture2D fake = { .id = 0, .width = image.width,
                           .height = image.height, .mipmaps = 1 };
        UnloadImage(image);
        return claim_texture_slot(db, key, fake);
    }

    Texture2D texture = LoadTexture(path);
    if (texture.id == 0) {
        return (mye_texture){ 0 };
    }
    return claim_texture_slot(db, key, texture);
}

mye_texture mye_texture_from_image(ecs_world_t *world, const char *name,
                                   Image image)
{
    mye_asset_db *db = db_get(world);
    if (db == NULL || name == NULL) {
        UnloadImage(image);
        return (mye_texture){ 0 };
    }

    char key[MYE_ASSET_KEY_MAX];
    copy_key(key, name);

    int existing = find_texture_by_key(db, key);
    if (existing >= 0) {
        UnloadImage(image); /* caller handed over ownership */
        ++db->textures[existing].refcount;
        return (mye_texture){ .index = (uint32_t)existing,
                              .generation = db->textures[existing].generation };
    }

    if (db->headless) {
        Texture2D fake = { .id = 0, .width = image.width,
                           .height = image.height, .mipmaps = 1 };
        UnloadImage(image);
        return claim_texture_slot(db, key, fake);
    }

    Texture2D texture = LoadTextureFromImage(image);
    UnloadImage(image); /* now lives on the GPU */
    if (texture.id == 0) {
        return (mye_texture){ 0 };
    }
    return claim_texture_slot(db, key, texture);
}

const Texture2D *mye_texture_get(const ecs_world_t *world, mye_texture handle)
{
    const texture_slot *slot = resolve_texture(db_get(world), handle);
    return slot != NULL ? &slot->texture : NULL;
}

const Texture2D *mye_texture_get_or_placeholder(const ecs_world_t *world,
                                                mye_texture handle)
{
    mye_asset_db *db = db_get(world);
    const texture_slot *slot = resolve_texture(db, handle);
    if (slot != NULL) {
        return &slot->texture;
    }
    return (db != NULL && db->placeholder_ready) ? &db->placeholder : NULL;
}

bool mye_texture_valid(const ecs_world_t *world, mye_texture handle)
{
    return resolve_texture(db_get(world), handle) != NULL;
}

void mye_texture_release(ecs_world_t *world, mye_texture handle)
{
    mye_asset_db *db = db_get(world);
    if (db == NULL || handle.generation == 0 ||
        handle.index >= MYE_MAX_TEXTURES) {
        return;
    }

    /* Open-coded rather than resolve_texture() only because the caller may
     * legitimately hold a stale handle here; releasing one twice must be a
     * no-op, not an error. */
    texture_slot *slot = &db->textures[handle.index];
    if (slot->generation != handle.generation ||
        slot->state != ASSET_LOADED) {
        return;
    }

    if (--slot->refcount > 0) {
        return; /* someone else still holds it */
    }

    if (slot->texture.id != 0) {
        UnloadTexture(slot->texture);
    }
    slot->state = ASSET_EMPTY;
    slot->key[0] = '\0';
    slot->texture = (Texture2D){ 0 };
    /* Bumping the generation is what makes every outstanding handle to this
     * slot resolve to NULL from now on. */
    ++slot->generation;
}

/* ---------------------------------------------------------------- models -- */

/* raylib 6.0's UnloadModel frees skeleton.bones and skeleton.bindPose but
 * NOT model.currentPose or model.boneMatrices, which LoadGLTF allocates for
 * every rigged model (rmodels.c ~6322). Every animated model therefore leaks
 * both -- invisible without a tracking allocator, which is how this surfaced.
 *
 * Freed here and nulled first, so if a later raylib fixes the oversight its
 * own free sees NULL and does nothing.
 *
 * mye_rl_free, not RL_FREE: raylib.h is included before core/rl_alloc.h in
 * this file, so the RL_FREE macro is raylib's plain free() and would be
 * handed a pointer our header-prefixed allocator produced. */
static void unload_model_fully(Model *model)
{
    if (model->currentPose != NULL) {
        mye_rl_free(model->currentPose);
        model->currentPose = NULL;
    }
    if (model->boneMatrices != NULL) {
        mye_rl_free(model->boneMatrices);
        model->boneMatrices = NULL;
    }
    UnloadModel(*model);
}

static model_slot *resolve_model(const mye_asset_db *db, mye_model handle)
{
    if (db == NULL || handle.generation == 0 ||
        handle.index >= MYE_MAX_MODELS) {
        return NULL;
    }
    model_slot *slot = &db->models[handle.index];
    if (slot->state != ASSET_LOADED || slot->generation != handle.generation) {
        return NULL;
    }
    return slot;
}

static mye_model claim_model_slot(mye_asset_db *db, const char *key,
                                  Model model)
{
    int index = -1;
    for (int i = 0; i < MYE_MAX_MODELS; ++i) {
        if (db->models[i].state == ASSET_EMPTY) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        if (!db->headless) {
            UnloadModel(model);
        }
        return (mye_model){ 0 };
    }

    model_slot *slot = &db->models[index];
    if (slot->generation == 0) {
        slot->generation = GENERATION_START;
    }
    slot->state = ASSET_LOADED;
    slot->refcount = 1;
    slot->scope = db->scope;
    slot->model = model;
    slot->animations = NULL;
    slot->animation_count = 0;
    copy_key(slot->key, key);

    return (mye_model){ .index = (uint32_t)index,
                        .generation = slot->generation };
}

static int find_model_by_key(const mye_asset_db *db, const char *key)
{
    for (int i = 0; i < MYE_MAX_MODELS; ++i) {
        if (db->models[i].state == ASSET_LOADED &&
            strcmp(db->models[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

/* Six is arbitrary but ample: build trees put an executable three or four
 * directories below the root, never dozens. */
#define MYE_ASSET_SEARCH_DEPTH 6

bool mye_asset_path(const char *relative, char *out, size_t out_size)
{
    if (relative == NULL || out == NULL || out_size == 0) {
        return false;
    }

    if (FileExists(relative)) {
        snprintf(out, out_size, "%s", relative);
        return true;
    }

    /* Walk up from the executable rather than the working directory: the
     * point is to be independent of where the program was started. */
    const char *base = GetApplicationDirectory();
    if (base == NULL) {
        snprintf(out, out_size, "%s", relative);
        return false;
    }

    char prefix[1024];
    snprintf(prefix, sizeof prefix, "%s", base);

    for (int level = 0; level < MYE_ASSET_SEARCH_DEPTH; ++level) {
        char candidate[2048];
        snprintf(candidate, sizeof candidate, "%s/%s", prefix, relative);
        if (FileExists(candidate)) {
            snprintf(out, out_size, "%s", candidate);
            return true;
        }

        char *slash = strrchr(prefix, '/');
        if (slash == NULL || slash == prefix) {
            break;
        }
        *slash = '\0';
    }

    snprintf(out, out_size, "%s", relative);
    return false;
}

mye_model mye_model_load(ecs_world_t *world, const char *path)
{
    mye_asset_db *db = db_get(world);
    if (db == NULL || path == NULL) {
        return (mye_model){ 0 };
    }

    char key[MYE_ASSET_KEY_MAX];
    copy_key(key, path);

    int existing = find_model_by_key(db, key);
    if (existing >= 0) {
        ++db->models[existing].refcount;
        return (mye_model){ .index = (uint32_t)existing,
                            .generation = db->models[existing].generation };
    }

    if (db->headless) {
        /* Loading a model uploads vertex buffers to the GPU, so headless
         * worlds cannot hold a real one. The registry still tracks the slot
         * so scene and refcount logic stays testable. */
        return claim_model_slot(db, key, (Model){ 0 });
    }

    Model model = LoadModel(path);
    if (model.meshCount == 0) {
        /* A failed load is not an empty one: raylib still allocates a default
         * material for the model it hands back, so dropping the struct here
         * leaks it. */
        UnloadModel(model);
        return (mye_model){ 0 };
    }

    mye_model handle = claim_model_slot(db, key, model);
    if (handle.generation == 0) {
        UnloadModel(model); /* registry full: nobody else will free it */
        return handle;
    }

    /* Animations ride along with the model: they are useless without the
     * skeleton they drive, and share its lifetime. */
    int animation_count = 0;
    ModelAnimation *animations = LoadModelAnimations(path, &animation_count);
    if (animations != NULL && animation_count > 0) {
        model_slot *slot = &db->models[handle.index];
        slot->animations = animations;
        slot->animation_count = animation_count;
    } else if (animations != NULL) {
        UnloadModelAnimations(animations, 0);
    }

    return handle;
}

mye_model mye_model_from_mesh(ecs_world_t *world, const char *name, Mesh mesh,
                              Color tint)
{
    mye_asset_db *db = db_get(world);
    if (db == NULL || name == NULL) {
        return (mye_model){ 0 };
    }

    char key[MYE_ASSET_KEY_MAX];
    copy_key(key, name);

    int existing = find_model_by_key(db, key);
    if (existing >= 0) {
        if (!db->headless) {
            UnloadMesh(mesh);
        }
        ++db->models[existing].refcount;
        return (mye_model){ .index = (uint32_t)existing,
                            .generation = db->models[existing].generation };
    }

    if (db->headless) {
        return claim_model_slot(db, key, (Model){ 0 });
    }

    Model model = LoadModelFromMesh(mesh); /* takes ownership of the mesh */
    if (model.meshCount == 0) {
        return (mye_model){ 0 };
    }
    model.materials[0].maps[MATERIAL_MAP_DIFFUSE].color = tint;
    return claim_model_slot(db, key, model);
}

const ModelAnimation *mye_model_animations(const ecs_world_t *world,
                                           mye_model handle, int *out_count)
{
    const model_slot *slot = resolve_model(db_get(world), handle);
    if (slot == NULL || slot->animation_count == 0) {
        if (out_count != NULL) {
            *out_count = 0;
        }
        return NULL;
    }
    if (out_count != NULL) {
        *out_count = slot->animation_count;
    }
    return slot->animations;
}

const Model *mye_model_get(const ecs_world_t *world, mye_model handle)
{
    const model_slot *slot = resolve_model(db_get(world), handle);
    return slot != NULL ? &slot->model : NULL;
}

bool mye_model_valid(const ecs_world_t *world, mye_model handle)
{
    return resolve_model(db_get(world), handle) != NULL;
}

void mye_model_release(ecs_world_t *world, mye_model handle)
{
    mye_asset_db *db = db_get(world);
    model_slot *slot = resolve_model(db, handle);
    if (slot == NULL || --slot->refcount > 0) {
        return;
    }
    if (!db->headless) {
        if (slot->animations != NULL) {
            UnloadModelAnimations(slot->animations, slot->animation_count);
        }
        if (slot->model.meshCount > 0) {
            unload_model_fully(&slot->model);
        }
    }
    slot->animations = NULL;
    slot->animation_count = 0;
    slot->state = ASSET_EMPTY;
    slot->key[0] = '\0';
    slot->model = (Model){ 0 };
    ++slot->generation;
}

/* ---------------------------------------------------------------- sounds -- */

static sound_slot *resolve_sound(const mye_asset_db *db, mye_sound handle)
{
    if (db == NULL || handle.generation == 0 ||
        handle.index >= MYE_MAX_SOUNDS) {
        return NULL;
    }
    sound_slot *slot = &db->sounds[handle.index];
    if (slot->state != ASSET_LOADED || slot->generation != handle.generation) {
        return NULL;
    }
    return slot;
}

mye_sound mye_sound_load(ecs_world_t *world, const char *path)
{
    mye_asset_db *db = db_get(world);
    if (db == NULL || path == NULL || !db->audio_ready) {
        return (mye_sound){ 0 };
    }

    char key[MYE_ASSET_KEY_MAX];
    copy_key(key, path);

    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].state == ASSET_LOADED &&
            strcmp(db->sounds[i].key, key) == 0) {
            ++db->sounds[i].refcount;
            return (mye_sound){ .index = (uint32_t)i,
                                .generation = db->sounds[i].generation };
        }
    }

    int index = -1;
    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].state == ASSET_EMPTY) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        return (mye_sound){ 0 };
    }

    Sound sound = LoadSound(path);
    if (sound.frameCount == 0) {
        return (mye_sound){ 0 };
    }

    sound_slot *slot = &db->sounds[index];
    if (slot->generation == 0) {
        slot->generation = GENERATION_START;
    }
    slot->state = ASSET_LOADED;
    slot->refcount = 1;
    slot->scope = db->scope;
    slot->sound = sound;
    copy_key(slot->key, key);

    return (mye_sound){ .index = (uint32_t)index,
                        .generation = slot->generation };
}

mye_sound mye_sound_from_wave(ecs_world_t *world, const char *name, Wave wave)
{
    mye_asset_db *db = db_get(world);
    if (db == NULL || name == NULL) {
        UnloadWave(wave);
        return (mye_sound){ 0 };
    }

    char key[MYE_ASSET_KEY_MAX];
    copy_key(key, name);

    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].state == ASSET_LOADED &&
            strcmp(db->sounds[i].key, key) == 0) {
            UnloadWave(wave); /* caller handed over ownership */
            ++db->sounds[i].refcount;
            return (mye_sound){ .index = (uint32_t)i,
                                .generation = db->sounds[i].generation };
        }
    }

    int index = -1;
    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].state == ASSET_EMPTY) {
            index = i;
            break;
        }
    }
    if (index < 0 || !db->audio_ready) {
        /* Headless or no device: the wave has nowhere to go. Callers get an
         * invalid handle and the game runs silently rather than failing. */
        UnloadWave(wave);
        return (mye_sound){ 0 };
    }

    Sound sound = LoadSoundFromWave(wave);
    UnloadWave(wave);
    if (sound.frameCount == 0) {
        return (mye_sound){ 0 };
    }

    sound_slot *slot = &db->sounds[index];
    if (slot->generation == 0) {
        slot->generation = GENERATION_START;
    }
    slot->state = ASSET_LOADED;
    slot->refcount = 1;
    slot->scope = db->scope;
    slot->sound = sound;
    copy_key(slot->key, key);

    return (mye_sound){ .index = (uint32_t)index,
                        .generation = slot->generation };
}

const Sound *mye_sound_get(const ecs_world_t *world, mye_sound handle)
{
    const sound_slot *slot = resolve_sound(db_get(world), handle);
    return slot != NULL ? &slot->sound : NULL;
}

bool mye_sound_valid(const ecs_world_t *world, mye_sound handle)
{
    return resolve_sound(db_get(world), handle) != NULL;
}

void mye_sound_release(ecs_world_t *world, mye_sound handle)
{
    sound_slot *slot = resolve_sound(db_get(world), handle);
    if (slot == NULL || --slot->refcount > 0) {
        return;
    }
    UnloadSound(slot->sound);
    slot->state = ASSET_EMPTY;
    slot->key[0] = '\0';
    ++slot->generation;
}

/* ---------------------------------------------------------------- scopes -- */

void mye_assets_set_scope(ecs_world_t *world, uint32_t scope)
{
    mye_asset_db *db = db_get(world);
    if (db != NULL) {
        db->scope = scope;
    }
}

uint32_t mye_assets_current_scope(const ecs_world_t *world)
{
    const mye_asset_db *db = db_get(world);
    return db != NULL ? db->scope : 0;
}

void mye_assets_release_scope(ecs_world_t *world, uint32_t scope)
{
    mye_asset_db *db = db_get(world);
    if (db == NULL || scope == 0) {
        return; /* scope 0 is "unscoped": never bulk-released */
    }

    /* Release rather than unload outright: an asset another scope also asked
     * for has a refcount above one and must survive. */
    for (int i = 0; i < MYE_MAX_TEXTURES; ++i) {
        if (db->textures[i].state == ASSET_LOADED &&
            db->textures[i].scope == scope) {
            mye_texture_release(world,
                                (mye_texture){ .index = (uint32_t)i,
                                               .generation =
                                                   db->textures[i].generation });
        }
    }
    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].state == ASSET_LOADED &&
            db->sounds[i].scope == scope) {
            mye_sound_release(world,
                              (mye_sound){ .index = (uint32_t)i,
                                           .generation =
                                               db->sounds[i].generation });
        }
    }
    for (int i = 0; i < MYE_MAX_MODELS; ++i) {
        if (db->models[i].state == ASSET_LOADED &&
            db->models[i].scope == scope) {
            mye_model_release(world,
                              (mye_model){ .index = (uint32_t)i,
                                           .generation =
                                               db->models[i].generation });
        }
    }
}

/* ----------------------------------------------------------------- stats -- */

mye_asset_stats mye_asset_stats_get(const ecs_world_t *world)
{
    mye_asset_stats stats = { 0 };
    const mye_asset_db *db = db_get(world);
    if (db == NULL) {
        return stats;
    }

    for (int i = 0; i < MYE_MAX_TEXTURES; ++i) {
        if (db->textures[i].state == ASSET_LOADED) ++stats.textures_live;
    }
    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].state == ASSET_LOADED) ++stats.sounds_live;
    }
    for (int i = 0; i < MYE_MAX_MODELS; ++i) {
        if (db->models[i].state == ASSET_LOADED) ++stats.models_live;
    }
    stats.textures_loaded_total = db->textures_loaded_total;
    return stats;
}

/* ------------------------------------------------------------- lifecycle -- */

/* Runs during ecs_fini, i.e. while the window and audio device are still up,
 * which is what UnloadTexture and UnloadSound require. */
static void assets_fini(ecs_world_t *world, void *ctx)
{
    (void)world;
    mye_asset_db *db = (mye_asset_db *)ctx;
    if (db == NULL) {
        return;
    }

    for (int i = 0; i < MYE_MAX_TEXTURES; ++i) {
        if (db->textures[i].state == ASSET_LOADED &&
            db->textures[i].texture.id != 0) {
            UnloadTexture(db->textures[i].texture);
        }
    }
    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].state == ASSET_LOADED) {
            UnloadSound(db->sounds[i].sound);
        }
    }
    for (int i = 0; i < MYE_MAX_MODELS; ++i) {
        if (db->models[i].state == ASSET_LOADED && !db->headless) {
            if (db->models[i].animations != NULL) {
                UnloadModelAnimations(db->models[i].animations,
                                      db->models[i].animation_count);
            }
            if (db->models[i].model.meshCount > 0) {
                unload_model_fully(&db->models[i].model);
            }
        }
    }
    if (db->placeholder_ready) {
        UnloadTexture(db->placeholder);
    }
    if (db->audio_ready) {
        CloseAudioDevice();
    }

    mye_allocator a = db->allocator;
    MYE_DELETE_ARRAY(a, db->textures, MYE_MAX_TEXTURES);
    MYE_DELETE_ARRAY(a, db->sounds, MYE_MAX_SOUNDS);
    MYE_DELETE_ARRAY(a, db->models, MYE_MAX_MODELS);
    MYE_DELETE(a, db);
}

void MyeAssetsModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeAssetsModule);

    ECS_COMPONENT_DEFINE(world, MyeAssets);
    ecs_add_id(world, ecs_id(MyeAssets), EcsSingleton);

    mye_allocator a = mye_allocator_of(world);
    mye_asset_db *db = MYE_NEW(a, mye_asset_db);
    if (db == NULL) {
        ecs_singleton_set(world, MyeAssets, { .db = NULL });
        return;
    }

    db->allocator = a;
    db->textures = MYE_NEW_ARRAY(a, texture_slot, MYE_MAX_TEXTURES);
    db->sounds = MYE_NEW_ARRAY(a, sound_slot, MYE_MAX_SOUNDS);
    db->models = MYE_NEW_ARRAY(a, model_slot, MYE_MAX_MODELS);
    if (db->textures == NULL || db->sounds == NULL || db->models == NULL) {
        MYE_DELETE_ARRAY(a, db->textures, MYE_MAX_TEXTURES);
        MYE_DELETE_ARRAY(a, db->sounds, MYE_MAX_SOUNDS);
        MYE_DELETE_ARRAY(a, db->models, MYE_MAX_MODELS);
        MYE_DELETE(a, db);
        ecs_singleton_set(world, MyeAssets, { .db = NULL });
        return;
    }

    const mye_engine *engine = mye_engine_get(world);
    db->headless = engine == NULL || engine->headless;

    if (!db->headless) {
        /* Visible stand-in for anything that failed to load. */
        Image placeholder = GenImageColor(8, 8, MAGENTA);
        db->placeholder = LoadTextureFromImage(placeholder);
        UnloadImage(placeholder);
        db->placeholder_ready = db->placeholder.id != 0;

        InitAudioDevice();
        db->audio_ready = IsAudioDeviceReady();
    }

    ecs_singleton_set(world, MyeAssets, { .db = db });

    ecs_atfini(world, assets_fini, db);
}
