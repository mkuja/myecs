#include "scene/scene.h"

#include "asset/asset.h"

#include <string.h>

#define MYE_MAX_SCENES 32
#define MYE_SCENE_NAME_MAX 64

typedef struct scene_entry {
    char name[MYE_SCENE_NAME_MAX];
    mye_scene_fn load;
    mye_scene_fn unload;
    void *user;
    /* The entity that owns this scene's contents. Every entity created while
     * the scene is active carries (MyeSceneOf, this), so deleting it deletes
     * them -- flecs' own cleanup policy does the work. */
    ecs_entity_t owner;
    bool used;
} scene_entry;

typedef struct MyeScenes {
    scene_entry entries[MYE_MAX_SCENES];
    int32_t count;

    int32_t active;         /* index into entries, or -1 */
    int32_t pending;        /* index requested this frame, or -1 */
    bool pending_is_reload;

} MyeScenes;

ECS_COMPONENT_DECLARE(MyeScenes);

/* Relationship marking scene ownership. Configured so that deleting the
 * target (the scene owner entity) deletes everything pointing at it. */
static ecs_entity_t MyeSceneOf = 0;

/* ------------------------------------------------------------- internals -- */

static MyeScenes *scenes_get(const ecs_world_t *world)
{
    /* ecs_singleton_ensure needs a mutable world; the const here is about the
     * caller's intent, not the storage. */
    ecs_world_t *w = (ecs_world_t *)(uintptr_t)world;
    return ecs_singleton_ensure(w, MyeScenes);
}

static int32_t find_scene(const MyeScenes *scenes, const char *name)
{
    if (name == NULL) {
        return -1;
    }
    for (int32_t i = 0; i < scenes->count; ++i) {
        if (scenes->entries[i].used &&
            strcmp(scenes->entries[i].name, name) == 0) {
            return i;
        }
    }
    return -1;
}

static void unload_active(ecs_world_t *world, MyeScenes *scenes)
{
    if (scenes->active < 0) {
        return;
    }
    scene_entry *entry = &scenes->entries[scenes->active];

    /* Clear the active scene before teardown, so anything the unload
     * callback creates is not immediately destroyed with it. */
    int32_t unloading = scenes->active;
    scenes->active = -1;

    if (entry->unload != NULL) {
        entry->unload(world, entry->user);
    }

    /* Deleting the owner deletes every entity tagged with it. */
    if (entry->owner != 0) {
        ecs_delete(world, entry->owner);
        entry->owner = 0;
    }

    /* Release assets this scene brought in. Refcounted, so anything another
     * scene also holds survives. */
    mye_assets_release_scope(world, (uint32_t)unloading + 1);
}

static void load_scene(ecs_world_t *world, MyeScenes *scenes, int32_t index)
{
    scene_entry *entry = &scenes->entries[index];

    /* Deliberately ecs_new, not mye_entity_new: the owner must not be
     * tagged as belonging to a scene (least of all itself). */
    entry->owner = ecs_new(world);
    ecs_set_name(world, entry->owner, entry->name);

    scenes->active = index;
    mye_assets_set_scope(world, (uint32_t)index + 1);

    if (entry->load != NULL) {
        entry->load(world, entry->user);
    }
}

/* -------------------------------------------------------------- public -- */

ecs_entity_t mye_scene_register(ecs_world_t *world,
                                const mye_scene_desc *desc)
{
    if (desc == NULL || desc->name == NULL || desc->name[0] == '\0') {
        return 0;
    }

    MyeScenes *scenes = scenes_get(world);
    if (scenes == NULL || scenes->count >= MYE_MAX_SCENES) {
        return 0;
    }
    if (find_scene(scenes, desc->name) >= 0) {
        return 0; /* duplicate name: refuse rather than shadow */
    }

    scene_entry *entry = &scenes->entries[scenes->count];
    size_t n = strlen(desc->name);
    if (n >= MYE_SCENE_NAME_MAX) {
        n = MYE_SCENE_NAME_MAX - 1;
    }
    memcpy(entry->name, desc->name, n);
    entry->name[n] = '\0';
    entry->load = desc->load;
    entry->unload = desc->unload;
    entry->user = desc->user;
    entry->owner = 0;
    entry->used = true;

    ++scenes->count;
    ecs_singleton_modified(world, MyeScenes);
    return (ecs_entity_t)scenes->count; /* non-zero handle = success */
}

