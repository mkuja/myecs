/* Scenes: a level, a menu, a loading screen -- loaded and unloaded as a unit.
 * See plan/07-roadmap.md (M6).
 *
 *   mye_scene_register(world, &(mye_scene_desc){
 *       .name = "menu", .load = menu_load });
 *   mye_scene_register(world, &(mye_scene_desc){
 *       .name = "game", .load = game_load });
 *   mye_scene_switch(world, "menu");
 *
 * Two things are handled for you, and they are the whole point:
 *
 *   1. ENTITY OWNERSHIP. Entities created with mye_entity_new() while a scene
 *      is active are tagged as belonging to it, so unloading deletes exactly
 *      them -- no manual bookkeeping, nothing forgotten. That includes
 *      entities gameplay spawns later, not just what the load callback made.
 *
 *      Note the requirement: mye_entity_new(), not bare ecs_new(). Every
 *      engine spawn helper already uses it.
 *
 *   2. ASSET OWNERSHIP. Assets loaded during a scene are released when it
 *      unloads, but only down to their refcount: an asset two scenes share
 *      survives the first unload. See plan/06-assets.md.
 *
 * Switching is deferred to the start of the next frame, never applied in the
 * middle of one: deleting entities out from under running systems is how
 * engines get mysterious crashes.
 */
#ifndef MYE_SCENE_SCENE_H
#define MYE_SCENE_SCENE_H

#include "core/engine.h"

typedef void (*mye_scene_fn)(ecs_world_t *world, void *user);

typedef struct mye_scene_desc {
    const char *name;

    /* Builds the scene: spawn entities, load assets, register nothing global.
     * Everything created here is owned by the scene. */
    mye_scene_fn load;

    /* Optional, called before the scene's entities are deleted -- for state
     * the engine cannot know about (saving a score, stopping music). */
    mye_scene_fn unload;

    void *user;
} mye_scene_desc;

/* Registers a scene. Returns 0 on failure (duplicate name, table full). */
ecs_entity_t mye_scene_register(ecs_world_t *world,
                                const mye_scene_desc *desc);

/* Requests a switch. The current scene is unloaded and the new one loaded at
 * the start of the next frame. Returns false if no scene by that name is
 * registered. Calling it twice in one frame keeps the last request. */
bool mye_scene_switch(ecs_world_t *world, const char *name);

/* Reloads the active scene -- unload then load again. Useful for "restart
 * level" and for testing that a scene leaves nothing behind. */
bool mye_scene_reload(ecs_world_t *world);

/* Name of the active scene, or NULL if none. */
const char *mye_scene_current(const ecs_world_t *world);

/* True while a switch is pending (requested but not yet applied). */
bool mye_scene_switch_pending(const ecs_world_t *world);

/* Number of entities the active scene owns. Mostly for tests and the debug
 * overlay. */
int32_t mye_scene_entity_count(const ecs_world_t *world);

/* Applies a pending switch. Called by mye_progress at the frame boundary;
 * games do not call this. Exposed so tests can drive it explicitly. */
void mye_scene_apply_pending(ecs_world_t *world);

/* The entity that owns the active scene's contents, or 0 if no scene is
 * active. mye_entity_new tags new entities with (relationship, owner). */
ecs_entity_t mye_scene_owner(const ecs_world_t *world);

/* The scene-ownership relationship. Deleting an owner deletes everything
 * tagged with it, which is how unloading works. */
ecs_entity_t mye_scene_relationship(void);

void MyeSceneModuleImport(ecs_world_t *world);

#endif /* MYE_SCENE_SCENE_H */
