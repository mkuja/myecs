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
