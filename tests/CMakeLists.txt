# Test harness interface: gives every test access to mye_test.h.
add_library(mye_test_harness INTERFACE)
target_include_directories(mye_test_harness INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(mye_test_harness INTERFACE mye_flags m)

# Sanitizer settings every test runs under, per plan/08-build.md ("the suite
# is expected to be clean under them, ASAN_OPTIONS/UBSAN_OPTIONS with
# halt_on_error=1 in CTest environment").
#
# halt_on_error is what makes a sanitizer report FAIL the test rather than
# scroll past. It matters most for UBSan: -fsanitize=undefined is recoverable
# by default, so an undefined-behaviour report prints a line and the program
# carries on to exit 0 -- a green test over a real bug. print_stacktrace turns
# that report into something actionable rather than one source line.
#
# Applied through the two helpers below, so a test cannot be added without it.
# Harmless in the non-sanitized configurations: the runtimes that read these
# variables are not linked in, and nothing else looks at them.
set(MYE_TEST_ENVIRONMENT
    "ASAN_OPTIONS=halt_on_error=1"
    "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1")

# mye_add_unit_test(<module>)      -> tests/unit/test_<module>.c
function(mye_add_unit_test module)
    add_executable(test_unit_${module} unit/test_${module}.c)
    target_link_libraries(test_unit_${module} PRIVATE mye_test_harness ${ARGN})
    add_test(NAME unit_${module} COMMAND test_unit_${module})
    set_tests_properties(unit_${module} PROPERTIES
        TIMEOUT 60
        ENVIRONMENT "${MYE_TEST_ENVIRONMENT}")
endfunction()

# mye_add_integration_test(<feature> [LABELS <label>...])
function(mye_add_integration_test feature)
    cmake_parse_arguments(ARG "" "" "LABELS;LIBS" ${ARGN})
    add_executable(test_int_${feature} integration/test_int_${feature}.c)
    target_link_libraries(test_int_${feature} PRIVATE mye_test_harness ${ARG_LIBS})
    add_test(NAME int_${feature} COMMAND test_int_${feature})
    set_tests_properties(int_${feature} PROPERTIES
        TIMEOUT 120
        ENVIRONMENT "${MYE_TEST_ENVIRONMENT}")
    if(ARG_LABELS)
        set_tests_properties(int_${feature} PROPERTIES LABELS "${ARG_LABELS}")
    endif()
endfunction()

mye_add_unit_test(harness)
mye_add_unit_test(alloc engine)
mye_add_unit_test(rl_alloc engine)

mye_add_unit_test(input engine)
mye_add_unit_test(collision engine)
mye_add_unit_test(sprite_anim engine)
mye_add_unit_test(channel engine)
mye_add_unit_test(jobs engine)
mye_add_unit_test(draw_order engine)
mye_add_unit_test(modules engine)

mye_add_integration_test(ecs_loop LIBS engine)
mye_add_integration_test(fixed_step LIBS engine)
mye_add_integration_test(asteroids LIBS asteroids_game)
mye_add_integration_test(asteroids_scenes LIBS asteroids_scenes)
mye_add_integration_test(assets LIBS engine)
mye_add_integration_test(audio_anim LIBS asteroids_game)
mye_add_integration_test(music LIBS engine)
mye_add_integration_test(prefabs LIBS asteroids_game)
mye_add_integration_test(text LIBS engine)
mye_add_integration_test(transform LIBS engine)
mye_add_integration_test(collision LIBS engine)
mye_add_integration_test(scene LIBS engine)
mye_add_integration_test(serialize LIBS engine)
mye_add_integration_test(workers LIBS engine)
mye_add_integration_test(interpolation LIBS engine)
mye_add_integration_test(overlay LIBS engine)
mye_add_integration_test(camera LIBS engine)
mye_add_integration_test(canvas LIBS engine)
mye_add_integration_test(net LIBS engine)
mye_add_integration_test(music_device LIBS engine LABELS render)
mye_add_integration_test(render_smoke LIBS engine LABELS render)
mye_add_integration_test(render_sprites LIBS engine LABELS render)
mye_add_integration_test(render_cameras LIBS engine LABELS render)
