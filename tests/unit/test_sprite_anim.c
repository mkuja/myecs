/* Unit tests for flipbook animation. Pure state machine and pure frame math,
 * so no window and no world. See plan/09-testing.md. */
#include "mye_test.h"
#include "render/render2d.h"

static MyeSpriteAnim make_anim(int frames, float fps, bool loop)
{
    return (MyeSpriteAnim){
        .first_frame = { 0.0f, 0.0f, 16.0f, 16.0f },
        .columns = 4,
        .frame_count = frames,
        .fps = fps,
        .loop = loop,
        .playing = true,
    };
}

/* --------------------------------------------------------- frame layout -- */

TEST(atlas_frames_walk_the_grid)
{
    Rectangle first = { 0.0f, 0.0f, 16.0f, 16.0f };

    Rectangle f0 = mye_atlas_frame(first, 4, 0);
    ASSERT_NEAR(0.0, f0.x, 1e-6);
    ASSERT_NEAR(0.0, f0.y, 1e-6);

    /* Across the first row. */
    ASSERT_NEAR(16.0, mye_atlas_frame(first, 4, 1).x, 1e-6);
    ASSERT_NEAR(48.0, mye_atlas_frame(first, 4, 3).x, 1e-6);

    /* Frame 4 wraps to the start of the second row. */
    Rectangle f4 = mye_atlas_frame(first, 4, 4);
    ASSERT_NEAR(0.0, f4.x, 1e-6);
    ASSERT_NEAR(16.0, f4.y, 1e-6);

    /* Frame 9 = row 2, column 1. */
    Rectangle f9 = mye_atlas_frame(first, 4, 9);
    ASSERT_NEAR(16.0, f9.x, 1e-6);
    ASSERT_NEAR(32.0, f9.y, 1e-6);

    /* Size is constant across frames. */
    ASSERT_NEAR(16.0, f9.width, 1e-6);
    ASSERT_NEAR(16.0, f9.height, 1e-6);
}

TEST(atlas_frames_respect_an_offset_origin)
{
    /* Frame 0 need not sit at the texture's top-left: an atlas may pack
     * several animations side by side. */
    Rectangle first = { 64.0f, 32.0f, 8.0f, 8.0f };

    Rectangle f0 = mye_atlas_frame(first, 2, 0);
    ASSERT_NEAR(64.0, f0.x, 1e-6);
    ASSERT_NEAR(32.0, f0.y, 1e-6);

    Rectangle f3 = mye_atlas_frame(first, 2, 3); /* row 1, column 1 */
    ASSERT_NEAR(72.0, f3.x, 1e-6);
    ASSERT_NEAR(40.0, f3.y, 1e-6);
}

TEST(atlas_frames_tolerate_bad_input)
{
    Rectangle first = { 0.0f, 0.0f, 16.0f, 16.0f };
    /* Zero columns would divide by zero; negative index would read backwards. */
    ASSERT_NEAR(0.0, mye_atlas_frame(first, 0, 5).x, 1e-6);
    ASSERT_NEAR(0.0, mye_atlas_frame(first, 4, -3).x, 1e-6);
}

/* ------------------------------------------------------- advance timing -- */

TEST(frames_advance_at_the_requested_rate)
{
    MyeSpriteAnim anim = make_anim(4, 10.0f, true); /* 0.1 s per frame */

    /* Not enough time yet. */
    ASSERT_FALSE(mye_sprite_anim_advance(&anim, 0.05f));
    ASSERT_EQ_INT(0, anim.current);

    /* Crossing the threshold steps exactly one frame. */
    ASSERT_TRUE(mye_sprite_anim_advance(&anim, 0.06f));
    ASSERT_EQ_INT(1, anim.current);

    /* Leftover time carries, so the rate does not drift. */
    ASSERT_FALSE(mye_sprite_anim_advance(&anim, 0.05f));
    ASSERT_TRUE(mye_sprite_anim_advance(&anim, 0.05f));
    ASSERT_EQ_INT(2, anim.current);
}

