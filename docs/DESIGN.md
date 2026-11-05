# HotPP — Design

Status: M0 and M1 implemented · Last updated: 2026-11-05

HotPP is a hot-reload tool for C++ on Linux and Windows. You edit a `.cpp` file or a header, save it, and the running program picks up the new code within about two seconds. It doesn't restart and doesn't lose state.

It works the way Live++ does. It recompiles only the affected translation units with their original compiler commands, links the resulting object files against the *running* process, maps the new code into that process, and redirects every old function to its new version. Global and heap state stays where it is.

---

## 1. Goals and non-goals

### Goals
- **Fast edit-to-running loop.** Under about 2 s for a single-TU change on a typical project.
- **State is preserved.** Heap, stack, globals, function-local statics and thread-locals all survive a patch.
- **Minimal integration.** Call `hotpp::init()` once, optionally call `hotpp::sync_point()` in the main loop, and enable the flags (one CMake call). No interfaces or base classes need to be added to user code.
- **Linux and Windows** with GCC, Clang and MSVC.
- **Fail safe.** A compile error, an unsupported change or a host crash never corrupts or kills the target process. The patch either applies completely or not at all.

### Non-goals (v1)
- macOS, ARM64, 32-bit x86, MinGW.
- LTO, `-flto`, `/GL`, `/LTCG`.
- Changing the layout of a struct or class while instances of it exist. Instead, HotPP reports *restart required* where it can detect the change (§7.6).
- Attaching to a process that doesn't link `hotpp_agent`. Injection is deferred to M5.
- Patching code in shared libraries or DLLs other than the main executable. Deferred to M5; the design already keys everything by module.
- Being a build system. HotPP uses the commands the real build recorded.

---

## 2. Platform matrix (v1)

| Platform | Compilers | Object / image format | Symbols / debug info |
|---|---|---|---|
| Linux x86-64 | GCC ≥ 11, Clang ≥ 14 | ELF relocatable `.o` / ELF executable | `.symtab` + DWARF |
| Windows x64 | MSVC (VS 2022, `cl.exe`), clang-cl | COFF `.obj` / PE | CodeView (`.debug$S`) + PDB |

clang-cl emits COFF/CodeView and is handled by the MSVC code path. Clang on Linux and GCC share the ELF code path. Compiler differences come down to flag handling and a few symbol-naming patterns.

---

## 3. Architecture

```
 ┌────────────────────────── hotpp (host process) ──────────────────────────┐
 │ watcher ─▶ dependency graph ─▶ compiler driver ─▶ object loader          │
 │                                                     │                    │
 │ image symbol index (ELF .symtab / PDB) ─▶ resolver + binding policy      │
 │                                                     │                    │
 │                                   layout + relocation ─▶ patch package   │
 └──────────────────────────────────────────────────────┼───────────────────┘
                                                        │ IPC (UDS / named pipe)
 ┌──────────────────────── target process ─────────────▼────────────────────┐
 │ user code   ···   hotpp_agent: reserved near region · thread pause ·     │
 │                   write code · patch sites · unwind reg · initializers   │
 └──────────────────────────────────────────────────────────────────────────┘
```

### 3.1 Host links, agent maps
The core split is that **the host does all the hard work and the agent only maps bytes.**

- **`hotpp` (host)** is a separate executable with no limits on dependencies or memory. It watches files, keeps the dependency graph, runs the compiler, and parses object files, executables and PDBs. It resolves symbols and applies relocations, producing final bytes for known target addresses. A bug in this complex code crashes the host, never the app.
- **`hotpp_agent`** is a small static library linked into the app. It depends only on the OS API and has no exceptions or RTTI. Its jobs:
  1. At `init`, it reserves a large address range (default 512 MB, configurable) within ±2 GB of the main executable (§7.3). v1 patches only the main executable; M5 adds a region per patched module.
  2. It connects to the host, launching it if configured to, and reports the process ID, executable path, module list with load bases, and reserved ranges.
  3. It receives patch packages and applies them (§8).

Because the agent reserves address space up front, the host knows the final addresses before linking. Applying a patch is therefore a one-way message (host → agent) followed by an acknowledgement. No allocation round trip is needed.

### 3.2 IPC
The host listens and agents connect. One host serves any number of agents.

- **Endpoints:**
  - Linux: a Unix domain socket, by default `$XDG_RUNTIME_DIR/hotpp.sock` (or `/tmp/hotpp-<uid>.sock`). Created with mode 0600. A socket file left by a dead host is detected and replaced.
  - Windows: a named pipe, by default `\\.\pipe\hotpp`, with remote clients rejected.
  - `$HOTPP_ENDPOINT` overrides the default for both the agent and the host. A second host on an endpoint that is in use refuses to start.
