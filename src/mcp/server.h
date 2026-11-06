// The Model Context Protocol layer of hotpp mcp: JSON-RPC 2.0 messages, one per
// line (the stdio transport), the lifecycle and tool calls. Tools come from a
// ToolProvider; this file knows nothing about HotPP.
#pragma once

#include "host/util/json.h"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace hotpp::mcp {

// Protocol revisions this server speaks, newest first.
extern const char* const kProtocolVersions[];
extern const size_t kProtocolVersionCount;

// What a running tool call can use.
struct CallContext {
    // Reports progress to the client (a no-op unless it asked for progress).
    std::function<void(const std::string& message)> progress = [](const std::string&) {};
    // Set when the client cancels the call; its result is then dropped.
    const std::atomic<bool>* cancelled = nullptr;
    bool is_cancelled() const { return cancelled && cancelled->load(); }
};

struct ToolResult {
    std::string text;       // what the model reads
    json::Value structured; // structuredContent; null for none
    bool is_error = false;  // the tool ran but failed (the model should react)
};

class ToolProvider {
public:
    virtual ~ToolProvider() = default;
    virtual json::Value list() const = 0; // the "tools" array of tools/list
    virtual bool has(const std::string& name) const = 0;
    // May block; runs on a worker thread. Exceptions become error results.
    virtual ToolResult call(const std::string& name, const json::Value& arguments, CallContext& ctx) = 0;
};

struct ServerInfo {
    std::string name, title, version, instructions;
};

class Server {
public:
    // Sends one message (a single line of JSON, without the newline).
    using Writer = std::function<void(const std::string& line)>;

    Server(ToolProvider& tools, ServerInfo info, Writer write, size_t max_calls = 16);
    ~Server();
    Server(const Server&) = delete;
    Server& operator=(const Server&) = delete;

    // A line from the client, without its newline. Tool calls run on worker
    // threads; everything else is answered before this returns.
    void handle_line(std::string_view line);
    // The client sent a line longer than the reader accepts (it was discarded).
    void line_too_long();
    // Cancels calls in progress and waits for them to end.
    void shutdown();
    // Waits until no call is in progress.
    void wait_idle();
    bool idle() const;

    std::string negotiated_version() const;

private:
    // The response to one message; null for notifications and client responses.
    // Tool calls are started on a worker and answered later, unless `inline_calls`.
    json::Value handle(const json::Value& msg, bool inline_calls);
    json::Value initialize(const json::Value& params);
    // A tools/call: the response, or null if it was handed to a worker (or cancelled).
    json::Value call_tool(const json::Value& id, const json::Value& params, bool inline_call);
    json::Value run_tool(const json::Value& id, const std::string& name, const json::Value& args,
                         const json::Value& progress_token, const std::shared_ptr<std::atomic<bool>>& cancelled);
    void send(const json::Value& msg);
    void reap();

    ToolProvider& tools_;
    const ServerInfo info_;
    const Writer write_;
    const size_t max_calls_;

    mutable std::mutex mutex_;
    std::condition_variable idle_cv_;
    std::string version_;
    // Calls in progress, by the JSON text of their id.
    std::map<std::string, std::shared_ptr<std::atomic<bool>>> calls_;
    struct Worker {
        std::thread thread;
        std::shared_ptr<std::atomic<bool>> done;
    };
    std::vector<Worker> workers_;
    bool shutting_down_ = false;
};

// Standard JSON-RPC error codes.
constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;

} // namespace hotpp::mcp
