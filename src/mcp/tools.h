// The tools hotpp mcp offers: status, launch, reload, restart, stop, output, logs
// and auto-reload, on top of Hosts.
#pragma once

#include "mcp/hosts.h"
#include "mcp/server.h"

#include <string>
#include <vector>

namespace hotpp::mcp {

struct ToolsOptions {
    std::string default_build; // build command for launched programs (hotpp mcp --build)
    std::string cwd;           // where relative paths are resolved
};

class HotppTools final : public ToolProvider {
public:
    HotppTools(Hosts& hosts, ToolsOptions options);
    ~HotppTools() override;

    json::Value list() const override;
    bool has(const std::string& name) const override;
    ToolResult call(const std::string& name, const json::Value& arguments, CallContext& ctx) override;

    class Args;
    struct Param;
    struct Tool;

private:
    ToolResult status(const Args& a, CallContext& ctx);
    ToolResult reload(const Args& a, CallContext& ctx);
    ToolResult launch(const Args& a, CallContext& ctx);
    ToolResult restart(const Args& a, CallContext& ctx);
    ToolResult stop(const Args& a, CallContext& ctx);
    ToolResult output(const Args& a, CallContext& ctx);
    ToolResult logs(const Args& a, CallContext& ctx);
    ToolResult set_auto_reload(const Args& a, CallContext& ctx);

    Hosts& hosts_;
    ToolsOptions options_;
    std::vector<Tool> tools_;
};

// The text for the model's instructions (initialize).
std::string instructions();

} // namespace hotpp::mcp