- **Two ways to start:**
  - *Serve:* run `hotpp` and start the app normally. If no host is listening, the agent launches one (`$HOTPP_HOST`, default `hotpp` on `PATH`), unless `launch_host` is off or `$HOTPP_LAUNCH_HOST=0`.
  - *Run:* `hotpp -- <program> [args]` listens on a private endpoint (`…hotpp-<host pid>…`), passes it to the child in `$HOTPP_ENDPOINT` and exits with the child's exit code. Tests use this mode. The host exits 2 if no agent connected and 3 if an agent reported an unusable layout.
- **Protocol:** length-prefixed binary messages with a version handshake. Framing is in `src/common/protocol.h` and messages in `src/common/messages.h`. Main messages:
  - `Hello{pid, exe, modules[], regions[]}` (agent → host)
  - `Patch{id, segments[], sites[], unwind, initializers[], hooks[]}` (host → agent)
  - `PatchResult{id, ok, error}` (agent → host)
  - `Log` (both directions)

### 3.3 Patch package
| Field | Contents |
|---|---|
| `segments[]` | target address, bytes and final protection (RX / R / RW) |
| `sites[]` | `(old_entry, new_entry)` for every function to redirect |
| `unwind` | the relocated `.eh_frame` range on Linux, or `.pdata` range plus base on Windows |
| `initializers[]` | entry addresses to run, plus the shadow-storage commit list (§8.6) |
| `hooks[]` | pre- and post-patch hook addresses found in the patch (§8.7) |
| `debug` | optional in-memory ELF for the GDB JIT interface (§9) |

---

## 4. Build requirements

`cmake/HotPP.cmake` provides `hotpp_enable(<target>)`. It applies the flags below and links `hotpp_agent`. It also turns on `CMAKE_EXPORT_COMPILE_COMMANDS`. Users of other build systems apply the same flags by hand.

### GCC / Clang (Linux)
| Flag | Why |
|---|---|
| `-g` | DWARF is used to tell local symbols apart when two files share a name (§6.1) and for debugger support (§9). |
| `-fpatchable-function-entry=7,5` | Puts 5 NOP bytes before each function entry and 2 at the entry. That's room for the patch-site jump (§8.3). |
| `-fno-ipa-icf` (GCC) | Identical-code folding merges identical functions, so patching one would patch them all. |
| No `-Wl,--gc-sections` | Functions that look unused would be removed and couldn't be patched later. |
| `-Wl,--icf=none` (gold / lld) | Same reason as `-fno-ipa-icf`, at link time. lld's default is already `none`. |
| Don't strip the binary | `.symtab` holds the local symbols HotPP needs. |
| No `-flto` | LTO breaks the one-function-per-symbol mapping and inlines across TUs. |

### MSVC / clang-cl (Windows)
| Flag | Why |
|---|---|
| `/Z7` | Keeps debug info in each `.obj` and avoids the shared compiler PDB, which a patch compile would contend for. |
| `/Gy` | Puts each function in its own COMDAT, so functions are separable units. |
| `/hotpatch` | Guarantees the first instruction is at least 2 bytes, so it can be replaced atomically. Already implied on x64; kept explicit. |
| `/DEBUG:FULL` | Gives a complete PDB, including per-module symbol streams, for the image symbol index. |
| `/FUNCTIONPADMIN` | Puts padding before each function (6 bytes on x64) to hold the long jump (§8.3). |
| `/OPT:NOREF` | Keeps functions that look unused. |
| `/OPT:NOICF` | Stops identical functions being folded. |
| `/INCREMENTAL:NO` | Incremental linking adds thunks and rewrites the image in ways that interfere with patching. |
| No `/GL` / `/LTCG` | Whole-program optimization breaks the per-TU model. |

### Both
- `compile_commands.json` must exist. CMake, Meson and Ninja can produce it; Bear can generate it for other build systems.
- Optimization level is free. `-O0` through `-O2`/`/O2` are tested (§12).

---

## 5. Change detection and recompilation

### 5.1 Watching
- The watcher uses inotify on Linux and `ReadDirectoryChangesW` on Windows.
- It watches the directories of every source file in `compile_commands.json` plus every directory that appears in the dependency graph.
- Events are debounced for 100 ms to absorb editors' save-via-rename and multi-file saves.
- A file only counts as changed if its content hash changed, which filters out `touch` and similar no-op writes.

### 5.2 Dependency graph
- For each TU, the include set comes from the compiler itself:
  - GCC/Clang: `-MM -MF <file>`
  - MSVC: `/sourceDependencies <file>` with `/Zs` (syntax-only)
