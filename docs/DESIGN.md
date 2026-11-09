# HotPP — Design

Status: version 1.1.2 · M0, M1, M3, M4, M5.1, M5.2 and M6 (MCP) implemented; M2 (Windows) and M5.3 (attach) dropped · Last updated: 2026-11-06

HotPP is a hot-reload tool for C++ on Linux (x86-64, GCC and Clang). You edit a `.cpp` file or a header, save it, and the running program picks up the new code, typically within a fraction of a second. It doesn't restart and doesn't lose state.

It works the way Live++ does:
1. It recompiles only the affected translation units with their original compiler commands.
2. It links the resulting object files against the *running* process.
3. It maps the new code into that process and redirects every old function to its new version.

Global and heap state stays where it is.

---

## 1. Goals and non-goals

### Goals
- **Fast edit-to-running loop.** Under about 2 s for a single-TU change on a typical project.
- **State is preserved.** Heap, stack, globals, function-local statics and thread-locals all survive a patch.
- **Minimal integration.** Call `hotpp::init()` once, optionally call `hotpp::sync_point()` in the main loop, and enable the flags (one CMake call). No interfaces or base classes need to be added to user code.
- **Fail safe.** A compile error, an unsupported change or a host crash never corrupts or kills the target process. A patch either applies completely or not at all.

### Non-goals
- Windows, macOS, ARM64, 32-bit x86. Windows (the former M2) was dropped on 2026-11-05.
- LTO (`-flto`).
- Changing the layout of a struct or class while instances of it exist. HotPP reports *restart required* where it can detect such a change, and warns where it can only suspect one (§7.6).
- Attaching to a process that doesn't link `hotpp_agent` (M5.3, dropped). As with Live++, a program opts in by linking the agent or the single header.
- Being a build system. HotPP uses the commands the real build recorded.

---

## 2. Platform

| | |
|---|---|
| OS / CPU | Linux, x86-64 |
| Compilers | GCC ≥ 11, Clang ≥ 14 (tested: GCC 14, Clang 19) |
| Formats | ELF relocatable objects, ELF executables (PIE and non-PIE) |
| Symbols / debug info | `.symtab` + DWARF 4/5 |
| C++ runtime | libstdc++ with libgcc's unwinder |

GCC and Clang share every code path. The differences are a few code patterns (padding NOP encodings, initializer function names), which the code handles explicitly.

---

## 3. Architecture

```
 ┌─────────────────────────── hotpp (host process) ───────────────────────────┐
 │ inotify watcher ─▶ dependency graph ─▶ compiler (original flags) ─▶ .o     │
 │                                                                     │      │
 │ executable symbol index (.symtab + DWARF) ─▶ patch linker ◀─────────┘      │
 │                                                  │ binding policy, layout, │
 │                                                  │ relocation, patch sites │
 └──────────────────────────────────────────────────┼─────────────────────────┘
                                                    │ Unix domain socket
 ┌────────────────────────── target process ────────▼─────────────────────────┐
 │ user code ··· hotpp_agent: patch region next to the executable,            │
 │               map segments · unwind tables · initializers ·                │
 │               pause threads · write patch sites · debugger registration    │
 └────────────────────────────────────────────────────────────────────────────┘
```

### 3.1 Host links, agent maps
The core split is that **the host does all the hard work and the agent only maps bytes.**

- **`hotpp` (host)** is a separate executable with no limits on dependencies or memory. It:
  - watches files and keeps the dependency graph
  - runs the compiler
  - parses objects, the executable and DWARF
  - resolves symbols and applies relocations, producing final bytes for known target addresses

  A bug in this complex code crashes the host, never the app.
- **`hotpp_agent`** is a small static library linked into the app (C++17, no RTTI or exceptions in its interface). Its jobs:
  1. At `init`, reserve a large address range (default 512 MB) within ±2 GB of the main executable (§7.3). The host asks for another one next to each shared library it patches.
  2. Connect to the host, launching it if needed. Report the process ID, executable path, the module list (load bases, build IDs, TLS module IDs and offsets) and the reserved range.
  3. Answer symbol lookups (`dlsym`) and apply patches (§8).

Because the region is reserved up front, the host knows the final addresses before linking. Applying a patch is one message plus one symbol-lookup round trip.

### 3.2 IPC
The host listens and agents connect. One host serves any number of agents.

- **Endpoint:** a Unix domain socket, by default `$XDG_RUNTIME_DIR/hotpp.sock` (or `/tmp/hotpp-<uid>.sock`), created with mode 0600.
  - A socket file left by a dead host is detected and replaced. A second host on a live endpoint refuses to start.
  - `$HOTPP_ENDPOINT` overrides the default for both the agent and the host.
- **Two ways to start:**
  - *Serve:* run `hotpp` and start the app normally. If no host is listening, the agent launches one (`$HOTPP_HOST`, default `hotpp` on `PATH`), unless `launch_host` is off or `$HOTPP_LAUNCH_HOST=0`.
  - *Run:* `hotpp -- <program> [args]` listens on a private endpoint, passes it to the child in `$HOTPP_ENDPOINT` and exits with the child's exit code. It exits 2 if no agent connected and 3 if an agent reported an unusable layout. Tests use this mode.
- **Protocol:** length-prefixed binary messages with a version number. Framing is in `src/common/protocol.h`, messages in `src/common/messages.h`.

| Message | Direction | Contents |
|---|---|---|
| `Hello` | agent → host | pid, exe, compile_commands path, modules (base, span, path, build ID), patch regions |
| `HelloAck` | host → agent | host pid and version |
| `ResolveRequest` / `ResolveReply` | host ↔ agent | symbol names → addresses (`dlsym`) |
| `ReserveRequest` / `ReserveReply` | host ↔ agent | a patch region next to a given module |
| `Patch` | host → agent | segments, patch sites, `.eh_frame` blocks, initializers, debug objects |
| `PatchResult` | agent → host | id, ok, error |
| `Restart` | host → agent | re-exec the (rebuilt) executable (§8.8) |
| `ModulesChanged` | agent → host | libraries loaded or unloaded at runtime (§6) |
| `ActiveFrames` | agent → host | before a `PatchResult` from `sync_point()`: the return addresses on that thread's stack. Hosts that don't know the type ignore it, so the protocol version stayed. |
| `Log`, `Bye` | both | text for the user; orderly disconnect |

### 3.3 Host restarts and reconnecting
The host may crash or be restarted while programs keep running.

**The agent:**
- remembers its endpoint and its hello
- tracks how much of each patch region holds applied patches (`RegionRecord::used`, counted even when applying fails half way)
- when the connection drops without a `shutdown()` or a hot restart, retries with backoff (250 ms up to 5 s) and sends the hello again, marked `resumed`

