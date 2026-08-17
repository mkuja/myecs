#include "core/engine.h"

#include "core/log.h"
#include "debug/overlay.h"

#include "asset/asset.h"
#include "audio/audio.h"
#include "input/input.h"
#include "render/camera.h"
#include "render/render2d.h"
#include "render/render3d.h"
#include "scene/scene.h"
#include "scene/serialize.h"
#include "scene/transform.h"

#include <raylib.h>

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

ECS_COMPONENT_DECLARE(MyeTime);
ECS_COMPONENT_DECLARE(MyeApp);

/* ------------------------------------------------- flecs allocator bridge -- */

/* flecs' OS API takes plain function pointers with no user context, so the
 * allocator it routes to has to be reachable globally. This is the only
 * global allocator in the engine and it exists solely because the library's
 * hook shape demands it -- engine code still passes allocators explicitly.
 *
 * THREADING: flecs calls these from its worker threads once ecs_set_threads()
 * is used (M7). The tracking allocator's counters are not atomic, so tracking
 * must either be made thread-safe or bypassed before workers are enabled.
 * See plan/05-concurrency.md. */
static mye_allocator g_flecs_allocator;

static void *mye_flecs_malloc(ecs_size_t size)
{
    if (size <= 0) {
        return NULL;
    }
    return mye_alloc_hdr(g_flecs_allocator, (size_t)size);
}

static void *mye_flecs_calloc(ecs_size_t size)
{
    if (size <= 0) {
        return NULL;
    }
    return mye_alloc_hdr_zeroed(g_flecs_allocator, (size_t)size);
}

static void *mye_flecs_realloc(void *ptr, ecs_size_t size)
{
    if (size <= 0) {
        mye_free_hdr(g_flecs_allocator, ptr);
        return NULL;
    }
    return mye_resize_hdr(g_flecs_allocator, ptr, (size_t)size);
}

static void mye_flecs_free(void *ptr)
{
    mye_free_hdr(g_flecs_allocator, ptr);
}

static void install_flecs_os_api(mye_allocator allocator)
{
    g_flecs_allocator = allocator;

    ecs_os_set_api_defaults();
    ecs_os_api_t api = ecs_os_get_api();
    api.malloc_ = mye_flecs_malloc;
    api.calloc_ = mye_flecs_calloc;
    api.realloc_ = mye_flecs_realloc;
    api.free_ = mye_flecs_free;
    ecs_os_set_api(&api);
}

/* --------------------------------------------------------- core systems -- */

/* Reclaims the frame arena before anything allocates from it this frame. */
static void MyeFrameArenaReset(ecs_iter_t *it)
{
    MyeApp *app = ecs_field(it, MyeApp, 0);
    mye_arena_reset(&app->engine->frame_arena);
}

static void MyeTimeUpdate(ecs_iter_t *it)
{
    MyeTime *time = ecs_field(it, MyeTime, 0);
    time->delta = (float)it->delta_time;
    time->elapsed += (double)it->delta_time;
    time->frame += 1;
}

ecs_entity_t MyeOnFixedUpdate = 0;
ecs_entity_t MyeOnCamera = 0;
ecs_entity_t MyeOnDraw3D = 0;
ecs_entity_t MyeOnDraw2D = 0;
ecs_entity_t MyeOnDrawUI = 0;
ecs_entity_t MyeOnRenderEnd = 0;

/* Each phase depends on the previous one, which is what fixes draw order
 * independently of the order modules happen to be imported in. */
static void create_render_phases(ecs_world_t *world)
{
    /* Camera systems run here: after transforms are propagated
     * (EcsPostUpdate) and blended for display (EcsPreStore), before anything
     * is drawn. A follow system registered in this phase therefore reads a
     * target's final, interpolated position for this frame -- an ordering
     * guarantee, not a hope. See render/camera.h. */
    MyeOnCamera = ecs_entity(world, {
        .name = "MyeOnCamera",
        .add = ecs_ids(EcsPhase, ecs_dependson(EcsOnStore)) });
    MyeOnDraw3D = ecs_entity(world, {
        .name = "MyeOnDraw3D",
        .add = ecs_ids(EcsPhase, ecs_dependson(MyeOnCamera)) });
    MyeOnDraw2D = ecs_entity(world, {
        .name = "MyeOnDraw2D",
        .add = ecs_ids(EcsPhase, ecs_dependson(MyeOnDraw3D)) });
    MyeOnDrawUI = ecs_entity(world, {
        .name = "MyeOnDrawUI",
        .add = ecs_ids(EcsPhase, ecs_dependson(MyeOnDraw2D)) });
    MyeOnRenderEnd = ecs_entity(world, {
        .name = "MyeOnRenderEnd",
        .add = ecs_ids(EcsPhase, ecs_dependson(MyeOnDrawUI)) });
}

