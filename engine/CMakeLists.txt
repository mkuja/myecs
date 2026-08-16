add_library(engine STATIC
    core/channel.c
    core/thread.c
    core/jobs.c
    core/log.c
    debug/overlay.c
    core/engine.c
    input/input.c
    asset/asset.c
    audio/audio.c
    render/render2d.c
    render/render3d.c
    scene/scene.c
    scene/serialize.c
    scene/transform.c
)

target_include_directories(engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(engine
    PRIVATE mye_flags
    PUBLIC  mye_alloc raylib flecs::flecs_static Threads::Threads)