- The initial scan runs in the background on a thread pool at host startup. Hot reload is usable before it finishes: `.cpp` edits work at once, and header edits wait for the scan.
- Results are cached in `<build>/.hotpp/deps/`, keyed by a hash of the command plus the mtimes of every listed dependency.
- Every patch compile also emits fresh dependency info, so the graph stays current as includes change.

### 5.3 Recompiling
1. Affected set = the changed `.cpp` file itself, or every TU whose include set contains the changed header.
2. For each affected TU, take the exact `command`/`arguments` from `compile_commands.json`. Rewrite only the output path (`-o` / `/Fo`) to `<build>/.hotpp/<pid>/patch-<n>/`, drop the build's own dependency-file flags (`-MD`, `-MF` and so on, which would overwrite its `.d` files), and add `-ffunction-sections -fdata-sections` (§7.2).
3. Compile in parallel, by default with as many jobs as hardware threads.
4. On any failure, show the compiler output in the host console and apply nothing. The next save retries.

Header edits can recompile many TUs. That is the price of correctness, because every TU that included the header may have inlined its code. It is the same trade-off Live++ makes.

---

## 6. Image symbol index

The host builds an index of every symbol in the running image, mapping identity key → runtime address, size, kind and section. It is built once at connect time from the on-disk files and the agent's module bases. Each applied patch adds to it, which later patches rely on (§7.5).

**Identity key:**
- external (global) symbols: `(module, mangled name)`
- internal symbols (`static`, anonymous namespace, internal-linkage constants): `(module, TU, mangled name)`

### 6.1 Linux (ELF)
- Read `.symtab` (not just `.dynsym`) of the executable.
- Runtime address = module base (from `dl_iterate_phdr`, reported by the agent) + `st_value` for PIE, or `st_value` itself for non-PIE.
- **Local scoping:** the linker writes local symbols grouped after an `STT_FILE` symbol naming their source file. That gives `(file name, symbol)` for free.
- **Same-name files:** `STT_FILE` holds only the base name, so for duplicates such as `a/util.cpp` and `b/util.cpp`, HotPP falls back to DWARF:
  - compile-unit address ranges (`DW_AT_ranges` / `.debug_aranges`) for functions
  - `DW_TAG_variable` locations for data
- The TU is mapped back to its `compile_commands.json` entry by the `DW_AT_name` / `DW_AT_comp_dir` of the compile unit. The set of compile units also decides which sources belong to the running program, so editing a file of another target in the same build is ignored rather than patched in.
- **Identity check:** the agent sends each module's GNU build ID as mapped in memory. If the executable on disk has a different one (it was rebuilt since the program started), the host refuses to patch, because its symbol table no longer describes the running code.

### 6.2 Windows (PE + PDB)
- The PDB is parsed with RawPDB (Molecular Matters; license to be checked before adopting), or a small in-house reader if the license doesn't fit.
- **External symbols:** the public symbol stream (`S_PUB32`) gives decorated names and section:offset, which convert to an RVA and then to an address.
- **Internal symbols:** the PDB's per-module symbol streams give `S_LPROC32` / `S_LDATA32` / `S_LTHREAD32` per compiland. These carry *undecorated* names plus a type index, so overloads need a signature to tell them apart. The match is done in CodeView on both sides:
  1. The new `.obj`'s `.debug$S` contains `S_LPROC32` records whose relocations point at their COFF symbols. That gives COFF symbol ↔ (qualified name, printed type signature).
  2. The PDB module stream for the same compiland gives (qualified name, printed type signature) ↔ RVA.
  3. Join on (compiland, qualified name, signature).
- A compiland is mapped to its `compile_commands.json` entry by the module's object path, which the PDB records.

This is the riskiest part of the Windows port, so it gets a spike at the start of M2 (§13).

---

## 7. Patch linker

The host links the new object files against the symbol index into the agent's reserved region. Nothing goes through the system linker.

The linker has a format-independent core: sections, symbols, the resolver, binding policy, layout, stubs and patch-site selection. Two format front-ends sit on top: `object/elf_object` with `link/reloc_elf_x64`, and `object/coff_object` with `link/reloc_coff_x64`.

### 7.1 Loading objects
- Parse sections, symbols, relocations and COMDAT groups.
  - ELF: `SHT_GROUP`.
  - COFF: `IMAGE_SCN_LNK_COMDAT` with a selection kind.
- When several patch objects define the same COMDAT (an inline function or template instance in several TUs), keep one copy, as a linker would.
- Debug sections are kept for the debug image (§9) and otherwise not loaded.

