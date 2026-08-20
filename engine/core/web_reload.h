/* Tier-2 hot reload: the world survives a rebuild of the web build.
 *
 * The page snapshots the world to JSON before it reloads and feeds that JSON
 * back to the fresh module afterwards -- a few dozen lines on top of the M6
 * serializer, which is why plan/11-web-dev-loop.md calls tier 2 cheap.
 *
 * Both entry points only *queue*: the serializing and the loading happen at
 * the next frame boundary, inside mye_progress. That is not fussiness. With
 * -sASYNCIFY the C stack spends most of its life unwound inside raylib's
 * frame, so a JS callback that reaches into the world does it from the middle
 * of a flecs pipeline run -- engine/net/net_web.c documents what that costs
 * (the loop simply stops advancing, with no error). Queueing frees the JS
 * side of every timing rule: call it whenever, it lands at the top of a frame.
 *
 * The world pointer needs no registration hook, because mye_progress already
 * has it: the only moment the JSON may be applied is also the only moment the
 * engine is between frames. Examples therefore need no change at all.
 *
 * Desktop builds compile all of this away.
 */
#ifndef MYE_CORE_WEB_RELOAD_H
#define MYE_CORE_WEB_RELOAD_H

#include <flecs.h>

#if defined(__EMSCRIPTEN__)

/* Called from JavaScript; both are EMSCRIPTEN_KEEPALIVE exports.
 *
 *   mye_web_snapshot()     asks for a snapshot. One frame later the JSON is
 *                          handed to Module.myeStashSnapshot(json), which
 *                          web/shell.html defines and stores in sessionStorage.
 *   mye_web_restore(json)  copies the string and applies it one frame later.
 *
 * Both return 1 when the request was accepted, 0 when it was not (a NULL
 * string, or no memory to copy it). A second restore queued before the first
 * has been applied replaces it -- the newest world wins. */
int mye_web_snapshot(void);
int mye_web_restore(const char *json);

/* Frame boundary: services whatever the two above queued. Called by
 * mye_progress after pending scene switches have landed, so a restore is
 * applied on top of a loaded scene rather than being wiped by one. */
void mye_web_reload_poll(ecs_world_t *world);

#else

static inline void mye_web_reload_poll(ecs_world_t *world)
{
    (void)world;
}

#endif /* __EMSCRIPTEN__ */

#endif /* MYE_CORE_WEB_RELOAD_H */
