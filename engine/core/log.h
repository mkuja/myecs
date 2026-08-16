/* One place for engine, raylib and flecs diagnostics. See
 * plan/01-architecture.md.
 *
 * Without this the three talk to different sinks: raylib to its TraceLog,
 * flecs to ecs_os_api.log_, and engine code to whatever fprintf it reached
 * for. That makes output impossible to filter, redirect or silence as a
 * whole -- which is what you want the moment a game is noisy or a test needs
 * quiet.
 *
 *   mye_log_info("loaded %d entities", count);
 *   mye_log_set_level(MYE_LOG_WARN);          // hush everything below
 *   mye_log_set_sink(my_sink, user);          // send it to a file, a HUD...
 *
 * Levels are ordered, so setting WARN also silences INFO and TRACE.
 */
#ifndef MYE_CORE_LOG_H
#define MYE_CORE_LOG_H

#include <stdarg.h>
#include <stdbool.h>

typedef enum mye_log_level {
    MYE_LOG_TRACE = 0,
    MYE_LOG_DEBUG,
    MYE_LOG_INFO,
    MYE_LOG_WARN,
    MYE_LOG_ERROR,
    MYE_LOG_OFF, /* only valid as a threshold, never as a message level */
} mye_log_level;

/* Where a line ends up. `source` is "engine", "raylib" or "flecs", so a sink
 * can tell them apart without parsing the message. */
typedef void (*mye_log_sink_fn)(mye_log_level level, const char *source,
                                const char *message, void *user);

void mye_log_set_level(mye_log_level level);
mye_log_level mye_log_get_level(void);

/* NULL restores the default sink (stderr, with a level and source prefix). */
void mye_log_set_sink(mye_log_sink_fn sink, void *user);

/* Routes raylib's TraceLog and flecs' ecs_os_api logging here. Called by
 * mye_init; exposed for tests and for tools that use the engine's pieces
 * without a world. */
void mye_log_install_hooks(void);

void mye_log_write(mye_log_level level, const char *source, const char *fmt,
                   ...);
void mye_log_writev(mye_log_level level, const char *source, const char *fmt,
                    va_list args);

#define mye_log_trace(...) mye_log_write(MYE_LOG_TRACE, "engine", __VA_ARGS__)
#define mye_log_debug(...) mye_log_write(MYE_LOG_DEBUG, "engine", __VA_ARGS__)
#define mye_log_info(...) mye_log_write(MYE_LOG_INFO, "engine", __VA_ARGS__)
#define mye_log_warn(...) mye_log_write(MYE_LOG_WARN, "engine", __VA_ARGS__)
#define mye_log_error(...) mye_log_write(MYE_LOG_ERROR, "engine", __VA_ARGS__)

/* Counts since startup, for the debug overlay and for tests that assert an
 * operation produced no warnings. */
typedef struct mye_log_counts {
    unsigned trace, debug, info, warn, error;
} mye_log_counts;

mye_log_counts mye_log_get_counts(void);
void mye_log_reset_counts(void);

#endif /* MYE_CORE_LOG_H */
