#include "debug/overlay.h"

#include "asset/asset.h"
#include "core/log.h"
#include "render/render2d.h"
#include "scene/scene.h"

#include <raylib.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

ECS_COMPONENT_DECLARE(MyeDebugOverlay);

#define PANEL_W 420
#define LINE_H 18
#define GRAPH_H 46

/* Counting walks queries. Four times a second is often enough to watch a leak
 * grow and slow enough that the digits stay readable. */
#define REFRESH_SECONDS 0.25

/* Queries the overlay owns. Built once at import -- building them per refresh
 * would allocate to report on allocation -- and kept in a singleton rather
 * than a global so two worlds cannot clobber each other. Same arrangement as
 * MyeRender2dState. */
typedef struct MyeDebugOverlayState {
    /* Every entity that holds at least one component or tag. flecs has no
     * public "count the entities" call outside the stats addon, and the stats
     * addon is compiled out of Release, so the overlay counts them itself. */
    ecs_query_t *entities;
} MyeDebugOverlayState;

ECS_COMPONENT_DECLARE(MyeDebugOverlayState);

/* Accessors take a const world because they only read, but flecs types its
 * iterators on a mutable one. The same cast mye_scene_entity_count() makes,
 * and for the same reason: the alternative is to stop describing them as
 * const. ecs_get_world() also unwraps a stage, so these work from inside a
 * system. */
static ecs_world_t *mutable_world(const ecs_world_t *world)
{
    return (ecs_world_t *)(uintptr_t)ecs_get_world(world);
}

/* Visibility is a two-part switch: the panel, and flecs' per-system timing.
 * Timing costs two clock reads per system per frame, which a hidden debug
 * tool has no business spending -- so it follows the panel. Nothing else in
 * the engine touches the flag, so the overlay can own it outright.
 *
 * The frame-time ring is deliberately NOT stopped this way: it is one float a
 * frame, and opening the overlay onto an empty graph would hide the spike you
 * opened it to look at. */
static void set_visible(ecs_world_t *world, MyeDebugOverlay *overlay,
                        bool visible)
{
    overlay->visible = visible;
#ifdef FLECS_STATS
    ecs_measure_system_time(mutable_world(world), visible);
#else
    (void)world;
#endif
}

static void record_frame(MyeDebugOverlay *overlay, float ms)
{
    overlay->frame_ms[overlay->cursor] = ms;
    overlay->cursor = (overlay->cursor + 1) % MYE_OVERLAY_HISTORY;
    if (overlay->samples < MYE_OVERLAY_HISTORY) {
        ++overlay->samples;
    }
    if (ms > overlay->worst_ms) {
        overlay->worst_ms = ms;
    }
}

/* --------------------------------------------------------------- counts -- */

mye_overlay_counts mye_overlay_counts_get(const ecs_world_t *world)
{
    mye_overlay_counts counts = { 0 };
    if (world == NULL) {
        return counts;
    }
    ecs_world_t *w = mutable_world(world);

    const MyeDebugOverlayState *state =
        ecs_singleton_get(w, MyeDebugOverlayState);
    if (state != NULL && state->entities != NULL) {
        ecs_iter_t it = ecs_query_iter(w, state->entities);
        while (ecs_query_next(&it)) {
            counts.entities += it.count;
        }
    }

    /* ecs_each_id is the light-weight form: no query object, so no allocation
     * for a number read four times a second. */
    ecs_iter_t systems = ecs_each_id(w, EcsSystem);
    while (ecs_each_next(&systems)) {
        counts.systems += systems.count;
    }

    counts.drawn_2d = mye_render2d_sprite_counts(w, &counts.interpolated);
    return counts;
}

/* -------------------------------------------------------------- profile -- */

#ifdef FLECS_STATS
static void copy_name(char *dst, size_t cap, const ecs_world_t *world,
                      ecs_entity_t entity)
{
    const char *name = ecs_get_name(world, entity);
    if (name == NULL) {
        name = "(unnamed)";
    }
    size_t length = strlen(name);
    if (length > cap - 1) {
        length = cap - 1;
    }
    memcpy(dst, name, length);
    dst[length] = '\0';
}
#endif

