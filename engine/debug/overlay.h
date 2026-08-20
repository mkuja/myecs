/* On-screen debug overlay. See plan/00-overview.md (Tier 2).
 *
 * Answers the questions you actually ask while a game misbehaves: is the
 * frame time spiking, is memory growing, are entities accumulating, did
 * anything warn, is the fixed step keeping up, and -- when the answer is "the
 * frame time" -- which system is spending it.
 *
 * Toggled with F3 by default. It reads the key directly rather than through
 * the action system, because a debug tool should work in a game that has not
 * bound anything -- and should not consume one of the game's action slots.
 *
 * The countable half of what it shows is reachable without a window, through
 * the accessors below. Drawing cannot be tested headlessly; counting can, so
 * the counting is kept out of the draw call.
 */
#ifndef MYE_DEBUG_OVERLAY_H
#define MYE_DEBUG_OVERLAY_H

#include "core/engine.h"

#define MYE_OVERLAY_HISTORY 120

/* How many systems the profile lists, and how much of a system name it keeps.
 * Both are fixed so the overlay's state is a plain struct with no allocation
 * behind it -- a tool that reports on allocation should not be doing any. */
#define MYE_OVERLAY_TOP_SYSTEMS 5
#define MYE_OVERLAY_NAME_MAX 40

/* The numbers the overlay puts on screen that are worth asserting on.
 *
 * `entities` counts every entity holding at least one component or tag, which
 * includes the several hundred flecs registers for itself -- the same figure
 * the flecs Explorer shows, and the reason a fresh world does not read zero.
 * An entity with no components at all lives in no table and is not counted.
 *
 * `systems` likewise counts every system in the world, both pipelines, flecs'
 * own included.
 *
 * `drawn_2d` and `interpolated` come from the sprite pass's own query, so they
 * describe what the renderer would actually draw this frame, not what happens
 * to carry a MyeSprite. */
typedef struct mye_overlay_counts {
    int32_t entities;
    int32_t systems;
    int32_t drawn_2d;
    int32_t interpolated;
} mye_overlay_counts;

/* One row of the per-system profile. `seconds` is the total time flecs has
 * measured inside that system -- which is time spent while the overlay was
 * open, not since the world was created; see mye_overlay_system_times. */
typedef struct mye_overlay_system_time {
    ecs_entity_t system;
    char name[MYE_OVERLAY_NAME_MAX];
    double seconds;
} mye_overlay_system_time;

typedef struct MyeDebugOverlay {
    bool visible;
    int toggle_key;  /* raylib key code; default KEY_F3 */

    /* Ring buffer of recent frame times, for the graph. */
    float frame_ms[MYE_OVERLAY_HISTORY];
    int cursor;
    int samples;

    /* Peak seen since the last reset, so a spike that has already passed is
     * still visible. */
    float worst_ms;

    /* Counting walks queries, so it happens on a slow cadence rather than
     * every frame -- and a number that changes 60 times a second is
     * unreadable anyway. These hold the last sample. */
    double next_refresh; /* MyeTime.elapsed at which to recount */
    mye_overlay_counts counts;
    mye_overlay_system_time top[MYE_OVERLAY_TOP_SYSTEMS];
    int top_count;
    double total_system_seconds; /* across every system, for the share column */

    /* Frames the profile below actually covers: flecs measures system time
     * only while the overlay is open, so this is the denominator that turns
     * the totals into a per-frame figure. Counting real frames instead would
     * dilute the average with every frame nobody measured. */
    uint64_t profiled_frames;
} MyeDebugOverlay;

extern ECS_COMPONENT_DECLARE(MyeDebugOverlay);

void MyeDebugOverlayModuleImport(ecs_world_t *world);

/* Shows or hides the overlay. Also turns flecs' per-system timing on and off:
 * a hidden debug tool should cost the game nothing, and timing every system
 * is two clock reads per system per frame. The profile below therefore
 * describes the frames the overlay has been open for. */
void mye_debug_overlay_show(ecs_world_t *world, bool visible);
bool mye_debug_overlay_visible(const ecs_world_t *world);
/* Clears the frame-time history and the worst-frame marker. */
void mye_debug_overlay_reset(ecs_world_t *world);

/* Counts what the overlay reports. Iterates, so it is called once per overlay
 * refresh rather than once per frame; cheap enough to call directly from a
 * test. Works headless and in every build configuration. */
mye_overlay_counts mye_overlay_counts_get(const ecs_world_t *world);

/* Fills `out` with the world's systems ordered by measured time, worst first,
 * and returns how many rows were written (at most `max`). `total_seconds`, if
 * non-NULL, receives the sum across *all* systems, so a caller can turn a row
 * into a share of the whole.
 *
 * This is the M7 "profile first" answer to "which system should be sharded".
 * Three things to know before believing it:
 *
 *   - flecs measures nothing until mye_debug_overlay_show(world, true) has
 *     been called, so a world whose overlay was never opened reports zeros.
 *   - the frame limiter's wait is charged to whichever system calls
 *     EndDrawing. A game sitting at its target framerate therefore shows
 *     MyeRenderEnd taking almost the whole frame, and that is the honest
 *     answer: it is waiting for the display, not for work.
 *   - flecs accumulates a system's time in one unsynchronised field, so for a
 *     system marked `multi_threaded` in a world with worker threads the total
 *     is the sum of every worker's share, written racily. Treat such a row as
 *     an estimate, and expect ThreadSanitizer to see the write if the overlay
 *     is open in that configuration. No engine system is multi_threaded, so
 *     this only arises in a game that opted in.
 *
 * It needs flecs' STATS addon, which is a Debug-only build (see
 * cmake/MyeDependencies.cmake): a Release build returns 0 and writes nothing.
 * Callers should treat 0 as "no profile available", not as "no systems". */
int mye_overlay_system_times(const ecs_world_t *world,
                             mye_overlay_system_time *out, int max,
                             double *total_seconds);

#endif /* MYE_DEBUG_OVERLAY_H */