### 7.2 Binding policy

For every symbol a patch defines that already exists in the index, the policy decides whether references use the **old** definition or the **new** one. This rule is what makes hot reload preserve state.

| Symbol kind | Binds to | Reason |
|---|---|---|
| Functions | **new** | The point of the patch. The old entry is redirected (§8.3). |
| Writable data (`.data`, `.bss`, `.data.rel`) | **old** | Preserves state. |
| Function-local statics and their guard variables (`_ZGV*`; MSVC `$TSS*`, `_Init_thread_epoch`) | **old** | Preserves state and avoids re-running the local-static initializer. |
| vtables (`_ZTV*`, `??_7*`) | **old** | Existing objects already point at the old vtable. Its entries reach new code through the redirected old functions. |
| RTTI (`_ZTI*`, `_ZTS*`, `??_R0*`–`??_R4*`) | **old** | `catch`, `dynamic_cast` and `typeid` compare type identity, so there must be exactly one copy. |
| Existing `thread_local` (`.tbss` / `.tdata`, `.tls$`) | **old** | Every thread's TLS block is already laid out. |
| Read-only named data (`.rodata`, `.rdata`) | **new** | Changed constants and tables take effect. |
| Unnamed constants, string literals, jump tables | **new** | They belong to the new code. |

**Binding is per section, not per symbol.** Compilers refer to internal variables through *section* symbols (`.bss._ZL5ticks - 4`), not through the variable's own symbol, so a per-symbol rule would miss them. Patch compiles therefore add `-fdata-sections`: each variable gets its own section, and a section whose named symbols all exist in the index is bound as a whole to the old address (`old address − symbol offset`). A section mixing existing and new variables is an error. Text sections are always loaded new.

**Canonical function addresses.** Calls in the patch go straight to the new code. *Taking the address* of an existing function (a `lea`, a GOT slot, a vtable or a pointer constant) yields the function's original address instead. Function pointers then compare equal between old and new code, and they reach the newest version through the redirect.

Undefined references in the patch objects resolve in this order:
1. definitions in the patch itself, subject to the policy above
2. the symbol index, which includes earlier patches
3. other loaded modules' exports (libc, the C++ runtime and so on). The host sends all such names to the agent in one `ResolveRequest`, and the agent looks them up with `dlsym(RTLD_DEFAULT)` (`GetProcAddress` on Windows). This picks the default symbol version and resolves GNU IFUNCs (`memcpy`, `strlen`) to the implementation, exactly as the dynamic linker did, which reading the libraries' `.dynsym` on disk would not.

Anything still unresolved is an error, and no patch is applied.

### 7.3 Memory placement
- The agent reserves the region near the module at `init`.
  - Linux: `mmap(PROT_NONE)` with address hints, searching outward from the module's mapping in 64 KB steps.
  - Windows: a `VirtualQuery` / `VirtualAlloc(MEM_RESERVE)` scan in 64 KB steps.
- The host allocates within it with a bump allocator per patch. Code, read-only data and new writable data go into separate page-aligned segments.
- Because the region is within ±2 GB, every rel32 reference reaches the image: `call`, `jmp`, RIP-relative loads, PC32 relocations.
- References to *other* modules, such as `libstdc++` or `ucrtbase`, that are out of rel32 range go through near **stubs** (calls) or near **GOT slots** (data loads), which the host generates inside the region.

### 7.4 Relocations

**ELF x86-64.** Here S = symbol value, A = addend, P = address of the relocated place.

| Relocation | How it's handled |
|---|---|
| `R_X86_64_64` | S + A |
| `R_X86_64_PC32`, `R_X86_64_PLT32` | S + A − P. If S is out of range, PLT32 goes through a stub. |
| `R_X86_64_32`, `R_X86_64_32S` | S + A, with an overflow check. Only appears in non-PIE builds; the overflow is an error. |
| `R_X86_64_GOTPCREL`, `GOTPCRELX`, `REX_GOTPCRELX` | Address of a synthesized near GOT slot holding S, minus P. The relaxable forms may be relaxed in place (`mov` → `lea`) when S is in range. |
| `R_X86_64_TPOFF32` | The thread-pointer offset of the existing TLS variable (§7.7). |
| `R_X86_64_GOTTPOFF` | A GOT slot holding the thread-pointer offset. |
| `R_X86_64_TLSGD`, `R_X86_64_TLSLD` | A GOT pair (module ID, offset) of the main executable, used by `__tls_get_addr`. |

**COFF AMD64:**