int mye_overlay_system_times(const ecs_world_t *world,
                             mye_overlay_system_time *out, int max,
                             double *total_seconds)
{
    if (total_seconds != NULL) {
        *total_seconds = 0.0;
    }
    if (world == NULL || out == NULL || max <= 0) {
        return 0;
    }

#ifndef FLECS_STATS
    /* Release: the stats addon is not in the binary at all. Saying so with a
     * zero is better than pretending every system took no time. */
    return 0;
#else
    ecs_world_t *w = mutable_world(world);

    /* One scratch record, reused. Each ecs_system_stats_get() advances the
     * struct's own ring cursor and writes that system's cumulative total at
     * the new position, so reading value[t] straight afterwards is correct
     * whatever the other slots hold. Zeroed once, because an uninitialised
     * cursor would index the ring out of bounds. */
    ecs_system_stats_t stats = { 0 };

    int count = 0;
    double total = 0.0;

    ecs_iter_t it = ecs_each_id(w, EcsSystem);
    while (ecs_each_next(&it)) {
        for (int i = 0; i < it.count; ++i) {
            ecs_entity_t system = it.entities[i];
            if (!ecs_system_stats_get(w, system, &stats)) {
                continue;
            }
            double seconds = stats.time_spent.counter.value[stats.query.t];
            total += seconds;

            /* Insertion into a sorted top-N: no sort buffer, no allocation,
             * and N is small enough that the shifting is free. */
            if (count == max && seconds <= out[max - 1].seconds) {
                continue;
            }
            int pos = count < max ? count : max - 1;
            while (pos > 0 && out[pos - 1].seconds < seconds) {
                out[pos] = out[pos - 1];
                --pos;
            }
            out[pos].system = system;
            out[pos].seconds = seconds;
            copy_name(out[pos].name, sizeof out[pos].name, w, system);
            if (count < max) {
                ++count;
            }
        }
    }

    if (total_seconds != NULL) {
        *total_seconds = total;
    }
    return count;
#endif
}

/* ---------------------------------------------------------------- drawing -- */

/* Frame-time graph. The 16.7 ms line is drawn so a spike past the 60 fps
 * budget is visible without reading numbers. */
static void draw_graph(const MyeDebugOverlay *overlay, int x, int y, int w)
{
    DrawRectangle(x, y, w, GRAPH_H, (Color){ 0, 0, 0, 120 });

    float scale = overlay->worst_ms > 16.7f ? overlay->worst_ms : 16.7f;
    int budget_y = y + GRAPH_H -
                   (int)((16.7f / scale) * (float)GRAPH_H);
    DrawLine(x, budget_y, x + w, budget_y, (Color){ 90, 200, 90, 160 });

    for (int i = 0; i < overlay->samples; ++i) {
        /* Oldest first, so the graph scrolls left to right. */
        int index = (overlay->cursor - overlay->samples + i +
                     MYE_OVERLAY_HISTORY * 2) % MYE_OVERLAY_HISTORY;
        float ms = overlay->frame_ms[index];
        int height = (int)((ms / scale) * (float)GRAPH_H);
        if (height < 1) height = 1;
        if (height > GRAPH_H) height = GRAPH_H;

        int bar_x = x + (i * w) / MYE_OVERLAY_HISTORY;
        Color color = ms > 16.7f ? (Color){ 230, 120, 90, 220 }
                                 : (Color){ 120, 190, 230, 200 };
        DrawRectangle(bar_x, y + GRAPH_H - height,
                      (w / MYE_OVERLAY_HISTORY) > 0 ? w / MYE_OVERLAY_HISTORY
                                                    : 1,
                      height, color);
    }
}

