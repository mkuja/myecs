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

# raylib links with -sEXPORTED_RUNTIME_METHODS=ccall for its own JS helpers.
# An -s setting is an assignment rather than a list, and a dependency's
# interface options land AFTER the target's own on the link line, so raylib's
# single value silently replaced ours: cwrap and the string helpers were
# missing at runtime, with the page aborting on the first call. Drop raylib's
# and let mye_web_configure() below supply a superset that still contains
# ccall.
function(mye_web_drop_raylib_runtime_methods)
    get_target_property(options raylib INTERFACE_LINK_OPTIONS)
    if(options)
        list(FILTER options EXCLUDE REGEX "^-sEXPORTED_RUNTIME_METHODS=")
        set_target_properties(raylib PROPERTIES INTERFACE_LINK_OPTIONS "${options}")
    endif()
endfunction()

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
        # ccall/cwrap reach mye_web_snapshot and mye_web_restore; the string
        # helpers put a snapshot back into wasm memory. malloc/free are here
        # because that copy must go on the heap: a world snapshot is bigger
        # than the 1 MB stack that cwrap's 'string' argument would use.
        #
        # The two reload entry points are deliberately NOT listed in
        # EXPORTED_FUNCTIONS, though plan/11 sketched them there.
        # EMSCRIPTEN_KEEPALIVE already exports them, and naming a symbol here
        # that a target does not define is a link error -- example_00_hello
        # links raylib alone, with no engine and so no reload support.
        -sEXPORTED_FUNCTIONS=['_main','_malloc','_free']
        -sEXPORTED_RUNTIME_METHODS=['ccall','cwrap','stringToUTF8','lengthBytesUTF8']
        -sASSERTIONS=1)
    set_target_properties(${target} PROPERTIES
        LINK_DEPENDS "${MYE_WEB_SHELL}")

    # One-shot run in a browser, with the app's stdout piped back to the
    # terminal: the microscope, where tools/web_dev.py is the loop. emrun
    # cannot serve the build-id endpoint or the isolation headers, so it is
    # the wrong tool for iterating and the right one for reading a crash.
    #
    #     cmake --build build/web --target run_example_06_tutorial
    add_custom_target(run_${target}
        COMMAND emrun --port 8080 "$<TARGET_FILE:${target}>"
        USES_TERMINAL
        COMMENT "emrun ${target} -- http://localhost:8080")
    add_dependencies(run_${target} ${target})
endfunction()
