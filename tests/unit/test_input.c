/* Unit tests for the input state machine. Headless: no window, no raylib
 * polling -- the frame_begin/apply/frame_end trio is fed synthetic input,
 * which is exactly what a replay system would do. See plan/09-testing.md. */
#include "input/input.h"
#include "mye_test.h"

#include <raylib.h>

#include <string.h>

enum { ACT_FIRE = 0, ACT_MOVE_X = 1, ACT_JUMP = 2 };

/* One frame: begin, apply the given held state, end. */
static void frame(MyeInput *in, int action, bool down, float value)
{
    mye_input_frame_begin(in);
    mye_input_apply(in, action, down, value);
    mye_input_frame_end(in);
}

TEST(press_and_release_edges)
{
    MyeInput in;
    memset(&in, 0, sizeof in);

    /* Frame 1: pressed this frame. */
    frame(&in, ACT_FIRE, true, 1.0f);
    ASSERT_TRUE(in.down[ACT_FIRE]);
    ASSERT_TRUE(in.pressed[ACT_FIRE]);
    ASSERT_FALSE(in.released[ACT_FIRE]);

    /* Frame 2: still held -- down but no longer a fresh press. */
    frame(&in, ACT_FIRE, true, 1.0f);
    ASSERT_TRUE(in.down[ACT_FIRE]);
    ASSERT_FALSE(in.pressed[ACT_FIRE]);
    ASSERT_FALSE(in.released[ACT_FIRE]);

    /* Frame 3: let go. */
    frame(&in, ACT_FIRE, false, 0.0f);
    ASSERT_FALSE(in.down[ACT_FIRE]);
    ASSERT_FALSE(in.pressed[ACT_FIRE]);
    ASSERT_TRUE(in.released[ACT_FIRE]);

    /* Frame 4: idle. */
    frame(&in, ACT_FIRE, false, 0.0f);
    ASSERT_FALSE(in.down[ACT_FIRE]);
    ASSERT_FALSE(in.pressed[ACT_FIRE]);
    ASSERT_FALSE(in.released[ACT_FIRE]);
}

TEST(tap_within_one_frame_reports_press)
{
    MyeInput in;
    memset(&in, 0, sizeof in);

    frame(&in, ACT_JUMP, true, 1.0f);
    ASSERT_TRUE(in.pressed[ACT_JUMP]);

    /* Released the very next frame: both edges are observable in sequence,
     * never simultaneously. */
    frame(&in, ACT_JUMP, false, 0.0f);
    ASSERT_TRUE(in.released[ACT_JUMP]);
    ASSERT_FALSE(in.pressed[ACT_JUMP]);
}

TEST(axis_values_sum_then_clamp)
{
    MyeInput in;
    memset(&in, 0, sizeof in);

    /* Opposing keys cancel -- the left/right binding pair in action form. */
    mye_input_frame_begin(&in);
    mye_input_apply(&in, ACT_MOVE_X, true, -1.0f);
    mye_input_apply(&in, ACT_MOVE_X, true, 1.0f);
    mye_input_frame_end(&in);
    ASSERT_NEAR(0.0f, in.value[ACT_MOVE_X], 1e-6);
    ASSERT_TRUE(in.down[ACT_MOVE_X]); /* something *is* held */

    /* Several sources pushing the same way clamp instead of exceeding 1. */
    mye_input_frame_begin(&in);
    mye_input_apply(&in, ACT_MOVE_X, true, 1.0f);
    mye_input_apply(&in, ACT_MOVE_X, true, 0.8f);
    mye_input_frame_end(&in);
    ASSERT_NEAR(1.0f, in.value[ACT_MOVE_X], 1e-6);

    mye_input_frame_begin(&in);
    mye_input_apply(&in, ACT_MOVE_X, true, -1.0f);
    mye_input_apply(&in, ACT_MOVE_X, true, -0.8f);
    mye_input_frame_end(&in);
    ASSERT_NEAR(-1.0f, in.value[ACT_MOVE_X], 1e-6);
}

TEST(values_reset_between_frames)
{
    MyeInput in;
    memset(&in, 0, sizeof in);

    frame(&in, ACT_MOVE_X, true, 1.0f);
    ASSERT_NEAR(1.0f, in.value[ACT_MOVE_X], 1e-6);

    /* Nothing applied this frame: the value must not linger. */
    mye_input_frame_begin(&in);
    mye_input_frame_end(&in);
    ASSERT_NEAR(0.0f, in.value[ACT_MOVE_X], 1e-6);
    ASSERT_FALSE(in.down[ACT_MOVE_X]);
    ASSERT_TRUE(in.released[ACT_MOVE_X]);
}

