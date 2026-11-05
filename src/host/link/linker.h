// The patch linker: links freshly compiled objects against the running process
// and produces the bytes and patch sites the agent applies (docs/DESIGN.md §7).
#pragma once

#include "common/messages.h"
#include "host/link/symbol_index.h"
#include "host/object/elf_object.h"

#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace hotpp::link {

struct LinkError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// The change can't be applied to the running program; it needs a restart.
struct RestartRequired : LinkError {
    using LinkError::LinkError;
};

// Looks up symbols exported by the process's other modules (shared libraries).
// Returns one address per name, 0 where not found.
using ExternalResolver = std::function<std::vector<uint64_t>(const std::vector<std::string>&)>;

struct LinkOptions {
    uint64_t region_cursor = 0; // first free byte of the patch region
    uint64_t region_end = 0;
    uint64_t page_size = 4096;
    // Size of the executable's static TLS block (PT_TLS memsz rounded up to its
    // alignment); thread-pointer offsets are symbol value minus this. 0: no TLS.
    uint64_t tls_block_size = 0;
};

struct LinkResult {
    proto::Patch patch; // id is left for the caller
    uint64_t bytes_used = 0;
    IndexDelta delta;
    size_t functions_redirected = 0;
    size_t functions_new = 0;
    size_t data_kept = 0;
    size_t data_new = 0;
    size_t initializers_run = 0;
    std::vector<std::string> notes; // things the user should know; not errors
    std::vector<std::string> log;   // every binding decision, for --verbose
};

// Throws LinkError (or RestartRequired) when the objects can't be linked.
LinkResult link_patch(const std::vector<const object::ElfObject*>& objects, const SymbolIndex& index,
                      const ExternalResolver& resolve, const LinkOptions& options);

} // namespace hotpp::link
