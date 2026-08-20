/* Module registration: after mye_init, every engine module is in the world
 * and its components and systems are where the rest of the engine expects to
 * find them.
 *
 * This is the "ECS glue" row of plan/09-testing.md. It exists because a
 * module that silently fails to import is close to invisible: ECS_IMPORT
 * returns nothing to check, the components it defines simply never appear,
 * and the first symptom is a query that matches nothing or a system that
 * never runs -- three files away from the cause.
 *
 * Names are checked by PATH rather than through ecs_id(), on purpose. The
 * ecs_id() globals are written by ECS_COMPONENT_DEFINE and would still hold
 * last world's value, so asserting on them can pass while the world is
 * empty. A path lookup asks the world itself.
 *
 * flecs derives the path from the C module name by lower-casing and splitting
 * on capitals: MyeRender2dModule -> mye.render2d.module. Ugly, and worth
 * knowing before a rename quietly moves everything.
 *
 * Headless, so no window and no GL: the draw systems (MyeRenderBegin,
 * MyeCanvasDraw, MyeRender3dPass, ...) are deliberately not registered here
 * and are not asserted on. The render smoke tests cover those. */
#include "core/engine.h"
#include "mye_test.h"

/* Every module mye_init imports, by the path flecs gives it. */
static const char *const module_paths[] = {
    "mye.core",
    "mye.input.module",
    "mye.assets.module",
    "mye.audio.module",
    "mye.transform.module",
    "mye.render2d.module",
    "mye.scene.module",
    "mye.render3d.module",
    "mye.camera.module",
    "mye.canvas.module",
    "mye.debug.overlay.module",
};

/* Load-bearing components, one or more per module: the vocabulary the engine,
 * the examples and the tests all speak. If one of these is missing, its whole
 * module failed to import. */
static const char *const component_paths[] = {
    "mye.core.MyeTime",
    "mye.core.MyeApp",
    "mye.input.module.MyeInput",
    "mye.assets.module.MyeAssets",
    "mye.audio.module.MyeAudio",
    "mye.transform.module.MyePosition2D",
    "mye.transform.module.MyeWorldTransform",
    "mye.render2d.module.MyeSprite",
    "mye.render2d.module.MyeCamera2D",
    "mye.render3d.module.MyeCamera3D",
    "mye.render3d.module.MyeMeshInstance",
    "mye.scene.module.MyeScenes",
    "mye.camera.module.MyeCameraFollow",
    "mye.canvas.module.MyeCanvas",
    "mye.debug.overlay.module.MyeDebugOverlay",
};

/* Systems that run headlessly: simulation, not drawing. */
static const char *const system_paths[] = {
    "mye.core.MyeFrameArenaReset",
    "mye.core.MyeTimeUpdate",
    "mye.audio.module.MyeAudioFlush",
    "mye.transform.module.MyeLocalFrom2D",
    "mye.transform.module.MyeLocalFrom3D",
    "mye.transform.module.MyePropagateTransforms",
    "mye.render2d.module.MyeCapturePrevPositions",
    "mye.render2d.module.MyeBlendRenderTransforms",
    "mye.render2d.module.MyeSpriteAnimUpdate",
    "mye.camera.module.MyeCameraFollowUpdate",
};

#define COUNT_OF(a_) (sizeof(a_) / sizeof((a_)[0]))

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){ .headless = true });
}

