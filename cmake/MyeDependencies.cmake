# Third-party dependencies, fetched at configure time and pinned to exact tags.
# SYSTEM makes their headers -isystem, so their warnings never break our
# -Werror build. See plan/08-build.md.
#
# FIND_PACKAGE_ARGS -- the stated reason for the CMake >= 3.24 floor -- lets an
# already-installed package satisfy a dependency instead of a fetch. It is
# OPT_IN by default: only declarations that name it are looked up at all, so
# adding it to one dependency changes nothing for the others.
#
# It is on for flecs alone, and that is a deliberate line rather than an
# oversight. A system package may stand in only where it can do the whole job:
#
#   flecs          -- yes. The engine reaches it through public API only: the
#                     allocator bridge is ecs_os_set_api() at runtime, so any
#                     build of the right version serves. Pinned EXACT, because
#                     4.x has moved query and pipeline semantics under us
#                     before and a "close enough" flecs is a debugging trap.
#                     Note that upstream installs no config-version file, so
#                     an EXACT request never matches an installed flecs today
#                     -- which is the safe outcome, not a bug: the pin wins
#                     until upstream can prove its version.
#
#   raylib         -- no, and it must stay no. We compile raylib ourselves with
#                     engine/core/rl_alloc.h force-included (see the bottom of
#                     this file) so its allocations route through mye_alloc and
#                     land in the leak report. An imported target cannot be
#                     given compile options at all, so a found raylib would
#                     fail the configure outright -- and if it somehow did not,
#                     it would silently drop raylib out of allocator tracking.
#
#   libwebsockets  -- no. The build below turns LWS_WITH_EXTERNAL_POLL on and
#                     TLS/extensions off; a distro build has its own answers to
#                     all three, and external poll is load-bearing (lws_service
#                     sleeps, which is fatal inside a frame).
#
# To ignore an installed flecs and always fetch:
#     cmake -DFETCHCONTENT_TRY_FIND_PACKAGE_MODE=NEVER ...

include(FetchContent)

