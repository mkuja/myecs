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
    render/camera.c
    render/canvas.c
    render/render2d.c
    render/render3d.c
    scene/scene.c
    scene/serialize.c
    scene/transform.c
    net/net.c
    $<$<NOT:$<BOOL:${EMSCRIPTEN}>>:net/net_lws.c>
    $<$<BOOL:${EMSCRIPTEN}>:net/net_web.c>
    # Tier-2 hot reload. Its header is a no-op off the web, so engine.c calls
    # it unconditionally and only this file is platform-specific.
    $<$<BOOL:${EMSCRIPTEN}>:core/web_reload.c>
)

target_include_directories(engine PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
find_package(Threads REQUIRED)
target_link_libraries(engine
    PRIVATE mye_flags
    PUBLIC  mye_alloc raylib flecs::flecs_static Threads::Threads)

if(EMSCRIPTEN)
    # The browser's WebSocket lives in JS; this links the glue emscripten
    # ships for it. No protocol library is compiled into the wasm.
    target_link_options(engine PUBLIC -lwebsocket.js)
else()
    target_link_libraries(engine PUBLIC websockets)
endif()
