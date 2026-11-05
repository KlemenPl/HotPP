# HotPP

Hot reload for C++ on Linux. Save a source file while your program runs, and the program picks up the new code in well under a second:
- Changed functions are replaced.
- Globals, statics and `thread_local`s keep their values.
- New globals are initialized.

HotPP works like Live++: it recompiles the changed files with your build's own commands, links them against the running process and redirects the old functions to the new ones.

Version 1.0.0 ([changelog](CHANGELOG.md)). Requires Linux on x86-64 and GCC ≥ 11 or Clang ≥ 14. Building HotPP also needs CMake ≥ 3.25, and the tests need Python 3. How it works: [docs/DESIGN.md](docs/DESIGN.md).

## Build HotPP

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This builds the host, `build/hotpp`. Programs built with `hotpp_enable()` know where it is; for others, put it on your `PATH` or set `HOTPP_HOST`.

To install the host, the agent library, the headers and a CMake package, run `cmake --install build --prefix <dir>`.

## Add it to your program

**With CMake**, as a subdirectory or an installed package:

```cmake
add_subdirectory(HotPP)              # or: find_package(HotPP REQUIRED)
add_executable(game main.cpp game.cpp)
hotpp_enable(game)     # links the agent, sets the flags, exports compile_commands.json
add_library(engine SHARED engine.cpp)
hotpp_enable(engine)   # shared libraries are patched too
```

**With the single header:** copy [`single_include/hotpp/hotpp.h`](single_include/hotpp/hotpp.h) into your project (installed as `share/hotpp/single_include/hotpp/hotpp.h`). In one `.cpp` file, ideally one you rarely edit:

```cpp
#define HOTPP_IMPLEMENTATION
#include "hotpp/hotpp.h"
```

Then:
- compile with `-g -fpatchable-function-entry=7,5`, plus `-fno-ipa-icf` with GCC
- link with `-ldl -pthread`
- keep `compile_commands.json` in the build tree; the agent finds it next to the executable or in a parent directory

With CMake, `hotpp_enable(game NO_AGENT)` sets those flags.

**In your code:**

```cpp
#include <hotpp/hotpp.h>

int main() {
    hotpp::init();                 // returns false if no host is available; the program runs normally
    while (running) {
        hotpp::sync_point();       // patches are applied here
        update();
    }
    hotpp::shutdown();
}
```

## Use

```bash
hotpp -- ./game          # start the program under a host
```

Or run `hotpp` in one terminal and start the program normally; if no host is listening, the agent launches one.

Edit and save any `.cpp` file or header the program is built from:

```
[hotpp] compiling game.cpp
[hotpp] patch 1 applied in 48 ms: game.cpp (2 functions replaced, 0 new, 1 variable kept, 8.0 KiB)
```

Debug patched code with gdb as usual: breakpoints, stepping and backtraces work in it.

## API

| | |
|---|---|
| `bool init(const Config& = {})` | Connect to the host. Call once. |
| `void sync_point()` | Apply pending patches. Call where no patched code is mid-update, e.g. the top of the main loop. |
| `void shutdown()` | Disconnect. |
| `bool connected()` | Connected to a host. |
| `bool restarted()` | This process was started by a hot restart. |
| `HOTPP_PRE_PATCH(fn)`, `HOTPP_POST_PATCH(fn)` | Run `void fn()` before / after every patch (rebuild caches, reload data). |
| `HOTPP_PRE_RESTART(fn)` | Run `void fn()` before a hot restart (save state). |

`Config` fields: `compile_commands`, `endpoint`, `host_path`, `launch_host`, `async` (apply patches on the agent thread, for programs without a main loop) and `region_size`. Defining `HOTPP_DISABLE` compiles everything to no-ops.

## Host

```
hotpp [options] [--once]                 wait for programs to connect
hotpp [options] -- <program> [args...]   start a program under the host
```

| Option | |
|---|---|
| `--build "<command>"` | Rebuild command for hot restarts, e.g. `"cmake --build build"`. |
| `--auto-restart` | When a change can't be patched, rebuild and restart the program (same pid, same arguments). |
| `-v` | Print every module at connect and every symbol decision per patch. |
| `--endpoint <path>` | Socket to listen on. |

`kill -USR1 <host pid>` rebuilds and restarts every connected program on demand.

| Environment | |
|---|---|
| `HOTPP_DISABLE=1` | Turn hot reload off. |
| `HOTPP_ASYNC=1` | Same as `Config::async`. |
| `HOTPP_HOST`, `HOTPP_LAUNCH_HOST=0` | Host to launch / don't launch one. |
| `HOTPP_ENDPOINT`, `HOTPP_COMPILE_COMMANDS` | Override the socket / `compile_commands.json` path. |
| `NO_COLOR` | Plain host output. |

## What needs a restart

HotPP rejects these and leaves the program untouched (with `--auto-restart`, it rebuilds and restarts instead):
- a global or static that changes size or type
- a new `thread_local`
- a new use of a shared-library variable the program didn't use before (GCC)

It **warns** when a struct's layout changes (a header gains a member): objects created before the patch keep the old layout. When a file gains a dynamically initialized global, that file's other initializers re-run into scratch copies, so their side effects repeat.

## Tips

- **"waiting for the app to call hotpp::sync_point()":** the program isn't calling it. Call it, or use async mode.
- **gdb stops with SIG38 while patching:** HotPP pauses threads with that signal. Run `handle SIG38 nostop noprint pass`.
- **Restarting the host** is safe: running programs reconnect and the new host continues from the previous one's patches. If it can't (its saved state is gone), it says so; restart the program then.
- **"was rebuilt since the program started":** the executable on disk no longer matches the running one. Restart the program, or use `--auto-restart`.
- **Plugins** loaded with `dlopen` are picked up within about a second of loading, if they're built from sources in the same `compile_commands.json`.
- **Not supported:** LTO, stripped binaries, `-Wl,--gc-sections`, and programs not built with HotPP (they have to link the agent or the single header).

## Develop

```bash
ctest --test-dir build --output-on-failure   # unit + end-to-end tests (gdb optional)
cmake -B build-asan -DHOTPP_SANITIZE=ON      # host and tests under ASan + UBSan (=thread: TSan)
```

CI (`.gitea/workflows/ci.yml`) runs the suite with GCC and Clang, Debug and Release, and under ASan/UBSan.

```bash
cmake --build build --target hotpp_single_header   # after changing the agent
```

## License

Proprietary. Copyright (c) 2026 HotPP contributors. All rights reserved. See [LICENSE](LICENSE).