| Relocation | How it's handled |
|---|---|
| `IMAGE_REL_AMD64_ADDR64` | S + A |
| `IMAGE_REL_AMD64_ADDR32NB` | S − `__ImageBase` of the patch (see below). |
| `IMAGE_REL_AMD64_REL32` … `REL32_5` | S + A − (P + 4 + n) |
| `IMAGE_REL_AMD64_SECREL` | The offset within the TLS section, for TLS (§7.7) and debug info. |
| `IMAGE_REL_AMD64_SECTION` | Debug info only. |

**`__ImageBase` per patch.** MSVC code uses image-base-relative addresses (RVAs): switch jump tables do `lea rcx, __ImageBase` and then index a table of RVAs, and `.pdata`/`.xdata` are RVAs too. Each patch therefore defines its own `__ImageBase` at the start of its code segment. Jump tables and code are both new, so they stay consistent. The same base is passed to `RtlAddFunctionTable`. A patch must fit in 4 GB above its own base, which it always does.

Relocations the linker doesn't recognise are a hard error that names the relocation type and object. New types are added as the test matrix finds them.

### 7.5 Repeated patches
- Each applied patch's new symbols are added to the index, tagged with the patch ID.
- A function patched a second time redirects the original entry **and** the entries of every earlier patch version to the newest one. Function pointers and vtable slots captured at any earlier point then all reach the newest code.
- Data introduced by patch *n* is "old" data for patch *n+1*, so its state carries over.
- Patch memory is never freed. Old code can still be on some thread's stack.

### 7.6 Restart-required detection
Before producing a package, the host rejects changes it knows it can't apply, with a clear message and no partial patch:

- an existing data symbol whose size changed (globals, statics, vtables)
- a new `thread_local` variable (§7.7)
- a relocation referencing an existing symbol of incompatible kind, for example something that was a function and is now data

Changes to struct layout behind pointers can't be seen from symbols. In M4, HotPP will add a *warning* by comparing debug-info type layouts (DWARF `DW_TAG_structure_type` / CodeView `LF_STRUCTURE`) of types used by the recompiled TUs.

### 7.7 Thread-local storage
- **Existing TLS variables** bind to their old slots.
  - Linux: the main executable's TLS block offsets are fixed at load time, from `PT_TLS` and the static TLS layout (x86-64 variant II). The host computes each thread-pointer offset from the symbol's `st_value` and the TLS segment size and alignment.
  - Windows: `_tls_index` resolves to the executable's own symbol, and `SECREL` relocations resolve to the variable's offset in the executable's `.tls` section. The `gs:[0x58][_tls_index]` access pattern then reaches the existing per-thread blocks.
- **New TLS variables** would need every existing thread's TLS block to grow. That isn't supported in v1 and is reported as restart required.

### 7.8 GCC function clones
GCC creates internal clones (`foo.constprop.0`, `foo.isra.0`, `foo.part.0`, `foo.cold`). These need no special handling:
- clones are local to their TU and have no address taken
- every old caller of an old clone is in the same TU, so it is itself redirected to new code, which calls new clones
- `.cold` fragments aren't entry points

Old clones become unreachable except from frames already on the stack, which is fine.

---

## 8. Applying a patch (agent)

### 8.1 Modes
- **Sync-point mode (default).** The agent receives the package on its IPC thread and queues it. The patch applies inside the next `hotpp::sync_point()` call, typically at the top of the frame or main loop. Pre- and post-patch hooks run at a well-defined moment, on the thread that calls `sync_point`.
- **Async mode** (`Config::async = true`). The patch applies immediately on the agent's IPC thread. Suited to apps without a natural loop.

In both modes, all threads other than the one applying the patch are paused while code and patch sites are written.

### 8.2 Pausing threads
- **Windows:**
  - Enumerate the process's threads with Toolhelp32 (`CreateToolhelp32Snapshot`), then call `SuspendThread` on each.
  - `GetThreadContext` forces the suspension to complete and gives the thread's instruction pointer (RIP).
- **Linux (in-process, no ptrace):**
  - Enumerate `/proc/self/task`, then send each thread a reserved real-time signal with `tgkill`. The handler is installed with `SA_RESTART`, and the agent picks a free signal at init.
  - The handler writes its RIP from `ucontext` into a per-thread slot, then waits on a futex until it is released.
  - Threads created during the pause are caught by re-enumerating until the set is stable.
- **Safety check:** if any paused thread's RIP is inside a byte range about to be rewritten (an entry's 2 bytes or its padding), release all threads, back off for 1 ms and retry, up to N times.

### 8.3 Patch-site scheme
The same scheme is used on both OSes, made possible by the padding flags in §4.

```
    entry-5:  E9 xx xx xx xx      jmp  new_entry       ; written first, into padding (never executed before)
    entry+0:  EB F9               jmp  short entry-5   ; then a single atomic 2-byte store
```

