// gdb's JIT interface: debuggers set a breakpoint on __jit_debug_register_code and
// read the in-memory object files linked from __jit_debug_descriptor. See
// https://sourceware.org/gdb/current/onlinedocs/gdb.html/JIT-Interface.html
//
// Both symbols are weak, so if another JIT in the process defines them too, the
// linker keeps one pair and both register into the same list.
#include "agent/platform.h"

#include <cstring>
#include <mutex>

extern "C" {

enum jit_actions_t : uint32_t { JIT_NOACTION = 0, JIT_REGISTER_FN = 1, JIT_UNREGISTER_FN = 2 };

struct jit_code_entry {
    jit_code_entry* next_entry;
    jit_code_entry* prev_entry;
    const char* symfile_addr;
    uint64_t symfile_size;
};

struct jit_descriptor {
    uint32_t version;
    uint32_t action_flag;
    jit_code_entry* relevant_entry;
    jit_code_entry* first_entry;
};

__attribute__((weak, noinline, used)) void __jit_debug_register_code() { __asm__ volatile("" ::: "memory"); }

__attribute__((weak, used)) jit_descriptor __jit_debug_descriptor = {1, JIT_NOACTION, nullptr, nullptr};

} // extern "C"

namespace hotpp::agent {

void register_debug_object(const std::vector<uint8_t>& object) {
    static std::mutex mutex;
    if (object.empty()) return;
    auto* bytes = new char[object.size()];
    std::memcpy(bytes, object.data(), object.size());
    auto* entry = new jit_code_entry{nullptr, nullptr, bytes, object.size()};

    std::lock_guard<std::mutex> lock(mutex);
    entry->next_entry = __jit_debug_descriptor.first_entry;
    if (entry->next_entry) entry->next_entry->prev_entry = entry;
    __jit_debug_descriptor.first_entry = entry;
    __jit_debug_descriptor.relevant_entry = entry;
    __jit_debug_descriptor.action_flag = JIT_REGISTER_FN;
    __jit_debug_register_code(); // an attached debugger stops here and reads the entry
    __jit_debug_descriptor.action_flag = JIT_NOACTION;
}

} // namespace hotpp::agent
