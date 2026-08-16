# Plan index

Design documents for a 2D + 3D game engine in strict C11, built on
[raylib](https://github.com/raysan5/raylib) (platform/rendering) and
[flecs](https://github.com/SanderMertens/flecs) (ECS).

| Doc | Contents |
|---|---|
| [00-overview.md](00-overview.md) | Vision, prioritized feature list, non-goals, beginner glossary |
| [01-architecture.md](01-architecture.md) | Layers, repo layout, main loop & frame phases |
| [02-ecs.md](02-ecs.md) | flecs usage, ECS vocabulary, core components |
| [03-rendering.md](03-rendering.md) | 2D & 3D passes, main-thread rule, mixed scenes |
| [04-memory.md](04-memory.md) | Arena/frame/pool allocators, TLSF, tracking, ownership |
| [05-concurrency.md](05-concurrency.md) | flecs workers, job pool, channels (Concurrency Kit) |
| [06-assets.md](06-assets.md) | Handles, registries, sync→async loading |
| [07-roadmap.md](07-roadmap.md) | Milestones M0–M7 with definitions of done |
| [08-build.md](08-build.md) | CMake + C11, warnings-as-errors, dependencies, configs |
| [09-testing.md](09-testing.md) | Unit & integration testing policy |
| [10-web.md](10-web.md) | WebAssembly / browser support (planned, M8) |
| [11-web-dev-loop.md](11-web-dev-loop.md) | Web dev workflow: build, serve, hot-reload (planned, M8) |

**Key decisions**: flecs as the ECS (pure C99, archetype-based, multithreaded
pipeline) · concurrency from libraries, not hand-rolled · CMake + C11 with
`-Werror` · 2D playable game first (M2), 3D after (M5).

Start at [00-overview.md](00-overview.md), then [07-roadmap.md](07-roadmap.md)
for what to build first.