1. Write `E9 rel32` (a 5-byte jump to the new entry) into the padding bytes before the entry. No thread executes those bytes, so the write is safe.
2. Overwrite the first 2 bytes at the entry with `EB disp8`, a short jump back into the padding, as one aligned atomic 16-bit store. On GCC/Clang these bytes are the 2 NOPs at the entry. On MSVC they are the start of the at-least-2-byte instruction `/hotpatch` guarantees.

With **CET/IBT** (`-fcf-protection`), the entry starts with `endbr64` and the patch area follows it. The short jump goes at `entry+4` and its displacement changes to match (`EB F5`). The new function's own `endbr64` keeps indirect calls valid.

If a function is too small or has no padding, for example because it came from a TU built without the flags, it is reported as unpatchable. Its callers within recompiled TUs still reach the new code.

### 8.4 Writing code (W^X)
1. Change protection of the target pages to RW: `mprotect` on Linux, `VirtualProtect` on Windows.
2. Copy the segments.
3. Set final protections (RX / R / RW).
4. On Windows, call `FlushInstructionCache`. x86-64 Linux needs no explicit flush; a serializing operation suffices.
5. Patch sites on existing code pages are the exception: they are made RWX for the write, then RX again. The agent's own code can share a page with a function being redirected, and making that page non-executable would crash the agent mid-write.

### 8.5 Unwind registration
- **Linux:** the relocated `.eh_frame` of the patch is registered with `__register_frame` (libgcc/libunwind). Exceptions thrown through new code unwind correctly, including across a mix of old and new frames.
- **Windows:** the relocated `.pdata` (`RUNTIME_FUNCTION[]`) is registered with `RtlAddFunctionTable(pdata, count, patch __ImageBase)`.

### 8.6 Dynamic initializers
A patch TU contains dynamic initializers for its globals: `.init_array` entries on ELF, `.CRT$XCU` on COFF. Running them as they are would re-initialize globals bound to old storage and reset state. Not running them would leave *new* globals uninitialized.

**Shadow storage:**
1. The host finds *initializer code*: functions reachable by call-graph walk (through relocations) from the init entries. Examples: `_GLOBAL__sub_I_*`, `__cxx_global_var_init*`, GCC's `__static_initialization_and_destruction_*`, MSVC `??__E*`.
2. Inside initializer code only, data relocations are bound to **fresh shadow copies** of every data symbol the patch defines, whether old or new.
3. The agent runs the initializers after the patch code is mapped but *before* patch sites are written. No thread can reach new code that way until new globals are initialized. `__cxa_atexit` / `atexit` / `_onexit` are interposed during the run, which records destructor registrations against shadow storage.
4. Commit:
   - for **new** symbols, the shadow *is* the storage; their recorded destructors are forwarded to the real `atexit`
   - for **existing** symbols, the shadow and its destructor registrations are discarded

**Known limitation.** Side effects other than writing the global itself run again. Examples are self-registration into factories or test registries, logging and I/O. Mitigations:
- the `HOTPP_NO_REINIT` macro marks an initializer to skip on patch
- post-patch hooks can deduplicate registrations

### 8.7 Hooks
```cpp
HOTPP_PRE_PATCH(on_before_patch);   // runs before threads pause (sync-point thread)
HOTPP_POST_PATCH(on_after_patch);   // runs after patch sites are live and threads resumed
```
- The macros place a function pointer in a dedicated section (`hotpp_hooks` on ELF, `.hotpp$h` on COFF).
- The agent reads the image's section at `init`, and the host reports any hooks found in each patch. A patch can therefore add hooks or change existing ones.
- Typical uses: rebuilding caches, re-reading data files, invalidating precomputed function-pointer tables.

### 8.8 Failure handling
Every step before patch sites are written can be undone: the region is reverted to unused. The exception is external side effects of initializers that have already run (§8.6). If the agent fails after writing patch sites, which shouldn't happen, it reports the error and leaves the process running with whatever was applied, which is still valid code. The host marks the session *dirty* and recommends a restart.

---

## 9. Debugger support

