/* Integration tests for audio queueing and sprite animation, driven through
 * the real engine loop and the real game. Headless: no audio device, so
 * nothing is audible -- what is asserted is that the queue and the animation
 * state machine behave the same with or without one. See plan/09-testing.md. */
#include "asteroids.h"
#include "audio/audio.h"
#include "mye_test.h"
#include "render/render2d.h"

#include <raylib.h>

#define FIXED_DT (1.0f / 60.0f)

static ecs_world_t *make_world(void)
{
    return mye_init(&(mye_config){ .headless = true, .asset_workers = -1 });
}

static int count_of(ecs_world_t *world, ecs_entity_t component)
{
    ecs_query_t *q = ecs_query(world, { .terms = {{ .id = component }} });
    if (q == NULL) {
        return -1;
    }
    int total = 0;
    ecs_iter_t it = ecs_query_iter(world, q);
    while (ecs_query_next(&it)) {
        total += it.count;
    }
    ecs_query_fini(q);
    return total;
}

/* ------------------------------------------------------------- audio -- */

TEST(identical_sounds_in_one_frame_collapse)
{
    /* The reason the queue exists: a fixed-step system that fires during a
     * catch-up frame would otherwise stack several copies of one sound into
     * a single blast. */
    MyeAudio audio = { .master_volume = 1.0f };
    mye_sound sfx = { .index = 3, .generation = 1 };

    ASSERT_TRUE(mye_audio_enqueue(&audio, sfx, 0.5f, 1.0f));
    ASSERT_TRUE(mye_audio_enqueue(&audio, sfx, 0.5f, 1.0f));
    ASSERT_TRUE(mye_audio_enqueue(&audio, sfx, 0.5f, 1.0f));
    ASSERT_EQ_INT(1, audio.queued);

    /* Different sounds still queue separately. */
    mye_sound other = { .index = 4, .generation = 1 };
    ASSERT_TRUE(mye_audio_enqueue(&audio, other, 1.0f, 1.0f));
    ASSERT_EQ_INT(2, audio.queued);
}

TEST(the_loudest_duplicate_wins)
{
    MyeAudio audio = { .master_volume = 1.0f };
    mye_sound sfx = { .index = 1, .generation = 1 };

    mye_audio_enqueue(&audio, sfx, 0.2f, 1.0f);
    mye_audio_enqueue(&audio, sfx, 0.9f, 1.0f);
    mye_audio_enqueue(&audio, sfx, 0.4f, 1.0f);

    ASSERT_EQ_INT(1, audio.queued);
    ASSERT_NEAR(0.9, audio.queue[0].volume, 1e-6);
}

TEST(a_full_queue_drops_rather_than_overflows)
{
    MyeAudio audio = { .master_volume = 1.0f };

    for (int i = 0; i < MYE_MAX_QUEUED_SOUNDS; ++i) {
        mye_sound s = { .index = (uint32_t)i, .generation = 1 };
        ASSERT_TRUE(mye_audio_enqueue(&audio, s, 1.0f, 1.0f));
    }
    ASSERT_EQ_INT(MYE_MAX_QUEUED_SOUNDS, audio.queued);

    mye_sound overflow = { .index = 999, .generation = 1 };
    ASSERT_FALSE(mye_audio_enqueue(&audio, overflow, 1.0f, 1.0f));
    ASSERT_EQ_INT(MYE_MAX_QUEUED_SOUNDS, audio.queued);
    ASSERT_EQ_INT(1, (int)audio.dropped);

    /* Invalid handles never occupy a slot. */
    mye_sound invalid = { 0 };
    ASSERT_FALSE(mye_audio_enqueue(&audio, invalid, 1.0f, 1.0f));
}

TEST(the_queue_drains_every_frame)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_sound sfx = { .index = 2, .generation = 1 };
    mye_sound_play(world, sfx);

    const MyeAudio *audio = ecs_singleton_get(world, MyeAudio);
    ASSERT_NOT_NULL(audio);
    ASSERT_EQ_INT(1, audio->queued);

    mye_progress(world, FIXED_DT);

    /* Drained even headless, so a silent build cannot accumulate a backlog. */
    audio = ecs_singleton_get(world, MyeAudio);
    ASSERT_EQ_INT(0, audio->queued);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST(muting_still_drains_the_queue)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    mye_audio_set_muted(world, true);
    mye_sound sfx = { .index = 5, .generation = 1 };
    mye_sound_play(world, sfx);
    mye_progress(world, FIXED_DT);

    /* Otherwise unmuting would fire off every sound banked while silent. */
    const MyeAudio *audio = ecs_singleton_get(world, MyeAudio);
    ASSERT_EQ_INT(0, audio->queued);
    ASSERT_TRUE(audio->muted);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* --------------------------------------------------------- animation -- */

typedef struct AnimProbe {
    char unused;
} AnimProbe;

ECS_COMPONENT_DECLARE(AnimProbe);

