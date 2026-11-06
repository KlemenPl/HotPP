#include "mcp/server.h"

#include <algorithm>

namespace hotpp::mcp {

const char* const kProtocolVersions[] = {"2025-11-25", "2025-06-18", "2025-03-26", "2024-11-05"};
const size_t kProtocolVersionCount = sizeof kProtocolVersions / sizeof kProtocolVersions[0];

namespace {

json::Value error_response(const json::Value& id, int code, const std::string& message) {
    json::Value e = json::Value::object();
    e.set("code", code).set("message", message);
    json::Value r = json::Value::object();
    r.set("jsonrpc", "2.0").set("id", id).set("error", std::move(e));
    return r;
}

json::Value result_response(const json::Value& id, json::Value result) {
    json::Value r = json::Value::object();
    r.set("jsonrpc", "2.0").set("id", id).set("result", std::move(result));
    return r;
}

json::Value tool_result(const ToolResult& t) {
    json::Value text = json::Value::object();
    text.set("type", "text").set("text", t.text);
    json::Value content = json::Value::array();
    content.push(std::move(text));
    json::Value r = json::Value::object();
    r.set("content", std::move(content));
    if (t.structured.is_object()) r.set("structuredContent", t.structured);
    r.set("isError", t.is_error);
    return r;
}

bool valid_id(const json::Value& id) { return id.is_string() || id.is_integer(); }

} // namespace

Server::Server(ToolProvider& tools, ServerInfo info, Writer write, size_t max_calls)
    : tools_(tools), info_(std::move(info)), write_(std::move(write)), max_calls_(std::max<size_t>(1, max_calls)) {}

Server::~Server() { shutdown(); }

std::string Server::negotiated_version() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return version_;
}

void Server::send(const json::Value& msg) { write_(msg.dump()); }

void Server::line_too_long() { send(error_response(json::Value(), kParseError, "Parse error: message too long")); }

void Server::handle_line(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.remove_prefix(1);
    if (line.empty()) return;
    json::Value msg;
    try {
        msg = json::Value::parse(line);
    } catch (const json::JsonError& e) {
        send(error_response(json::Value(), kParseError, std::string("Parse error: ") + e.what()));
        return;
    }
    reap();
    if (!msg.is_array()) {
        json::Value response = handle(msg, false);
        if (!response.is_null()) send(response);
        return;
    }
    // A batch (protocol 2025-03-26): answered as one array, so its tool calls run
    // one after another on a single worker.
    if (msg.as_array().empty()) {
        send(error_response(json::Value(), kInvalidRequest, "Invalid Request: empty batch"));
        return;
    }
    auto done = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lock(mutex_);
    if (shutting_down_) return;
    workers_.push_back({std::thread([this, msg = std::move(msg), done] {
                            json::Value responses = json::Value::array();
                            for (const json::Value& m : msg.as_array()) {
                                json::Value r = handle(m, true);
                                if (!r.is_null()) responses.push(std::move(r));
                            }
                            if (responses.size()) send(responses);
                            *done = true;
                            idle_cv_.notify_all();
                        }),
                        done});
}

json::Value Server::handle(const json::Value& msg, bool inline_calls) {
    if (!msg.is_object()) return error_response(json::Value(), kInvalidRequest, "Invalid Request: not an object");
    const json::Value* id = msg.get("id");
    const json::Value* method = msg.get("method");
    const json::Value* jsonrpc = msg.get("jsonrpc");
    if (!method) {
        if (msg.get("result") || msg.get("error")) return json::Value(); // a response; we send no requests
        return error_response(id && valid_id(*id) ? *id : json::Value(), kInvalidRequest,
                              "Invalid Request: no method");
    }
    const bool notification = !id;
    if (!jsonrpc || !jsonrpc->is_string() || jsonrpc->as_string() != "2.0" || !method->is_string() ||
        (id && !valid_id(*id))) {
        if (notification) return json::Value();
        return error_response(valid_id(*id) ? *id : json::Value(), kInvalidRequest,
                              "Invalid Request: needs \"jsonrpc\": \"2.0\", a string method and a string or integer id");
    }
    static const json::Value kEmpty = json::Value::object();
    const json::Value* params = msg.get("params");
    if (params && !params->is_object() && !params->is_array() && !params->is_null()) {
        if (notification) return json::Value();
        return error_response(*id, kInvalidParams, "Invalid params: must be an object");
    }
    if (!params || !params->is_object()) params = &kEmpty;
    const std::string& name = method->as_string();

    if (notification) {
        if (name == "notifications/cancelled") {
            const json::Value* req = params->get("requestId");
            if (req) {
                std::lock_guard<std::mutex> lock(mutex_);
                auto it = calls_.find(req->dump());
                if (it != calls_.end()) *it->second = true;
            }
        }
        return json::Value(); // notifications/initialized and anything else: nothing to say
    }
    if (name == "initialize") return result_response(*id, initialize(*params));
    if (name == "ping") return result_response(*id, json::Value::object());
    if (name == "tools/list") {
        json::Value r = json::Value::object();
        r.set("tools", tools_.list());
        return result_response(*id, std::move(r));
    }
    if (name == "tools/call") return call_tool(*id, *params, inline_calls);
    return error_response(*id, kMethodNotFound, "Method not found: " + name);
}

