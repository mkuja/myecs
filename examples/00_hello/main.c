/* M0 -- Hello window: proves the toolchain (CMake + raylib) end to end.
 *
 * This one deliberately uses raw raylib with no engine, as the minimal
 * reference for what the engine sits on top of. It honours MYE_MAX_FRAMES
 * by hand so that, like every engine-based example, it can be run
 * unattended:  MYE_MAX_FRAMES=60 ./example_00_hello  */
#include "raylib.h"

#include <stdlib.h>

static unsigned long long max_frames_from_env(void)
{
    const char *value = getenv("MYE_MAX_FRAMES");
    if (value == NULL) {
        return 0; /* unlimited */
    }
    char *end = NULL;
    unsigned long long parsed = strtoull(value, &end, 10);
    return (end != value && *end == '\0') ? parsed : 0;
}

int main(void)
{
    InitWindow(1280, 720, "myecs -- M0 hello");
    SetTargetFPS(60);

    const unsigned long long max_frames = max_frames_from_env();
    unsigned long long frame = 0;

    while (!WindowShouldClose()) {
        if (max_frames > 0 && frame >= max_frames) {
            break;
        }
        ++frame;

        BeginDrawing();
        ClearBackground(RAYWHITE);
        DrawText("myecs: M0 -- window is alive", 40, 40, 28, DARKGRAY);
        DrawFPS(40, 90);
        EndDrawing();
    }

    CloseWindow();
    return 0;
}
