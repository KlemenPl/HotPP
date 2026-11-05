// OS layer of the agent: platform_linux.cpp, threads_linux.cpp.
#pragma once

#include "common/messages.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hotpp::agent {

struct Connection;
struct Thread;

uint32_t current_pid();
std::string executable_path();
std::string get_env(const char* name); // empty if unset

// All loaded modules; element 0 is the main executable.
std::vector<proto::ModuleRecord> enumerate_modules();
// The dynamic linker's running counts of loaded and unloaded objects; a cheap way
// to tell that enumerate_modules() would return something new.
void module_counters(uint64_t& adds, uint64_t& subs);

// Reserves `size` bytes of inaccessible address space within rel32 reach of `module`.
bool reserve_near(const proto::ModuleRecord& module, uint64_t size, proto::RegionRecord& out);
void release_region(const proto::RegionRecord& region);

uint64_t page_size();
// Sets protection (proto::kProt* bits) of whole pages covering [addr, addr + size).
bool protect(uint64_t addr, uint64_t size, uint8_t prot);
void flush_icache(uint64_t addr, uint64_t size);
// Address of an exported symbol in any loaded module (default version), or 0.
uint64_t resolve_symbol(const std::string& name);

// Makes the unwinder aware of a zero-terminated .eh_frame block. False if the
// runtime offers no way to do so.
bool register_unwind_info(uint64_t eh_frame);

// The calling thread is the agent's own; it is never paused.
void mark_agent_thread();
// Stops every other thread of the process (except the agent thread) and reports
// where each one stopped. On failure nothing stays paused.
bool pause_other_threads(std::vector<uint64_t>& stopped_at, std::string& error);
void resume_other_threads();
// After modifying code other threads may run: serialize every core (no-op if unsupported).
void sync_cores();

// Hands an in-memory ELF object describing patched code to an attached debugger
// (gdb's JIT interface). The bytes are kept for the life of the process.
void register_debug_object(const std::vector<uint8_t>& object);

// Byte stream to the host. send/recv transfer exactly `size` bytes or fail.
Connection* ipc_connect(const std::string& endpoint);
bool ipc_send(Connection* c, const void* data, size_t size);
bool ipc_recv(Connection* c, void* data, size_t size);
void ipc_shutdown(Connection* c); // unblocks a pending ipc_recv on another thread
// Waits up to `ms` for data (or the end of the connection) to arrive.
bool ipc_readable(Connection* c, int ms);
void ipc_close(Connection* c);

// The command line the process was started with (/proc/self/cmdline).
std::vector<std::string> command_line();
// Replaces the process image; returns only on failure (with errno-style text).
std::string exec_self(const std::string& path, const std::vector<std::string>& argv);

// Starts `program` (searched on PATH) without waiting for it.
bool spawn_detached(const std::string& program, const std::vector<std::string>& args);

Thread* thread_start(void (*fn)(void*), void* arg);
void thread_join(Thread* t);
void sleep_ms(unsigned ms);

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
void log(const char* fmt, ...);

} // namespace hotpp::agent
