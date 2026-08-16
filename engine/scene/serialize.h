/* Scene serialization: world state to and from JSON.
 * See plan/07-roadmap.md (M6) and plan/06-assets.md.
 *
 * Built on flecs' reflection: a component can only be serialized if its
 * layout has been described to flecs with ecs_struct(). The engine describes
 * its own components; a game must describe its own, and this header provides
 * the helper for doing so:
 *
 *   ECS_COMPONENT_DEFINE(world, Health);
 *   ecs_struct(world, {
 *       .entity = ecs_id(Health),
 *       .members = {{ .name = "current", .type = ecs_id(ecs_i32_t) },
 *                   { .name = "max",     .type = ecs_id(ecs_i32_t) }}
 *   });
 *
 * Components without reflection data are silently skipped rather than
 * failing the save -- which is a real trap, so mye_serialize_check() exists
 * to report what would be lost.
 */
#ifndef MYE_SCENE_SERIALIZE_H
#define MYE_SCENE_SERIALIZE_H

#include "core/engine.h"

/* Serializes the whole world to a JSON string. The caller owns the result and
 * frees it with mye_json_free(). NULL on failure. */
char *mye_world_to_json(ecs_world_t *world);

/* Serializes only the entities owned by the active scene. This is what a save
 * file usually wants: the level's contents, not the engine's singletons and
 * internal entities. NULL if no scene is active. */
char *mye_scene_to_json(ecs_world_t *world);

void mye_json_free(ecs_world_t *world, char *json);

/* Loads entities from JSON into the world. Existing entities are left alone
 * unless the JSON refers to them by id. Returns false on a parse error. */
bool mye_world_from_json(ecs_world_t *world, const char *json);

/* Writes/reads a JSON file. Convenience over the string forms. */
bool mye_world_save_json(ecs_world_t *world, const char *path);
bool mye_world_load_json(ecs_world_t *world, const char *path);

/* True if `component` has reflection data and can be serialized. */
bool mye_component_serializable(const ecs_world_t *world,
                                ecs_entity_t component);

/* Registers reflection for the engine's own serializable components. Called
 * by the scene module; exposed for tests. */
void mye_serialize_register_engine_components(ecs_world_t *world);

#endif /* MYE_SCENE_SERIALIZE_H */