TEST(every_engine_module_is_imported)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    for (size_t i = 0; i < COUNT_OF(module_paths); ++i) {
        ecs_entity_t module = ecs_lookup(world, module_paths[i]);
        if (module == 0) {
            MYE_FAIL_("module '%s' is not in the world -- its ECS_IMPORT in "
                      "mye_init did not run, or it was renamed",
                      module_paths[i]);
        }
        /* Not just an entity that happens to share the name: flecs tags every
         * imported module with EcsModule. */
        if (!ecs_has_id(world, module, EcsModule)) {
            MYE_FAIL_("'%s' exists but is not tagged EcsModule",
                      module_paths[i]);
        }
    }

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(each_module_registers_its_components)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    for (size_t i = 0; i < COUNT_OF(component_paths); ++i) {
        ecs_entity_t id = ecs_lookup(world, component_paths[i]);
        if (id == 0) {
            MYE_FAIL_("component '%s' is not registered -- its module did not "
                      "import, or its ECS_COMPONENT_DEFINE is gone",
                      component_paths[i]);
        }
        /* A registered component, not a bare entity: it must carry
         * EcsComponent with real storage behind it. */
        const EcsComponent *info = ecs_get(world, id, EcsComponent);
        if (info == NULL) {
            MYE_FAIL_("'%s' exists but is not a component",
                      component_paths[i]);
        }
        if (info->size <= 0) {
            MYE_FAIL_("component '%s' has size %d", component_paths[i],
                      (int)info->size);
        }
    }

    /* The C-side handles point at those same entities. This is the half that
     * makes ecs_id(MyeSprite) usable from game code, and it is a separate
     * failure: a component can be registered in the world while the global
     * that names it was never written. */
    ASSERT_EQ_U64(ecs_lookup(world, "mye.core.MyeTime"), ecs_id(MyeTime));
    ASSERT_EQ_U64(ecs_lookup(world, "mye.core.MyeApp"), ecs_id(MyeApp));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(each_module_registers_its_headless_systems)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    for (size_t i = 0; i < COUNT_OF(system_paths); ++i) {
        ecs_entity_t sys = ecs_lookup(world, system_paths[i]);
        if (sys == 0) {
            MYE_FAIL_("system '%s' is not registered", system_paths[i]);
        }
        if (!ecs_has_id(world, sys, EcsSystem)) {
            MYE_FAIL_("'%s' exists but is not a system", system_paths[i]);
        }
    }

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* The singletons the engine sets up at import, reachable the way a game
 * reaches them. A module can import and register its components and still
 * leave its singleton unset, which is the shape of bug where every system
 * reading it quietly does nothing. */
TEST(engine_singletons_exist_after_init)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ASSERT_NOT_NULL(ecs_singleton_get(world, MyeTime));
    ASSERT_NOT_NULL(ecs_singleton_get(world, MyeApp));
    ASSERT_NOT_NULL(mye_engine_get(world));

    /* The render phases are entities too, and the draw order depends on them
     * existing before any module registers a system into one. They are
     * created inside MyeCoreImport, so they sit under mye.core; the fixed
     * phase is created after the import returns, so it sits at the root. */
    ASSERT_TRUE(MyeOnFixedUpdate != 0);
    ASSERT_EQ_U64(MyeOnFixedUpdate, ecs_lookup(world, "MyeOnFixedUpdate"));
    ASSERT_EQ_U64(MyeOnCamera, ecs_lookup(world, "mye.core.MyeOnCamera"));
    ASSERT_EQ_U64(MyeOnDrawCanvases,
                  ecs_lookup(world, "mye.core.MyeOnDrawCanvases"));
    ASSERT_EQ_U64(MyeOnDraw3D, ecs_lookup(world, "mye.core.MyeOnDraw3D"));
    ASSERT_EQ_U64(MyeOnDraw2D, ecs_lookup(world, "mye.core.MyeOnDraw2D"));
    ASSERT_EQ_U64(MyeOnDrawUI, ecs_lookup(world, "mye.core.MyeOnDrawUI"));
    ASSERT_EQ_U64(MyeOnRenderEnd,
                  ecs_lookup(world, "mye.core.MyeOnRenderEnd"));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Control: the lookups above have to be able to fail. A path scheme that
 * resolved anything -- or a stale global surviving ecs_fini -- would make
 * every assertion in this file pass for free. */
TEST(a_name_no_module_registers_is_not_found)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ASSERT_EQ_U64(0, ecs_lookup(world, "mye.render2d.module.MyeNotAComponent"));
    ASSERT_EQ_U64(0, ecs_lookup(world, "mye.nosuch.module"));
    /* Components live under their module, not at the root: a lookup that
     * found this would mean the paths above are not testing what they say. */
    ASSERT_EQ_U64(0, ecs_lookup(world, "MyeSprite"));

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(every_engine_module_is_imported),
          TEST_CASE(each_module_registers_its_components),
          TEST_CASE(each_module_registers_its_headless_systems),
          TEST_CASE(engine_singletons_exist_after_init),
          TEST_CASE(a_name_no_module_registers_is_not_found))