- **Linux (M4):** the GDB JIT interface. The agent exports `__jit_debug_descriptor` and `__jit_debug_register_code`. For each patch, the host builds an in-memory ELF from the patch objects with section addresses set to their final location and DWARF relocated, and the agent registers it. gdb supports this natively; lldb does with `plugin.jit-loader.gdb.enable`. Breakpoints, stepping and call stacks then work in patched code.
- **Windows (open question, research in M4):** v1 has no debug info for patched code. The debugger shows addresses, and breakpoints set in old code don't fire in new code. The leading candidate:
  - Emit each patch as a real DLL via `link.exe`, linked `/FIXED` at a free address inside the reserved region.
  - Resolve references to existing symbols through a generated `.obj` of `IMAGE_SYM_ABSOLUTE` symbols.
  - Load it with `LoadLibrary` at its preferred base. The debugger then loads its PDB as normal.

  This would replace the custom COFF linker's output for Windows, but keep its resolver and policy. It needs a spike to confirm `link.exe` behaviour with absolute symbols in PC-relative relocations.

---

## 10. Repository layout and public API

```
HotPP/
├── CMakeLists.txt
├── cmake/HotPP.cmake              # hotpp_enable(<target>)
├── docs/DESIGN.md
├── include/hotpp/hotpp.h          # public agent API
├── src/
│   ├── common/protocol.h          # IPC messages, versioned
│   ├── agent/                     # static lib, C++17, no exceptions/RTTI
│   │   ├── agent.cpp
│   │   ├── memory_{linux,win}.cpp     # near-region reservation, protections
│   │   ├── threads_{linux,win}.cpp    # pause/resume, RIP check
│   │   ├── patch_x64.cpp              # patch-site writes
│   │   ├── unwind_{linux,win}.cpp
│   │   ├── init_runner.cpp            # shadow initializers, atexit interposition
│   │   ├── jit_debug.cpp              # GDB JIT interface (Linux)
│   │   └── ipc_{linux,win}.cpp
│   └── host/                      # hotpp executable, C++20
│       ├── main.cpp, session.cpp
│       ├── watcher_{linux,win}.cpp
│       ├── compile_db.cpp, deps.cpp
│       ├── compiler/{gcc,clang,msvc}.cpp   # flag rewriting, dependency flags, diagnostics
│       ├── image/{elf_image,pe_image,pdb_symbols,dwarf_cu}.cpp
│       ├── object/{elf_object,coff_object,codeview}.cpp
│       └── link/{resolver,policy,layout,stubs,reloc_elf_x64,reloc_coff_x64,initializers}.cpp
├── tests/
│   ├── unit/
│   └── e2e/
└── examples/
```

### Public API (`include/hotpp/hotpp.h`)
```cpp
namespace hotpp {

struct Config {
    const char* compile_commands = nullptr; // default: discovered from the build dir next to the exe
    const char* host_path        = nullptr; // default: "hotpp" on PATH
    bool        launch_host      = true;    // spawn the host if none is listening
    bool        async            = false;   // apply patches immediately instead of at sync_point()
    size_t      region_size      = 512u << 20;
};

bool init(const Config& cfg = {});   // false if HotPP is disabled or unavailable; the app keeps running
void sync_point();                    // apply pending patches (sync-point mode)
void shutdown();

} // namespace hotpp

#define HOTPP_PRE_PATCH(fn)   /* registers fn in the hooks section */
#define HOTPP_POST_PATCH(fn)
#define HOTPP_NO_REINIT       /* attribute-like marker for initializers to skip on patch */
```
Defining `HOTPP_DISABLE` turns the whole API into inline no-ops for shipping builds.

---

## 11. Milestones

| # | Scope | Done when |
|---|---|---|
| **M0** | Skeleton: CMake, `hotpp_enable`, agent and host binaries, IPC handshake, region reservation, sample app on Linux and Windows | The host prints the module list and reserved regions of the connected sample on both OSes. |
| **M1** | Linux MVP (GCC + Clang): compile DB, `.cpp`-only changes, ELF object loader, `.symtab` index, binding policy, stubs and GOT, patch sites, sync-point mode | Editing a function body in the sample changes behaviour live while a global counter keeps counting. |
| **M2** | Windows MVP (MSVC + clang-cl): spike on PDB internal-symbol matching first; then COFF loader, PDB index, `__ImageBase`, `.pdata` registration | Same demo as M1 on Windows. |
| **M3** | Correctness: dependency graph and header edits, several TUs per patch, repeated patches, shadow initializers, local statics, existing TLS, unwinding and RTTI across patches, thread pausing, restart-required checks | The full e2e suite (§12) passes on all four toolchains at `-O0` and `-O2`. |
| **M4** | Usability: GDB JIT debug info, Windows debug research, hooks, async mode, dependency caching, layout-change warnings, polished console output | You can step through patched code in gdb; a decision on Windows debug support is recorded. |
| **M5** | Extended: patching `.so`/`.dll` modules, hot restart, injecting into an app without the agent, ARM64 | Scoped separately. |

