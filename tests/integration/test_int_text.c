/* Text rendering: the component, and above all who owns its string.
 *
 * Headless, so it runs everywhere. There is no window, so the draw systems
 * are not registered -- the same rule the sprite pass follows -- and what is
 * asserted here is that the pipeline runs with text entities in it, that the
 * string is copied rather than borrowed, and that every copy is freed. That
 * last one is what mye_shutdown() returning 0 means: the engine allocator is
 * a tracking allocator, so a copy that leaked makes it return 1 and fails
 * the test. Drawing through real GL is covered by test_int_render_smoke.c.
 *
 * See engine/render/text.h and plan/06-assets.md. */
#include "core/engine.h"
#include "render/render2d.h"
#include "render/text.h"
#include "scene/transform.h"
#include "mye_test.h"

#include <raylib.h>

#include <string.h>

#define FIXED_DT (1.0f / 60.0f)

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){ .headless = true });
}

TEST(text_entities_run_through_a_headless_frame)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t hud = mye_text_spawn(world, "SCORE 0", 16.0f, 16.0f, RAYWHITE);
    ASSERT_TRUE(ecs_get(world, hud, MyeText) != NULL);

    /* World space is a flag, never inferred: the same component, drawn by the
     * other pass. */
    ecs_entity_t label = mye_text_spawn(world, "enemy", 400.0f, 300.0f, RED);
    MyeText *t = ecs_get_mut(world, label, MyeText);
    t->world_space = true;
    ecs_modified(world, label, MyeText);

    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(mye_progress(world, FIXED_DT));
    }

    ASSERT_STR_EQ("SCORE 0", ecs_get(world, hud, MyeText)->text);
    ASSERT_TRUE(ecs_get(world, label, MyeText)->world_space);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* If the component merely kept the caller's pointer, a stack buffer would be
 * a dangling read the moment the function returned -- and the usual caller,
 * raylib's TextFormat, hands out a slot in a rotating ring that is overwritten
 * a few calls later. */
TEST(the_component_copies_the_string_it_is_given)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    char scratch[32];
    snprintf(scratch, sizeof scratch, "LIVES 3");

    ecs_entity_t e = mye_text_spawn(world, scratch, 0.0f, 0.0f, WHITE);

    /* Scribble over the caller's buffer. A borrowed pointer would now read
     * "XXXXXXX"; a copy is unaffected. */
    memset(scratch, 'X', sizeof scratch - 1);
    scratch[sizeof scratch - 1] = '\0';

    ASSERT_STR_EQ("LIVES 3", ecs_get(world, e, MyeText)->text);
    /* And the component is not pointing into that buffer. */
    ASSERT_TRUE(ecs_get(world, e, MyeText)->text != scratch);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Every set replaces the previous copy. The leak this guards against is
 * invisible without the tracking allocator: the string is still readable, the
 * game still works, and the old copy is simply unreachable forever. */
TEST(setting_the_string_repeatedly_frees_the_previous_copy)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t e = mye_text_spawn(world, "0", 0.0f, 0.0f, WHITE);

    for (int i = 1; i <= 50; ++i) {
        /* Lengths vary, so a copy that was reused in place rather than
         * reallocated would show up as a mismatch here. */
        char buffer[64];
        snprintf(buffer, sizeof buffer, "%*d", i % 20 + 1, i);
        mye_text_set(world, e, buffer);
        ASSERT_STR_EQ(buffer, ecs_get(world, e, MyeText)->text);
    }

    /* Setting the whole component again, the other way a string is replaced. */
    ecs_set(world, e, MyeText, { .text = "done", .size = 24.0f, .color = RED });
    ASSERT_STR_EQ("done", ecs_get(world, e, MyeText)->text);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Setting the string an entity already holds: the copy has to be made before
 * the previous one is freed, or this reads freed memory. */
TEST(setting_the_string_an_entity_already_holds_is_safe)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t e = mye_text_spawn(world, "unchanged", 0.0f, 0.0f, WHITE);
    mye_text_set(world, e, ecs_get(world, e, MyeText)->text);
    ASSERT_STR_EQ("unchanged", ecs_get(world, e, MyeText)->text);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(deleting_a_text_entity_frees_its_string)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    for (int i = 0; i < 32; ++i) {
        ecs_entity_t e = mye_text_spawn(world, "a string long enough to be a "
                                               "real allocation",
                                        0.0f, 0.0f, WHITE);
        ecs_delete(world, e);
    }
    ASSERT_TRUE(mye_progress(world, FIXED_DT));

    /* Removing the component alone, without deleting the entity. */
    ecs_entity_t kept = mye_text_spawn(world, "removed later", 0, 0, WHITE);
    ecs_remove(world, kept, MyeText);
    ASSERT_TRUE(ecs_get(world, kept, MyeText) == NULL);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Text still on an entity when the world goes away: the destructor runs
 * during ecs_fini, which is where a component that owns memory usually gets
 * forgotten. */