TEST(actions_are_independent)
{
    MyeInput in;
    memset(&in, 0, sizeof in);

    mye_input_frame_begin(&in);
    mye_input_apply(&in, ACT_FIRE, true, 1.0f);
    mye_input_frame_end(&in);

    ASSERT_TRUE(in.down[ACT_FIRE]);
    ASSERT_FALSE(in.down[ACT_JUMP]);
    ASSERT_FALSE(in.pressed[ACT_JUMP]);
    ASSERT_NEAR(0.0f, in.value[ACT_MOVE_X], 1e-6);
}

TEST(out_of_range_actions_are_ignored)
{
    MyeInput in;
    memset(&in, 0, sizeof in);

    /* Must not write outside the arrays -- ASan would catch it if it did. */
    mye_input_frame_begin(&in);
    mye_input_apply(&in, -1, true, 1.0f);
    mye_input_apply(&in, MYE_MAX_ACTIONS, true, 1.0f);
    mye_input_apply(&in, MYE_MAX_ACTIONS + 99, true, 1.0f);
    mye_input_frame_end(&in);

    for (int i = 0; i < MYE_MAX_ACTIONS; ++i) {
        ASSERT_FALSE(in.down[i]);
    }
}

/* --------------------------------------------------- bindings in a world -- */

TEST(bindings_register_against_a_world)
{
    ecs_world_t *world = mye_init(&(mye_config){ .headless = true });
    ASSERT_NOT_NULL(world);

    ASSERT_TRUE(mye_input_bind_key(world, ACT_FIRE, KEY_SPACE));
    ASSERT_TRUE(mye_input_bind_axis_keys(world, ACT_MOVE_X, KEY_LEFT, KEY_RIGHT));

    const MyeInput *in = ecs_singleton_get(world, MyeInput);
    ASSERT_NOT_NULL(in);
    ASSERT_EQ_INT(3, in->binding_count); /* one key + two axis keys */

    /* Out-of-range actions are refused rather than corrupting the table. */
    ASSERT_FALSE(mye_input_bind_key(world, -1, KEY_A));
    ASSERT_FALSE(mye_input_bind_key(world, MYE_MAX_ACTIONS, KEY_A));
    in = ecs_singleton_get(world, MyeInput);
    ASSERT_EQ_INT(3, in->binding_count);

    mye_input_clear_bindings(world);
    in = ecs_singleton_get(world, MyeInput);
    ASSERT_EQ_INT(0, in->binding_count);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(queries_read_synthetic_state)
{
    ecs_world_t *world = mye_init(&(mye_config){ .headless = true });
    ASSERT_NOT_NULL(world);

    MyeInput *in = ecs_singleton_ensure(world, MyeInput);
    ASSERT_NOT_NULL(in);
    in->synthetic = true; /* polling must leave our values alone */

    mye_input_frame_begin(in);
    mye_input_apply(in, ACT_FIRE, true, 1.0f);
    mye_input_frame_end(in);
    ecs_singleton_modified(world, MyeInput);

    ASSERT_TRUE(mye_action_down(world, ACT_FIRE));
    ASSERT_TRUE(mye_action_pressed(world, ACT_FIRE));
    ASSERT_NEAR(1.0f, mye_action_value(world, ACT_FIRE), 1e-6);
    ASSERT_FALSE(mye_action_down(world, ACT_JUMP));

    /* A frame of engine progress must not clobber synthetic input. */
    mye_progress(world, 1.0f / 60.0f);
    ASSERT_TRUE(mye_action_down(world, ACT_FIRE));

    /* Out-of-range queries are safe. */
    ASSERT_FALSE(mye_action_down(world, -1));
    ASSERT_FALSE(mye_action_pressed(world, MYE_MAX_ACTIONS));
    ASSERT_NEAR(0.0f, mye_action_value(world, MYE_MAX_ACTIONS), 1e-6);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(press_and_release_edges),
          TEST_CASE(tap_within_one_frame_reports_press),
          TEST_CASE(axis_values_sum_then_clamp),
          TEST_CASE(values_reset_between_frames),
          TEST_CASE(actions_are_independent),
          TEST_CASE(out_of_range_actions_are_ignored),
          TEST_CASE(bindings_register_against_a_world),
          TEST_CASE(queries_read_synthetic_state))