Each milestone ends with a git commit, pushed to the Gitea remote, once its "done when" condition is met, for example `M1: Linux MVP`.

**Known limits after M1** (each is planned for M3 unless noted):
- Only `.cpp` edits are patched; header edits are reported and ignored.
- Dynamic initializers in a patch don't run, so a *new* global that needs one starts zeroed.
- `.eh_frame` isn't registered yet. An exception thrown through new code can't unwind and ends in `std::terminate`.
- `thread_local` variables, new or existing, make a patch fail with "restart required".
- Other threads aren't paused while patch sites are written. Applying patches only in `sync_point()` keeps this safe for code that just the calling thread runs.
- A new direct reference to a shared-library *variable* (GCC with `-fPIE` emits these when the executable has no copy) is rejected with "restart required".
- Windows processes connect but aren't patched (M2).

---

## 12. Testing

### Unit tests
- Object and image parsers and the relocation appliers, tested against golden object files that the test build compiles from small sources with each toolchain.
- Binding-policy tests, each a table of symbol → expected binding.
- Patch-site encoder tests, covering displacement calculation and the CET offset.

### End-to-end harness
Each case is a directory `tests/e2e/<case>/` containing `v1/`, `v2/` and `expect.txt`. The harness:
1. builds the app from `v1/` with `hotpp_enable`
2. launches it
3. waits for the agent handshake
4. copies `v2/` sources over `v1/`
5. waits for `PatchResult`
6. checks the app's reported output against `expect.txt`

The test app reports over stdout, one line per sync point.

| Case | Checks |
|---|---|
| `body_change` | A changed function body takes effect. |
| `new_function` | A new function called from patched code works. |
| `global_state` | Globals keep their values across the patch. |
| `local_static` | Function-local statics keep their values and don't re-run their initializers. |
| `new_global` | A new global with a dynamic initializer is initialized; existing ones aren't reset. |
| `header_inline` | Editing an inline function in a header updates every caller. |
| `virtual_change` | Existing objects call the new body of a virtual function. |
| `lambda_template` | Lambda bodies and template instantiations update. |
| `fn_pointer` | A function pointer stored before the patch calls the new code. |
| `exceptions` | An exception thrown in new code is caught in an old frame and vice versa; RTTI matches. |
| `thread_spin` | A thread busy in a hot loop during the patch keeps running and picks up the new code. |
| `repeat_x3` | Three consecutive patches to the same function; pointers from every stage reach the newest. |
| `tls_existing` | Existing `thread_local` values are kept on every thread. |
| `compile_error` | A broken edit applies nothing; the fix applies afterwards. |
| `global_resize` | Changing a global's size is rejected with restart required. |
| `new_tls` | Adding a `thread_local` is rejected with restart required. |

**Matrix:** {Linux GCC, Linux Clang, Windows MSVC, Windows clang-cl} × {`-O0`, `-O2`}. CI uses Gitea Actions. Windows runner availability is open (§13).

Until a Windows machine or runner is set up, the Windows code is cross-compiled with MinGW (`cmake/toolchains/mingw-w64.cmake`) and its tests run under Wine. That covers the OS layer (pipes, process spawning, `VirtualAlloc` placement), but not MSVC-specific behaviour. MinGW binaries built with `-fpatchable-function-entry` fail at startup, so `hotpp_enable` leaves that flag out for MinGW and warns that the target can't be patched.

---

## 13. Risks and open questions

| # | Risk / question | Plan |
|---|---|---|
| 1 | Matching internal symbols in the Windows PDB to new `.obj` symbols (§6.2) is the riskiest piece of the Windows port. | Spike at the start of M2 against a test corpus of statics, overloads, anonymous namespaces, lambdas and templates. Fallback: also read the original `.obj` files from the build directory and match via PDB section contributions. |
| 2 | Initializer side effects run again (§8.6). | `HOTPP_NO_REINIT`, hooks, and documentation. Measure on real code in M3. |
| 3 | No debugger visibility of patched code on Windows (§9). | Research `/FIXED` DLL emission in M4. |
| 4 | Initial dependency scan is slow on large codebases. | Background scan with a persistent cache; evaluate `clang-scan-deps` for Clang and GCC TUs. |
| 5 | Is a Windows runner available for Gitea Actions CI? | Check before M2; otherwise run a self-hosted runner. |
| 6 | Thread pausing via signals interrupts syscalls (`EINTR`) in user code. | `SA_RESTART`; document it; sync-point mode keeps the pause short. Consider a ptrace-based helper if it causes problems. |
| 7 | Compiler flags in `compile_commands.json` may be relative to `directory` or use response files (`@file`). | Resolve against `directory`; expand response files in the compiler driver. |