static void MyeDebugOverlayDraw(ecs_iter_t *it)
{
    MyeDebugOverlay *overlay = ecs_field(it, MyeDebugOverlay, 0);
    ecs_world_t *world = it->world;

    if (IsKeyPressed(overlay->toggle_key)) {
        set_visible(world, overlay, !overlay->visible);
    }

    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    if (time != NULL) {
        record_frame(overlay, time->delta * 1000.0f);
    }

    if (!overlay->visible) {
        /* Still recording while hidden, so opening it shows real history. */
        return;
    }
    ++overlay->profiled_frames; /* the denominator the profile divides by */

    const mye_engine *engine = mye_engine_get(world);
    const ecs_world_info_t *info = ecs_get_world_info(world);
    mye_asset_stats assets = mye_asset_stats_get(world);
    mye_log_counts logs = mye_log_get_counts();

    /* Recount on a cadence, not per frame: these walk queries, and digits
     * that change 60 times a second cannot be read anyway. */
    double now = time != NULL ? time->elapsed : 0.0;
    if (now >= overlay->next_refresh) {
        overlay->counts = mye_overlay_counts_get(world);
        overlay->top_count = mye_overlay_system_times(
            world, overlay->top, MYE_OVERLAY_TOP_SYSTEMS,
            &overlay->total_system_seconds);
        overlay->next_refresh = now + REFRESH_SECONDS;
    }

    int x = 12;
    int y = 12;
    int line = 0;

    /* The panel is sized from the lines it is about to draw, so a build
     * without the profile does not leave a band of empty background. */
    int line_count = 8; /* frame, ecs, store, draw, assets, scene, log, hint */
    if (time != NULL) {
        ++line_count; /* fixed */
    }
    if (engine != NULL) {
        line_count += 2; /* memory, frame arena */
    }
    if (overlay->top_count > 0) {
        line_count += 1 + overlay->top_count; /* header plus one row each */
    }

    DrawRectangle(x - 6, y - 6, PANEL_W, GRAPH_H + LINE_H * line_count + 16,
                  (Color){ 12, 14, 20, 210 });

    draw_graph(overlay, x, y, PANEL_W - 12);
    y += GRAPH_H + 6;

    /* Per-frame scratch: the overlay must not allocate lastingly to report
     * on allocation. */
    mye_allocator frame = mye_frame_allocator(world);
    char *text = MYE_NEW_ARRAY(frame, char, 128);
    if (text == NULL) {
        return;
    }

#define OVERLAY_LINE(color, ...)                                              \
    do {                                                                      \
        snprintf(text, 128, __VA_ARGS__);                                      \
        DrawText(text, x, y + LINE_H * line, 16, (color));                     \
        ++line;                                                                \
    } while (0)

    const Color label = (Color){ 220, 224, 235, 255 };
    const Color dim = (Color){ 150, 158, 175, 255 };

    float avg = 0.0f;
    for (int i = 0; i < overlay->samples; ++i) {
        avg += overlay->frame_ms[i];
    }
    avg /= (float)(overlay->samples > 0 ? overlay->samples : 1);

    OVERLAY_LINE(label, "frame  %.2f ms avg   worst %.2f ms", (double)avg,
                 (double)overlay->worst_ms);

    if (time != NULL) {
        OVERLAY_LINE(dim, "fixed  %d steps/frame   alpha %.2f   total %llu",
                     time->steps_this_frame, (double)time->alpha,
                     (unsigned long long)time->fixed_step);
    }

    OVERLAY_LINE(dim, "ecs    %d entities   %d systems",
                 overlay->counts.entities, overlay->counts.systems);

    OVERLAY_LINE(dim, "store  %lld tables   %lld merges",
                 (long long)info->table_count,
                 (long long)info->merge_count_total);

    /* The promise from plan/03-rendering.md: the overlay is where opt-in
     * interpolation becomes discoverable, by saying how much of the frame is
     * actually being smoothed. */
    OVERLAY_LINE(dim, "draw   %d sprites   interpolated %d/%d",
                 overlay->counts.drawn_2d, overlay->counts.interpolated,
                 overlay->counts.drawn_2d);

    if (engine != NULL) {
        double live_kb = (double)engine->tracking.live_bytes / 1024.0;
        double peak_kb = (double)engine->tracking.peak_bytes / 1024.0;
        OVERLAY_LINE(dim, "memory %.0f KB live   %.0f KB peak   %zu allocs",
                     live_kb, peak_kb, engine->tracking.live_count);

        double arena_kb = (double)mye_arena_high_water(&engine->frame_arena) /
                          1024.0;
        double arena_cap = (double)mye_arena_capacity(&engine->frame_arena) /
                           1024.0;
        OVERLAY_LINE(dim, "frame arena %.0f / %.0f KB peak", arena_kb,
                     arena_cap);
    }

    OVERLAY_LINE(dim, "assets %u tex   %u snd   %u mdl   %u mus",
                 assets.textures_live, assets.sounds_live, assets.models_live,
                 assets.music_live);

    const char *scene = mye_scene_current(world);
    OVERLAY_LINE(dim, "scene  %s   %d owned entities",
                 scene != NULL ? scene : "(none)",
                 mye_scene_entity_count(world));

    /* Warnings and errors are the line worth colouring: a zero here is a
     * meaningful statement about the run.
     *
     * The colour is computed into a variable first because a compound
     * literal's commas would otherwise split the macro's argument list -- the
     * preprocessor does not know (Color){a, b, c} is one argument. */
    Color log_color = (logs.warn + logs.error) > 0
                          ? (Color){ 235, 170, 90, 255 }
                          : dim;
    OVERLAY_LINE(log_color, "log    %u warn   %u error", logs.warn,
                 logs.error);

    /* The M7 "profile first" half: which system is spending the frame.
     * Totals over the frames the overlay has been open, divided by their
     * count -- not by every frame the world has run, most of which nobody
     * measured.
     *
     * A game sitting at its target framerate puts MyeRenderEnd at the top,
     * because the frame limiter waits inside EndDrawing. That is the honest
     * reading: the frame is going on the display, not on work. */
    if (overlay->top_count > 0) {
        double frames = (double)overlay->profiled_frames;
        if (frames < 1.0) {
            frames = 1.0;
        }
        OVERLAY_LINE(label, "systems  ms/frame   share of %.1f ms in systems",
                     overlay->total_system_seconds * 1000.0 / frames);
        for (int i = 0; i < overlay->top_count; ++i) {
            double ms = overlay->top[i].seconds * 1000.0 / frames;
            double share = overlay->total_system_seconds > 0.0
                               ? overlay->top[i].seconds * 100.0 /
                                     overlay->total_system_seconds
                               : 0.0;
            OVERLAY_LINE(dim, "  %-26.26s %6.2f ms %3.0f%%",
                         overlay->top[i].name, ms, share);
        }
    }

    OVERLAY_LINE(dim, "F3 to hide");

#undef OVERLAY_LINE
}