bool mye_scene_switch(ecs_world_t *world, const char *name)
{
    MyeScenes *scenes = scenes_get(world);
    if (scenes == NULL) {
        return false;
    }

    int32_t index = find_scene(scenes, name);
    if (index < 0) {
        return false;
    }

    scenes->pending = index;
    scenes->pending_is_reload = false;
    ecs_singleton_modified(world, MyeScenes);
    return true;
}

bool mye_scene_reload(ecs_world_t *world)
{
    MyeScenes *scenes = scenes_get(world);
    if (scenes == NULL || scenes->active < 0) {
        return false;
    }
    scenes->pending = scenes->active;
    scenes->pending_is_reload = true;
    ecs_singleton_modified(world, MyeScenes);
    return true;
}

void mye_scene_apply_pending(ecs_world_t *world)
{
    MyeScenes *scenes = scenes_get(world);
    if (scenes == NULL || scenes->pending < 0) {
        return;
    }

    int32_t target = scenes->pending;
    scenes->pending = -1;

    if (target == scenes->active && !scenes->pending_is_reload) {
        return; /* already there */
    }

    unload_active(world, scenes);
    load_scene(world, scenes, target);
    ecs_singleton_modified(world, MyeScenes);
}

ecs_entity_t mye_scene_owner(const ecs_world_t *world)
{
    const MyeScenes *scenes = ecs_singleton_get(world, MyeScenes);
    if (scenes == NULL || scenes->active < 0) {
        return 0;
    }
    return scenes->entries[scenes->active].owner;
}

ecs_entity_t mye_scene_relationship(void)
{
    return MyeSceneOf;
}

const char *mye_scene_current(const ecs_world_t *world)
{
    const MyeScenes *scenes = ecs_singleton_get(world, MyeScenes);
    if (scenes == NULL || scenes->active < 0) {
        return NULL;
    }
    return scenes->entries[scenes->active].name;
}

bool mye_scene_switch_pending(const ecs_world_t *world)
{
    const MyeScenes *scenes = ecs_singleton_get(world, MyeScenes);
    return scenes != NULL && scenes->pending >= 0;
}

int32_t mye_scene_entity_count(const ecs_world_t *world)
{
    const MyeScenes *scenes = ecs_singleton_get(world, MyeScenes);
    if (scenes == NULL || scenes->active < 0) {
        return 0;
    }
    ecs_entity_t owner = scenes->entries[scenes->active].owner;
    if (owner == 0) {
        return 0;
    }

    ecs_world_t *w = (ecs_world_t *)(uintptr_t)world;
    ecs_query_t *q = ecs_query(w, {
        .terms = {{ .id = ecs_pair(MyeSceneOf, owner) }},
        .cache_kind = EcsQueryCacheNone,
    });
    if (q == NULL) {
        return 0;
    }

    int32_t total = 0;
    ecs_iter_t it = ecs_query_iter(w, q);
    while (ecs_query_next(&it)) {
        total += it.count;
    }
    ecs_query_fini(q);
    return total;
}

/* ------------------------------------------------------------- lifecycle -- */

void MyeSceneModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeSceneModule);

    ECS_COMPONENT_DEFINE(world, MyeScenes);
    ecs_add_id(world, ecs_id(MyeScenes), EcsSingleton);

    MyeSceneOf = ecs_entity(world, { .name = "MyeSceneOf" });
    /* Deleting a scene owner deletes everything that points at it. This is
     * flecs' cleanup policy doing the work rather than the engine tracking
     * lists of entities by hand. */
    ecs_add_pair(world, MyeSceneOf, EcsOnDeleteTarget, EcsDelete);
    /* Scene membership must not be inherited by prefab instances, or an
     * instance would be owned by whichever scene defined the prefab. */
    ecs_add_pair(world, MyeSceneOf, EcsOnInstantiate, EcsDontInherit);

    ecs_singleton_set(world, MyeScenes, { .active = -1, .pending = -1 });

}
