#!/usr/bin/env python3
"""Compile every C code block in TUTORIAL.md.

    tools/check_tutorial.py [--build build/debug]

Documentation rots silently: a renamed function or a changed signature leaves
the prose looking perfectly reasonable. Every block therefore carries a tag in
its fence saying what shape it is, and this compiles each one under the same
warnings-as-errors flags the engine itself uses.

    ```c            a complete program -- compiled and linked
    ```c file       file-scope code -- wrapped with includes
    ```c ctx        statements -- wrapped with world/player/camera/cfg
    ```c fn         statements that declare their own world -- wrapped bare
    ```c test       a test file -- compiled against mye_test.h
    ```c capstone   the capstone listing -- diffed against the real source

The capstone is diffed rather than compiled: the point is that the listing in
the tutorial IS examples/06_tutorial/main.c, not merely that it happens to
compile.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
TUTORIAL = ROOT / "TUTORIAL.md"
CAPSTONE = ROOT / "examples" / "06_tutorial" / "main.c"

WARNINGS = ["-std=c11", "-Wall", "-Wextra", "-Wpedantic", "-Wconversion",
            "-Wshadow", "-Werror"]

# The Debug libraries are built with the sanitizers, so anything linking
# against them needs the same runtime.
SANITIZE = ["-fsanitize=address,undefined"]

# A fragment is an illustration: the function it defines is never called, and
# a variable may exist only to show its type. Relaxed for fragments alone --
# standalone programs are held to the engine's own flags exactly.
FRAGMENT_RELAXATIONS = ["-Wno-unused-function", "-Wno-unused-variable"]

# Enough context for a fragment to stand up, and no more. Declarations only:
# fragments are compiled, never linked, so a prototype is as good as a body
# and cannot go stale against one.
PREAMBLE = """\
#include "asset/asset.h"
#include "audio/audio.h"
#include "core/alloc.h"
#include "core/channel.h"
#include "core/engine.h"
#include "core/jobs.h"
#include "core/log.h"
#include "core/rl_alloc.h"
#include "input/input.h"
#include "render/camera.h"
#include "render/render2d.h"
#include "render/render3d.h"
#include "scene/scene.h"
#include "scene/serialize.h"
#include "scene/transform.h"

#include <raylib.h>
#include <raymath.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Types the fragments share. Guarded, because a fragment whose whole point
 * is to declare one must be free to do so itself; the driver defines the
 * matching macro when it sees the type in the body. */
#ifndef MYE_DOC_HAS_Score
typedef struct Score { int points; } Score;
ECS_COMPONENT_DECLARE(Score);
#endif
#ifndef MYE_DOC_HAS_Thing
typedef struct Thing { int value; } Thing;
#endif
#ifndef MYE_DOC_HAS_Particle
typedef struct Particle { float x, y; } Particle;
#endif
#ifndef MYE_DOC_HAS_Velocity
typedef struct Velocity { float x, y; } Velocity;
ECS_COMPONENT_DECLARE(Velocity);
#endif

/* Referred to by fragments that are showing how to register or call them. */
void Move(ecs_iter_t *it);
void game_setup(ecs_world_t *world);
"""

CTX_OPEN = """
static void mye_doc_fragment(ecs_world_t *world, ecs_entity_t player,
                             ecs_entity_t camera, MyeRender3dConfig *cfg)
{
    (void)world; (void)player; (void)camera; (void)cfg;
"""

FN_OPEN = """
static void mye_doc_fragment(void)
{
"""


def blocks(text):
    """Every fenced C block, as (kind, body, ordinal)."""
    found = []
    for match in re.finditer(r"```c([^\n]*)\n(.*?)```", text, re.S):
        info = match.group(1).strip()
        if info not in ("", "file", "ctx", "fn", "test", "capstone"):
            continue  # ```cmake and friends
        found.append((info or "standalone", match.group(2), len(found) + 1))
    return found


def source_for(kind, body):
    if kind == "standalone":
        return body
    if kind == "test":
        return body
    if kind == "file":
        return PREAMBLE + "\n" + body
    if kind == "ctx":
        return PREAMBLE + CTX_OPEN + body + "}\n"
    if kind == "fn":
        return PREAMBLE + FN_OPEN + body + "}\n"
    raise AssertionError(kind)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", default="build/debug",
                        help="configured build dir, for the fetched headers")
    args = parser.parse_args()

    build = ROOT / args.build
    deps = build / "_deps"
    if not deps.is_dir():
        print(f"no {deps}; configure a build first", file=sys.stderr)
        return 2

    includes = [f"-I{ROOT / 'engine'}", f"-I{ROOT / 'tests'}",
                f"-I{deps / 'raylib-src' / 'src'}",
                f"-I{deps / 'flecs-src' / 'include'}"]
    libs = [str(build / "engine" / "libengine.a"),
            str(build / "libmye_alloc.a"),
            str(deps / "raylib-build" / "raylib" / "libraylib.a"),
            str(deps / "flecs-build" / "libflecs_static.a"),
            "-lm", "-lpthread", "-ldl", "-lGL", "-lX11"]

    text = TUTORIAL.read_text()
    failures = []
    counts = {}

    with tempfile.TemporaryDirectory() as tmp:
        for kind, body, index in blocks(text):
            counts[kind] = counts.get(kind, 0) + 1

            if kind == "capstone":
                if body != CAPSTONE.read_text():
                    failures.append(
                        f"block {index}: the capstone listing has drifted from "
                        f"{CAPSTONE.relative_to(ROOT)}")
                    print(f"  DRIFT   block {index} (capstone)")
                else:
                    print(f"  ok      block {index} (capstone matches source)")
                continue

            path = Path(tmp) / f"block_{index}.c"
            path.write_text(source_for(kind, body))

            # Standalone blocks are whole programs, so link them: that catches
            # a call to something that no longer exists. Fragments are only
            # compiled -- they are illustrations, not programs.
            if kind == "standalone":
                cmd = ["gcc", *WARNINGS, *SANITIZE, *includes, str(path),
                       "-o", str(path.with_suffix(".bin")), *libs]
            else:
                # Suppress the preamble's copy of any type the fragment
                # declares for itself.
                shadowed = [f"-DMYE_DOC_HAS_{name}"
                            for name in ("Score", "Thing", "Particle",
                                         "Velocity")
                            if f"struct {name} " in body]
                cmd = ["gcc", *WARNINGS, *FRAGMENT_RELAXATIONS, *shadowed,
                       *includes, "-c", str(path),
                       "-o", str(path.with_suffix(".o"))]

            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode == 0:
                print(f"  ok      block {index} ({kind})")
            else:
                first = [line for line in result.stderr.splitlines()
                         if ": error:" in line or "undefined reference" in line
                         ][:3]
                failures.append(f"block {index} ({kind}):\n    " +
                                "\n    ".join(first))
                print(f"  FAIL    block {index} ({kind})")

    print()
    print("  ".join(f"{n} {k}" for k, n in sorted(counts.items())))

    if failures:
        print(f"\n{len(failures)} block(s) failed:\n", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    print("every code block in TUTORIAL.md compiles")
    return 0


if __name__ == "__main__":
    sys.exit(main())
