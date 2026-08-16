#include "scene/serialize.h"

#include "render/render2d.h"
#include "scene/scene.h"
#include "scene/transform.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------- reflection -- */

void mye_serialize_register_engine_components(ecs_world_t *world)
{
    /* Only components a save file should contain. Deliberately absent:
     * MyeLocalTransform / MyeWorldTransform (derived every frame from the
     * placement below -- storing them would bloat saves and let them go
     * stale), and anything holding a pointer or an asset handle, which
     * cannot be meaningfully restored into a different process. */
    ecs_struct(world, {
        .entity = ecs_id(MyePosition2D),
        .members = {
            { .name = "x", .type = ecs_id(ecs_f32_t) },
            { .name = "y", .type = ecs_id(ecs_f32_t) },
        }
    });

    ecs_struct(world, {
        .entity = ecs_id(MyeRotation2D),
        .members = {{ .name = "angle", .type = ecs_id(ecs_f32_t) }}
    });

    ecs_struct(world, {
        .entity = ecs_id(MyeScale2D),
        .members = {
            { .name = "x", .type = ecs_id(ecs_f32_t) },
            { .name = "y", .type = ecs_id(ecs_f32_t) },
        }
    });

    /* Vector3 and Quaternion are raylib structs, so describe them once and
     * reuse. flecs needs a named entity per type. */
    ecs_entity_t vec3 = ecs_struct(world, {
        .entity = ecs_entity(world, { .name = "mye_vec3" }),
        .members = {
            { .name = "x", .type = ecs_id(ecs_f32_t) },
            { .name = "y", .type = ecs_id(ecs_f32_t) },
            { .name = "z", .type = ecs_id(ecs_f32_t) },
        }
    });
    ecs_entity_t quat = ecs_struct(world, {
        .entity = ecs_entity(world, { .name = "mye_quat" }),
        .members = {
            { .name = "x", .type = ecs_id(ecs_f32_t) },
            { .name = "y", .type = ecs_id(ecs_f32_t) },
            { .name = "z", .type = ecs_id(ecs_f32_t) },
            { .name = "w", .type = ecs_id(ecs_f32_t) },
        }
    });

    ecs_struct(world, {
        .entity = ecs_id(MyePosition3D),
        .members = {{ .name = "v", .type = vec3 }}
    });
    ecs_struct(world, {
        .entity = ecs_id(MyeRotation3D),
        .members = {{ .name = "q", .type = quat }}
    });
    ecs_struct(world, {
        .entity = ecs_id(MyeScale3D),
        .members = {{ .name = "v", .type = vec3 }}
    });
}

bool mye_component_serializable(const ecs_world_t *world,
                                ecs_entity_t component)
{
    if (component == 0) {
        return false;
    }
    /* flecs stores reflection as an EcsStruct component on the component
     * entity itself. No EcsStruct means no serializer. */
    return ecs_has(world, component, EcsStruct);
}

/* ------------------------------------------------------------------ JSON -- */

char *mye_world_to_json(ecs_world_t *world)
{
    if (world == NULL) {
        return NULL;
    }
    return ecs_world_to_json(world, NULL);
}

char *mye_scene_to_json(ecs_world_t *world)
{
    ecs_entity_t owner = mye_scene_owner(world);
    if (owner == 0) {
        return NULL;
    }

    /* Just the entities this scene owns, not the engine's singletons. */
    ecs_query_t *q = ecs_query(world, {
        .terms = {{ .id = ecs_pair(mye_scene_relationship(), owner) }},
        .cache_kind = EcsQueryCacheNone,
    });
    if (q == NULL) {
        return NULL;
    }

    ecs_iter_t it = ecs_query_iter(world, q);
    ecs_iter_to_json_desc_t desc = {
        .serialize_values = true,
        .serialize_entity_ids = true,
        /* Without this only the matched term (the scene-ownership pair) is
         * written, and the entities would come back with no components at
         * all. "table" means every component the entity actually has. */
        .serialize_table = true,
        .serialize_builtin = true, /* keeps entity names */
        .serialize_full_paths = true,
    };
    char *json = ecs_iter_to_json(&it, &desc);
    ecs_query_fini(q);
    return json;
}

void mye_json_free(ecs_world_t *world, char *json)
{
    (void)world;
    /* flecs allocated it through our os api, so it must go back the same
     * way rather than through free(). */
    ecs_os_free(json);
}

bool mye_world_from_json(ecs_world_t *world, const char *json)
{
    if (world == NULL || json == NULL) {
        return false;
    }
    return ecs_world_from_json(world, json, NULL) != NULL;
}

bool mye_world_save_json(ecs_world_t *world, const char *path)
{
    char *json = mye_world_to_json(world);
    if (json == NULL) {
        return false;
    }

    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        mye_json_free(world, json);
        return false;
    }

    size_t len = strlen(json);
    bool ok = fwrite(json, 1, len, f) == len;
    fclose(f);
    mye_json_free(world, json);
    return ok;
}

bool mye_world_load_json(ecs_world_t *world, const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return false;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return false;
    }
    long size = ftell(f);
    if (size <= 0) {
        fclose(f);
        return false;
    }
    rewind(f);

    mye_allocator a = mye_allocator_of(world);
    char *buffer = MYE_NEW_ARRAY(a, char, (size_t)size + 1);
    if (buffer == NULL) {
        fclose(f);
        return false;
    }

    size_t read = fread(buffer, 1, (size_t)size, f);
    fclose(f);
    buffer[read] = '\0';

    bool ok = mye_world_from_json(world, buffer);
    MYE_DELETE_ARRAY(a, buffer, (size_t)size + 1);
    return ok;
}
