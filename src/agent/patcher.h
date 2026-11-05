// Applies a host-built patch inside the target process.
#pragma once

#include "common/messages.h"

#include <string>
#include <vector>

namespace hotpp::agent {

// Maps the patch segments and redirects the patch sites. Everything is validated
// before the first byte is written; returns an empty string on success, otherwise
// a description of the problem. Not thread safe: call with patched code quiescent.
std::string apply_patch(const proto::Patch& patch, const std::vector<proto::RegionRecord>& regions,
                        const std::vector<proto::ModuleRecord>& modules);

} // namespace hotpp::agent
