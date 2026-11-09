# HotPP

Hot reload for C++ on Linux and Windows. Save a source file while your program runs, and the program picks up the new code in well under a second:
- Changed functions are replaced.
- Globals, statics and `thread_local`s keep their values.
- New globals are initialized.

HotPP works like Live++: it recompiles the changed files with your build's own commands, links them against the running process and redirects the old functions to the new ones.

AI coding agents can drive it too: `hotpp mcp` is an [MCP](https://modelcontextprotocol.io) server that lets an agent launch your program, patch its edits in and read compile errors and output (see [AI agents](#ai-agents-mcp)).

Version 1.2.0 ([changelog](CHANGELOG.md)). Requires x86-64 and either
- Linux with GCC ≥ 11 or Clang ≥ 14, or
- Windows 10 or later with MSVC (Visual Studio 2022) or clang-cl, building with Ninja (CMake's Visual Studio generators write no `compile_commands.json`).

Building HotPP also needs CMake ≥ 3.25, and the tests need Python 3. How it works: [docs/DESIGN.md](docs/DESIGN.md).

## Build HotPP

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

This builds the host, `build/hotpp` (`hotpp.exe` on Windows). Programs built with `hotpp_enable()` know where it is; for others, put it on your `PATH` or set `HOTPP_HOST`.

On Windows, build from a Visual Studio developer prompt with Ninja: `cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release` (add `-DCMAKE_CXX_COMPILER=clang-cl` for clang-cl). Start the host (or the program, which may launch it) from such a prompt too: patch compiles need its `INCLUDE` and `LIB` settings.

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
- GCC, Clang: compile with `-g -fpatchable-function-entry=7,5`, plus `-fno-ipa-icf` with GCC; link with `-ldl -pthread`
- MSVC, clang-cl: compile with `/Z7 /Gy /hotpatch`; link with `/DEBUG:FULL /FUNCTIONPADMIN /OPT:NOREF /OPT:NOICF /INCREMENTAL:NO` and `psapi.lib`
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

Debug patched code with gdb as usual: breakpoints, stepping and backtraces work in it (Linux; on Windows, debuggers don't see patched code yet).

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
| `--auto-restart` | When a change can't be patched, rebuild and restart the program (same arguments; same pid on Linux, a new process on Windows). |
| `--no-auto-reload` | Don't patch saved files until a controller (`hotpp mcp`) asks. |
| `--capture-output` | Run the program on a pseudo-terminal (Windows: a pseudo-console) and keep its output for `hotpp mcp` (it's still printed). |
| `-v` | Print every module at connect and every symbol decision per patch. |
| `--endpoint <path>` | Socket (Windows: named pipe) to listen on. |

`hotpp restart` rebuilds and restarts every connected program on demand (`--pid N` for one); on Linux, `kill -USR1 <host pid>` does the same.

| Environment | |
|---|---|
| `HOTPP_DISABLE=1` | Turn hot reload off. |
| `HOTPP_ASYNC=1` | Same as `Config::async`. |
| `HOTPP_HOST`, `HOTPP_LAUNCH_HOST=0` | Host to launch / don't launch one. |
| `HOTPP_ENDPOINT`, `HOTPP_COMPILE_COMMANDS` | Override the socket or pipe / `compile_commands.json` path. |
| `NO_COLOR` | Plain host output. |

## AI agents (MCP)

`hotpp mcp` is a [Model Context Protocol](https://modelcontextprotocol.io) server on stdio. With it, an agent such as Claude Code, Cursor or Codex can start your program under HotPP, edit sources and patch them into the running program, and see what happened. Add it to Claude Code:

```bash
claude mcp add hotpp -- hotpp mcp --build "cmake --build build"
```

Or for everyone working on the project, in `.mcp.json` (Cursor reads the same format from `.cursor/mcp.json`):

```json
{
  "mcpServers": {
    "hotpp": { "command": "hotpp", "args": ["mcp", "--build", "cmake --build build"] }
  }
}
```

Use the full path to `hotpp` if it isn't on the agent's `PATH`. Then ask the agent to run the program and change it.

| Tool | |
|---|---|
| `hotpp_launch` | Start a program under HotPP, e.g. `["./build/game"]`, and wait until it can be patched. |
| `hotpp_reload` | Apply every changed source and header as one patch; returns success, the compiler's errors, or that a restart is needed. `observe_ms` adds what the program prints right after. |
| `hotpp_restart` | Rebuild (with the build command) and restart the program in place. |
| `hotpp_status` | Programs under HotPP: what they watch, whether they run the sources on disk, the last patch. |
| `hotpp_output` | What a launched program printed; `wait_ms` watches it for a while. |
| `hotpp_logs` | The host's messages and patch history; can wait for new ones. |
| `hotpp_set_auto_reload` | Patch on every save, or only on `hotpp_reload`. |
| `hotpp_stop` | Stop a launched program. |

How it behaves:
- **Programs the agent launches** don't change until it calls `hotpp_reload`, so a change spread over several files is applied in one patch. They stop when the MCP server exits, for example when you quit the agent.
- **Programs you start** with `hotpp -- ./game`, a `hotpp` in a terminal, or a host the agent library launched are found too. The agent can patch and restart them, but not stop them or read their output. If it turns auto-reload off on your host, the setting reverts when the MCP server exits.
- `--no-launch` limits the agent to programs you start yourself.
- `--endpoint <path>` adds a host on a socket outside the usual place.
- `--structured` also returns machine-readable results (`structuredContent`), for scripts. It's off by default because some clients, Claude Code among them, give the model that data instead of the text.

## What needs a restart

HotPP rejects these and leaves the program untouched (with `--auto-restart`, it rebuilds and restarts instead):
- a global or static that changes size or type (with MSVC, any change of a variable's type)
- a new `thread_local`
- a new use of a shared-library variable the program didn't use before (GCC)

It **warns** when a struct's layout changes (a header gains a member): objects created before the patch keep the old layout. It also warns when a patch replaces a function that is running, such as `main()` or a loop that calls `sync_point()`: the running call keeps its old code until it returns. Put per-frame code in functions the loop calls. When a file gains a dynamically initialized global, that file's other initializers re-run into scratch copies, so their side effects repeat.

## Tips

- **Windows:** the executable is moved aside before a rebuild for a hot restart, since Windows doesn't let a running program's file be overwritten; the host deletes the old copies. DLLs aren't patched yet, only the executable.
- **"waiting for the app to call hotpp::sync_point()":** the program isn't calling it. Call it, or use async mode.
- **gdb stops with SIG38 while patching:** HotPP pauses threads with that signal. Run `handle SIG38 nostop noprint pass`.
- **Restarting the host** is safe: running programs reconnect and the new host continues from the previous one's patches. If it can't (its saved state is gone), it says so; restart the program then.
- **"was rebuilt since the program started":** the executable on disk no longer matches the running one. Restart the program, or use `--auto-restart`.
- **Plugins** loaded with `dlopen` are picked up within about a second of loading, if they're built from sources in the same `compile_commands.json`.
- **Not supported:** LTO (`-flto`, `/GL`), stripped binaries, `-Wl,--gc-sections`, incremental linking and edit-and-continue (`/ZI`), and programs not built with HotPP (they have to link the agent or the single header).

## Develop

```bash
ctest --test-dir build --output-on-failure   # unit + end-to-end tests (gdb optional)
cmake -B build-asan -DHOTPP_SANITIZE=ON      # host and tests under ASan + UBSan (=thread: TSan)
```

CI (`.github/workflows/ci.yml`) runs the suite with GCC and Clang, Debug and Release, and under ASan/UBSan and TSan; on Windows with MSVC and clang-cl; and a Windows build cross-compiled with clang-cl under Wine.

Windows builds can be tested from Linux, with clang-cl, the MSVC runtime and SDK unpacked by [xwin](https://github.com/Jake-Shadle/xwin), and Wine:

```bash
xwin --accept-license splat --output ~/.xwin     # accepts Microsoft's license
cmake -B build-win -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/clang-cl-xwin.cmake
cmake --build build-win && ctest --test-dir build-win   # runs under Wine
```

```bash
cmake --build build --target hotpp_single_header   # after changing the agent
```

## License

MIT. Copyright (c) 2026 HotPP contributors. See [LICENSE](LICENSE).