/* -------------------------------------------------------------- public -- */

void mye_debug_overlay_show(ecs_world_t *world, bool visible)
{
    MyeDebugOverlay *overlay = ecs_singleton_ensure(world, MyeDebugOverlay);
    if (overlay != NULL) {
        set_visible(world, overlay, visible);
        ecs_singleton_modified(world, MyeDebugOverlay);
    }
}

bool mye_debug_overlay_visible(const ecs_world_t *world)
{
    const MyeDebugOverlay *overlay = ecs_singleton_get(world, MyeDebugOverlay);
    return overlay != NULL && overlay->visible;
}

void mye_debug_overlay_reset(ecs_world_t *world)
{
    MyeDebugOverlay *overlay = ecs_singleton_ensure(world, MyeDebugOverlay);
    if (overlay == NULL) {
        return;
    }
    overlay->cursor = 0;
    overlay->samples = 0;
    overlay->worst_ms = 0.0f;
    overlay->next_refresh = 0.0;
    ecs_singleton_modified(world, MyeDebugOverlay);
}

static void overlay_fini(ecs_world_t *world, void *ctx)
{
    (void)world;
    MyeDebugOverlayState *state = (MyeDebugOverlayState *)ctx;
    if (state != NULL && state->entities != NULL) {
        ecs_query_fini(state->entities);
        state->entities = NULL;
    }
}

void MyeDebugOverlayModuleImport(ecs_world_t *world)
{
    ECS_MODULE(world, MyeDebugOverlayModule);

    ECS_COMPONENT_DEFINE(world, MyeDebugOverlay);
    ecs_add_id(world, ecs_id(MyeDebugOverlay), EcsSingleton);
    /* MYE_OVERLAY=1 starts it visible, so a screenshot run can capture it
     * without someone pressing F3. */
    const char *env = getenv("MYE_OVERLAY");
    bool start_visible = env != NULL && env[0] == '1';

    ecs_singleton_set(world, MyeDebugOverlay, { .toggle_key = KEY_F3 });
    /* Through mye_debug_overlay_show rather than the initialiser above, so
     * starting visible also starts the profiling that visibility implies. */
    mye_debug_overlay_show(world, start_visible);

    ECS_COMPONENT_DEFINE(world, MyeDebugOverlayState);
    ecs_add_id(world, ecs_id(MyeDebugOverlayState), EcsSingleton);
    ecs_singleton_set(world, MyeDebugOverlayState, { 0 });
    MyeDebugOverlayState *state =
        ecs_singleton_ensure(world, MyeDebugOverlayState);

    /* EcsAny matches every entity that is in a table, once, whatever it
     * holds. Uncached: the alternative is a cache that every archetype change
     * in the world would touch, to serve a number read four times a second. */
    state->entities = ecs_query(world, {
        .terms = {{ .id = EcsAny }},
        .cache_kind = EcsQueryCacheNone,
    });
    ecs_atfini(world, overlay_fini, state);

    const mye_engine *engine = mye_engine_get(world);
    if (engine != NULL && engine->headless) {
        return; /* nothing to draw on */
    }

    /* MyeOnDrawUI: over the game, under nothing. */
    ECS_SYSTEM(world, MyeDebugOverlayDraw, MyeOnDrawUI, MyeDebugOverlay);
}
