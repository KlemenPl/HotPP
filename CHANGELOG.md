# Changelog

## Unreleased

From a trial run with Claude Code:
- MCP results are text only unless `hotpp mcp --structured` is given. Claude Code gave the model the structured data instead of the text, which was longer and lacked the guidance.
- `hotpp_reload` and `hotpp_restart` take `observe_ms`: after the change, wait that long and include what the program printed. `hotpp_output` takes `wait_ms`. The agent had no way to watch the program for a few seconds.
- A layout warning in `hotpp_reload` comes with plain advice to restart.
- The host warns when a patch replaces a function that is running on the thread that applied it, such as `main()` or a loop calling `sync_point()`: that call keeps its old code. The agent had changed `main()`'s loop and saw no effect. The agent library sends a new `ActiveFrames` message; older hosts ignore it, and older agents still work.

## 1.1.0 — 2026-11-06

AI coding agents can use HotPP: `hotpp mcp` is a Model Context Protocol server.

### MCP server
- `hotpp mcp` speaks MCP over stdio (protocol versions 2024-11-05 to 2025-11-25). Add it to Claude Code with `claude mcp add hotpp -- hotpp mcp --build "cmake --build build"`, or to any client that runs stdio servers.
- Tools: `hotpp_launch`, `hotpp_reload`, `hotpp_restart`, `hotpp_status`, `hotpp_output`, `hotpp_logs`, `hotpp_set_auto_reload` and `hotpp_stop`.
- `hotpp_reload` applies every changed file as one patch. It returns success, the compiler's output, or that a restart is needed, and says whether the program runs the sources on disk.
- Programs an agent launches are patched only when it calls `hotpp_reload`, keep their output for it, and stop when the server exits, even if it is killed.
- Programs you start yourself are found too; the agent can patch and restart them but not stop them. `--no-launch` limits it to those.
- Progress notifications, cancellation, strict argument checking, and readable text plus structured results.

### Host
- Hosts accept controllers on their socket, next to agents (JSON over the same framing). The agent protocol is unchanged: programs built with 1.0 work with this host.
- New options: `--no-auto-reload`, `--capture-output` (the program runs on a pseudo-terminal and its output is kept) and `--lifeline <fd>`.
- Every patch attempt is recorded with its status, files, errors and compiler output.
- A reload request also rescans tracked files' sizes and modification times, so changes inotify missed are found.
- Programs started by the host get `SIGPIPE` back at its default, and the host no longer dies when its stdout closes.

### Tested
- Unit tests for the MCP layer (including thousands of mutated messages), the control channel, the event log and the JSON writer.
- An end-to-end test drives `hotpp mcp` like an agent: launch, a two-file patch, a compile error and its fix, a restart, attaching to a user's host, and the server being killed. It runs in all six CI configurations, including ASan and TSan.

## 1.0.0 — 2026-11-05

First release. Hot reload for C++ on Linux (x86-64, GCC ≥ 11 and Clang ≥ 14).

### Patching
- Save a `.cpp` file or a header of a running program and it is recompiled with the build's own command, linked against the process and applied, typically in well under a second.
- Changed functions are redirected to their new versions. Repeated patches redirect every earlier version.
- State is kept: globals, file statics, anonymous-namespace variables, function-local statics, existing `thread_local`s, heap and stack.
- New functions and variables work. New globals with dynamic initializers are initialized, without re-initializing existing ones.
- Header edits recompile every including file into one patch.
- Inline functions, templates, lambdas, `std::function`s holding old lambdas, virtual functions, `static` functions and function pointers all reach the new code.
- Exceptions and RTTI work across old and new code.
- Multithreaded programs: other threads are paused only while function entries are rewritten.
- Patches apply in `hotpp::sync_point()`, or immediately in async mode.
- The executable, shared libraries it links and libraries loaded later with `dlopen` are all patched.

### When a change can't be patched
- Resized or retyped globals, new `thread_local`s and similar changes are rejected without touching the program.
- With `--build "<cmd>" --auto-restart`, the host rebuilds and restarts the program instead (same pid, same arguments). `HOTPP_PRE_RESTART` hooks and `hotpp::restarted()` carry state across.
- Struct layout changes trigger a warning.

### Integration
- CMake: `add_subdirectory(HotPP)` or `find_package(HotPP)`, then `hotpp_enable(target)`.
- Single header: `single_include/hotpp/hotpp.h` with `HOTPP_IMPLEMENTATION`.
- `HOTPP_PRE_PATCH` / `HOTPP_POST_PATCH` hooks.
- gdb: breakpoints, stepping and backtraces in patched code.
- `cmake --install` installs the host, the agent library, the headers and the CMake package.

### Robustness
- The host can crash or be restarted: programs reconnect, and the new host resumes the patch history, or refuses to patch if it can't verify it.
- Clean shutdown on `SIGINT`/`SIGTERM`.
- Response files in compile commands; `compile_commands.json` is reloaded when it changes.
- Tested with GCC 14 and Clang 19 at `-O0` and `-O2`, under ASan/UBSan and TSan, with corrupted-input tests for the ELF and DWARF parsers, in CI.

### Not supported
- Windows, macOS, ARM; LTO; stripped binaries; `--gc-sections`.
- Attaching to a program that wasn't built with the agent.