TEST(the_animation_system_drives_the_sprite_source_rect)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    ECS_COMPONENT_DEFINE(world, AnimProbe);

    ecs_entity_t e = ecs_new(world);
    ecs_set(world, e, AnimProbe, { 0 });
    ecs_set(world, e, MyePosition2D, { 0.0f, 0.0f });
    ecs_set(world, e, MyeSprite,
            { .source = { 0.0f, 0.0f, 16.0f, 16.0f }, .tint = WHITE });
    ecs_set(world, e, MyeSpriteAnim,
            { .first_frame = { 0.0f, 0.0f, 16.0f, 16.0f },
              .columns = 4,
              .frame_count = 4,
              .fps = 60.0f, /* one frame per fixed step */
              .loop = true,
              .playing = true });

    /* Frame 0 to start. */
    const MyeSprite *sprite = ecs_get(world, e, MyeSprite);
    ASSERT_NEAR(0.0, sprite->source.x, 1e-6);

    mye_progress(world, FIXED_DT);
    sprite = ecs_get(world, e, MyeSprite);
    const MyeSpriteAnim *anim = ecs_get(world, e, MyeSpriteAnim);
    ASSERT_EQ_INT(1, anim->current);
    /* The system wrote the new frame's rect into the sprite. */
    ASSERT_NEAR(16.0, sprite->source.x, 1e-6);

    mye_progress(world, FIXED_DT);
    mye_progress(world, FIXED_DT);
    sprite = ecs_get(world, e, MyeSprite);
    ASSERT_NEAR(48.0, sprite->source.x, 1e-6); /* frame 3 */

    /* Looping wraps back to frame 0. */
    mye_progress(world, FIXED_DT);
    sprite = ecs_get(world, e, MyeSprite);
    ASSERT_NEAR(0.0, sprite->source.x, 1e-6);

    ASSERT_EQ_INT(0, mye_shutdown(world));
}

/* ------------------------------------------- both, inside the real game -- */

TEST(destroying_a_rock_spawns_an_explosion_that_cleans_itself_up)
{
    ecs_world_t *world = make_world();
    ASSERT_NOT_NULL(world);

    SetRandomSeed(99);
    asteroids_setup(world);

    MyeInput *input = ecs_singleton_ensure(world, MyeInput);
    input->synthetic = true;
    ecs_singleton_modified(world, MyeInput);

    ASSERT_EQ_INT(0, count_of(world, ecs_id(Explosion)));

    /* Stage a rock in front of the ship and shoot it. */
    ecs_delete_with(world, ecs_id(Rock));
    GameState *state = ecs_singleton_ensure(world, GameState);
    state->rocks_alive = 0;

    ecs_entity_t rock = ecs_new(world);
    ecs_set(world, rock, MyePosition2D,
            { SCREEN_W * 0.5f + 100.0f, SCREEN_H * 0.5f });
    ecs_set(world, rock, MyeRotation2D, { 0.0f });
    ecs_set(world, rock, Velocity, { 0.0f, 0.0f });
    ecs_set(world, rock, Collider, { 36.0f });
    ecs_set(world, rock, Rock, { 3 });
    state->rocks_alive = 1;

    ecs_query_t *ships = ecs_query(world, {
        .terms = {{ .id = ecs_id(MyeRotation2D) }, { .id = ecs_id(Ship) }},
    });
    ASSERT_NOT_NULL(ships);
    ecs_iter_t it = ecs_query_iter(world, ships);
    while (ecs_query_next(&it)) {
        MyeRotation2D *rot = ecs_field(&it, MyeRotation2D, 0);
        for (int i = 0; i < it.count; ++i) {
            rot[i].angle = 0.0f;
        }
    }
    ecs_query_fini(ships);

    mye_input_frame_begin(input);
    mye_input_apply(input, ACT_FIRE, true, 1.0f);
    mye_input_frame_end(input);
    ecs_singleton_modified(world, MyeInput);
    mye_progress(world, FIXED_DT);

    /* Watch for the explosion rather than guessing when it appears: the
     * bullet takes ~12 frames to cross, and the explosion only lives ~17,
     * so a fixed wait can easily miss the window entirely. */
    bool saw_explosion = false;
    for (int i = 0; i < 40 && !saw_explosion; ++i) {
        mye_input_frame_begin(input);
        mye_input_frame_end(input);
        ecs_singleton_modified(world, MyeInput);
        mye_progress(world, FIXED_DT);
        saw_explosion = count_of(world, ecs_id(Explosion)) > 0;
    }

    ASSERT_TRUE(saw_explosion);
    /* And the rock it came from is gone, with the score to show for it. */
    const GameState *after = ecs_singleton_get(world, GameState);
    ASSERT_TRUE(after->score > 0);

    /* The animation runs out and the explosions delete themselves, so they
     * cannot accumulate over a long game. */
    for (int i = 0; i < 60; ++i) {
        mye_progress(world, FIXED_DT);
    }
    ASSERT_EQ_INT(0, count_of(world, ecs_id(Explosion)));

    asteroids_teardown(world);
    ASSERT_EQ_INT(0, mye_shutdown(world));
}

TEST_MAIN(TEST_CASE(identical_sounds_in_one_frame_collapse),
          TEST_CASE(the_loudest_duplicate_wins),
          TEST_CASE(a_full_queue_drops_rather_than_overflows),
          TEST_CASE(the_queue_drains_every_frame),
          TEST_CASE(muting_still_drains_the_queue),
          TEST_CASE(the_animation_system_drives_the_sprite_source_rect),
          TEST_CASE(destroying_a_rock_spawns_an_explosion_that_cleans_itself_up))