TEST(a_long_frame_skips_ahead_rather_than_lagging)
{
    /* After a stall the animation should be where wall-clock says it is, not
     * one frame further along. */
    MyeSpriteAnim anim = make_anim(8, 10.0f, false);

    ASSERT_TRUE(mye_sprite_anim_advance(&anim, 0.35f)); /* 3.5 frames */
    ASSERT_EQ_INT(3, anim.current);
    ASSERT_TRUE(anim.playing);
}

TEST(looping_wraps_forever)
{
    MyeSpriteAnim anim = make_anim(3, 10.0f, true);

    for (int i = 0; i < 3; ++i) {
        mye_sprite_anim_advance(&anim, 0.1f);
    }
    ASSERT_EQ_INT(0, anim.current); /* wrapped back to the start */
    ASSERT_TRUE(anim.playing);
    ASSERT_FALSE(anim.finished);

    /* A huge step still lands on a valid frame. */
    mye_sprite_anim_advance(&anim, 100.0f);
    ASSERT_TRUE(anim.current >= 0 && anim.current < 3);
    ASSERT_TRUE(anim.playing);
}

TEST(non_looping_holds_the_last_frame_and_reports_finished)
{
    MyeSpriteAnim anim = make_anim(3, 10.0f, false);

    mye_sprite_anim_advance(&anim, 0.1f);
    ASSERT_EQ_INT(1, anim.current);
    ASSERT_FALSE(anim.finished);

    /* Reaching the last frame is NOT finished: that frame still owes its
     * display time. Finishing early would despawn an explosion before its
     * final frame was ever drawn. */
    mye_sprite_anim_advance(&anim, 0.1f);
    ASSERT_EQ_INT(2, anim.current);
    ASSERT_FALSE(anim.finished);
    ASSERT_TRUE(anim.playing);

    /* Once the last frame's duration elapses, it finishes and holds. */
    mye_sprite_anim_advance(&anim, 0.1f);
    ASSERT_EQ_INT(2, anim.current);
    ASSERT_TRUE(anim.finished);
    ASSERT_FALSE(anim.playing);

    /* Further time changes nothing: it holds, never wraps or runs past. */
    ASSERT_FALSE(mye_sprite_anim_advance(&anim, 10.0f));
    ASSERT_EQ_INT(2, anim.current);
}

TEST(restart_replays_from_the_beginning)
{
    MyeSpriteAnim anim = make_anim(3, 10.0f, false);
    mye_sprite_anim_advance(&anim, 1.0f);
    ASSERT_TRUE(anim.finished);

    mye_sprite_anim_restart(&anim);
    ASSERT_EQ_INT(0, anim.current);
    ASSERT_TRUE(anim.playing);
    ASSERT_FALSE(anim.finished);
    ASSERT_NEAR(0.0, anim.elapsed, 1e-6);
}

TEST(degenerate_animations_do_nothing)
{
    MyeSpriteAnim stopped = make_anim(4, 10.0f, true);
    stopped.playing = false;
    ASSERT_FALSE(mye_sprite_anim_advance(&stopped, 10.0f));
    ASSERT_EQ_INT(0, stopped.current);

    MyeSpriteAnim no_frames = make_anim(0, 10.0f, true);
    ASSERT_FALSE(mye_sprite_anim_advance(&no_frames, 10.0f));

    MyeSpriteAnim no_rate = make_anim(4, 0.0f, true);
    ASSERT_FALSE(mye_sprite_anim_advance(&no_rate, 10.0f));

    ASSERT_FALSE(mye_sprite_anim_advance(NULL, 1.0f));
    mye_sprite_anim_restart(NULL); /* no crash */
}

TEST_MAIN(TEST_CASE(atlas_frames_walk_the_grid),
          TEST_CASE(atlas_frames_respect_an_offset_origin),
          TEST_CASE(atlas_frames_tolerate_bad_input),
          TEST_CASE(frames_advance_at_the_requested_rate),
          TEST_CASE(a_long_frame_skips_ahead_rather_than_lagging),
          TEST_CASE(looping_wraps_forever),
          TEST_CASE(non_looping_holds_the_last_frame_and_reports_finished),
          TEST_CASE(restart_replays_from_the_beginning),
          TEST_CASE(degenerate_animations_do_nothing))
