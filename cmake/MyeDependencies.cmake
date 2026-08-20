# Third-party dependencies, fetched at configure time and pinned to exact tags.
# SYSTEM makes their headers -isystem, so their warnings never break our
# -Werror build. See plan/08-build.md.

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
    SYSTEM)

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
if(NOT EMSCRIPTEN)
    FetchContent_MakeAvailable(libwebsockets)

    # libwebsockets uses POSIX (localtime_r, and more), which -std=c11 hides.
    # Grant GNU extensions to the dependency alone, exactly as raylib gets
    # them for EM_ASM: our own targets stay strict ISO C11.
    set_target_properties(websockets PROPERTIES C_EXTENSIONS ON)
endif()

set(CMAKE_SKIP_INSTALL_RULES ${_mye_skip_install_backup})

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