static void MyeCoreImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeCore);

    ECS_COMPONENT_DEFINE(world, MyeTime);
    ECS_COMPONENT_DEFINE(world, MyeApp);

    /* The Singleton trait makes queries resolve these to the one instance
     * living on the component entity, so systems just name the component. */
    ecs_add_id(world, ecs_id(MyeTime), EcsSingleton);
    ecs_add_id(world, ecs_id(MyeApp), EcsSingleton);

    ECS_SYSTEM(world, MyeFrameArenaReset, EcsPreFrame, MyeApp);
    ECS_SYSTEM(world, MyeTimeUpdate, EcsOnLoad, MyeTime);

    create_render_phases(world);
}

/* Custom phase + pipeline for fixed systems. Kept out of the main pipeline
 * so ecs_progress() never runs them; the driver above runs them instead. */
static ecs_entity_t create_fixed_pipeline(ecs_world_t *world)
{
    /* Deliberately NOT tagged EcsPhase. The builtin pipeline matches any
     * system whose DependsOn target carries that tag, so tagging this one
     * would make ecs_progress() run every fixed system a second time each
     * frame -- on top of the steps the driver already ran. */
    MyeOnFixedUpdate = ecs_entity(world, { .name = "MyeOnFixedUpdate" });

    return ecs_pipeline(world, {
        .entity = ecs_entity(world, { .name = "MyeFixedPipeline" }),
        .query.terms = {
            { .id = EcsSystem },
            { .id = ecs_dependson(MyeOnFixedUpdate) },
            { .id = EcsDisabled, .src.id = EcsUp, .trav = EcsDependsOn,
              .oper = EcsNot },
            { .id = EcsDisabled, .src.id = EcsUp, .trav = EcsChildOf,
              .oper = EcsNot },
        },
    });
}

/* -------------------------------------------------------------- lifecycle -- */

