# HotPP

Hot reload for C++ on Linux (x86-64, GCC and Clang). Edit a source file and the running program picks up the new code without restarting, keeping its state. It works like Live++: changed translation units are recompiled with their original commands and linked against the running process, and the old functions are redirected to the new ones.

**Status:** M4. Saving a `.cpp` file or a header of a running program patches it live:
- changed functions are replaced, while globals, statics and `thread_local`s keep their values
- new globals get initialized
- exceptions, virtual calls, lambdas and threads work across old and new code
- gdb can set breakpoints in and step through patched code

See [docs/DESIGN.md](docs/DESIGN.md) for how it works and §11 for the roadmap and current limits.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires Linux on x86-64, CMake ≥ 3.25, GCC ≥ 11 or Clang ≥ 14, and Python 3 for the end-to-end tests.

## Use

In your CMake project:

```cmake
add_subdirectory(HotPP)
add_executable(game main.cpp)
hotpp_enable(game)   # links the agent and sets the required compiler/linker flags
```

In your program:

```cpp
#include <hotpp/hotpp.h>

int main() {
    hotpp::init();             // connects to the host; returns false if unavailable
    while (running) {
        hotpp::sync_point();   // patches are applied here
        update();
    }
    hotpp::shutdown();
}
```

Then either start the program under the host:

```bash
hotpp -- ./game
```

or run `hotpp` in a terminal and start the program normally. If no host is listening, the agent launches one.

Now edit a `.cpp` file the program is built from and save. The host recompiles it with the build's own command, links it against the running program and the change takes effect at the next `sync_point()`:

```
[hotpp] compiling game.cpp
[hotpp] patch 1 applied in 50 ms: game.cpp (2 functions replaced, 0 new, 1 variable kept, 4.0 KiB)
```

`hotpp -v` also prints the full module list on connect and what happened to every symbol in each patch. To try it, run the example (`build/examples/hotpp_example_basic` under `hotpp --`) and edit `examples/basic/game.cpp`.

### Hooks, async mode, debugging

```cpp
static void reload_assets() { /* ... */ }
HOTPP_POST_PATCH(reload_assets);   // runs after every patch is live (HOTPP_PRE_PATCH: before)
```

- **Programs without a main loop:** set `Config::async = true` (or `HOTPP_ASYNC=1`), and patches apply on the agent thread as soon as they arrive.
- **gdb** sees patched code automatically: breakpoints by function or `file:line`, stepping and backtraces work. When debugging a multithreaded program while patching, run `handle SIG38 nostop noprint pass` in gdb; HotPP pauses threads with that real-time signal.
- **Layout changes:** if a struct's layout changes (a header gains a member), the host warns. Objects created before the patch keep the old layout.

| Variable | Effect |
|---|---|
| `HOTPP_ENDPOINT` | Socket path / pipe name used by the host and the agent |
| `HOTPP_HOST` | Host executable the agent launches (default `hotpp` on `PATH`) |
| `HOTPP_LAUNCH_HOST=0` | Don't launch a host |
| `HOTPP_DISABLE=1` | Turn hot reload off at runtime |
| `HOTPP_COMPILE_COMMANDS` | Override the `compile_commands.json` path |
| `HOTPP_ASYNC=1` | Apply patches on the agent thread instead of in `sync_point()` |
| `NO_COLOR` | Plain host output on a terminal |
