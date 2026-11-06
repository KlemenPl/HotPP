// hotpp mcp: a Model Context Protocol server (JSON-RPC over stdio) that lets AI
// agents launch, patch, restart and inspect programs through HotPP hosts.
#pragma once

namespace hotpp::mcp {

// `hotpp mcp [options]`; argv[0] is "mcp". Returns the exit code.
int run(int argc, char** argv);

} // namespace hotpp::mcp
