# WebAssembly build settings. Included only when configuring under emcmake.
# See plan/11-web-dev-loop.md.

message(STATUS "myecs: configuring for WebAssembly")

# WebGL 2 rather than raylib's default WebGL 1. GLSL ES 300 is near-identical
# to the desktop GLSL 330 shaders, so only the version line differs and there
# is no second shader to maintain. WebGL 1 is deliberately out of scope.
set(PLATFORM Web CACHE STRING "" FORCE)
set(OPENGL_VERSION "ES 3.0" CACHE STRING "" FORCE)

# Emscripten emits a page, not a bare executable.
set(CMAKE_EXECUTABLE_SUFFIX ".html")

set(MYE_WEB_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html")

# Applied to every example by mye_web_configure() below.
#
#  ASYNCIFY      lets the existing `while (mye_running(world))` loop yield to
#                the browser, so no engine or example code changes. It costs
#                binary size and some speed; inverting the loop removes both,
#                and is the follow-up once the cost is measured rather than
#                assumed.
#  ALLOW_MEMORY_GROWTH
#                the engine allocates at load time (procedural textures and
#                audio) and would otherwise hit the default 16 MB heap.
#  no -pthread   emscripten threads require cross-origin isolation, which is
#                a deployment constraint. The engine already degrades
#                gracefully: MYE_THREADS_NONE makes mye_jobs_create fail, and
#                asset loading falls back to synchronous.
function(mye_web_configure target)
    target_link_options(${target} PRIVATE
        --shell-file "${MYE_WEB_SHELL}"
        -sASYNCIFY
        -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=1MB
        -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']
        -sASSERTIONS=1)
    set_target_properties(${target} PROPERTIES
        LINK_DEPENDS "${MYE_WEB_SHELL}")
endfunction()