**The host** writes a state file, `<build>/.hotpp/<pid>/state`, after every patch. It holds:
- each module's path, build ID, region base and cursor
- every applied index delta (new symbols and function versions), in order
- the next patch number

When a session ends while the process is still alive, the state file stays (only the compiled objects are removed). When a `resumed` hello arrives:
- the state file must exist, name the same pid and modules, and have cursors equal to the agent's `used` values
- the deltas are replayed into the fresh indexes, and patching continues with the next patch number
- regions already reserved for libraries are reused

If the state is missing (and something was patched) or doesn't match, the host refuses to patch, with a message to restart the program. New code could otherwise land on live code, and the old patches' new symbols would be unknown. A hello that isn't `resumed` (a new process, or a hot restart reusing the pid) deletes any stale state file.

The host itself shuts down cleanly on `SIGINT`/`SIGTERM`: it stops accepting, removes the socket file, ends open connections and joins the session threads. In run mode, it passes `SIGTERM` (or a second Ctrl+C) on to the program.

### 3.4 Controllers
Besides agents, a host accepts **controllers** on the same socket: today that's `hotpp mcp` (§14). The first message decides which a connection is: `Hello` for an agent, `ControlHello` for a controller. Agents and their protocol version are untouched, so programs built with 1.0 work with a 1.1 host. A 1.0 host drops a `ControlHello`, and controllers report it as one they can't control.

- **Framing:** the usual 12-byte header, then UTF-8 JSON (`src/host/control_protocol.h`). The controller sends `{"control_version": 1, "client": ...}`. The host answers with a description of itself: version, pid, endpoint, mode, build command, whether it captures output, and its child in run mode.
- **Requests** are `{"id", "method", "params"}`. Answers are `{"id", "result"}` or `{"id", "error": {"message"}}`, with any number of `{"id", "progress": "<log line>"}` before them. Requests run concurrently, at most 8 per connection.

| Method | Does |
|---|---|
| `status` | the host and every connected program's `SessionInfo` |
| `reload` | apply all changes now (see below) and report |
| `restart` | rebuild (if asked and configured), re-exec, wait until the same pid is back and set up |
| `events` | the event log after a cursor, optionally waiting for more |
| `output` | the captured program output after a cursor, or its last lines |
| `set_auto_reload` | patch on save or only on request, for one program |
| `wait_program` | wait until a program is set up, or the child exits (used right after launching) |
| `stop` | stop the child and let the host exit; only on hosts started with `--lifeline` |

**What other threads see of a session.** A `SessionHandle` per session (`src/host/registry.h`) is all other threads touch. It holds:
- a snapshot of its state, updated by the session's own threads: state, modules, source and header counts, patch counters, the last 20 `PatchReport`s, pending and failed files, auto-reload
- a queue of requests (`reload`, `restart`) that the watcher thread takes between file events. An eventfd wakes it.

The host's `Registry` lists the newest session of each pid, so a hot restart's lingering old session is hidden.

**Reports and events.**
- Every patch attempt produces a `PatchReport`:
  - id, trigger (`watch` or `request`), the files and the changed headers that pulled them in
  - status: `applied`, `compile_failed`, `link_failed`, `restart_required`, `failed_in_app` or `not_applied`
  - the console line, the error, warnings and notes, the counts
  - the compiler's output, with ANSI color codes removed and capped at 48 KiB of the start plus 8 KiB of the end
- Reports, log lines, connects, disconnects and the child's exit go into a bounded `EventLog` (5000 events). Its sequence numbers serve as cursors.

**Reload.** A reload request takes, as one patch:
- the files deferred while auto-reload was off
- watcher events not read yet
- every tracked source and header whose size or modification time changed (hashing only those). This catches what inotify missed.

It returns the outcome:
- `up_to_date` when nothing changed and nothing is failing
- `out_of_sync` when nothing changed but an earlier patch of some file failed: that report comes back again
- `waiting_for_sync_point` if the program hasn't applied the patch 2 s after receiving it. The patch stays queued.

**Auto-reload** is per program. `--no-auto-reload` turns it off at the start. A controller's `set_auto_reload` lasts while its connection does: the registry keeps it per pid, across hot restarts, and reverts it when that controller disconnects. That way an agent can't leave a user's host paused.

**Launched hosts.** `hotpp mcp` starts hosts in run mode with these flags:
- `--capture-output`: the program's stdout and stderr go to a pseudo-terminal, so it keeps line buffering. The host keeps the last 1 MiB with byte offsets, and still prints it.
- `--lifeline <fd>`: the read end of a pipe whose only write end the MCP server holds. End of file means the server is gone, however it died: the host sends the program `SIGTERM`, then `SIGKILL` after 5 s, and exits. A death signal (`PR_SET_PDEATHSIG`) would fire when the spawning *thread* exits, not the process.
- After the program exits, such a host stays until a `stop` or the lifeline closes, so the agent can still read the output.
- Child processes get `SIGPIPE` at its default and an empty signal mask, whatever the host has.

**Security.** The socket is mode 0600 in the user's runtime directory. The host also checks the peer's uid (`SO_PEERCRED`) on control connections. A controller can't change the build command; only whoever starts the host sets it. At most 16 controllers per host, requests up to 1 MiB.

---

## 4. Build requirements

`cmake/HotPP.cmake` provides `hotpp_enable(<target>)`. For an executable, it:
- links `hotpp::agent` (unless `NO_AGENT`)
- applies the flags below, exports compile commands and disables IPO
- defines the build's `compile_commands.json` path, and the host's path when the `hotpp::hotpp` target exists, so the agent can launch the host without `PATH`

`hotpp_enable()` comes with `add_subdirectory(HotPP)` and with the installed package (`find_package(HotPP)`; `cmake --install` puts the host, the agent library, both headers and the package files under the prefix). Users of other build systems apply the same flags by hand.

| Flag | Why |
|---|---|
| `-g` | DWARF tells the host which sources the program is built from (§6) and gives the debugger the patched code (§9). |
| `-fpatchable-function-entry=7,5` | Puts 5 NOP bytes before each function entry and 2 at the entry: room for the patch-site jump (§8.3). |
| `-fno-ipa-icf` (GCC) | Identical-code folding merges identical functions, so patching one would patch them all. |
| No `-Wl,--gc-sections` | Functions that look unused would be removed and couldn't be patched later. |
| No `--icf` (gold / lld) | Same reason as `-fno-ipa-icf`, at link time. Off by default. |
| Don't strip the binary | `.symtab` holds the local symbols HotPP needs. |
| No `-flto` | LTO breaks the one-function-per-symbol mapping and inlines across TUs. |

