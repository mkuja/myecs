cmake_minimum_required(VERSION 3.24)

# Strict C project: declaring only the C language means a stray C++ source
# fails to configure. See plan/08-build.md.
project(myecs
    VERSION 0.1.0
    DESCRIPTION "2D/3D ECS game engine on raylib + flecs"
    LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

# Included by full path, and prefixed: putting our cmake/ on CMAKE_MODULE_PATH
# would shadow same-named modules inside dependencies (raylib does
# include(CompilerFlags) from its own cmake/ directory).
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MyeCompilerFlags.cmake")

# Defined here, before the dependencies, because raylib is compiled against it:
# its RL_MALLOC macros are redirected into this library so that raylib's
# allocations are covered by our tracking allocator. Keeping it separate from
# `engine` avoids a dependency cycle (raylib -> mye_alloc, engine -> raylib).
add_library(mye_alloc STATIC
    engine/core/alloc.c
    engine/core/rl_alloc.c)
target_include_directories(mye_alloc PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/engine")
target_link_libraries(mye_alloc PRIVATE mye_flags)

# The web build has no sanitizers, no threads and no test runner: emscripten
# provides none of them, and the tests are a desktop concern.
#
# BEFORE the dependencies, not after: MyeWeb.cmake sets the OPENGL_VERSION
# raylib is configured with, and raylib reads it as it is added below. With
# this include after MyeDependencies the FORCE landed in the cache too late,
# so a FRESH build tree silently got raylib's default WebGL 1 -- and only a
# second configure of the same tree picked up WebGL 2. The symptom was the
# lighting shader failing to compile with "unsupported shader version 300".
if(EMSCRIPTEN)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MyeWeb.cmake")
else()
    enable_testing()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/MyeDependencies.cmake")

add_subdirectory(engine)
add_subdirectory(examples)

if(NOT EMSCRIPTEN)
    add_subdirectory(tests)
endif()