json::Value Server::initialize(const json::Value& params) {
    std::string chosen = kProtocolVersions[0];
    if (const json::Value* v = params.get("protocolVersion"); v && v->is_string())
        for (size_t i = 0; i < kProtocolVersionCount; ++i)
            if (v->as_string() == kProtocolVersions[i]) chosen = v->as_string();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        version_ = chosen;
    }
    json::Value tools = json::Value::object();
    tools.set("listChanged", false);
    json::Value caps = json::Value::object();
    caps.set("tools", std::move(tools));
    json::Value server = json::Value::object();
    server.set("name", info_.name).set("title", info_.title).set("version", info_.version);
    json::Value r = json::Value::object();
    r.set("protocolVersion", chosen).set("capabilities", std::move(caps)).set("serverInfo", std::move(server));
    if (!info_.instructions.empty()) r.set("instructions", info_.instructions);
    return r;
}

json::Value Server::call_tool(const json::Value& id, const json::Value& params, bool inline_call) {
    const json::Value* name = params.get("name");
    if (!name || !name->is_string()) return error_response(id, kInvalidParams, "Invalid params: 'name' must be a string");
    if (!tools_.has(name->as_string())) return error_response(id, kInvalidParams, "Unknown tool: " + name->as_string());
    const json::Value* args = params.get("arguments");
    static const json::Value kEmpty = json::Value::object();
    if (args && !args->is_object() && !args->is_null())
        return error_response(id, kInvalidParams, "Invalid params: 'arguments' must be an object");
    if (!args || args->is_null()) args = &kEmpty;
    json::Value token;
    if (const json::Value* meta = params.get("_meta"))
        if (const json::Value* t = meta->get("progressToken"); t && valid_id(*t)) token = *t;

    auto cancelled = std::make_shared<std::atomic<bool>>(false);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (shutting_down_) return error_response(id, kInternalError, "the server is shutting down");
        if (!inline_call && calls_.size() >= max_calls_) {
            ToolResult busy;
            busy.is_error = true;
            busy.text = "HotPP is busy with " + std::to_string(calls_.size()) +
                        " tool calls already; try again when one of them has finished.";
            return result_response(id, tool_result(busy));
        }
        calls_[id.dump()] = cancelled;
    }
    if (inline_call) return run_tool(id, name->as_string(), *args, token, cancelled);

    auto done = std::make_shared<std::atomic<bool>>(false);
    std::lock_guard<std::mutex> lock(mutex_);
    workers_.push_back({std::thread([this, id, tool = name->as_string(), args = *args, token, cancelled, done] {
                            json::Value response = run_tool(id, tool, args, token, cancelled);
                            if (!response.is_null()) send(response);
                            *done = true;
                            idle_cv_.notify_all();
                        }),
                        done});
    return json::Value();
}

json::Value Server::run_tool(const json::Value& id, const std::string& name, const json::Value& args,
                             const json::Value& token, const std::shared_ptr<std::atomic<bool>>& cancelled) {
    CallContext ctx;
    ctx.cancelled = cancelled.get();
    if (!token.is_null()) {
        auto count = std::make_shared<int64_t>(0);
        ctx.progress = [this, token, count, cancelled](const std::string& message) {
            if (*cancelled) return;
            json::Value p = json::Value::object();
            p.set("progressToken", token).set("progress", ++*count).set("message", message);
            json::Value n = json::Value::object();
            n.set("jsonrpc", "2.0").set("method", "notifications/progress").set("params", std::move(p));
            send(n);
        };
    }
    ToolResult result;
    try {
        result = tools_.call(name, args, ctx);
    } catch (const std::exception& e) {
        result = ToolResult{};
        result.is_error = true;
        result.text = std::string("internal error in ") + name + ": " + e.what();
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        calls_.erase(id.dump());
    }
    if (*cancelled) return json::Value(); // the client no longer wants an answer
    return result_response(id, tool_result(result));
}

void Server::reap() {
    std::vector<std::thread> finished;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto it = workers_.begin(); it != workers_.end();) {
            if (!*it->done) {
                ++it;
                continue;
            }
            finished.push_back(std::move(it->thread));
            it = workers_.erase(it);
        }
    }
    for (std::thread& t : finished) t.join();
}

void Server::wait_idle() {
    {
        std::unique_lock<std::mutex> lock(mutex_);
        idle_cv_.wait(lock, [&] {
            return std::all_of(workers_.begin(), workers_.end(), [](const Worker& w) { return w.done->load(); });
        });
    }
    reap();
}

bool Server::idle() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::all_of(workers_.begin(), workers_.end(), [](const Worker& w) { return w.done->load(); });
}

void Server::shutdown() {
    std::vector<Worker> workers;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shutting_down_ = true;
        for (auto& [id, flag] : calls_) *flag = true;
        workers = std::move(workers_);
    }
    for (Worker& w : workers) w.thread.join();
}

} // namespace hotpp::mcp
