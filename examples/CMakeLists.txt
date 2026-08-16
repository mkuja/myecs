add_executable(example_00_hello 00_hello/main.c)
target_link_libraries(example_00_hello PRIVATE mye_flags raylib)

add_executable(example_01_bounce 01_bounce/main.c)
target_link_libraries(example_01_bounce PRIVATE mye_flags engine)

# Game logic as a library so the integration test can drive it headlessly.
add_library(asteroids_game STATIC 02_asteroids/asteroids.c)
target_include_directories(asteroids_game PUBLIC 02_asteroids)
target_link_libraries(asteroids_game PRIVATE mye_flags PUBLIC engine)

add_executable(example_02_asteroids 02_asteroids/main.c)
target_link_libraries(example_02_asteroids PRIVATE mye_flags asteroids_game)

add_executable(example_03_scene3d 03_scene3d/main.c)
target_link_libraries(example_03_scene3d PRIVATE mye_flags engine)

add_executable(example_04_stress 04_stress/main.c)
target_link_libraries(example_04_stress PRIVATE mye_flags engine)

add_executable(example_05_showcase 05_showcase/main.c)
target_link_libraries(example_05_showcase PRIVATE mye_flags engine)

add_executable(example_06_tutorial 06_tutorial/main.c)
target_link_libraries(example_06_tutorial PRIVATE mye_flags engine)

add_executable(example_07_net 07_net/main.c)
target_link_libraries(example_07_net PRIVATE mye_flags engine)

# Web builds: shell page, ASYNCIFY, growable heap. Defined in
# cmake/MyeWeb.cmake, which is only included under emcmake.
if(EMSCRIPTEN)
    foreach(target
            example_00_hello example_01_bounce example_02_asteroids
            example_03_scene3d example_04_stress
            example_06_tutorial example_07_net)
        mye_web_configure(${target})
    endforeach()

    # The only example that reads files from disk, so the only one that needs
    # anything packaged into the virtual filesystem.
    mye_web_configure(example_05_showcase PRELOAD assets/models)
endif()
