# HotPP — Design

Status: M0, M1 and M3 implemented; M2 (Windows) dropped; M4 in progress · Last updated: 2026-11-05

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
- Attaching to a process that doesn't link `hotpp_agent` (M5).
- Patching code in shared libraries other than the main executable (M5). Everything is already keyed by module.
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
  1. At `init`, reserve a large address range (default 512 MB) within ±2 GB of the main executable (§7.3).
  2. Connect to the host, launching it if needed. Report the process ID, executable path, module list with load bases and build IDs, and the reserved range.
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
| `Patch` | host → agent | segments, patch sites, `.eh_frame` address, initializers, debug objects |
| `PatchResult` | agent → host | id, ok, error |
| `Log`, `Bye` | both | text for the user; orderly disconnect |

---

## 4. Build requirements

`cmake/HotPP.cmake` provides `hotpp_enable(<target>)`. It links `hotpp_agent`, applies the flags below, exports compile commands and disables IPO for the target. Users of other build systems apply the same flags by hand.

| Flag | Why |
|---|---|
| `-g` | DWARF tells the host which sources the program is built from (§6) and gives the debugger the patched code (§9). |
| `-fpatchable-function-entry=7,5` | Puts 5 NOP bytes before each function entry and 2 at the entry: room for the patch-site jump (§8.3). |
| `-fno-ipa-icf` (GCC) | Identical-code folding merges identical functions, so patching one would patch them all. |
| No `-Wl,--gc-sections` | Functions that look unused would be removed and couldn't be patched later. |
| No `--icf` (gold / lld) | Same reason as `-fno-ipa-icf`, at link time. Off by default. |
| Don't strip the binary | `.symtab` holds the local symbols HotPP needs. |
| No `-flto` | LTO breaks the one-function-per-symbol mapping and inlines across TUs. |

`compile_commands.json` must exist (CMake with Ninja or Makefiles, Meson, or Bear). Optimization level is free; `-O0` and `-O2` are tested (§12).

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
2. Each TU compiles with its exact `arguments`/`command` from `compile_commands.json`, with these changes only:
   - the output goes to `<build>/.hotpp/<pid>/patch-<n>/`
   - the build's own dependency-file flags (`-MD`, `-MF`, …) are dropped, so its `.d` files aren't overwritten
   - `-ffunction-sections -fdata-sections` are added (§7.2)
3. All affected TUs compile in parallel and go into a single patch.
4. On any failure, the compiler output is shown and nothing is applied. The next save retries.

Header edits can recompile many TUs. That is the price of correctness: every TU that included the header may have inlined its code. Live++ makes the same trade-off.

---

## 6. Executable symbol index

The host indexes every symbol of the running executable, mapping identity key → runtime address, size, kind and writability. It is read once from the file on disk, plus the load bias the agent reports. Each applied patch adds to it (§7.5).

- **Identity key:** global symbols by mangled name; local symbols (`static`, anonymous namespace, lambdas in non-inline functions) by `(defining file, mangled name)`.
- **Local scoping:** the linker groups local symbols after an `STT_FILE` symbol naming their source file, so `(file, symbol)` comes for free. Two locals with the same key (two files with the same base name both defining it) are marked *ambiguous*. Data bound to them is an error; functions are loaded new without redirecting.
- **Runtime address:** load bias (`dl_iterate_phdr`) + `st_value`. TLS symbols keep their offset in the TLS template.
- **Patch sites:** for every function, the host checks the file bytes around its entry for the expected padding (§8.3). Functions without it, for example from code built without the flags, are never redirected.
- **Program membership:** the DWARF compile units (`DW_AT_name` joined with `DW_AT_comp_dir`; DWARF 4 and 5, including `strx` forms) list exactly the sources the executable was built from. Edits to other files in the same build are ignored instead of being patched in.
- **Identity check:** the agent sends each module's GNU build ID as mapped in memory. If the executable on disk has a different one (rebuilt since the program started), the host refuses to patch, because its symbol table no longer describes the running code.

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
- The agent reserves the region at `init` with `mmap(PROT_NONE, MAP_FIXED_NOREPLACE)`. It places the region in the closest free gap below the executable (from `/proc/self/maps`) such that every byte of it is within ±2 GB of every byte of the executable. If nothing fits below, it goes above, leaving 64 MB for the brk heap; if 512 MB doesn't fit at all, smaller sizes are tried.
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
- The thread-pointer offset of an executable TLS variable is `st_value − round_up(PT_TLS.memsz, PT_TLS.align)` (x86-64 TLS variant II; the executable's block sits right below the thread pointer).
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
- **Async mode (M4,** `Config::async = true`**).** The agent thread applies the patch as soon as it arrives. This suits programs without a natural loop.

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

### 8.7 Hooks (M4)
```cpp
HOTPP_PRE_PATCH(on_before_patch);   // before a patch is applied
HOTPP_POST_PATCH(on_after_patch);   // after it is live
```
- Each macro defines a static registrar object whose constructor registers the function with the agent.
- Hooks run on the thread that applies the patch (the `sync_point()` caller, or the agent thread in async mode).
- Registration composes with patching on its own:
  - A hook added by a patch is a new variable, so its initializer runs and registers it.
  - An existing hook isn't registered again; a re-run registration is deduplicated.
  - A hook whose body changes is redirected like any function, since the registry stores its canonical address.
- Typical uses: rebuilding caches, re-reading data files, deduplicating registrations.

---

## 9. Debugger support (M4)

Patched code is registered with gdb's JIT interface. The agent defines `__jit_debug_descriptor` and `__jit_debug_register_code` (weak, so they merge with another JIT's definitions in the same process).