# ---------------------------------------------------------------- raylib ----
set(BUILD_EXAMPLES   OFF CACHE BOOL "" FORCE)
set(BUILD_GAMES      OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(raylib
    GIT_REPOSITORY https://github.com/raysan5/raylib.git
    GIT_TAG        6.0
    GIT_SHALLOW    TRUE
    SYSTEM)

# ----------------------------------------------------------------- flecs ----
set(FLECS_STATIC ON  CACHE BOOL "" FORCE)
set(FLECS_SHARED OFF CACHE BOOL "" FORCE)
set(FLECS_TESTS  OFF CACHE BOOL "" FORCE)

FetchContent_Declare(flecs
    GIT_REPOSITORY https://github.com/SanderMertens/flecs.git
    GIT_TAG        v4.1.6
    GIT_SHALLOW    TRUE
    SYSTEM
    # EXACT: nothing but the pinned version may stand in. See the note at the
    # top of this file.
    FIND_PACKAGE_ARGS 4.1.6 EXACT CONFIG)

# The REST and STATS addons are debug tooling: REST is the HTTP server the
# flecs Explorer connects to, STATS is the per-system timing the debug overlay
# profiles with. Neither belongs in a shipped game, so Release compiles them
# OUT rather than merely leaving them unstarted. See plan/08-build.md. The
# switch itself is further down, after FetchContent_MakeAvailable: the flecs
# targets do not exist before that point.

# -------------------------------------------------------- libwebsockets ----
# Native only: a web build gets its WebSocket from the browser, so shipping a
# protocol implementation into the wasm would be dead weight. See
# plan/12-networking.md.
#
# MIT (a few files are BSD/CC0/ZLIB -- see THIRD-PARTY-NOTICES.md). Trimmed
# hard: no TLS (N2 decides that), no extensions (permessage-deflate is the
# only one, and it would pull in zlib), no test apps or examples.
if(NOT EMSCRIPTEN)
    set(LWS_WITH_SSL              OFF CACHE BOOL "" FORCE)
    set(LWS_WITHOUT_EXTENSIONS    ON  CACHE BOOL "" FORCE)
    set(LWS_WITH_STATIC           ON  CACHE BOOL "" FORCE)
    set(LWS_WITH_SHARED           OFF CACHE BOOL "" FORCE)
    set(LWS_WITHOUT_TESTAPPS      ON  CACHE BOOL "" FORCE)
    set(LWS_WITHOUT_TEST_SERVER   ON  CACHE BOOL "" FORCE)
    set(LWS_WITHOUT_TEST_CLIENT   ON  CACHE BOOL "" FORCE)
    set(LWS_WITH_MINIMAL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(LWS_WITH_NETLINK          OFF CACHE BOOL "" FORCE)
    set(LWS_ROLE_RAW_PROXY        OFF CACHE BOOL "" FORCE)
    # lws_service() sleeps until an event arrives -- fine for a dedicated
    # network thread, fatal inside a frame. External poll hands us the file
    # descriptors so the engine can poll them with a zero timeout instead.
    set(LWS_WITH_EXTERNAL_POLL    ON  CACHE BOOL "" FORCE)

    FetchContent_Declare(libwebsockets
        GIT_REPOSITORY https://github.com/warmcat/libwebsockets.git
        GIT_TAG        v4.5.8
        GIT_SHALLOW    TRUE
        SYSTEM)
endif()

# raylib generates install(EXPORT) rules that would demand mye_alloc be part
# of its export set once we link it in below. We never install these targets,
# so skip install rules for the dependencies and restore them afterwards.
set(_mye_skip_install_backup ${CMAKE_SKIP_INSTALL_RULES})
set(CMAKE_SKIP_INSTALL_RULES ON)

FetchContent_MakeAvailable(raylib flecs)

# Say which flecs the build is actually using: "it worked on my machine" and
# "it found a different flecs on yours" look identical without this line.
if(flecs_FOUND)
    message(STATUS "flecs: system package ${flecs_VERSION} (FIND_PACKAGE_ARGS)")
else()
    message(STATUS "flecs: fetched pin v4.1.6")
endif()

if(NOT EMSCRIPTEN)
    FetchContent_MakeAvailable(libwebsockets)

    # libwebsockets uses POSIX (localtime_r, and more), which -std=c11 hides.
    # Grant GNU extensions to the dependency alone, exactly as raylib gets
    # them for EM_ASM: our own targets stay strict ISO C11.
    set_target_properties(websockets PROPERTIES C_EXTENSIONS ON)
endif()

set(CMAKE_SKIP_INSTALL_RULES ${_mye_skip_install_backup})

# flecs selects its addons with preprocessor macros, not CMake options: it has
# no FLECS_REST/FLECS_STATS build flag, so the only lever is a blacklist macro
# (FLECS_NO_<addon>) read by include/flecs/private/addons.h.
#
# PUBLIC is not optional here. The macros change what flecs.h *declares*, so
# the library and everything that includes it must agree; defining them PRIVATE
# would compile a flecs without the REST addon while the engine still believed
# `EcsRest` existed, and the mismatch would only surface at link time.
#
# The $<CONFIG:Debug> generator expression is evaluated per configuration, so
# this works with single-config generators (Makefiles, Ninja) as well as
# multi-config ones -- the whole target is compiled once per configuration
# either way, so flecs and our own code always see the same definitions. No
# configure-time `if(CMAKE_BUILD_TYPE STREQUAL ...)` is needed, and using one
# would quietly break Ninja Multi-Config.
target_compile_definitions(flecs_static PUBLIC
    $<$<NOT:$<CONFIG:Debug>>:FLECS_NO_REST;FLECS_NO_STATS>)

# Route raylib's allocations through mye_alloc. The macros are #ifndef-guarded
# in raylib.h, rlgl.h and raudio.c, so defining them here wins; -include makes
# the declarations visible before raylib's first use of them.
# XM and MOD tracker-music support pulls in jar_xm/jar_mod, which are WTFPL --
# permissive in intent, but not a licence every downstream is willing to carry.
# We have no use for tracker music, so compile it out. The SUPPORT_* macros in
# raylib's config.h are #ifndef-guarded, so these definitions win.
target_compile_definitions(raylib PRIVATE
    SUPPORT_FILEFORMAT_XM=0
    SUPPORT_FILEFORMAT_MOD=0)

# raylib's web backend uses EM_ASM, which clang rejects under -std=c11:
# "EM_ASM does not work in -std=c* modes, use -std=gnu* modes instead". Grant
# GNU extensions to raylib alone -- our own targets stay strict ISO C11, the
# same way -Werror applies only to first-party code.
if(EMSCRIPTEN)
    set_target_properties(raylib PROPERTIES C_EXTENSIONS ON)
    # ...and its runtime-method export would otherwise overwrite ours; see
    # cmake/MyeWeb.cmake.
    mye_web_drop_raylib_runtime_methods()
endif()

target_link_libraries(raylib PRIVATE mye_alloc)
set(_mye_rl_alloc_header "${CMAKE_CURRENT_LIST_DIR}/../engine/core/rl_alloc.h")
target_compile_options(raylib PRIVATE -include "${_mye_rl_alloc_header}")
# The RL_* macros are defined inside rl_alloc.h itself: CMake does not pass
# function-like macros through target_compile_definitions reliably.
#
# A force-included header is NOT tracked as a dependency, so editing it would
# otherwise leave raylib's objects stale -- silently reverting to plain malloc
# while every flag still looks correct. Declare the dependency explicitly.
get_target_property(_mye_raylib_sources raylib SOURCES)
set_source_files_properties(${_mye_raylib_sources}
    DIRECTORY "${raylib_SOURCE_DIR}/src"
    PROPERTIES OBJECT_DEPENDS "${_mye_rl_alloc_header}")
