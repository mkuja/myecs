#include "debug/overlay.h"

#include "asset/asset.h"
#include "core/log.h"
#include "scene/scene.h"

#include <raylib.h>

#include <stdio.h>
#include <stdlib.h>

ECS_COMPONENT_DECLARE(MyeDebugOverlay);

#define PANEL_W 320
#define LINE_H 18
#define GRAPH_H 46

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
        overlay->visible = !overlay->visible;
    }

    const MyeTime *time = ecs_singleton_get(world, MyeTime);
    if (time != NULL) {
        record_frame(overlay, time->delta * 1000.0f);
    }

    if (!overlay->visible) {
        /* Still recording while hidden, so opening it shows real history. */
        return;
    }

    const mye_engine *engine = mye_engine_get(world);
    const ecs_world_info_t *info = ecs_get_world_info(world);
    mye_asset_stats assets = mye_asset_stats_get(world);
    mye_log_counts logs = mye_log_get_counts();

    int x = 12;
    int y = 12;
    int line = 0;

    DrawRectangle(x - 6, y - 6, PANEL_W, GRAPH_H + LINE_H * 9 + 16,
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

    OVERLAY_LINE(dim, "ecs    %lld tables   %lld merges",
                 (long long)info->table_count,
                 (long long)info->merge_count_total);

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

    OVERLAY_LINE(dim, "assets %u tex   %u snd   %u mdl",
                 assets.textures_live, assets.sounds_live, assets.models_live);

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

    OVERLAY_LINE(dim, "F3 to hide");

#undef OVERLAY_LINE
}

/* -------------------------------------------------------------- public -- */

void mye_debug_overlay_show(ecs_world_t *world, bool visible)
{
    MyeDebugOverlay *overlay = ecs_singleton_ensure(world, MyeDebugOverlay);
    if (overlay != NULL) {
        overlay->visible = visible;
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
    ecs_singleton_modified(world, MyeDebugOverlay);
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

    ecs_singleton_set(world, MyeDebugOverlay,
                      { .toggle_key = KEY_F3, .visible = start_visible });

    const mye_engine *engine = mye_engine_get(world);
    if (engine != NULL && engine->headless) {
        return; /* nothing to draw on */
    }

    /* MyeOnDrawUI: over the game, under nothing. */
    ECS_SYSTEM(world, MyeDebugOverlayDraw, MyeOnDrawUI, MyeDebugOverlay);
}
