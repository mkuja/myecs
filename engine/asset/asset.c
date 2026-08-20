#include "asset/asset.h"

#include "core/rl_alloc.h"

#include <stdio.h>
#include <string.h>

ECS_COMPONENT_DECLARE(MyeAssets);

#define MYE_MAX_TEXTURES 256
#define MYE_MAX_SOUNDS 128
#define MYE_MAX_MODELS 128
#define MYE_MAX_FONTS 32
#define MYE_ASSET_KEY_MAX 128

typedef enum asset_state {
    ASSET_EMPTY = 0,
    ASSET_LOADED,
    /* A load that failed, with the key it was asked for kept so the registry
     * can say which path it was. Not an asset: no handle resolves to it, and
     * the next load of the same key reuses this slot to retry. See asset.h. */
    ASSET_FAILED,
} asset_state;

typedef struct texture_slot {
    uint32_t generation;
    asset_state state;
    uint32_t refcount;
    uint32_t scope; /* which scene loaded it; 0 = unscoped */
    char key[MYE_ASSET_KEY_MAX];
    Texture2D texture;
    /* False for a texture the registry only *refers* to -- a canvas's colour
     * attachment belongs to its RenderTexture2D, and UnloadRenderTexture
     * frees it. Unloading it here as well is a double free of a GL name,
     * which shows up later as some unrelated texture turning black. */
    bool owned;
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

typedef struct font_slot {
    uint32_t generation;
    asset_state state;
    uint32_t refcount;
    uint32_t scope; /* which scene loaded it; 0 = unscoped */
    /* "<path>@<size>": the rasterised size is part of what the slot holds,
     * so it has to be part of what identifies it. See asset.h. */
    char key[MYE_ASSET_KEY_MAX];
    Font font;
} font_slot;

struct mye_asset_db {
    mye_allocator allocator;

    texture_slot *textures;
    sound_slot *sounds;
    model_slot *models;
    font_slot *fonts;

    Texture2D placeholder;
    bool placeholder_ready;
    /* raylib's built-in font, borrowed not owned: it belongs to raylib and is
     * unloaded by CloseWindow. Never pass it to UnloadFont. */
    Font placeholder_font;
    bool placeholder_font_ready;
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
static int find_texture_in_state(const mye_asset_db *db, const char *key,
                                 asset_state state)
{
    for (int i = 0; i < MYE_MAX_TEXTURES; ++i) {
        if (db->textures[i].state == state &&
            strcmp(db->textures[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_texture_by_key(const mye_asset_db *db, const char *key)
{
    return find_texture_in_state(db, key, ASSET_LOADED);
}

/* An empty slot, or -- when there is none left -- a failure record, which is
 * bookkeeping and must never be the reason a real asset cannot load. */
static int find_free_texture_slot(const mye_asset_db *db)
{
    int failed = -1;
    for (int i = 0; i < MYE_MAX_TEXTURES; ++i) {
        if (db->textures[i].state == ASSET_EMPTY) {
            return i;
        }
        if (failed < 0 && db->textures[i].state == ASSET_FAILED) {
            failed = i;
        }
    }
    return failed;
}

/* Where a load of `key` should land. A failure record for this very key is
 * the slot to take: the load is a retry of it, and leaving the record behind
 * would double-count the same asset as both failed and loaded. */
static int claim_index_for_texture(const mye_asset_db *db, const char *key)
{
    int retry = find_texture_in_state(db, key, ASSET_FAILED);
    return retry >= 0 ? retry : find_free_texture_slot(db);
}

/* Records that a load failed, keyed by what was asked for. Reuses the record
 * already standing for this key, so a game retrying every frame does not fill
 * the registry with copies of one failure -- and warns only when the record
 * is new, for the same reason. */
static void record_texture_failure(mye_asset_db *db, const char *key)
{
    int index = find_texture_in_state(db, key, ASSET_FAILED);
    if (index < 0) {
        index = find_free_texture_slot(db);
        if (index < 0) {
            return; /* registry full: nothing left to record into */
        }
        mye_log_warn("assets: texture '%s' failed to load", key);
    }

    texture_slot *slot = &db->textures[index];
    if (slot->generation == 0) {
        slot->generation = GENERATION_START;
    }
    slot->state = ASSET_FAILED;
    slot->refcount = 0;
    slot->scope = db->scope;
    slot->texture = (Texture2D){ 0 };
    slot->owned = false;
    copy_key(slot->key, key);
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

static mye_texture claim_texture_slot_ex(mye_asset_db *db, const char *key,
                                         Texture2D texture, bool owned)
{
    int index = claim_index_for_texture(db, key);
    if (index < 0) {
        if (texture.id != 0 && owned) {
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
    slot->owned = owned;
    copy_key(slot->key, key);
    ++db->textures_loaded_total;

    return (mye_texture){ .index = (uint32_t)index,
                          .generation = slot->generation };
}

static mye_texture claim_texture_slot(mye_asset_db *db, const char *key,
                                      Texture2D texture)
{
    return claim_texture_slot_ex(db, key, texture, true);
}

mye_texture mye_texture_adopt(ecs_world_t *world, const char *name,
                              Texture2D texture)
{
    mye_asset_db *db = db_get(world);
    if (db == NULL || name == NULL) {
        return (mye_texture){ 0 };
    }
    /* Deliberately NOT the dedupe that mye_texture_load does. Sharing a slot
     * is right for a file, where the same path really is the same pixels; it
     * is wrong here, because the caller is handing over a GPU texture it
     * already owns. Deduping would return somebody else's texture -- the
     * canvas would display an unrelated image -- and hand this caller's
     * lifetime to a slot that may UnloadTexture it, or leave a handle the
     * registry still calls valid after the real owner freed the GL name.
     *
     * A collision is malformed data, so it is reported rather than absorbed. */
    if (find_texture_by_key(db, name) >= 0) {
        mye_log_error("assets: '%s' is already a texture; an adopted texture "
                      "needs a name of its own", name);
        return (mye_texture){ 0 };
    }

    mye_texture handle = claim_texture_slot_ex(db, name, texture, false);
    if (handle.generation == 0) {
        mye_log_error("assets: no free texture slot to adopt '%s' into", name);
    }
    return handle;
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
            record_texture_failure(db, key);
            return (mye_texture){ 0 };
        }
        Texture2D fake = { .id = 0, .width = image.width,
                           .height = image.height, .mipmaps = 1 };
        UnloadImage(image);
        return claim_texture_slot(db, key, fake);
    }

    Texture2D texture = LoadTexture(path);
    if (texture.id == 0) {
        record_texture_failure(db, key);
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
        record_texture_failure(db, key);
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

    if (slot->texture.id != 0 && slot->owned) {
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

static int find_model_in_state(const mye_asset_db *db, const char *key,
                               asset_state state)
{
    for (int i = 0; i < MYE_MAX_MODELS; ++i) {
        if (db->models[i].state == state &&
            strcmp(db->models[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_model_slot(const mye_asset_db *db)
{
    int failed = -1;
    for (int i = 0; i < MYE_MAX_MODELS; ++i) {
        if (db->models[i].state == ASSET_EMPTY) {
            return i;
        }
        if (failed < 0 && db->models[i].state == ASSET_FAILED) {
            failed = i;
        }
    }
    return failed;
}

/* See claim_index_for_texture. */
static int claim_index_for_model(const mye_asset_db *db, const char *key)
{
    int retry = find_model_in_state(db, key, ASSET_FAILED);
    return retry >= 0 ? retry : find_free_model_slot(db);
}

static void record_model_failure(mye_asset_db *db, const char *key)
{
    int index = find_model_in_state(db, key, ASSET_FAILED);
    if (index < 0) {
        index = find_free_model_slot(db);
        if (index < 0) {
            return;
        }
        mye_log_warn("assets: model '%s' failed to load", key);
    }

    model_slot *slot = &db->models[index];
    if (slot->generation == 0) {
        slot->generation = GENERATION_START;
    }
    slot->state = ASSET_FAILED;
    slot->refcount = 0;
    slot->scope = db->scope;
    slot->model = (Model){ 0 };
    slot->animations = NULL;
    slot->animation_count = 0;
    copy_key(slot->key, key);
}

static mye_model claim_model_slot(mye_asset_db *db, const char *key,
                                  Model model)
{
    int index = claim_index_for_model(db, key);
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
    return find_model_in_state(db, key, ASSET_LOADED);
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
        record_model_failure(db, key);
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
        record_model_failure(db, key);
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

static int find_sound_in_state(const mye_asset_db *db, const char *key,
                               asset_state state)
{
    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].state == state &&
            strcmp(db->sounds[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_sound_slot(const mye_asset_db *db)
{
    int failed = -1;
    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].state == ASSET_EMPTY) {
            return i;
        }
        if (failed < 0 && db->sounds[i].state == ASSET_FAILED) {
            failed = i;
        }
    }
    return failed;
}

/* See claim_index_for_texture. */
static int claim_index_for_sound(const mye_asset_db *db, const char *key)
{
    int retry = find_sound_in_state(db, key, ASSET_FAILED);
    return retry >= 0 ? retry : find_free_sound_slot(db);
}

static void record_sound_failure(mye_asset_db *db, const char *key)
{
    int index = find_sound_in_state(db, key, ASSET_FAILED);
    if (index < 0) {
        index = find_free_sound_slot(db);
        if (index < 0) {
            return;
        }
        mye_log_warn("assets: sound '%s' failed to load", key);
    }

    sound_slot *slot = &db->sounds[index];
    if (slot->generation == 0) {
        slot->generation = GENERATION_START;
    }
    slot->state = ASSET_FAILED;
    slot->refcount = 0;
    slot->scope = db->scope;
    slot->sound = (Sound){ 0 };
    copy_key(slot->key, key);
}

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
        /* No device: see the note in mye_sound_from_wave. Not a failure of
         * the asset, so nothing is recorded. */
        return (mye_sound){ 0 };
    }

    char key[MYE_ASSET_KEY_MAX];
    copy_key(key, path);

    int existing = find_sound_in_state(db, key, ASSET_LOADED);
    if (existing >= 0) {
        ++db->sounds[existing].refcount;
        return (mye_sound){ .index = (uint32_t)existing,
                            .generation = db->sounds[existing].generation };
    }

    int index = claim_index_for_sound(db, key);
    if (index < 0) {
        return (mye_sound){ 0 };
    }

    Sound sound = LoadSound(path);
    if (sound.frameCount == 0) {
        record_sound_failure(db, key);
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

    int existing = find_sound_in_state(db, key, ASSET_LOADED);
    if (existing >= 0) {
        UnloadWave(wave); /* caller handed over ownership */
        ++db->sounds[existing].refcount;
        return (mye_sound){ .index = (uint32_t)existing,
                            .generation = db->sounds[existing].generation };
    }

    int index = claim_index_for_sound(db, key);
    if (index < 0 || !db->audio_ready) {
        /* Headless or no device: the wave has nowhere to go. Callers get an
         * invalid handle and the game runs silently rather than failing.
         *
         * Deliberately NOT recorded as a failure: a missing audio device is a
         * property of the world, not of the asset, and recording it would
         * turn every headless run into a wall of failure records about
         * sounds that are perfectly fine. */
        UnloadWave(wave);
        return (mye_sound){ 0 };
    }

    Sound sound = LoadSoundFromWave(wave);
    UnloadWave(wave);
    if (sound.frameCount == 0) {
        record_sound_failure(db, key);
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

/* ----------------------------------------------------------------- fonts -- */

/* "<path>@<size>", truncated to a key. Composed by hand rather than with
 * snprintf into a scratch buffer, because snprintf truncates the TAIL: an
 * over-long path would lose the "@<size>" that is the whole point of the key,
 * and two sizes of one font would collide in a single slot. Here the PATH
 * gives way instead, which is also what copy_key does with a long key. */
static void font_key(char *out, const char *path, int size)
{
    char suffix[24];
    int written = snprintf(suffix, sizeof suffix, "@%d", size);
    size_t suffix_len = written > 0 ? (size_t)written : 0;
    if (suffix_len >= MYE_ASSET_KEY_MAX) {
        suffix_len = MYE_ASSET_KEY_MAX - 1; /* unreachable; keeps the maths safe */
    }

    size_t room = MYE_ASSET_KEY_MAX - 1 - suffix_len;
    size_t path_len = strlen(path);
    if (path_len > room) {
        path += path_len - room; /* keep the tail: the filename identifies it */
        path_len = room;
    }

    memcpy(out, path, path_len);
    memcpy(out + path_len, suffix, suffix_len);
    out[path_len + suffix_len] = '\0';
}

static int find_font_in_state(const mye_asset_db *db, const char *key,
                              asset_state state)
{
    for (int i = 0; i < MYE_MAX_FONTS; ++i) {
        if (db->fonts[i].state == state &&
            strcmp(db->fonts[i].key, key) == 0) {
            return i;
        }
    }
    return -1;
}

static int find_free_font_slot(const mye_asset_db *db)
{
    int failed = -1;
    for (int i = 0; i < MYE_MAX_FONTS; ++i) {
        if (db->fonts[i].state == ASSET_EMPTY) {
            return i;
        }
        if (failed < 0 && db->fonts[i].state == ASSET_FAILED) {
            failed = i;
        }
    }
    return failed;
}

/* See claim_index_for_texture. */
static int claim_index_for_font(const mye_asset_db *db, const char *key)
{
    int retry = find_font_in_state(db, key, ASSET_FAILED);
    return retry >= 0 ? retry : find_free_font_slot(db);
}

static void record_font_failure(mye_asset_db *db, const char *key)
{
    int index = find_font_in_state(db, key, ASSET_FAILED);
    if (index < 0) {
        index = find_free_font_slot(db);
        if (index < 0) {
            return;
        }
        mye_log_warn("assets: font '%s' failed to load", key);
    }

    font_slot *slot = &db->fonts[index];
    if (slot->generation == 0) {
        slot->generation = GENERATION_START;
    }
    slot->state = ASSET_FAILED;
    slot->refcount = 0;
    slot->scope = db->scope;
    slot->font = (Font){ 0 };
    copy_key(slot->key, key);
}

static font_slot *resolve_font(const mye_asset_db *db, mye_font handle)
{
    if (db == NULL || handle.generation == 0 ||
        handle.index >= MYE_MAX_FONTS) {
        return NULL;
    }
    font_slot *slot = &db->fonts[handle.index];
    if (slot->state != ASSET_LOADED || slot->generation != handle.generation) {
        return NULL;
    }
    return slot;
}

static mye_font claim_font_slot(mye_asset_db *db, const char *key, Font font)
{
    int index = claim_index_for_font(db, key);
    if (index < 0) {
        if (!db->headless && font.texture.id != 0) {
            UnloadFont(font); /* registry full: do not leak the atlas */
        }
        return (mye_font){ 0 };
    }

    font_slot *slot = &db->fonts[index];
    if (slot->generation == 0) {
        slot->generation = GENERATION_START;
    }
    slot->state = ASSET_LOADED;
    slot->refcount = 1;
    slot->scope = db->scope;
    slot->font = font;
    copy_key(slot->key, key);

    return (mye_font){ .index = (uint32_t)index,
                       .generation = slot->generation };
}

mye_font mye_font_load(ecs_world_t *world, const char *path, int size)
{
    mye_asset_db *db = db_get(world);
    if (db == NULL || path == NULL) {
        return (mye_font){ 0 };
    }
    if (size <= 0) {
        size = MYE_FONT_DEFAULT_SIZE;
    }

    char key[MYE_ASSET_KEY_MAX];
    font_key(key, path, size);

    int existing = find_font_in_state(db, key, ASSET_LOADED);
    if (existing >= 0) {
        ++db->fonts[existing].refcount; /* same file AND same size */
        return (mye_font){ .index = (uint32_t)existing,
                           .generation = db->fonts[existing].generation };
    }

    int data_size = 0;
    unsigned char *data = LoadFileData(path, &data_size);
    if (data == NULL || data_size == 0) {
        UnloadFileData(data);
        record_font_failure(db, key);
        return (mye_font){ 0 };
    }

    if (db->headless) {
        /* Rasterise the glyphs on the CPU and throw them away, exactly as the
         * headless texture path decodes an image for its dimensions: enough
         * to know the file really is a font, without a GL context to upload
         * an atlas into. */
        int glyph_count = 0;
        GlyphInfo *glyphs = LoadFontData(data, data_size, size, NULL, 0,
                                         FONT_DEFAULT, &glyph_count);
        UnloadFileData(data);
        if (glyphs == NULL || glyph_count == 0) {
            UnloadFontData(glyphs, glyph_count);
            record_font_failure(db, key);
            return (mye_font){ 0 };
        }
        UnloadFontData(glyphs, glyph_count);

        Font fake = { .baseSize = size, .glyphCount = glyph_count };
        return claim_font_slot(db, key, fake);
    }

    Font font = LoadFontFromMemory(GetFileExtension(path), data, data_size,
                                   size, NULL, 0);
    UnloadFileData(data);

    /* raylib does not report a font it could not parse: LoadFontFromMemory
     * hands back GetFontDefault() instead. Owning that would call UnloadFont
     * on raylib's own font at release and blank every piece of text in the
     * program, so the fallback is detected by identity and refused. */
    if (!IsFontValid(font) || font.texture.id == 0 ||
        font.texture.id == GetFontDefault().texture.id) {
        record_font_failure(db, key);
        return (mye_font){ 0 };
    }
    return claim_font_slot(db, key, font);
}

const Font *mye_font_get(const ecs_world_t *world, mye_font handle)
{
    const font_slot *slot = resolve_font(db_get(world), handle);
    return slot != NULL ? &slot->font : NULL;
}

const Font *mye_font_get_or_placeholder(const ecs_world_t *world,
                                        mye_font handle)
{
    mye_asset_db *db = db_get(world);
    const font_slot *slot = resolve_font(db, handle);
    if (slot != NULL) {
        return &slot->font;
    }
    return (db != NULL && db->placeholder_font_ready) ? &db->placeholder_font
                                                      : NULL;
}

bool mye_font_valid(const ecs_world_t *world, mye_font handle)
{
    return resolve_font(db_get(world), handle) != NULL;
}

void mye_font_release(ecs_world_t *world, mye_font handle)
{
    mye_asset_db *db = db_get(world);
    font_slot *slot = resolve_font(db, handle);
    if (slot == NULL || --slot->refcount > 0) {
        return;
    }
    if (!db->headless && slot->font.texture.id != 0) {
        UnloadFont(slot->font);
    }
    slot->state = ASSET_EMPTY;
    slot->key[0] = '\0';
    slot->font = (Font){ 0 };
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
     * for has a refcount above one and must survive.
     *
     * Failure records go too, rather than being released: a record of what
     * this scene could not load is this scene's bookkeeping, and keeping it
     * past the unload would make the count grow with every scene switch and
     * name paths nothing is asking for any more. */
    for (int i = 0; i < MYE_MAX_TEXTURES; ++i) {
        if (db->textures[i].scope != scope) {
            continue;
        }
        if (db->textures[i].state == ASSET_LOADED) {
            mye_texture_release(world,
                                (mye_texture){ .index = (uint32_t)i,
                                               .generation =
                                                   db->textures[i].generation });
        } else if (db->textures[i].state == ASSET_FAILED) {
            db->textures[i].state = ASSET_EMPTY;
            db->textures[i].key[0] = '\0';
        }
    }
    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].scope != scope) {
            continue;
        }
        if (db->sounds[i].state == ASSET_LOADED) {
            mye_sound_release(world,
                              (mye_sound){ .index = (uint32_t)i,
                                           .generation =
                                               db->sounds[i].generation });
        } else if (db->sounds[i].state == ASSET_FAILED) {
            db->sounds[i].state = ASSET_EMPTY;
            db->sounds[i].key[0] = '\0';
        }
    }
    for (int i = 0; i < MYE_MAX_MODELS; ++i) {
        if (db->models[i].scope != scope) {
            continue;
        }
        if (db->models[i].state == ASSET_LOADED) {
            mye_model_release(world,
                              (mye_model){ .index = (uint32_t)i,
                                           .generation =
                                               db->models[i].generation });
        } else if (db->models[i].state == ASSET_FAILED) {
            db->models[i].state = ASSET_EMPTY;
            db->models[i].key[0] = '\0';
        }
    }
    for (int i = 0; i < MYE_MAX_FONTS; ++i) {
        if (db->fonts[i].scope != scope) {
            continue;
        }
        if (db->fonts[i].state == ASSET_LOADED) {
            mye_font_release(world,
                             (mye_font){ .index = (uint32_t)i,
                                         .generation =
                                             db->fonts[i].generation });
        } else if (db->fonts[i].state == ASSET_FAILED) {
            db->fonts[i].state = ASSET_EMPTY;
            db->fonts[i].key[0] = '\0';
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
        if (db->textures[i].state == ASSET_FAILED) ++stats.assets_failed;
    }
    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].state == ASSET_LOADED) ++stats.sounds_live;
        if (db->sounds[i].state == ASSET_FAILED) ++stats.assets_failed;
    }
    for (int i = 0; i < MYE_MAX_MODELS; ++i) {
        if (db->models[i].state == ASSET_LOADED) ++stats.models_live;
        if (db->models[i].state == ASSET_FAILED) ++stats.assets_failed;
    }
    for (int i = 0; i < MYE_MAX_FONTS; ++i) {
        if (db->fonts[i].state == ASSET_LOADED) ++stats.fonts_live;
        if (db->fonts[i].state == ASSET_FAILED) ++stats.assets_failed;
    }
    stats.textures_loaded_total = db->textures_loaded_total;
    return stats;
}

/* ------------------------------------------------------------- lifecycle -- */

/* plan/06-assets.md: "debug builds report any handle still live at shutdown
 * with the path that loaded it". The allocator's leak report says how many
 * bytes went missing; this says which asset, which is the half you can act on.
 *
 * Unconditional rather than debug-only. It is one pass over the slot arrays,
 * once, at a point where the program is exiting anyway -- and a shipped game
 * that leaks a texture per scene transition has the same bug a debug build
 * would, with fewer people watching.
 *
 * A warning, not an error: holding an asset for as long as the world lives is
 * perfectly legitimate, and plenty of the engine's own tests do it. What the
 * report catches is the scene that meant to release and did not.
 *
 * Adopted textures are left out: the registry never owned them (a canvas's
 * colour attachment belongs to its RenderTexture2D), so it is in no position
 * to call one a leak. */
static void report_live_at_shutdown(const mye_asset_db *db)
{
    for (int i = 0; i < MYE_MAX_TEXTURES; ++i) {
        if (db->textures[i].state == ASSET_LOADED && db->textures[i].owned) {
            mye_log_warn("assets: texture '%s' still live at shutdown "
                         "(refcount %u)",
                         db->textures[i].key, db->textures[i].refcount);
        }
    }
    for (int i = 0; i < MYE_MAX_SOUNDS; ++i) {
        if (db->sounds[i].state == ASSET_LOADED) {
            mye_log_warn("assets: sound '%s' still live at shutdown "
                         "(refcount %u)",
                         db->sounds[i].key, db->sounds[i].refcount);
        }
    }
    for (int i = 0; i < MYE_MAX_MODELS; ++i) {
        if (db->models[i].state == ASSET_LOADED) {
            mye_log_warn("assets: model '%s' still live at shutdown "
                         "(refcount %u)",
                         db->models[i].key, db->models[i].refcount);
        }
    }
    for (int i = 0; i < MYE_MAX_FONTS; ++i) {
        if (db->fonts[i].state == ASSET_LOADED) {
            mye_log_warn("assets: font '%s' still live at shutdown "
                         "(refcount %u)",
                         db->fonts[i].key, db->fonts[i].refcount);
        }
    }
}

/* Runs during ecs_fini, i.e. while the window and audio device are still up,
 * which is what UnloadTexture and UnloadSound require. */
static void assets_fini(ecs_world_t *world, void *ctx)
{
    (void)world;
    mye_asset_db *db = (mye_asset_db *)ctx;
    if (db == NULL) {
        return;
    }

    report_live_at_shutdown(db);

    for (int i = 0; i < MYE_MAX_TEXTURES; ++i) {
        if (db->textures[i].state == ASSET_LOADED &&
            db->textures[i].texture.id != 0 && db->textures[i].owned) {
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
    for (int i = 0; i < MYE_MAX_FONTS; ++i) {
        if (db->fonts[i].state == ASSET_LOADED && !db->headless &&
            db->fonts[i].font.texture.id != 0) {
            UnloadFont(db->fonts[i].font);
        }
    }
    if (db->placeholder_ready) {
        UnloadTexture(db->placeholder);
    }
    /* placeholder_font is raylib's, unloaded by CloseWindow. Not ours to
     * free -- see the field's declaration. */
    if (db->audio_ready) {
        CloseAudioDevice();
    }

    mye_allocator a = db->allocator;
    MYE_DELETE_ARRAY(a, db->textures, MYE_MAX_TEXTURES);
    MYE_DELETE_ARRAY(a, db->sounds, MYE_MAX_SOUNDS);
    MYE_DELETE_ARRAY(a, db->models, MYE_MAX_MODELS);
    MYE_DELETE_ARRAY(a, db->fonts, MYE_MAX_FONTS);
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
    db->fonts = MYE_NEW_ARRAY(a, font_slot, MYE_MAX_FONTS);
    if (db->textures == NULL || db->sounds == NULL || db->models == NULL ||
        db->fonts == NULL) {
        MYE_DELETE_ARRAY(a, db->textures, MYE_MAX_TEXTURES);
        MYE_DELETE_ARRAY(a, db->sounds, MYE_MAX_SOUNDS);
        MYE_DELETE_ARRAY(a, db->models, MYE_MAX_MODELS);
        MYE_DELETE_ARRAY(a, db->fonts, MYE_MAX_FONTS);
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

        /* raylib loaded its built-in font during InitWindow. Borrowed as the
         * font placeholder so text with a missing font still reads on screen,
         * and so a zeroed mye_font means "the default font". */
        db->placeholder_font = GetFontDefault();
        db->placeholder_font_ready = db->placeholder_font.texture.id != 0;

        InitAudioDevice();
        db->audio_ready = IsAudioDeviceReady();
    }

    ecs_singleton_set(world, MyeAssets, { .db = db });

    ecs_atfini(world, assets_fini, db);
}
