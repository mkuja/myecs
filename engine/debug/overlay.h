/* On-screen debug overlay. See plan/00-overview.md (Tier 2).
 *
 * Answers the questions you actually ask while a game misbehaves: is the
 * frame time spiking, is memory growing, are entities accumulating, did
 * anything warn, and is the fixed step keeping up.
 *
 * Toggled with F3 by default. It reads the key directly rather than through
 * the action system, because a debug tool should work in a game that has not
 * bound anything -- and should not consume one of the game's action slots.
 */
#ifndef MYE_DEBUG_OVERLAY_H
#define MYE_DEBUG_OVERLAY_H

#include "core/engine.h"

#define MYE_OVERLAY_HISTORY 120

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
} MyeDebugOverlay;

extern ECS_COMPONENT_DECLARE(MyeDebugOverlay);

void MyeDebugOverlayModuleImport(ecs_world_t *world);

void mye_debug_overlay_show(ecs_world_t *world, bool visible);
bool mye_debug_overlay_visible(const ecs_world_t *world);
/* Clears the frame-time history and the worst-frame marker. */
void mye_debug_overlay_reset(ecs_world_t *world);

#endif /* MYE_DEBUG_OVERLAY_H */
