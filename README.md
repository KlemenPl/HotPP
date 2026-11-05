# HotPP

Hot reload for C++ on Linux and Windows. Edit a source file and the running program picks up the new code without restarting, keeping its state. It works like Live++: changed translation units are recompiled with their original commands and linked against the running process, and the old functions are redirected to the new ones.

**Status:** M1. Editing a `.cpp` file of a running Linux program (GCC or Clang) patches it live: changed functions are replaced, globals and statics keep their values. Headers, exceptions through patched code, `thread_local` and Windows come in later milestones; see [docs/DESIGN.md](docs/DESIGN.md) §11 for the roadmap and current limits.

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Requires CMake ≥ 3.25 and GCC ≥ 11, Clang ≥ 14 or MSVC (VS 2022).

To cross-check the Windows code from Linux with MinGW and Wine:

```bash
cmake -B build/mingw -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-w64.cmake
cmake --build build/mingw -j && ctest --test-dir build/mingw
```

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

`hotpp -v` also prints what happened to every symbol. To try it, run the example (`build/examples/hotpp_example_basic` under `hotpp --`) and edit `examples/basic/game.cpp`.

| Variable | Effect |
|---|---|
| `HOTPP_ENDPOINT` | Socket path / pipe name used by the host and the agent |
| `HOTPP_HOST` | Host executable the agent launches (default `hotpp` on `PATH`) |
| `HOTPP_LAUNCH_HOST=0` | Don't launch a host |
| `HOTPP_DISABLE=1` | Turn hot reload off at runtime |
| `HOTPP_COMPILE_COMMANDS` | Override the `compile_commands.json` path |
