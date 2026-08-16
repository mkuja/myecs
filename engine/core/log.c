#include "core/log.h"

#include <flecs.h>
#include <raylib.h>

#include <stdio.h>
#include <string.h>

/* Logging configuration is process-wide by nature -- raylib's and flecs'
 * hooks are global function pointers, so there is nowhere else to put it. */
static mye_log_level g_level = MYE_LOG_INFO;
static mye_log_sink_fn g_sink = NULL;
static void *g_sink_user = NULL;
static mye_log_counts g_counts;

static const char *level_name(mye_log_level level)
{
    switch (level) {
    case MYE_LOG_TRACE: return "TRACE";
    case MYE_LOG_DEBUG: return "DEBUG";
    case MYE_LOG_INFO:  return "INFO";
    case MYE_LOG_WARN:  return "WARN";
    case MYE_LOG_ERROR: return "ERROR";
    case MYE_LOG_OFF:   break;
    }
    return "?";
}

static void count_line(mye_log_level level)
{
    switch (level) {
    case MYE_LOG_TRACE: ++g_counts.trace; break;
    case MYE_LOG_DEBUG: ++g_counts.debug; break;
    case MYE_LOG_INFO:  ++g_counts.info; break;
    case MYE_LOG_WARN:  ++g_counts.warn; break;
    case MYE_LOG_ERROR: ++g_counts.error; break;
    case MYE_LOG_OFF:   break;
    }
}

static void default_sink(mye_log_level level, const char *source,
                         const char *message, void *user)
{
    (void)user;
    fprintf(stderr, "[%-5s %s] %s\n", level_name(level), source, message);
}

void mye_log_set_level(mye_log_level level) { g_level = level; }
mye_log_level mye_log_get_level(void) { return g_level; }

void mye_log_set_sink(mye_log_sink_fn sink, void *user)
{
    g_sink = sink;
    g_sink_user = user;
}

mye_log_counts mye_log_get_counts(void) { return g_counts; }

void mye_log_reset_counts(void)
{
    g_counts = (mye_log_counts){ 0 };
}

void mye_log_writev(mye_log_level level, const char *source, const char *fmt,
                    va_list args)
{
    /* Counted even when filtered out: "were there warnings?" should not
     * depend on the verbosity setting. */
    count_line(level);

    if (level < g_level || g_level == MYE_LOG_OFF) {
        return;
    }

    char message[1024];
    vsnprintf(message, sizeof message, fmt, args);

    if (g_sink != NULL) {
        g_sink(level, source, message, g_sink_user);
    } else {
        default_sink(level, source, message, NULL);
    }
}

void mye_log_write(mye_log_level level, const char *source, const char *fmt,
                   ...)
{
    va_list args;
    va_start(args, fmt);
    mye_log_writev(level, source, fmt, args);
    va_end(args);
}

/* ---------------------------------------------------------------- hooks -- */

static mye_log_level from_raylib(int raylib_level)
{
    switch (raylib_level) {
    case LOG_TRACE:   return MYE_LOG_TRACE;
    case LOG_DEBUG:   return MYE_LOG_DEBUG;
    case LOG_INFO:    return MYE_LOG_INFO;
    case LOG_WARNING: return MYE_LOG_WARN;
    case LOG_ERROR:
    case LOG_FATAL:   return MYE_LOG_ERROR;
    default:          return MYE_LOG_INFO;
    }
}

static void raylib_sink(int raylib_level, const char *fmt, va_list args)
{
    mye_log_writev(from_raylib(raylib_level), "raylib", fmt, args);
}

/* flecs hands us a fully formatted message plus the file and line it came
 * from, and uses negative levels for warnings and errors. */
static void flecs_sink(int32_t level, const char *file, int32_t line,
                       const char *message)
{
    mye_log_level mapped = MYE_LOG_TRACE;
    if (level >= 0) {
        mapped = MYE_LOG_INFO;
    } else if (level == -2) {
        mapped = MYE_LOG_WARN;
    } else if (level <= -3) {
        mapped = MYE_LOG_ERROR;
    }

    if (mapped >= MYE_LOG_WARN && file != NULL) {
        /* Location is worth the noise for problems, not for chatter. */
        mye_log_write(mapped, "flecs", "%s (%s:%d)", message, file, (int)line);
    } else {
        mye_log_write(mapped, "flecs", "%s", message);
    }
}

void mye_log_install_hooks(void)
{
    SetTraceLogCallback(raylib_sink);

    ecs_os_api_t api = ecs_os_get_api();
    api.log_ = flecs_sink;
    ecs_os_set_api(&api);
}