ecs_world_t *mye_init(const mye_config *config)
{
    mye_config cfg = config != NULL ? *config : (mye_config){ 0 };
    if (cfg.width <= 0) cfg.width = 1280;
    if (cfg.height <= 0) cfg.height = 720;
    if (cfg.title == NULL) cfg.title = "myecs";
    if (cfg.target_fps <= 0) cfg.target_fps = 60;
    if (cfg.frame_arena_bytes == 0) cfg.frame_arena_bytes = 1024 * 1024;
    if (cfg.fixed_dt <= 0.0f) cfg.fixed_dt = 1.0f / 60.0f;
    if (cfg.max_steps_per_frame <= 0) cfg.max_steps_per_frame = 5;
    if (cfg.explorer_port == 0) cfg.explorer_port = 27750;

    /* On by default in debug builds only: an HTTP server has no business in
     * a shipped game. */
    /* Windowed debug builds only. Not release -- a shipped game has no
     * business running an HTTP server -- and not headless, because the test
     * suite creates hundreds of worlds that would each bind the same port
     * and log the same line. Tests opt in via mye_config.explorer. */
#if defined(MYE_DEBUG)
    bool explorer = !cfg.headless;
#else
    bool explorer = false;
#endif
    if (cfg.explorer) {
        explorer = true;
    }
    const char *explorer_env = getenv("MYE_EXPLORER");
    if (explorer_env != NULL) {
        explorer = explorer_env[0] == '1';
    }

    mye_allocator base = mye_allocator_valid(cfg.allocator)
                             ? cfg.allocator
                             : mye_heap_allocator();

    /* The engine struct itself comes from the base allocator, so it is not
     * counted by its own tracking and the leak report can reach zero. */
    mye_engine *engine = MYE_NEW(base, mye_engine);
    if (engine == NULL) {
        return NULL;
    }

    engine->base = base;
    engine->width = cfg.width;
    engine->height = cfg.height;
    mye_tracking_init(&engine->tracking, base);
    engine->allocator = mye_tracking_allocator(&engine->tracking);
    engine->headless = cfg.headless;
    engine->window_open = false;
    engine->max_frames = cfg.max_frames;

    engine->screenshot_path = cfg.screenshot_path;
    const char *screenshot_env = getenv("MYE_SCREENSHOT");
    if (screenshot_env != NULL && screenshot_env[0] != '\0') {
        engine->screenshot_path = screenshot_env;
    }

    /* Environment override, so an example can be smoke-tested unmodified. */
    const char *max_frames_env = getenv("MYE_MAX_FRAMES");
    if (max_frames_env != NULL) {
        char *end = NULL;
        unsigned long long parsed = strtoull(max_frames_env, &end, 10);
        if (end != max_frames_env && *end == '\0') {
            engine->max_frames = (uint64_t)parsed;
        }
    }

    if (!mye_arena_init(&engine->frame_arena, engine->allocator,
                        cfg.frame_arena_bytes)) {
        MYE_DELETE(base, engine);
        return NULL;
    }

    install_flecs_os_api(engine->allocator);
    /* After the os api is set, since installing hooks reads and rewrites it. */
    mye_log_install_hooks();
    mye_rl_alloc_set(engine->allocator);

    ecs_world_t *world = ecs_init();
    if (world == NULL) {
        mye_arena_deinit(&engine->frame_arena);
        MYE_DELETE(base, engine);
        return NULL;
    }

    ECS_IMPORT(world, MyeCore);
    engine->fixed_pipeline = create_fixed_pipeline(world);

    ecs_singleton_set(world, MyeApp, { .engine = engine });
    ecs_singleton_set(world, MyeTime,
                      { .fixed_dt = cfg.fixed_dt,
                        .max_steps_per_frame = cfg.max_steps_per_frame });

    /* The window must exist before the asset and render modules import: they
     * create GPU resources (the placeholder texture) and open the audio
     * device, both of which need an initialised platform. */
    if (!cfg.headless) {
        InitWindow(cfg.width, cfg.height, cfg.title);
        if (!IsWindowReady()) {
            ecs_fini(world);
            mye_arena_deinit(&engine->frame_arena);
            MYE_DELETE(base, engine);
            return NULL;
        }
        SetTargetFPS(cfg.target_fps);
        engine->window_open = true;
    }

    /* Workers are opt-in and off by default: correctness first, and every
     * system must be audited before it is marked multi_threaded. */
    if (cfg.worker_threads > 1) {
        ecs_set_threads(world, cfg.worker_threads);
    }

    /* Serve the world to the flecs Explorer. Headless worlds get it too --
     * inspecting a test run is exactly when it is most useful. */
    if (explorer) {
        ecs_singleton_set(world, EcsRest, { .port = cfg.explorer_port });
        mye_log_info("explorer: https://www.flecs.dev/explorer/?host=localhost:%u",
                     (unsigned)cfg.explorer_port);
    }

    ECS_IMPORT(world, MyeInputModule);
    ECS_IMPORT(world, MyeAssetsModule);
    ECS_IMPORT(world, MyeAudioModule);
    ECS_IMPORT(world, MyeTransformModule);
    ECS_IMPORT(world, MyeRender2dModule);
    ECS_IMPORT(world, MyeSceneModule);
    mye_serialize_register_engine_components(world);
    ECS_IMPORT(world, MyeRender3dModule);
    /* After both renderers: the camera components belong to them, and a
     * query cannot name a component that is not registered yet. The
     * renderers only call into this module at runtime, so nothing needs it
     * earlier. */
    ECS_IMPORT(world, MyeCameraModule);
    ECS_IMPORT(world, MyeDebugOverlayModule);

    return world;
}

/* ------------------------------------------------------------------ frame -- */