`compile_commands.json` must exist (CMake with Ninja or Makefiles, Meson, or Bear). The agent's path for it comes from, in order:
1. `$HOTPP_COMPILE_COMMANDS`
2. `Config::compile_commands` (set by `hotpp_enable()`)
3. a search of the executable's directory and its parents, then the working directory

Optimization level is free; `-O0` and `-O2` are tested (§12).

**Single header.** `single_include/hotpp/hotpp.h` is the public API plus the whole agent, generated by `tools/amalgamate.py` (CMake target `hotpp_single_header`; the `single_header_up_to_date` test fails if it's stale). One `.cpp` file defines `HOTPP_IMPLEMENTATION` before including it. Such programs link `-ldl -pthread` instead of `hotpp_agent`, and `hotpp_enable(<target> NO_AGENT)` gives them just the flags.

---

## 5. Change detection and recompilation

### 5.1 Watching
- inotify (`IN_CLOSE_WRITE | IN_MOVED_TO`) on the directories of every source the program is built from, plus the directories of every header in the dependency graph.
- Events are debounced: after one arrives, the watcher keeps collecting until 100 ms pass without another. That absorbs editors' save-via-rename and multi-file saves.
- A file only counts as changed if its content hash changed, which filters out `touch` and no-op saves.

### 5.2 Dependency graph
- Each TU's includes come from the compiler itself: the original command with `-MM -MF <file>` instead of compiling. `-MM` leaves out system headers, so `/usr/include` is never watched.
- The scan runs in the background on all cores when the agent connects. `.cpp` edits work at once; header edits wait for the scan.
- Results are cached in `<build>/.hotpp/deps/`, one depfile per TU named by a hash of its path and arguments. A cached depfile is reused while neither the TU nor any file it lists is newer than it.
- Every patch compile also writes fresh dependencies (`-MMD -MF`). The graph stays current as includes change, and newly included headers are watched from then on.

### 5.3 Recompiling
1. Affected set = the changed `.cpp` file, or every TU whose include set contains the changed header. TUs that aren't part of the running program (other targets in the same build) are left out.
2. Each TU compiles with its exact `arguments`/`command` from `compile_commands.json`. Response files (`@file`, nested ones too) are expanded first, so flags inside them can be rewritten. The command gets these changes only:
   - the output goes to `<build>/.hotpp/<pid>/patch-<n>/`
   - the build's own dependency-file flags (`-MD`, `-MF`, …) are dropped, so its `.d` files aren't overwritten
   - `-ffunction-sections -fdata-sections` are added (§7.2)
3. All affected TUs compile in parallel and go into a single patch.
4. On any failure, the compiler output is shown and nothing is applied. The next save retries.

`compile_commands.json` is reloaded whenever it changes on disk (the build system was re-run), so later patches use the new flags.

Header edits can recompile many TUs. That is the price of correctness: every TU that included the header may have inlined its code. Live++ makes the same trade-off.

---

## 6. Module symbol index

The host patches every loaded module (the executable and shared libraries) that is built from sources in `compile_commands.json`:
- Modules in system directories (`/lib`, `/usr/lib`, …) are skipped without being read.
- Other modules are tracked if their DWARF compile units include a source in the compile database.
- The executable is always tracked.

Each tracked module gets its own symbol index, patch region and source set. A changed source is linked against every module built from it, and the results go out as one patch.

**Libraries loaded at runtime.** While idle, the agent thread checks the dynamic linker's load/unload counters (`dlpi_adds`, `dlpi_subs`) about once a second. When they change, it sends `ModulesChanged`, listing the modules that appeared and the indices of those that went away. Module indices stay stable for the life of the process: new modules are appended, and unloaded ones keep their slot with an empty span, so no patch site validates against them. The host:
- tracks a new module like the startup ones (a region via `ReserveRequest`, a symbol index, watched sources, a dependency scan, layouts)
- stops patching a module once it is unloaded

The module list in a resumed hello includes runtime-loaded modules, so a restarted host picks them up too.

Each index maps identity key → runtime address, size, kind and writability. It is read once from the module's file on disk, plus the load bias the agent reports. Each applied patch adds to it (§7.5).

- **Identity key:** global symbols by mangled name; local symbols (`static`, anonymous namespace, lambdas in non-inline functions) by `(defining file, mangled name)`.
- **Local scoping:** the linker groups local symbols after an `STT_FILE` symbol naming their source file, so `(file, symbol)` comes for free. Two locals with the same key (two files with the same base name both defining it) are marked *ambiguous*. Data bound to them is an error; functions are loaded new without redirecting.
- **Runtime address:** load bias (`dl_iterate_phdr`) + `st_value`. TLS symbols keep their offset in the TLS template.
- **Patch sites:** for every function, the host checks the file bytes around its entry for the expected padding (§8.3). Functions without it, for example from code built without the flags, are never redirected.
- **Program membership:** the DWARF compile units (`DW_AT_name` joined with `DW_AT_comp_dir`; DWARF 4 and 5, including `strx` forms) list exactly the sources the executable was built from. Edits to other files in the same build are ignored instead of being patched in.
- **Identity check:** the agent sends each module's GNU build ID as mapped in memory. If the file on disk has a different one (rebuilt since it was loaded), the host doesn't patch that module, because its symbol table no longer describes the running code. For the executable, that disables hot reload.
- **Interposed variables:** a shared library's exported variables may not live in the library. A PIE executable that uses one gets its own copy (a *copy relocation*), and the dynamic linker binds the library's references to that copy. For every exported variable of a library, the host asks the agent (`dlsym`) where it really is and binds to that address.

---

## 7. Patch linker

The host links the new object files against the symbol index into the agent's reserved region. No system linker is involved. The code is `src/host/link/linker.cpp`, on top of `object/elf_object` (relocatable objects) and `link/symbol_index`.

### 7.1 Loading objects
- Sections, symbols (with `SHN_XINDEX`), RELA relocations and COMDAT groups are parsed.
- When several patch objects define the same COMDAT group (an inline function or template instance in several TUs), the first copy is kept. References to a discarded copy's sections go to the kept copy's section of the same name.
- Skipped: `.note*`, `__patchable_function_entries`, `.fini_array`, and non-allocated sections (debug info goes to the debugger image instead, §9).

### 7.2 Binding policy
For every symbol a patch defines that already exists in the index, the policy decides whether references use the **old** definition or the **new** one. This rule is what makes hot reload preserve state.

| Symbol kind | Binds to | Reason |
|---|---|---|
| Functions | **new** | The point of the patch. The old entry is redirected (§8.3). |
| Writable data (`.data`, `.bss`, `.data.rel`) | **old** | Preserves state. |
| Function-local statics and their guard variables (`_ZGV*`) | **old** | Preserves state; the initializer doesn't run again. |
| vtables (`_ZTV*`, `_ZTT*`) | **old** | Existing objects point at the old vtable; its entries reach new code through the redirected functions. |
| RTTI (`_ZTI*`, `_ZTS*`) | **old** | `catch`, `dynamic_cast` and `typeid` compare type identity. |
| Existing `thread_local` (`.tdata`, `.tbss`) | **old** | Every thread's TLS block is already laid out (§7.7). |
| Read-only data (`.rodata`, `.data.rel.ro`) | **new** | Changed constants and tables take effect. |
| Unnamed constants, string literals, jump tables | **new** | They belong to the new code. |

**Binding is per section, not per symbol.** Compilers refer to internal variables through *section* symbols (`.bss._ZL5ticks - 4`), not through the variable's own symbol. Patch compiles therefore use `-fdata-sections`, which gives each variable its own section. A section whose named symbols all exist is bound as a whole to the old address (old address − symbol offset). A section mixing existing and new variables is an error. Text sections are always loaded new.

**Canonical function addresses.** Calls go straight to the new code. *Taking the address* of an existing function (a `lea`, a GOT slot, a vtable or a pointer constant) yields its original address. Function pointers then compare equal between old and new code, and reach the newest version through the redirect.

**Undefined references** resolve in this order:
1. definitions in the patch itself, subject to the policy above
2. the symbol index, which includes earlier patches
3. exports of the process's other modules (libc, libstdc++, …). All such names go to the agent in one `ResolveRequest`, and the agent looks them up with `dlsym(RTLD_DEFAULT)`. That picks the default symbol version and resolves GNU IFUNCs (`memcpy`, `strlen`) to their implementation, exactly as the dynamic linker did.

A weak reference that stays unresolved becomes 0. Anything else unresolved is an error, and no patch is applied.

### 7.3 Memory placement
- The agent reserves the executable's region at `init`, and a library's region when the host asks (`ReserveRequest`), with `mmap(PROT_NONE, MAP_FIXED_NOREPLACE)`. It places the region in the closest free gap below the executable (from `/proc/self/maps`) such that every byte of it is within ±2 GB of every byte of the executable. If nothing fits below, it goes above, leaving 64 MB for the brk heap; if 512 MB doesn't fit at all, smaller sizes are tried.
- The host allocates within the region with a bump pointer. Each patch gets page-aligned segments: code + stubs (RX), read-only data + GOT + `.eh_frame` (R), data + bss + scratch copies (RW).
- Every rel32 reference between the patch and the executable reaches. Calls into shared libraries go through 14-byte stubs (`jmp *0(%rip)` + address). `GOTPCREL` loads use GOT slots in the patch.
- Patch memory is never freed.

### 7.4 Relocations (x86-64)
S = symbol value, A = addend, P = address of the relocated place.

| Relocation | Handling |
|---|---|
| `64`, `PC64` | S + A, S + A − P |
| `PC32`, `PLT32` | S + A − P, range-checked. A `PLT32` to a shared library goes through a stub. A `PC32` to a shared-library *variable* can't reach and is "restart required" (the executable has no copy of it). |
| `GOTPCREL`, `GOTPCRELX`, `REX_GOTPCRELX` | A GOT slot holding S (deduplicated); not relaxed. |
| `32`, `32S` | S + A with an overflow check (non-PIE code). |
| `SIZE32`, `SIZE64` | symbol size + A |
| `TPOFF32/64`, `GOTTPOFF`, `TLSGD`, `TLSLD`, `DTPOFF32/64`, `DTPMOD64` | §7.7 |
| TLS descriptors, anything else | Error naming the type and object. |

### 7.5 Repeated patches
- Each applied patch's new symbols, and the new version of every function, go into the index.
- A function patched again redirects its original entry **and** every earlier version to the newest one. Pointers captured at any point reach the newest code.
- Data introduced by patch *n* is "old" data for patch *n+1*.

### 7.6 Restart-required detection
Before producing a patch, the host rejects changes it knows it can't apply, with a message and without touching the program:
- an existing variable (global, static, vtable) whose size changed
- a new `thread_local` variable, or a variable that became or stopped being `thread_local`
- a symbol that changed between function and variable
- a new direct reference to a shared-library variable

**Layout warnings (M4).** A struct whose layout changes behind pointers can't be seen from symbols. For every struct/class defined in a recompiled TU, the host compares the DWARF layout (size, member names and offsets) with the one in the executable's DWARF. If it differs, it warns that objects created before the patch still have the old layout. The patch still applies.

### 7.7 Thread-local storage
- `.tdata`/`.tbss` sections of a patch bind to the existing variables like other data (per section).
- The agent reports each module's TLS layout as the process sees it:
  - `dlpi_tls_modid`, the module ID `__tls_get_addr` takes
  - for modules in static TLS (everything loaded at startup), the block's offset from the thread pointer (`dlpi_tls_data − %fs:0`)

  The thread-pointer offset of a variable is that offset + its `st_value`. Library code built with `-fPIC` uses the general/local-dynamic models with the library's module ID.
- Access models:

| Model | Relocations | Handling |
|---|---|---|
| local-exec | `TPOFF32/64` | the thread-pointer offset |
| initial-exec | `GOTTPOFF` | a GOT slot holding the offset |
| general / local dynamic | `TLSGD`, `TLSLD`, `DTPOFF*` | a GOT pair `{module 1, offset}` for `__tls_get_addr` |

- TLS descriptors (`-mtls-dialect=gnu2`) are rejected.
- New TLS variables would need every thread's TLS block to grow, so they are "restart required".

### 7.8 GCC function clones
GCC creates internal clones (`foo.constprop.0`, `foo.isra.0`, `foo.part.0`, `foo.cold`). These need no special handling:
- clones are local and never address-taken
- every old caller of an old clone is in the same TU and is itself redirected to new code, which calls the new clones
- `.cold` fragments aren't entry points

---

## 8. Applying a patch (agent)

### 8.1 Modes
- **Sync-point mode (default).** The agent thread receives the patch and queues it. The patch applies inside the next `hotpp::sync_point()` call, typically at the top of the main loop.
- **Async mode** (`Config::async = true` or `$HOTPP_ASYNC=1`). The agent thread applies the patch as soon as it arrives, even while the main thread is busy in a loop that never reaches `sync_point()`. This suits programs without a natural loop. Thread pausing (step 5) protects the patch sites exactly as in sync-point mode.

Either way, other threads are paused only while the patch sites are written (§8.2).

### 8.2 Order of operations
1. Validate everything (segments inside the region, sites inside the executable or region, expected bytes at each entry) before writing anything.
2. Map the segments: RW, copy, then the final protection (RX / R / RW).
3. Register the patch's `.eh_frame` (§8.5).
4. Run the initializers (§8.6). Nothing can reach the new code yet, so other threads keep running.
5. Pause the other threads:
   - Signal each one listed in `/proc/self/task` (except the agent's own thread) with a reserved real-time signal. The handler records its RIP from `ucontext` and waits on a futex.
   - Re-list until no new threads appear.
   - If any RIP is inside a byte range about to be rewritten, release everyone, wait 1 ms and retry (up to 200 times).
   - A thread that doesn't stop within 2 s (it blocks the signal) fails the patch without changing anything.
6. Write the patch sites (§8.3), then `membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED_SYNC_CORE)` to serialize every core that runs the process. That is the x86 rule for cross-modifying code.
7. Resume the threads, register the debug objects (§9), run the post-patch hooks (§8.7) and report the result.

### 8.3 Patch-site scheme
```
    entry-5:  E9 xx xx xx xx      jmp  new_entry       ; written first, into padding never executed
    entry+0:  EB F9               jmp  short entry-5   ; then one atomic 2-byte store
```
- `-fpatchable-function-entry=7,5` gives 5 NOP bytes before the entry and 2 at it. GCC emits `90 90`; Clang emits `66 90` (one instruction). The host accepts any standard NOP encoding in the padding.
- With CET (`-fcf-protection`) the entry starts with `endbr64`; the short jump goes at `entry+4` (`EB F5`).
- Code pages are made RWX for the write, then RX again. They can't be made RW only, because the agent's own code may share a page with a function being redirected.

### 8.4 Failure handling
Steps 1–4 only touch fresh region memory, so a failure there leaves the program exactly as it was. The one exception is external side effects of initializers that already ran (§8.6). A failure while writing sites is reported. The process keeps running with whatever was applied, which is still valid code.

### 8.5 Unwind registration
- The `.eh_frame` sections of all patch objects are placed back to back, because an FDE finds its CIE by relative offset, followed by a 4-byte zero terminator. The block is registered with libgcc's `__register_frame`.
- Exceptions then unwind through any mix of old and new frames.
- `__register_frame` is referenced weakly. LLVM's libunwind would need per-FDE registration (not supported).

### 8.6 Dynamic initializers
A patch TU contains dynamic initializers for its globals (`.init_array`). Running them as they are would reset globals that are bound to old storage. Not running them would leave *new* globals uninitialized.

1. **Initializer code** is each `.init_array` entry plus the compiler-generated helpers it reaches through relocations, recognised by name: `_GLOBAL__sub_I_*`, `__cxx_global_var_init*`, `__static_initialization_and_destruction_*`, `__tcf_*`, `__dtor_*`, `__cxx_global_array_dtor*`. User functions an initializer calls are not initializer code; they run normally against the real variables.
2. **An initializer runs only if it sets up a new variable**, meaning its code refers to a writable section loaded new. Editing a file whose globals all exist runs nothing.
3. When it runs, its references to the file's *existing* writable variables go to **scratch copies** of their sections, initialized from the object. The real variables are untouched, including their guard variables.
4. Scratch copies are never freed. Destructors registered for them with `__cxa_atexit` run at exit, so construction and destruction stay paired.
5. An exception escaping an initializer fails the patch.

GCC folds all of a file's initializers into one function, and Clang calls them all from one entry. So adding one dynamically initialized global re-runs the initializers of the file's other globals, into scratch copies, and their side effects (registration, I/O) happen again. The host prints a note when that happens.

The converse also holds: an initializer that writes *no* new variable is skipped, even if it has side effects. One example is an empty registrar object that the optimizer removed, leaving only a call. Code that should register something when a patch adds it needs a variable for the registration to write. The hook macros (§8.7) do this.

### 8.7 Hooks
```cpp
HOTPP_PRE_PATCH(on_before_patch);   // before a patch is applied
HOTPP_POST_PATCH(on_after_patch);   // after it is live
```
- Each macro defines a static registrar object (`[[gnu::used]]`, storing the function pointer) whose constructor registers the function with the agent. Because the registrar is a real variable, a patch that adds a hook runs its initializer (§8.6).
- Hooks run on the thread that applies the patch (the `sync_point()` caller, or the agent thread in async mode).
- Registration composes with patching on its own:
  - A hook added by a patch is a new variable, so its initializer runs and registers it.
  - An existing hook isn't registered again; a re-run registration is deduplicated.
  - A hook whose body changes is redirected like any function, since the registry stores its canonical address.
- Typical uses: rebuilding caches, re-reading data files, deduplicating registrations.

### 8.8 Hot restart
Some changes can't be patched: a resized global, a new `thread_local`, a changed struct layout that live objects use. For those the host can **rebuild and restart** the program instead.

1. **Trigger:** with `--auto-restart`, a patch rejected as "restart required" triggers it; `kill -USR1 <host pid>` triggers it for every connected program.
2. **Rebuild:** the host runs the `--build "<command>"` (via `/bin/sh -c`, in the host's working directory, e.g. `cmake --build build`). If the build fails, the restart is cancelled and the program keeps running.
3. **Restart:** the host sends `Restart`. Like a patch, it is handled in `sync_point()`, or on the agent thread in async mode. The agent:
   - runs the `HOTPP_PRE_RESTART` hooks (save state to a file, say)
   - sets `HOTPP_RESTARTED` to the restart count and flushes stdio
   - shuts down the connection
   - `execv`s the executable path recorded at startup, which is now the rebuilt file, not `/proc/self/exe`, with the original arguments (`/proc/self/cmdline`) and environment
4. **Reconnect:** the process keeps its pid, so `hotpp --` run mode carries on, and the new image's agent connects as a new session. `hotpp::restarted()` is true in it.

State isn't carried over automatically. Hooks and `restarted()` let the program save and restore what matters.

Patched code is registered with gdb's JIT interface. The agent defines `__jit_debug_descriptor` and `__jit_debug_register_code` (weak, so they merge with another JIT's definitions in the same process).

For each patch object, the host sends a **debug object**, a copy of the `.o` with:
- every allocated section's `sh_addr` set to where it now lives: its patch address, or the old address for data bound to the running program
- the contents of loaded sections replaced by their final, relocated bytes
- the relocations of the debug sections applied, and all relocation sections then disabled, so the debugger doesn't apply them a second time

After the patch is live, the agent registers each debug object. gdb then has full symbols and line info for the new code:
- breakpoints by function or `file:line` also resolve in patched code
- `step`, `bt` and `print` work in it
- variables kept from the old program show their real (old) storage

Old and new versions of a function both stay known to gdb, which treats them as separate locations of the same breakpoint. Only the newest is ever reached: the old body sits behind the jump at its entry.

The `gdb_step` e2e case checks this: under gdb, with the patch live, it
- breaks at a `file:line` that exists only in the new code (gdb reports an `<in-memory@…>` objfile)
- reads a local and a kept global, whose value matches the program's
- takes a backtrace from new code into old
- runs `finish` back into old code

At `-O2` the local is optimized out, as in any optimized build.

**gdb and thread pausing.** Pausing other threads (§8.2) uses a real-time signal: the first free one from `SIGRTMIN+4`, usually `SIG38`. gdb stops on such signals by default. When debugging a multithreaded program while patching, tell gdb to pass it through: `handle SIG38 nostop noprint pass`.

---

## 10. Repository layout and public API

```
HotPP/
├── CMakeLists.txt, cmake/HotPP.cmake     # hotpp_enable(<target>)
├── include/hotpp/hotpp.h                 # public agent API
├── single_include/hotpp/hotpp.h          # API + agent in one header (generated)
├── tools/amalgamate.py                   # generates it
├── src/common/                           # protocol.h, messages.h, endpoint.h
├── src/agent/                            # static library linked into the app
│   ├── agent.cpp                         # init, IPC thread, sync_point, async mode, hooks
│   ├── patcher.cpp                       # validate, map, initializers, sites
│   ├── jit_debug.cpp                     # gdb JIT interface
│   ├── platform_linux.cpp                # modules, region, memory, IPC, processes
│   ├── threads_linux.cpp                 # thread pausing, unwind registration, membarrier
│   └── region_select.cpp                 # where the region goes (pure, unit tested)
├── src/host/                             # the hotpp executable
│   ├── main.cpp, session.cpp             # serve/run modes; per-agent pipeline
│   ├── control.cpp, control_protocol.cpp # controllers (§3.4)
│   ├── registry.cpp, events.cpp, report.cpp # session handles, event log, patch reports
│   ├── compile_db.cpp, deps.cpp          # compile_commands.json, dependency graph
│   ├── watcher_linux.cpp, platform_linux.cpp
│   ├── elf/elf_file.cpp                  # bounds-checked ELF reading
│   ├── image/elf_image.cpp               # executable symtab, TLS layout, build ID
│   ├── image/dwarf_reader.cpp, dwarf.cpp # DWARF: compile units, struct layouts
│   ├── object/elf_object.cpp             # relocatable objects
│   ├── link/linker.cpp, symbol_index.cpp, patch_site.cpp
│   └── util/json.cpp
├── src/mcp/                              # hotpp mcp (§14)
│   ├── server.cpp                        # JSON-RPC, MCP lifecycle, tool calls
│   ├── hosts.cpp                         # discovery, control client, launching
│   └── tools.cpp                         # the tools and their text
├── tests/unit/, tests/e2e/, tests/mcp/   # §12
└── examples/basic/
```

### Public API (`include/hotpp/hotpp.h`)
```cpp
namespace hotpp {
struct Config {
    const char* compile_commands; // set by hotpp_enable(); $HOTPP_COMPILE_COMMANDS overrides
    const char* endpoint;         // $HOTPP_ENDPOINT, then the default
    const char* host_path;        // $HOTPP_HOST, then "hotpp" on PATH
    bool launch_host = true;      // spawn a host if none is listening
    bool async = false;           // apply patches as they arrive instead of at sync_point()
    size_t region_size = 512 << 20;
};
bool init(const Config& = {});    // false if disabled ($HOTPP_DISABLE=1) or unavailable; the app keeps running
void sync_point();                // apply pending patches
void shutdown();
bool connected();
}
#define HOTPP_PRE_PATCH(fn)  // void fn(), before each patch
#define HOTPP_POST_PATCH(fn) // void fn(), after each patch is live
```
Defining `HOTPP_DISABLE` turns the whole API into inline no-ops.

---

## 11. Milestones

| # | Scope | Done when | Status |
|---|---|---|---|
| **M0** | Skeleton: CMake, `hotpp_enable`, agent and host, IPC handshake, region reservation, sample app | The host prints the module list and reserved region of the connected sample. | done |
| **M1** | Live patching: compile DB, `.cpp` edits, ELF object loader, symbol index, binding policy, stubs and GOT, patch sites, sync-point mode | Editing a function body in the sample changes behaviour live while a global counter keeps counting. | done |
| **M2** | Windows (MSVC, COFF/PDB) | — | dropped |
| **M3** | Correctness: header edits, several TUs per patch, repeated patches, initializers, local statics, existing TLS, exceptions and RTTI across patches, thread pausing, restart-required checks | The e2e suite (§12) passes with GCC and Clang at `-O0` and `-O2`. | done |
| **M4** | Usability: gdb JIT debug info, hooks, async mode, layout-change warnings, quieter console output | You can step through patched code in gdb (automated test). | done |
| **M5.1** | Shared libraries: every module built from the project's sources is patched, with its own index and region | A `.so`'s globals (including copy-relocated ones), statics and `thread_local`s survive a patch; it can call back into the program (`shared_lib` case). | done |
| **M5.2** | Hot restart: for changes that need a restart, rebuild with the user's build command and re-exec with the same command line | The program restarts on the new build from the host, without the user restarting it (`restart` case). | done |
| **M5.3** | Attach to a running program that wasn't linked with the agent | — | dropped: programs link the agent or the single header, as with Live++ |
| **M6** | AI agents: a control channel in every host (§3.4) and `hotpp mcp`, an MCP server (§14) | An MCP client launches a program, applies a two-file edit as one patch, gets a compile error with the compiler's output, restarts after a resize, attaches to a user's host, and its launched program dies with it (`mcp` case). | done (1.1) |

Each milestone ends with a git commit, pushed, once its "done when" condition is met.

**Known limits:**
- Struct layout changes that live objects use are only warned about, never fixed up.
- An initializer that registers something without writing a new variable is skipped when a patch adds it (§8.6); the hook macros don't have this problem.
- Adding a dynamically initialized global re-runs the initializers of the file's other globals into scratch copies, repeating their side effects (§8.6).
- New `thread_local` variables, resized or retyped globals, and new direct references to shared-library variables need a restart.
- `__attribute__((destructor))` functions in a patch aren't registered.
- Threads that block real-time signals can't be paused, so patches fail while such threads exist.
- A library unloaded with `dlclose` and loaded again is a new module: patches applied to its earlier copy are gone.
- A running call keeps its old code: a patch redirects *new* calls. For the thread that calls `sync_point()` (e.g. `main()`, or a loop calling it) the host knows which functions are running: the agent sends `backtrace()` of `sync_point()`'s callers in `ActiveFrames`. If the patch replaced one of them, the host warns. Other threads' running functions aren't detected.

---

## 12. Testing

### Unit tests (`tests/unit`)
- corrupted input: a real object file and the test binary are mutated thousands of times and fed through the ELF, DWARF and linker code; they must be rejected, never crash (`HOTPP_FUZZ_ITERATIONS` scales it). With `-DHOTPP_SANITIZE=ON`, the host and the tests are built with ASan and UBSan; with `=thread`, with TSan. The whole suite runs under both in CI.
- the session state file round trip and its rejection of corrupt files
- protocol encoding and decoding
- region placement
- JSON, compile DB and depfile parsing
- the dependency graph
- patch-site analysis (GCC, Clang and CET padding)
- ELF image and object parsing against the test binary itself
- the linker, run against a real object file compiled like a patch, with a hand-made index: bindings, stubs, canonical addresses (including `static` functions referenced through their section symbol), resize rejection
- struct layouts read from the test binary's own DWARF
- the JSON writer (exact integers, invalid UTF-8 replaced), the event log and output buffer (cursors, trimming, UTF-8 boundaries), ANSI stripping and output capping, the registry (newest session per pid, auto-reload owners, requests)
- the MCP layer with a fake tool provider: version negotiation, errors for every kind of bad message, batches, progress, cancellation, the call limit, and thousands of mutated messages
- the control channel against a real controller on a socket: requests, errors, a long poll ended by the controller leaving, discovery skipping stale sockets, a 1.0-style host recognized as incompatible, tool argument checking

### End-to-end cases (`tests/e2e`)
Each case is a directory `tests/e2e/cases/<case>/` with:
- `main.cpp`: never edited; implements `case_setup` / `case_frame` / `case_finish` from `common/e2e.h`
- `v1/`: the sources the program is built from
- further version directories
- `case.json`: the steps

CMake builds each program from *copies* of `v1/` in the build tree, with `hotpp_enable`. `run_case.py` then:
1. starts the program under `hotpp --` and waits for the dependency scan and a few frames
2. for each step, copies the step's files over the build-tree copies and waits for the host line it expects (`patch N applied`, `restart required: …`, `failed to compile`), then a few more frames
3. creates a stop file

The program checks its invariants every frame (state never resets, old and new results never mix) and its final state at the end, and prints `PASS`. The copies are then restored to v1, content and timestamps.

| Case | Checks |
|---|---|
| `state` | A changed body takes effect; a new function and variable work; a global, a file `static`, an anonymous-namespace variable and a function-local static keep their values; a pointer taken before the patch reaches the new code. |
| `new_global` | New dynamically initialized globals (plain and with a constructor) are initialized; existing ones keep their state. |
| `header_inline` | Editing an inline function and a template in a header recompiles both includers into one patch. |
| `virtual_change` | Objects created before and after the patch run the new virtual function; `dynamic_cast` still works. |
| `lambda_template` | Lambda bodies and template instantiations update, including a `std::function` holding the old lambda. |
| `exceptions` | New code throws and old code catches; old code throws and new code catches; unwinding through a new frame works. |
| `thread_spin` | A thread calling the patched function in a tight loop keeps running and picks up the new code. |
| `repeat` | Three consecutive patches; a pointer from the start reaches the newest version. |
| `tls` | Existing `thread_local` variables keep their values on the main thread and on a second thread. |
| `restart_required` | A resized global and a new `thread_local` are rejected without touching the program; a valid change afterwards applies. |
| `compile_error` | A broken edit applies nothing; the fix applies afterwards. |
| `hooks` | Pre/post-patch hooks run exactly once per patch; a changed hook body takes effect; a hook added by a patch is registered and runs. |
| `async` | In async mode, a patch applies while the main thread spins in a loop that never reaches `sync_point()`. |
| `layout_warning` | A struct that grows in a header triggers the layout warning, and the patch still applies. |
| `running_loop` | A patch that replaces the function `sync_point()` was called from, while it runs, warns that the running call keeps its old code; later calls run the new code. |
| `shared_lib` | Code in a shared library (`lib_*.cpp` in the case) is patched: its global (copy-relocated into the executable), `static` and `thread_local` keep their values, and it calls back into the executable. |
| `restart` | A resized global is rebuilt and restarted with `--build … --auto-restart`; the pre-restart hook saves state, the restarted process (same pid) sees `restarted()`, reads it back and runs the new code. The harness rebuilds the program from v1 afterwards. |
| `single_header` | A program built with only the single header (no agent library, no `compile_commands.json` path built in) connects, finds `compile_commands.json` itself, is patched and runs its hook. |
| `dlopen_plugin` | A library the program `dlopen`s after connecting is picked up, patched (its `static` keeps counting) and dropped once it's `dlclose`d. |
| `host_restart` | The host is killed (`SIGKILL`) and restarted, later stopped (`SIGTERM`) and restarted; each time the agent reconnects, the new host resumes the patch history (numbering, function versions, a variable a patch added), and further patches apply with all state intact. |
| `install_and_consume` (not an e2e case) | Installs HotPP into a temporary prefix, builds a separate `find_package(HotPP)` project against it and runs it under the installed host. |
| `gdb_step` | Under gdb: a breakpoint in new code, locals and kept globals, a backtrace into old code, `finish` (§9). Runs only if gdb is installed. |
| `mcp` | Driven by `tests/mcp/run_mcp_test.py`, a minimal MCP client, with a private `$XDG_RUNTIME_DIR`. It launches the program and edits two files; with auto-reload off, status lists them pending and nothing changes until `hotpp_reload` applies both as one patch, with the counter kept. Then a compile error (output included, no color codes, `out_of_sync` on repeat) and its fix, malformed messages, an automatic patch seen through a long-polled `hotpp_logs`, a resize, `restart_required` and `hotpp_restart` (same pid). It stops the program, attaches to a host started with `hotpp --` (patch, temporary auto-reload setting reverted on exit, no stop, no output), and checks that a program launched by an MCP server killed with `SIGKILL` dies too. |

**Matrix:** {GCC, Clang} × {Debug (`-O0`), Release (`-O2`)}, in four build directories. With gdb mode, `case.json` can also run the program under gdb (`"gdb": "<script>"`, `"expect_output": [...]`). `"env"` sets environment variables, and a step's `"expect"` may list several lines in order.

---

## 13. Risks and open questions

| # | Risk / question | Plan |
|---|---|---|
| 1 | Initializer side effects run again when a file gains a dynamically initialized global (§8.6). | Documented; hooks (§8.7) can deduplicate. A `HOTPP_NO_REINIT` marker could skip specific initializers if this hurts in practice. |
| 2 | Initial dependency scan time on large codebases. | Background scan with a persistent cache; evaluate `clang-scan-deps`. |
| 3 | Signals used to pause threads interrupt some syscalls (`EINTR`) in user code. | `SA_RESTART`; the pause is short and only happens while sites are written. A ptrace-based helper is the fallback if it causes problems. |
| 4 | Compiler arguments with response files (`@file`). | Pass through unchanged today; expand them in the compiler driver if the output or depfile flags hide inside one. |
| 5 | Programs using LLVM's libunwind instead of libgcc. | Per-FDE registration via `__unw_add_dynamic_fde` if needed. |

### Windows spike (W0, 2026-11-09)
Four questions decided whether the Windows port (MSVC and clang-cl) can follow the Linux design. They were checked with throwaway programs, built with clang-cl 19 and lld-link against the MSVC CRT and SDK (from xwin), and run under Wine 10:

1. **Matching static symbols between the PDB and a patch `.obj`.** The PDB's module streams name statics *undecorated* (`over`, `` `anonymous namespace'::helper ``, a local static `calls` nested in its function's scope). The object uses decorated COFF names (`?over@@YAHH@Z`, `?over@@YAHN@Z`) or plain C names (`counter`). Overloads share a name. The patch object's `.debug$S` maps each COFF symbol to its CodeView name and type through relocations. Matching on (compiland, enclosing function, name) with the printed type signature as a tie-breaker therefore identifies every static. **Feasible.**
2. **Patch-site room.** With `/hotpatch` and `/FUNCTIONPADMIN`, lld-link leaves at least 16 `int3` bytes before each function. Every function starts with an instruction of at least 2 bytes; clang emits `66 90` where it would otherwise be 1 byte. The Linux site scheme works unchanged: a jump in the padding, then a 2-byte short jump at the entry. **Yes for clang-cl/lld-link; cl/link still to be checked on Windows CI.**
3. **Exceptions from patch code.** Code in memory *above* the executable throws, unwinds and is caught in the executable when:
   - its `.pdata`/`.xdata` are registered with `RtlAddFunctionTable` using the executable's image base as the base for every RVA
   - a copy of the `ThrowInfo` lives in that memory, with RVAs relative to the same base
   - the throw goes through a shim that passes that base to `RaiseException`, because `_CxxThrowException` derives it with `RtlPcToFileHeader`, which fails for non-image memory

   **Works under Wine; to be confirmed on Windows.**
4. **Compiling from a host running under Wine.** A Windows program under Wine can `CreateProcess` the native Linux `clang-cl` (as `/usr/…` or `Z:\usr\…`), so the cross-built host can run the build's own compile commands. **Yes.**

---

## 14. MCP server (`hotpp mcp`)

`hotpp mcp` lets AI coding agents use HotPP through the [Model Context Protocol](https://modelcontextprotocol.io). It is a mode of the `hotpp` executable that speaks JSON-RPC 2.0 over stdio, one message per line. It holds no hot-reload state of its own: it translates tool calls into control requests (§3.4) to hosts, so it can be restarted at any time.

**Protocol** (`src/mcp/server.cpp`):
- Versions 2025-11-25, 2025-06-18, 2025-03-26 and 2024-11-05. The client's version is echoed if supported, otherwise the newest is offered.
- `initialize` returns the `tools` capability and `instructions` on how to use HotPP. The server also answers `ping`, `tools/list` and `tools/call`, and accepts `notifications/initialized` and `notifications/cancelled`; a cancelled call gets no reply.
- `notifications/progress` messages carry the host's log lines when the call has a `progressToken`. Batches (2025-03-26) are answered as one array.
- Standard error codes for bad messages: -32700, -32600, -32601, -32602. Tool failures are results with `isError: true` and a message the model can act on, including bad arguments (each tool's `inputSchema` is strict).
- Tool calls run on worker threads, at most 16 at once (more are turned away with an error result). Results carry readable `text`, with compiler output verbatim and advice on what to do next. With `--structured` they also carry the raw host data as `structuredContent`. That's off by default: Claude Code gives the model `structuredContent` *instead of* the text when both are present. That costs more tokens and drops the advice, which was seen in a trial run.
- **stdout carries only protocol.** At startup it is duplicated to a private descriptor and fd 1 is pointed at stderr, so nothing else can corrupt the stream. Children never inherit it.
- Lines over 16 MiB are refused and skipped to the next newline. `SIGPIPE` is ignored.
- End of input lets calls in progress finish; `SIGTERM`, `SIGINT` or `SIGHUP` cancel them. Either way, launched programs are stopped before it exits.

**Finding programs** (`src/mcp/hosts.cpp`):
- Every socket named `hotpp.sock` or `hotpp-*.sock` in the runtime directory is a candidate, plus `--endpoint` paths and `$HOTPP_ENDPOINT`.
- Connections are kept per host and reopened when lost. Stale sockets are skipped, and incompatible hosts are remembered until their socket file changes.
- A tool's `pid` picks the program. Without one: the only program there is; else the only one this server launched; else an error that lists them.

**Launching.** `hotpp_launch` starts `/proc/self/exe --endpoint <runtime>/hotpp-mcp-<pid>-<n>.sock --capture-output --lifeline 3 [--no-auto-reload] [--build …] -- <command>`:
- in its own process group, so Ctrl+C in the agent's terminal doesn't reach it
- with stdin from `/dev/null`
- with its stdout piped to the server, which keeps the last 64 KiB for error messages

It then waits for `wait_program` and reports the result:
- ready: what the program watches, and its output so far
- the program exited: the output, and the host is dismissed
- no agent connected: probably not built with HotPP
- hot reload unavailable, with the reason

**Tools.**

| Tool | Notes |
|---|---|
| `hotpp_status` | all programs and hosts; read-only |
| `hotpp_reload` | as §3.4; reports earlier patches the agent hasn't seen (cursor per host and pid); `observe_ms` appends the output printed right after the patch; a layout warning comes with advice to restart |
| `hotpp_launch` | `command`, `cwd`, `env`, `build_command` (default `--build`), `auto_restart`, `auto_reload` (default off) |
| `hotpp_restart` | `rebuild` (default true); waits until the program is back; `observe_ms` as above |
| `hotpp_stop` | only programs this server launched |
| `hotpp_output` | only captured (launched) programs; cursor or last lines; `wait_ms` returns what it prints during the wait |
| `hotpp_logs` | cursor and long poll |
| `hotpp_set_auto_reload` | temporary on hosts the user started (§3.4) |

`--no-launch` turns off launching, which leaves observing and patching programs the user started.

**Not done:** the Streamable HTTP transport (local stdio covers coding agents), resources and prompts, sending input to a program.
