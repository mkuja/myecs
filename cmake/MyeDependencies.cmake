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

# raylib generates install(EXPORT) rules that would demand mye_alloc be part
# of its export set once we link it in below. We never install these targets,
# so skip install rules for the dependencies and restore them afterwards.
set(_mye_skip_install_backup ${CMAKE_SKIP_INSTALL_RULES})
set(CMAKE_SKIP_INSTALL_RULES ON)

FetchContent_MakeAvailable(raylib flecs)

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