bool mye_progress(ecs_world_t *world, float dt)
{
    mye_engine *engine = mye_engine_get(world);
    if (engine == NULL) {
        return false;
    }

    /* Scene switches land here, at a frame boundary: deleting a scene's
     * entities while systems are iterating them is how engines crash. */
    mye_scene_apply_pending(world);

    /* Input is sampled here rather than from a system because the fixed steps
     * below must see *this* frame's input, and they cannot run from inside a
     * pipeline -- flecs forbids re-entering pipeline execution. */
    mye_input_poll(world);

    /* Fixed timestep ("Fix Your Timestep", Gaffer on Games): consume whole
     * steps of simulation, keep the remainder for next frame, and expose it
     * as `alpha` so rendering can interpolate. */
    MyeTime *time = ecs_singleton_ensure(world, MyeTime);
    if (time != NULL && time->fixed_dt > 0.0f) {
        time->accumulator += dt;

        int steps = 0;
        while (time->accumulator >= time->fixed_dt &&
               steps < time->max_steps_per_frame) {
            ecs_run_pipeline(world, engine->fixed_pipeline, time->fixed_dt);
            time->accumulator -= time->fixed_dt;
            ++steps;
            ++time->fixed_step;
        }

        /* After a long stall, drop the backlog rather than trying to catch up
         * forever -- otherwise every later frame inherits the debt. */
        if (steps == time->max_steps_per_frame &&
            time->accumulator >= time->fixed_dt) {
            time->accumulator = 0.0f;
        }

        time->steps_this_frame = steps;
        time->alpha = time->accumulator / time->fixed_dt;
        ecs_singleton_modified(world, MyeTime);
    }

    /* Everything else: EcsPreFrame .. EcsOnStore, rendering included. */
    bool result = ecs_progress(world, dt);

    /* The end-of-run screenshot is taken by MyeRenderEnd, inside the frame
     * before the buffer swap. Reading after ecs_progress -- as this used to
     * -- gets the back buffer after the swap, which is the PREVIOUS frame on
     * most drivers. */

    return result;
}

int mye_shutdown(ecs_world_t *world)
{
    if (world == NULL) {
        return 1;
    }

    mye_engine *engine = mye_engine_get(world);
    if (engine == NULL) {
        ecs_fini(world);
        return 1;
    }

    /* Order matters: flecs frees through our allocator, so the world has to
     * go before the arena and before the leak report. */
    ecs_fini(world);

    if (engine->window_open) {
        CloseWindow();
        engine->window_open = false;
    }

    mye_arena_deinit(&engine->frame_arena);

    bool leaked = mye_tracking_has_leaks(&engine->tracking);
    if (leaked) {
        mye_tracking_report(&engine->tracking, "engine shutdown: LEAK");
    }

    /* The engine's tracking state dies with it, so point the (necessarily
     * global) flecs allocator back at the heap. Anything flecs frees after
     * this -- there should be nothing -- hits a valid allocator instead of
     * freed memory. */
    g_flecs_allocator = mye_heap_allocator();
    mye_rl_alloc_reset();

    mye_allocator base = engine->base;
    MYE_DELETE(base, engine);
    return leaked ? 1 : 0;
}

/* -------------------------------------------------------------- accessors -- */

double mye_time_now(void)
{
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) == 0) {
        return 0.0;
    }
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

ecs_entity_t mye_entity_new(ecs_world_t *world)
{
    ecs_entity_t e = ecs_new(world);

    /* Tag it for the active scene, if there is one. Done explicitly rather
     * than through flecs' ecs_set_with: that mechanism is meant for a
     * bracketed burst of creations and does not survive a frame cycle, so
     * gameplay-spawned entities silently escaped scene ownership. */
    ecs_entity_t owner = mye_scene_owner(world);
    if (owner != 0) {
        ecs_add_pair(world, e, mye_scene_relationship(), owner);
    }
    return e;
}

mye_engine *mye_engine_get(const ecs_world_t *world)
{
    if (world == NULL) {
        return NULL;
    }
    const MyeApp *app = ecs_singleton_get(world, MyeApp);
    return app != NULL ? app->engine : NULL;
}

mye_allocator mye_frame_allocator(const ecs_world_t *world)
{
    mye_engine *engine = mye_engine_get(world);
    if (engine == NULL) {
        return (mye_allocator){ 0 };
    }
    return mye_arena_allocator(&engine->frame_arena);
}

mye_allocator mye_allocator_of(const ecs_world_t *world)
{
    mye_engine *engine = mye_engine_get(world);
    return engine != NULL ? engine->allocator : (mye_allocator){ 0 };
}

bool mye_running(const ecs_world_t *world)
{
    mye_engine *engine = mye_engine_get(world);
    if (engine == NULL) {
        return false;
    }

    if (engine->max_frames > 0) {
        const MyeTime *now = ecs_singleton_get(world, MyeTime);
        if (now != NULL && now->frame >= engine->max_frames) {
            return false;
        }
    }

    if (engine->headless) {
        return true;
    }
    return !WindowShouldClose();
}
