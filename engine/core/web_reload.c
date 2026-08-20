/* The C half of tier-2 hot reload. Web only -- see core/web_reload.h for why
 * the two entry points queue instead of acting, and plan/11-web-dev-loop.md
 * for the loop this belongs to.
 *
 * The JS half is web/shell.html: it stores the snapshot in sessionStorage
 * before location.reload() and feeds it back once the new module is up.
 */
#include "core/web_reload.h"

#include "core/log.h"
#include "scene/serialize.h"

#include <emscripten/emscripten.h>

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

/* Requested from JS, serviced at the next frame boundary. */
static bool g_snapshot_wanted;
static char *g_pending_json;

/* Hands the finished snapshot to the page. Defined by web/shell.html, which
 * owns the storage key: where a snapshot lives is a page concern, and the C
 * side has no business knowing about sessionStorage. Absent on a page that
 * does not want reload support, hence the check. */
EM_JS(void, mye_web_stash, (const char *json), {
    if (Module.myeStashSnapshot) {
        Module.myeStashSnapshot(UTF8ToString(json));
    }
})

EMSCRIPTEN_KEEPALIVE int mye_web_snapshot(void)
{
    g_snapshot_wanted = true;
    return 1;
}

EMSCRIPTEN_KEEPALIVE int mye_web_restore(const char *json)
{
    if (json == NULL) {
        return 0;
    }

    /* Copied because the page frees its buffer the moment this returns, and
     * the world is a frame away. libc malloc rather than the engine
     * allocator: this is called before mye_init has run -- the page queues the
     * snapshot from onRuntimeInitialized, which emscripten fires before
     * main() -- so there is no world to take an allocator from yet. */
    size_t size = strlen(json) + 1;
    char *copy = (char *)malloc(size);
    if (copy == NULL) {
        mye_log_error("web: no memory for a %zu byte snapshot", size);
        return 0;
    }
    memcpy(copy, json, size);

    free(g_pending_json); /* a second restore before the first landed wins */
    g_pending_json = copy;
    return 1;
}

void mye_web_reload_poll(ecs_world_t *world)
{
    if (world == NULL) {
        return;
    }

    if (g_pending_json != NULL) {
        char *json = g_pending_json;
        g_pending_json = NULL; /* taken first: applying it must not re-enter */

        size_t size = strlen(json);
        if (mye_world_from_json(world, json)) {
            mye_log_info("web: restored %zu bytes of world state", size);
        } else {
            mye_log_error("web: the snapshot did not parse; "
                          "continuing from a cold start");
        }
        free(json);
    }

    if (g_snapshot_wanted) {
        g_snapshot_wanted = false;

        char *json = mye_world_to_json(world);
        if (json == NULL) {
            mye_log_error("web: the world could not be serialized");
            return;
        }
        mye_log_info("web: snapshot of %zu bytes", strlen(json));
        mye_web_stash(json);
        mye_json_free(world, json);
    }
}
