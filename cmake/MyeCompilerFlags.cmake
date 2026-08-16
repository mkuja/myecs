# Warning and sanitizer policy for FIRST-PARTY targets only.
# Third-party dependencies are added with SYSTEM (see Dependencies.cmake) so
# their warnings never reach us and -Werror never applies to them.
# See plan/08-build.md.

add_library(mye_flags INTERFACE)

target_compile_options(mye_flags INTERFACE
    $<$<C_COMPILER_ID:GNU,Clang>:
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wdouble-promotion
        -Wvla
        -Wcast-qual
        -Wpointer-arith
        -Werror
    >
    $<$<C_COMPILER_ID:MSVC>:/W4;/WX>
)

# Debug builds: no optimization, full debug info, ASan + UBSan.
#
# ThreadSanitizer cannot be combined with AddressSanitizer, so it gets its own
# build directory:
#   cmake -S . -B build/tsan -DCMAKE_BUILD_TYPE=Debug -DMYE_SANITIZE_THREAD=ON
#   ctest --test-dir build/tsan -LE render
#
# The render label is excluded because TSan and the GPU driver do not coexist;
# that test needs a real OpenGL context and is covered by the ASan build.
# Use it whenever the channel, the job pool, or flecs workers change.
option(MYE_SANITIZE "Enable ASan/UBSan in Debug builds" ON)
option(MYE_SANITIZE_THREAD "Enable ThreadSanitizer instead of ASan/UBSan" OFF)

if(MYE_SANITIZE_THREAD)
    target_compile_options(mye_flags INTERFACE
        $<$<C_COMPILER_ID:GNU,Clang>:
            -fsanitize=thread
            -fno-omit-frame-pointer
            -g
        >)
    target_link_options(mye_flags INTERFACE
        $<$<C_COMPILER_ID:GNU,Clang>:-fsanitize=thread>)
elseif(MYE_SANITIZE)
    target_compile_options(mye_flags INTERFACE
        $<$<AND:$<CONFIG:Debug>,$<C_COMPILER_ID:GNU,Clang>>:
            -fsanitize=address,undefined
            -fno-omit-frame-pointer
        >)
    target_link_options(mye_flags INTERFACE
        $<$<AND:$<CONFIG:Debug>,$<C_COMPILER_ID:GNU,Clang>>:
            -fsanitize=address,undefined
        >)
endif()

target_compile_definitions(mye_flags INTERFACE
    $<$<CONFIG:Debug>:MYE_DEBUG=1>
    $<$<NOT:$<CONFIG:Debug>>:NDEBUG>
)
