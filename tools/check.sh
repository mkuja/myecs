#!/usr/bin/env bash
# Runs every verification the project has: three build configurations and the
# windowed examples. This is what would be a CI job if the project were
# hosted; there is deliberately no CI (see plan/09-testing.md).
#
#   tools/check.sh            # everything
#   tools/check.sh --quick    # debug only, for a fast inner loop
set -uo pipefail

cd "$(dirname "$0")/.."
quick=${1:-}
failures=0
jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

step() {
    printf '\n\033[1m== %s\033[0m\n' "$1"
}

fail() {
    printf '\033[31mFAILED: %s\033[0m\n' "$1"
    failures=$((failures + 1))
}

run_config() {
    local name="$1" dir="$2" ctest_args="${3:-}"
    shift 3
    step "$name"
    cmake -S . -B "$dir" "$@" > /dev/null || { fail "$name configure"; return; }
    # Warnings are errors, so a clean build is part of the check.
    cmake --build "$dir" -j"$jobs" || { fail "$name build"; return; }
    # shellcheck disable=SC2086
    ctest --test-dir "$dir" --output-on-failure $ctest_args || fail "$name tests"
}

run_config "Debug (ASan + UBSan)" build/debug "" \
    -DCMAKE_BUILD_TYPE=Debug

if [[ "$quick" != "--quick" ]]; then
    # Release earns its place: the optimiser diagnoses what -O0 cannot, such
    # as strict-aliasing violations.
    run_config "Release" build/release "" \
        -DCMAKE_BUILD_TYPE=Release

    # TSan cannot share a build with ASan, and cannot coexist with the GPU
    # driver, hence the excluded label.
    run_config "ThreadSanitizer" build/tsan "-LE render" \
        -DCMAKE_BUILD_TYPE=Debug -DMYE_SANITIZE_THREAD=ON

    step "Examples (bounded runs, leak-checked on exit)"
    for example in example_00_hello example_01_bounce example_02_asteroids \
                   example_03_scene3d example_05_showcase; do
        binary="build/debug/examples/$example"
        [[ -x "$binary" ]] || continue
        if MYE_MAX_FRAMES=120 "$binary" > /dev/null 2>&1; then
            printf '  ok    %s\n' "$example"
        else
            fail "$example (non-zero exit means leaks or a crash)"
        fi
    done
fi

printf '\n'
if (( failures == 0 )); then
    printf '\033[32mall checks passed\033[0m\n'
else
    printf '\033[31m%d check(s) failed\033[0m\n' "$failures"
fi
exit $(( failures > 0 ))