TEST(text_left_set_at_shutdown_is_still_freed)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    mye_text_spawn(world, "never released by hand", 8.0f, 8.0f, WHITE);
    mye_text_spawn(world, "nor this one", 8.0f, 32.0f, WHITE);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Adding a component moves the entity to another table, which moves the
 * MyeText value with it. A move that copied the pointer without clearing the
 * source would have the old storage's destructor free the string the new one
 * is using. */
TEST(moving_between_tables_keeps_the_string)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t e = mye_text_spawn(world, "still here", 0.0f, 0.0f, WHITE);
    ecs_add(world, e, MyeHidden);
    ecs_add(world, e, MyeScale2D);
    ASSERT_STR_EQ("still here", ecs_get(world, e, MyeText)->text);

    ecs_remove(world, e, MyeHidden);
    ASSERT_STR_EQ("still here", ecs_get(world, e, MyeText)->text);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* A set from inside a system is deferred: flecs constructs the value into its
 * command buffer and moves it into place at the merge, which is a different
 * path through the hooks than a set from outside one. */
static void RelabelEveryFrame(ecs_iter_t *it)
{
    const MyeTime *time = ecs_singleton_get(it->world, MyeTime);
    for (int i = 0; i < it->count; ++i) {
        mye_text_set(it->world, it->entities[i],
                     TextFormat("frame %llu",
                                (unsigned long long)(time != NULL ? time->frame
                                                                  : 0)));
    }
}

TEST(setting_the_string_from_a_system_is_owned_too)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t e = mye_text_spawn(world, "before", 0.0f, 0.0f, WHITE);
    ECS_SYSTEM(world, RelabelEveryFrame, EcsOnUpdate, MyeText);

    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(mye_progress(world, FIXED_DT));
    }

    const MyeText *text = ecs_get(world, e, MyeText);
    ASSERT_NOT_NULL(text);
    ASSERT_TRUE(strncmp(text->text, "frame ", 6) == 0);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* Measuring needs glyph metrics, which live in the atlas the GPU holds.
 * Headless there is none, and zero is the honest answer rather than a
 * plausible-looking number a layout would silently be built on. */
TEST(measuring_headless_is_zero)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t e = mye_text_spawn(world, "measure me", 0.0f, 0.0f, WHITE);
    Vector2 size = mye_text_measure(world, ecs_get(world, e, MyeText));
    ASSERT_NEAR(0.0, size.x, 0.0001);
    ASSERT_NEAR(0.0, size.y, 0.0001);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(setting_text_on_something_that_has_none_is_a_no_op)
{
    ecs_world_t *world = make_world();
    ASSERT_TRUE(world != NULL);

    ecs_entity_t plain = mye_entity_new(world);
    mye_text_set(world, plain, "ignored");
    ASSERT_TRUE(ecs_get(world, plain, MyeText) == NULL);

    /* A deleted entity, and a NULL string: gameplay code reaches both. */
    ecs_entity_t gone = mye_text_spawn(world, "here", 0.0f, 0.0f, WHITE);
    ecs_delete(world, gone);
    mye_text_set(world, gone, "after deletion");
    mye_text_set(world, plain, NULL);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(text_entities_run_through_a_headless_frame),
          TEST_CASE(the_component_copies_the_string_it_is_given),
          TEST_CASE(setting_the_string_repeatedly_frees_the_previous_copy),
          TEST_CASE(setting_the_string_an_entity_already_holds_is_safe),
          TEST_CASE(deleting_a_text_entity_frees_its_string),
          TEST_CASE(text_left_set_at_shutdown_is_still_freed),
          TEST_CASE(moving_between_tables_keeps_the_string),
          TEST_CASE(setting_the_string_from_a_system_is_owned_too),
          TEST_CASE(measuring_headless_is_zero),
          TEST_CASE(setting_text_on_something_that_has_none_is_a_no_op))
