# Changelog

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
