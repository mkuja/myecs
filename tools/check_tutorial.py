#!/usr/bin/env python3
"""Check, render and extract the tutorial's code blocks.

TUTORIAL.mdt is the authored source of the tutorial; TUTORIAL.md is
GENERATED from it and must not be edited by hand. Inside any ```c-family
fence, a line whose content starts with `///-` is a hidden line: part of
the compiled program, absent from the rendered page. (Rust marks hidden
doctest lines with `#`, but `#` opens C preprocessor lines, so it cannot
be the marker here.)

    tools/check_tutorial.py                   # check (the default)
    tools/check_tutorial.py render            # TUTORIAL.mdt -> TUTORIAL.md
    tools/check_tutorial.py extract --out F   # named `c test` blocks -> one
                                              # generated mye_test.h suite

Documentation rots silently: a renamed function or a changed signature leaves
the prose looking perfectly reasonable. Every block therefore carries a tag in
its fence saying what shape it is, and `check` compiles each one under the
same warnings-as-errors flags the engine itself uses. An unknown or missing
tag is an error -- an untagged block would silently escape checking. `check`
also fails when TUTORIAL.md is stale relative to TUTORIAL.mdt.

    ```c            a complete program -- compiled, linked and RUN, bounded
                    by MYE_MAX_FRAMES with exit 0 required (which, via
                    mye_shutdown, is also a leak check); `norun` on the
                    fence opts a block out, --no-run opts the whole pass out
    ```c file       file-scope code -- wrapped with includes
    ```c ctx        statements -- wrapped with world/player/camera/canvas/cfg
    ```c fn         statements that declare their own world -- wrapped bare
    ```c test       a self-contained test file -- compiled against mye_test.h
    ```c test name=some-id
                    a test BODY: `extract` wraps it in TEST(tutorial_some_id)
                    inside a generated suite that ctest builds and runs as
                    unit_tutorial, so hidden ASSERT lines verify what the
                    prose claims, under every configuration
    ```c capstone   the capstone listing -- diffed against the real source
    ```sh           shell -- syntax-checked with `bash -n`, never run
    ```cmake / ```text    illustration only, never checked

The capstone is diffed rather than compiled: the point is that the listing in
the tutorial IS examples/06_tutorial/main.c, not merely that it happens to
compile.
"""

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MDT = ROOT / "TUTORIAL.mdt"
RENDERED = ROOT / "TUTORIAL.md"
CAPSTONE = ROOT / "examples" / "06_tutorial" / "main.c"

HIDDEN = "///-"
GENERATED_HEADER = ("<!-- GENERATED FILE: rendered from TUTORIAL.mdt by "
                    "tools/check_tutorial.py render. Edit TUTORIAL.mdt. -->")

