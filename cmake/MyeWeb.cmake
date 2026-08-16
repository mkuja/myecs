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
#  PRELOAD <dir>...
#                packages a directory into the module's virtual filesystem.
#                A web build has no access to the host disk: unless a file is
#                packaged at link time it does not exist at runtime, and no
#                amount of path resolution will find it. Directories are
#                mounted at the same relative path they have in the project,
#                so the same "assets/models/Fox.glb" works on both targets.
function(mye_web_configure target)
    cmake_parse_arguments(ARG "" "" "PRELOAD" ${ARGN})

    set(preload_options "")
    foreach(dir IN LISTS ARG_PRELOAD)
        if(EXISTS "${CMAKE_SOURCE_DIR}/${dir}")
            list(APPEND preload_options
                 "--preload-file" "${CMAKE_SOURCE_DIR}/${dir}@/${dir}")
        else()
            # Not fatal: the examples degrade to a message on screen, and
            # sample assets are an optional download.
            message(WARNING
                "${target}: ${dir} does not exist, so it will not be packaged "
                "into the web build -- run tools/fetch_sample_assets.sh and "
                "re-configure if you want it")
        endif()
    endforeach()

    target_link_options(${target} PRIVATE
        ${preload_options}
        --shell-file "${MYE_WEB_SHELL}"
        -sASYNCIFY
        -sALLOW_MEMORY_GROWTH=1
        -sSTACK_SIZE=1MB
        -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap']
        -sASSERTIONS=1)
    set_target_properties(${target} PROPERTIES
        LINK_DEPENDS "${MYE_WEB_SHELL}")
endfunction()