For each patch object, the host sends a **debug object**, a copy of the `.o` with:
- every allocated section's `sh_addr` set to where it now lives: its patch address, or the old address for data bound to the running program
- the contents of loaded sections replaced by their final, relocated bytes
- the relocations of the debug sections applied, and all relocation sections then disabled, so the debugger doesn't apply them a second time

After the patch is live, the agent registers each debug object. gdb then has full symbols and line info for the new code:
- breakpoints by function or `file:line` also resolve in patched code
- `step`, `bt` and `print` work in it
- variables kept from the old program show their real (old) storage

Old and new versions of a function both stay known to gdb, which treats them as separate locations of the same breakpoint.

---

## 10. Repository layout and public API

```
HotPP/
├── CMakeLists.txt, cmake/HotPP.cmake     # hotpp_enable(<target>)
├── include/hotpp/hotpp.h                 # public agent API
├── src/common/                           # protocol.h, messages.h, endpoint.h
├── src/agent/                            # static library linked into the app
│   ├── agent.cpp                         # init, IPC thread, sync_point, hooks
│   ├── patcher.cpp                       # validate, map, initializers, sites
│   ├── platform_linux.cpp                # modules, region, memory, IPC, processes
│   ├── threads_linux.cpp                 # thread pausing, unwind registration, membarrier
│   └── region_select.cpp                 # where the region goes (pure, unit tested)
├── src/host/                             # the hotpp executable
│   ├── main.cpp, session.cpp             # serve/run modes; per-agent pipeline
│   ├── compile_db.cpp, deps.cpp          # compile_commands.json, dependency graph
│   ├── watcher_linux.cpp, platform_linux.cpp
│   ├── elf/elf_file.cpp                  # bounds-checked ELF reading
│   ├── image/elf_image.cpp, dwarf.cpp    # executable symtab, DWARF
│   ├── object/elf_object.cpp             # relocatable objects
│   ├── link/linker.cpp, symbol_index.cpp, patch_site.cpp
│   └── util/json.cpp
├── tests/unit/, tests/e2e/               # §12
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
#define HOTPP_PRE_PATCH(fn)  // M4
#define HOTPP_POST_PATCH(fn) // M4
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
| **M4** | Usability: gdb JIT debug info, hooks, async mode, layout-change warnings, quieter console output | You can step through patched code in gdb (automated test). | in progress |
| **M5** | Extended: patching shared libraries, hot restart, injecting into an app without the agent | Scoped separately. | — |

Each milestone ends with a git commit, pushed to the Gitea remote, once its "done when" condition is met.

**Known limits:**
- Struct layout changes that live objects use are only warned about (M4), never fixed up.
- Adding a dynamically initialized global re-runs the initializers of the file's other globals into scratch copies, repeating their side effects (§8.6).
- New `thread_local` variables, resized or retyped globals, and new direct references to shared-library variables need a restart.
- `__attribute__((destructor))` functions in a patch aren't registered.
- Threads that block real-time signals can't be paused, so patches fail while such threads exist.
- Only the main executable is patched (M5).

---

## 12. Testing

### Unit tests (`tests/unit`)
- protocol encoding and decoding
- region placement
- JSON, compile DB and depfile parsing
- the dependency graph
- patch-site analysis (GCC, Clang and CET padding)
- ELF image and object parsing against the test binary itself
- the linker, run against a real object file compiled like a patch, with a hand-made index (bindings, stubs, canonical addresses, resize rejection)

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

**Matrix:** {GCC, Clang} × {Debug (`-O0`), Release (`-O2`)}, in four build directories.

---

## 13. Risks and open questions

| # | Risk / question | Plan |
|---|---|---|
| 1 | Initializer side effects run again when a file gains a dynamically initialized global (§8.6). | Documented; hooks (§8.7) can deduplicate. A `HOTPP_NO_REINIT` marker could skip specific initializers if this hurts in practice. |
| 2 | Initial dependency scan time on large codebases. | Background scan with a persistent cache; evaluate `clang-scan-deps`. |
| 3 | Signals used to pause threads interrupt some syscalls (`EINTR`) in user code. | `SA_RESTART`; the pause is short and only happens while sites are written. A ptrace-based helper is the fallback if it causes problems. |
| 4 | Compiler arguments with response files (`@file`). | Pass through unchanged today; expand them in the compiler driver if the output or depfile flags hide inside one. |
| 5 | Programs using LLVM's libunwind instead of libgcc. | Per-FDE registration via `__unw_add_dynamic_fde` if needed. |