C_KINDS = ("standalone", "file", "ctx", "fn", "test", "capstone")
PLAIN_KINDS = ("cmake", "sh", "text")

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
#include "net/net.h"
#include "net/net_module.h"
#include "render/camera.h"
#include "render/canvas.h"
#include "render/render2d.h"
#include "render/render3d.h"
#include "render/text.h"
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
                             ecs_entity_t camera, ecs_entity_t canvas,
                             MyeRender3dConfig *cfg)
{
    (void)world; (void)player; (void)camera; (void)canvas; (void)cfg;
"""

FN_OPEN = """
static void mye_doc_fragment(void)
{
"""


class Block:
    def __init__(self, info, line_no):
        self.info = info
        self.line_no = line_no  # 1-based line of the opening fence
        self.lines = []         # body lines, without trailing newlines


def scan(text):
    """All column-0 fenced blocks, with the line number of each fence."""
    blocks, errors = [], []
    current = None
    for no, line in enumerate(text.split("\n"), 1):
        if line.startswith("```"):
            if current is None:
                current = Block(line[3:].strip(), no)
            else:
                blocks.append(current)
                current = None
        elif current is not None:
            current.lines.append(line)
    if current is not None:
        errors.append(f"TUTORIAL.mdt:{current.line_no}: fence never closed")
    return blocks, errors


def classify(block):
    """-> (kind, params). Raises ValueError for anything not in the taxonomy,
    so a mistagged block fails the check instead of silently escaping it."""
    tokens = block.info.split()
    if not tokens:
        raise ValueError("untagged fence: every block must declare its tag "
                         "(use ```text for non-code, or an indented block)")
    if tokens[0] in PLAIN_KINDS:
        if len(tokens) > 1:
            raise ValueError(f"'{tokens[0]}' takes no parameters")
        return tokens[0], {}
    if tokens[0] != "c":
        raise ValueError(f"unknown fence tag {block.info!r}")

    kind = tokens[1] if len(tokens) > 1 else "standalone"
    rest = tokens[2:]
    if kind not in C_KINDS:
        # ```c with only key=value params is a standalone program.
        if "=" in kind:
            rest = tokens[1:]
            kind = "standalone"
        else:
            raise ValueError(f"unknown c block kind {kind!r}")

    params = {}
    for token in rest:
        if token == "norun":
            params["norun"] = ""
            continue
        if "=" not in token:
            raise ValueError(f"malformed parameter {token!r}")
        key, value = token.split("=", 1)
        params[key] = value

    allowed = {"test": {"name"}, "standalone": {"norun"}}.get(kind, set())
    for key in params:
        if key not in allowed:
            raise ValueError(f"parameter {key!r} not allowed on 'c {kind}'")
    if "name" in params and not re.fullmatch(r"[a-z][a-z0-9-]*",
                                             params["name"]):
        raise ValueError(f"name {params['name']!r}: use lower-case "
                         "kebab-case")
    return kind, params


def split_hidden(lines):
    """Resolve hidden markers: strip `///-` (and one following space),
    keeping the line. 1:1 with the input, so #line stays exact."""
    code = []
    for line in lines:
        stripped = line.lstrip()
        if stripped.startswith(HIDDEN):
            indent = line[: len(line) - len(stripped)]
            rest = stripped[len(HIDDEN):]
            code.append(indent + rest[1:] if rest.startswith(" ")
                        else indent + rest)
        else:
            code.append(line)
    return code


def has_hidden(lines):
    return any(line.lstrip().startswith(HIDDEN) for line in lines)


def render_text(mdt_text):
    """TUTORIAL.md: the .mdt with hidden lines dropped from c-family fences
    and everything else byte-identical."""
    out = [GENERATED_HEADER]
    in_c_fence = None  # None outside a fence; else bool
    for line in mdt_text.split("\n"):
        if line.startswith("```"):
            if in_c_fence is None:
                tokens = line[3:].strip().split()
                in_c_fence = bool(tokens) and tokens[0] == "c"
            else:
                in_c_fence = None
            out.append(line)
        elif in_c_fence and line.lstrip().startswith(HIDDEN):
            continue
        else:
            out.append(line)
    return "\n".join(out)


def test_ident(params):
    return "tutorial_" + params["name"].replace("-", "_")


def build_suite(named_blocks):
    """One mye_test.h suite from the named `c test` blocks. #line directives
    point compiler errors and sanitizer stacks back at TUTORIAL.mdt."""
    parts = ["/* GENERATED from TUTORIAL.mdt by tools/check_tutorial.py "
             "extract. Do not edit. */\n",
             PREAMBLE,
             '#include "mye_test.h"\n']
    cases = []
    if not named_blocks:
        parts.append("TEST(tutorial_no_generated_tests_yet)\n{\n"
                     "    ASSERT_TRUE(true);\n}\n")
        cases.append("tutorial_no_generated_tests_yet")
    for block, params in named_blocks:
        ident = test_ident(params)
        body = "\n".join(split_hidden(block.lines))
        parts.append(f"TEST({ident})\n{{\n"
                     f'#line {block.line_no + 1} "TUTORIAL.mdt"\n'
                     f"{body}\n}}\n")
        cases.append(ident)
    joined = ",\n          ".join(f"TEST_CASE({c})" for c in cases)
    parts.append(f"TEST_MAIN({joined})\n")
    return "\n".join(parts)


def structure(mdt_text):
    """Scan + classify, with all structural errors collected."""
    blocks, errors = scan(mdt_text)
    classified = []
    for block in blocks:
        try:
            kind, params = classify(block)
        except ValueError as error:
            errors.append(f"TUTORIAL.mdt:{block.line_no}: {error}")
            continue
        if kind in PLAIN_KINDS + ("capstone",) and has_hidden(block.lines):
            errors.append(f"TUTORIAL.mdt:{block.line_no}: hidden lines are "
                          f"only meaningful in compiled c blocks")
        classified.append((block, kind, params))
    names = {}
    for block, kind, params in classified:
        if kind == "test" and "name" in params:
            name = params["name"]
            if name in names:
                errors.append(f"TUTORIAL.mdt:{block.line_no}: duplicate test "
                              f"name {name!r} (first at line {names[name]})")
            names[name] = block.line_no
    return classified, errors


def source_for(kind, body):
    if kind in ("standalone", "test"):
        return body
    if kind == "file":
        return PREAMBLE + "\n" + body
    if kind == "ctx":
        return PREAMBLE + CTX_OPEN + body + "}\n"
    if kind == "fn":
        return PREAMBLE + FN_OPEN + body + "}\n"
    raise AssertionError(kind)


def do_check(classified, mdt_text, build_dir, run_blocks):
    deps = build_dir / "_deps"
    if not deps.is_dir():
        print(f"no {deps}; configure a build first", file=sys.stderr)
        return 2

    includes = [f"-I{ROOT / 'engine'}", f"-I{ROOT / 'tests'}",
                f"-I{deps / 'raylib-src' / 'src'}",
                f"-I{deps / 'flecs-src' / 'include'}"]
    # libwebsockets is here because the engine pumps the network from
    # mye_progress, so every program that links the engine reaches the
    # transport whether or not it opens a connection.
    libs = [str(build_dir / "engine" / "libengine.a"),
            str(build_dir / "libmye_alloc.a"),
            str(deps / "raylib-build" / "raylib" / "libraylib.a"),
            str(deps / "flecs-build" / "libflecs_static.a"),
            str(deps / "libwebsockets-build" / "lib" / "libwebsockets.a"),
            "-lm", "-lpthread", "-ldl", "-lGL", "-lX11", "-lcap"]

    failures = []
    counts = {}

    if RENDERED.read_text() != render_text(mdt_text):
        failures.append("TUTORIAL.md is stale: run tools/check_tutorial.py "
                        "render")
        print("  STALE   TUTORIAL.md (run: tools/check_tutorial.py render)")

    named = []
    with tempfile.TemporaryDirectory() as tmp:
        for index, (block, kind, params) in enumerate(classified, 1):
            counts[kind] = counts.get(kind, 0) + 1

            if kind == "sh":
                path = Path(tmp) / f"block_{index}.sh"
                path.write_text("\n".join(block.lines) + "\n")
                result = subprocess.run(["bash", "-n", str(path)],
                                        capture_output=True, text=True)
                if result.returncode == 0:
                    print(f"  ok      block {index} (sh syntax)")
                else:
                    failures.append(f"block {index} (sh): "
                                    f"{result.stderr.strip()}")
                    print(f"  FAIL    block {index} (sh)")
                continue
            if kind in PLAIN_KINDS:
                continue

            if kind == "capstone":
                body = "\n".join(block.lines) + "\n"
                if body != CAPSTONE.read_text():
                    failures.append(
                        f"block {index}: the capstone listing has drifted "
                        f"from {CAPSTONE.relative_to(ROOT)}")
                    print(f"  DRIFT   block {index} (capstone)")
                else:
                    print(f"  ok      block {index} (capstone matches source)")
                continue

            if kind == "test" and "name" in params:
                named.append((block, params))
                continue  # compiled once, aggregated, below

            body = "\n".join(split_hidden(block.lines)) + "\n"
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
            if result.returncode != 0:
                first = [line for line in result.stderr.splitlines()
                         if ": error:" in line or "undefined reference" in line
                         ][:3]
                failures.append(f"block {index} ({kind}):\n    " +
                                "\n    ".join(first))
                print(f"  FAIL    block {index} ({kind})")
                continue

            if kind != "standalone" or not run_blocks or "norun" in params:
                print(f"  ok      block {index} ({kind})")
                continue

            # Every tutorial program honours MYE_MAX_FRAMES and exits through
            # mye_shutdown, so a bounded run is a real check: non-zero means
            # a crash or a leak.
            env = dict(os.environ, MYE_MAX_FRAMES="60")
            env.pop("MYE_SCREENSHOT", None)
            env.pop("MYE_OVERLAY", None)
            try:
                run = subprocess.run([str(path.with_suffix(".bin"))],
                                     capture_output=True, text=True,
                                     timeout=60, env=env, cwd=ROOT)
            except subprocess.TimeoutExpired:
                failures.append(f"block {index} (standalone): still running "
                                f"after 60 s despite MYE_MAX_FRAMES")
                print(f"  HANG    block {index} (standalone)")
                continue
            if run.returncode == 0:
                print(f"  ok run  block {index} (standalone)")
            else:
                tail = run.stderr.strip().splitlines()[-3:]
                failures.append(f"block {index} (standalone) ran and exited "
                                f"{run.returncode}:\n    " +
                                "\n    ".join(tail))
                print(f"  FAIL    block {index} (ran, exit "
                      f"{run.returncode})")

        if named:
            path = Path(tmp) / "test_tutorial.c"
            path.write_text(build_suite(named))
            cmd = ["gcc", *WARNINGS, *includes, "-c", str(path),
                   "-o", str(path.with_suffix(".o"))]
            result = subprocess.run(cmd, capture_output=True, text=True)
            if result.returncode == 0:
                print(f"  ok      generated suite ({len(named)} named tests)")
            else:
                first = [line for line in result.stderr.splitlines()
                         if ": error:" in line][:5]
                failures.append("generated test suite:\n    " +
                                "\n    ".join(first))
                print("  FAIL    generated suite")

    print()
    print("  ".join(f"{n} {k}" for k, n in sorted(counts.items())))

    if failures:
        print(f"\n{len(failures)} check(s) failed:\n", file=sys.stderr)
        for failure in failures:
            print(f"  {failure}", file=sys.stderr)
        return 1

    ran = ", standalone programs ran clean" if run_blocks else ""
    print(f"every code block in TUTORIAL.mdt compiles{ran}, "
          "and TUTORIAL.md is fresh")
    return 0


def main():
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("command", nargs="?", default="check",
                        choices=["check", "render", "extract"])
    parser.add_argument("--build", default="build/debug",
                        help="configured build dir, for the fetched headers")
    parser.add_argument("--out", help="extract: generated test file path")
    parser.add_argument("--no-run", action="store_true",
                        help="check: compile standalone blocks but do not "
                             "execute them (for display-less environments)")
    args = parser.parse_args()

    if not MDT.is_file():
        print("TUTORIAL.mdt is missing (it is the authored source; "
              "TUTORIAL.md is generated from it)", file=sys.stderr)
        return 2

    mdt_text = MDT.read_text()
    classified, errors = structure(mdt_text)
    if errors:
        for error in errors:
            print(f"  {error}", file=sys.stderr)
        return 1

    if args.command == "render":
        RENDERED.write_text(render_text(mdt_text))
        print(f"rendered {RENDERED.relative_to(ROOT)}")
        return 0

    if args.command == "extract":
        if not args.out:
            print("extract needs --out", file=sys.stderr)
            return 2
        named = [(block, params) for block, kind, params in classified
                 if kind == "test" and "name" in params]
        out = Path(args.out)
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_text(build_suite(named))
        print(f"extracted {len(named)} tutorial test(s) -> {args.out}")
        return 0

    return do_check(classified, mdt_text, ROOT / args.build,
                    run_blocks=not args.no_run)


if __name__ == "__main__":
    sys.exit(main())
