// The MCP layer (JSON-RPC lifecycle and tool calls, with a fake tool provider) and
// the control channel between hotpp mcp and a host (a real controller on a socket).
#include "common/endpoint.h"
#include "host/control.h"
#include "host/control_protocol.h"
#include "mcp/hosts.h"
#include "mcp/server.h"
#include "mcp/tools.h"
#include "unit/test.h"

#include <filesystem>
#include <mutex>
#include <set>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

using namespace hotpp;
using namespace std::chrono_literals;
using json::Value;

namespace {

class FakeTools final : public mcp::ToolProvider {
public:
    Value list() const override {
        Value t = Value::object();
        t.set("name", "echo").set("inputSchema", Value::object());
        Value a = Value::array();
        a.push(std::move(t));
        return a;
    }
    bool has(const std::string& name) const override {
        return name == "echo" || name == "block" || name == "throw";
    }
    mcp::ToolResult call(const std::string& name, const Value& args, mcp::CallContext& ctx) override {
        if (name == "throw") throw std::runtime_error("boom");
        if (name == "block") {
            const auto deadline = std::chrono::steady_clock::now() + 5s;
            while (!ctx.is_cancelled() && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
            return {"unblocked", Value(), false};
        }
        ctx.progress("working");
        Value structured = Value::object();
        structured.set("args", args);
        return {"echo " + args.dump(), structured, false};
    }
};

struct Harness {
    FakeTools tools;
    std::mutex mutex;
    std::vector<std::string> lines;
    mcp::Server server;
    explicit Harness(size_t max_calls = 16)
        : server(tools, {"test", "Test", "0", "use it"},
                 [this](const std::string& l) {
                     std::lock_guard<std::mutex> lock(mutex);
                     lines.push_back(l);
                 },
                 max_calls) {}

    std::vector<Value> take() {
        server.wait_idle();
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Value> out;
        for (const std::string& l : lines) out.push_back(Value::parse(l));
        lines.clear();
        return out;
    }
    // The one reply to `line`.
    Value one(const std::string& line) {
        server.handle_line(line);
        auto all = take();
        CHECK(all.size() == 1);
        return all.empty() ? Value() : all[0];
    }
};

int error_code(const Value& v) {
    const Value* e = v.get("error");
    return e && e->get("code") ? int(e->get("code")->as_number()) : 0;
}

} // namespace

HOTPP_TEST(mcp_initialize_negotiates_the_version) {
    Harness h;
    Value r = h.one(R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-03-26"}})");
    CHECK(r.get("id")->as_integer() == 1);
    const Value* res = r.get("result");
    CHECK(res && res->get("protocolVersion")->as_string() == "2025-03-26");
    CHECK(res->get("capabilities")->get("tools"));
    CHECK(res->get("serverInfo")->get("name")->as_string() == "test");
    CHECK(res->get("instructions")->as_string() == "use it");
    r = h.one(R"({"jsonrpc":"2.0","id":"a","method":"initialize","params":{"protocolVersion":"2099-01-01"}})");
    CHECK(r.get("id")->as_string() == "a");
    CHECK(r.get("result")->get("protocolVersion")->as_string() == mcp::kProtocolVersions[0]);
    r = h.one(R"({"jsonrpc":"2.0","id":2,"method":"initialize"})");
    CHECK(r.get("result")->get("protocolVersion")->as_string() == mcp::kProtocolVersions[0]);
    h.server.handle_line(R"({"jsonrpc":"2.0","method":"notifications/initialized"})");
    CHECK(h.take().empty());
    r = h.one(R"({"jsonrpc":"2.0","id":3,"method":"ping"})");
    CHECK(r.get("result")->is_object() && r.get("result")->size() == 0);
    r = h.one(R"({"jsonrpc":"2.0","id":4,"method":"tools/list"})");
    CHECK(r.get("result")->get("tools")->as_array().size() == 1);
}

HOTPP_TEST(mcp_tool_calls_answer_with_content_and_progress) {
    Harness h;
    h.server.handle_line(
        R"({"jsonrpc":"2.0","id":5,"method":"tools/call","params":{"name":"echo","arguments":{"x":1},"_meta":{"progressToken":"p"}}})");
    auto all = h.take();
    CHECK(all.size() == 2);
    const Value* progress = nullptr;
    const Value* reply = nullptr;
    for (const Value& v : all) (v.get("method") ? progress : reply) = &v;
    CHECK(progress && progress->get("params")->get("progressToken")->as_string() == "p");
    CHECK(progress->get("params")->get("message")->as_string() == "working");
    CHECK(reply && reply->get("id")->as_integer() == 5);
    const Value* res = reply->get("result");
    CHECK(res->get("content")->as_array()[0].get("type")->as_string() == "text");
    CHECK(res->get("content")->as_array()[0].get("text")->as_string() == R"(echo {"x":1})");
    CHECK(res->get("structuredContent")->get("args")->get("x")->as_integer() == 1);
    CHECK(!res->get("isError")->as_bool());

    Value r = h.one(R"({"jsonrpc":"2.0","id":6,"method":"tools/call","params":{"name":"throw"}})");
    CHECK(r.get("result")->get("isError")->as_bool());
    CHECK(r.get("result")->get("content")->as_array()[0].get("text")->as_string().find("boom") != std::string::npos);
}

HOTPP_TEST(mcp_rejects_bad_messages_and_keeps_going) {
    Harness h;
    CHECK(error_code(h.one("{not json")) == mcp::kParseError);
    CHECK(error_code(h.one("1")) == mcp::kInvalidRequest);
    Value r = h.one(R"({"jsonrpc":"1.0","id":7,"method":"ping"})");
    CHECK(error_code(r) == mcp::kInvalidRequest && r.get("id")->as_integer() == 7);
    r = h.one(R"({"jsonrpc":"2.0","id":{"x":1},"method":"ping"})");
    CHECK(error_code(r) == mcp::kInvalidRequest && r.get("id")->is_null());
    CHECK(error_code(h.one(R"({"jsonrpc":"2.0","id":8,"method":"resources/list"})")) == mcp::kMethodNotFound);
    CHECK(error_code(h.one(R"({"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"nope"}})")) ==
          mcp::kInvalidParams);
    CHECK(error_code(h.one(R"({"jsonrpc":"2.0","id":10,"method":"tools/call","params":{}})")) == mcp::kInvalidParams);
    CHECK(error_code(h.one(R"({"jsonrpc":"2.0","id":11,"method":"tools/call","params":{"name":"echo","arguments":3}})")) ==
          mcp::kInvalidParams);
    CHECK(error_code(h.one(R"({"jsonrpc":"2.0","id":12,"method":"ping","params":5})")) == mcp::kInvalidParams);
    // Notifications and responses get no reply, whatever they are.
    h.server.handle_line(R"({"jsonrpc":"2.0","method":"notifications/unknown"})");
    h.server.handle_line(R"({"jsonrpc":"2.0","id":99,"result":{}})");
    h.server.handle_line("   ");
    CHECK(h.take().empty());
    h.server.line_too_long();
    CHECK(error_code(h.take().at(0)) == mcp::kParseError);
    CHECK(h.one(R"({"jsonrpc":"2.0","id":13,"method":"ping"})").get("result"));
}

HOTPP_TEST(mcp_batches_get_one_array) {
    Harness h;
    h.server.handle_line(R"([{"jsonrpc":"2.0","id":1,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/initialized"},)"
                         R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"echo"}}])");
    auto all = h.take();
    CHECK(all.size() == 1 && all[0].is_array() && all[0].as_array().size() == 2);
    CHECK(error_code(h.one("[]")) == mcp::kInvalidRequest);
}

HOTPP_TEST(mcp_cancelled_calls_get_no_reply) {
    Harness h(1);
    h.server.handle_line(R"({"jsonrpc":"2.0","id":77,"method":"tools/call","params":{"name":"block"}})");
    // Only one call at a time: the next is turned away at once.
    h.server.handle_line(R"({"jsonrpc":"2.0","id":78,"method":"tools/call","params":{"name":"echo"}})");
    {
        std::lock_guard<std::mutex> lock(h.mutex);
        CHECK(h.lines.size() == 1);
        if (!h.lines.empty()) {
            const Value busy = Value::parse(h.lines[0]);
            CHECK(busy.get("id")->as_integer() == 78 && busy.get("result")->get("isError")->as_bool());
        }
        h.lines.clear();
    }
    h.server.handle_line(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":77}})");
    CHECK(h.take().empty());
    // shutdown() cancels what's still running.
    h.server.handle_line(R"({"jsonrpc":"2.0","id":79,"method":"tools/call","params":{"name":"block"}})");
    const auto t0 = std::chrono::steady_clock::now();
    h.server.shutdown();
    CHECK(std::chrono::steady_clock::now() - t0 < 3s);
    CHECK(h.take().empty());
}

// ---- the control channel ------------------------------------------------------

namespace {

std::string temp_dir() {
    char tmpl[] = "/tmp/hotpp-unit-XXXXXX";
    const char* d = mkdtemp(tmpl);
    return d ? d : "/tmp";
}

// A host's control side on a socket: what main.cpp does with a ControlHello.
class TestHost {
public:
    explicit TestHost(std::string path) : path_(std::move(path)) {
        ctx.endpoint = path_;
        std::string error;
        listener_ = host::listen(path_, error);
        CHECK(listener_ != nullptr);
        acceptor_ = std::thread([this] {
            while (auto s = listener_->accept()) {
                host::Stream* raw = s.get();
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    open_.insert(raw);
                }
                threads_.emplace_back([this, s = std::move(s)]() mutable {
                    uint8_t header[proto::kHeaderSize];
                    proto::Header h;
                    json::Value hello;
                    if (s->recv(header, sizeof header) && proto::decode_header(header, h) &&
                        h.type == proto::MsgType::ControlHello &&
                        host::recv_json_payload(*s, h, hello, host::kMaxControlRequest) == host::RecvStatus::Ok)
                        host::serve_controller(*s, hello, ctx);
                    std::lock_guard<std::mutex> lock(mutex_);
                    open_.erase(s.get());
                });
            }
        });
    }
    ~TestHost() {
        listener_->close();
        acceptor_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (host::Stream* s : open_) s->shutdown_read();
        }
        for (std::thread& t : threads_) t.join();
    }
    host::HostContext ctx;

private:
    std::string path_;
    std::unique_ptr<host::Listener> listener_;
    std::thread acceptor_;
    std::mutex mutex_;
    std::set<host::Stream*> open_;
    std::vector<std::thread> threads_;
};

std::string thrown(const std::function<void()>& f) {
    try {
        f();
    } catch (const mcp::HostError& e) {
        return e.what();
    }
    return {};
}

} // namespace

HOTPP_TEST(control_channel_answers_requests) {
    const std::string dir = temp_dir();
    std::shared_ptr<mcp::HostClient> client;
    {
        TestHost host(dir + "/hotpp.sock");
        std::string error;
        bool incompatible = false;
        client = mcp::HostClient::connect(dir + "/hotpp.sock", 2s, error, incompatible);
        CHECK(client != nullptr);
        if (!client) return;
        CHECK(client->host_pid() == int64_t(getpid()));
        CHECK(client->hello().get("mode")->as_string() == "serve");

        Value status = client->request("status", Value::object(), 5s);
        CHECK(status.get("programs")->as_array().empty());

        // Run mode before the program is started: it hasn't exited, so waiting for
        // it waits (CI once caught a launch reported as exited this way).
        host.ctx.run_mode = true;
        Value wait = Value::object();
        wait.set("timeout_ms", 200);
        const auto w0 = std::chrono::steady_clock::now();
        Value waited = client->request("wait_program", wait, 5s);
        CHECK(std::chrono::steady_clock::now() - w0 >= 150ms);
        CHECK(!waited.get("ready")->as_bool());
        const Value* child = waited.get("host")->get("child");
        CHECK(child && child->get("running")->as_bool() && !child->get("exit_code"));
        {
            std::lock_guard<std::mutex> lock(host.ctx.child.mutex);
            host.ctx.child.pid = 12345;
            host.ctx.child.running = false;
            host.ctx.child.exit_code = 3;
        }
        const auto w1 = std::chrono::steady_clock::now();
        waited = client->request("wait_program", wait, 5s);
        CHECK(std::chrono::steady_clock::now() - w1 < 150ms); // exited: no wait
        CHECK(waited.get("host")->get("child")->get("exit_code")->as_integer() == 3);
        host.ctx.run_mode = false;
        host.ctx.events.add(0, "log", "hello");
        Value p = Value::object();
        p.set("since", 0);
        Value events = client->request("events", p, 5s);
        CHECK(events.get("events")->as_array().size() == 1 && events.get("next")->as_integer() == 1);

        CHECK(thrown([&] { client->request("output", Value::object(), 5s); }).find("doesn't capture") != std::string::npos);
        CHECK(thrown([&] { client->request("nope", Value::object(), 5s); }).find("unknown method") != std::string::npos);
        CHECK(thrown([&] { client->request("reload", Value::object(), 5s); }).find("no program") != std::string::npos);
        Value bad = Value::object();
        bad.set("limit", "lots");
        CHECK(thrown([&] { client->request("events", bad, 5s); }).find("must be a number") != std::string::npos);
        CHECK(thrown([&] { client->request("stop", Value::object(), 5s); }).find("hotpp mcp") != std::string::npos);

        // A program, as a Session registers it (no watcher thread: requests time out).
        auto handle = std::make_shared<host::SessionHandle>(host.ctx.registry, 4242, host.ctx.registry.next_session(), true);
        handle->update([](host::SessionInfo& i) {
            i.state = "watching";
            i.exe = "/bin/game";
        });
        host.ctx.registry.add(handle);
        status = client->request("status", Value::object(), 5s);
        CHECK(status.get("programs")->as_array().size() == 1);
        Value off = Value::object();
        off.set("enabled", false);
        client->request("set_auto_reload", off, 5s);
        CHECK(!handle->auto_reload());
        Value reload = Value::object();
        reload.set("timeout_ms", 100);
        CHECK(client->request("reload", reload, 5s).get("outcome")->as_string() == "timeout");

        // A long poll in progress when the controller goes away ends promptly, and
        // its auto-reload setting goes with it.
        std::thread poll([&] {
            Value w = Value::object();
            w.set("since", 100).set("wait_ms", 60000);
            thrown([&] { client->request("events", w, 70s); });
        });
        std::this_thread::sleep_for(50ms);
        const auto t0 = std::chrono::steady_clock::now();
        client->close();
        poll.join();
        CHECK(std::chrono::steady_clock::now() - t0 < 5s);
        CHECK(host.ctx.registry.wait([&] { return handle->auto_reload(); }, 5s));
        CHECK(!client->alive());
    }
    std::filesystem::remove_all(dir);
}

HOTPP_TEST(control_client_spots_hosts_that_cant_be_controlled) {
    const std::string dir = temp_dir();
    // Accepts and closes after the first message, like a 1.0 host does with a ControlHello.
    std::string error;
    auto listener = host::listen(dir + "/old.sock", error);
    std::thread old([&] {
        while (auto s = listener->accept()) {
            uint8_t header[proto::kHeaderSize];
            s->recv(header, sizeof header);
        }
    });
    bool incompatible = false;
    CHECK(mcp::HostClient::connect(dir + "/old.sock", 2s, error, incompatible) == nullptr);
    CHECK(incompatible);
    CHECK(mcp::HostClient::connect(dir + "/nobody.sock", 2s, error, incompatible) == nullptr);
    CHECK(!incompatible);
    listener->close();
    old.join();
    std::filesystem::remove_all(dir);
}

HOTPP_TEST(hosts_are_discovered_in_the_runtime_directory) {
    const std::string dir = temp_dir();
    const char* saved = std::getenv("XDG_RUNTIME_DIR");
    const std::string old_runtime = saved ? saved : "";
    setenv("XDG_RUNTIME_DIR", dir.c_str(), 1);
    {
        // A socket left behind by a host that died: not listed.
        const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        const std::string stale = dir + "/hotpp-1.sock";
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", stale.c_str());
        CHECK(::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) == 0);
        ::close(fd);

        TestHost live(dir + "/hotpp-2.sock");
        mcp::Hosts hosts({});
        const auto found = hosts.discover();
        CHECK(found.size() == 1 && found[0].endpoint == dir + "/hotpp-2.sock" && found[0].client);
        CHECK(hosts.programs().empty());
        CHECK(thrown([&] { hosts.find(std::nullopt); }).find("No program") != std::string::npos);

        auto handle = std::make_shared<host::SessionHandle>(live.ctx.registry, 77, live.ctx.registry.next_session(), true);
        handle->update([](host::SessionInfo& i) { i.state = "watching"; });
        live.ctx.registry.add(handle);
        CHECK(hosts.find(std::nullopt).pid == 77);
        CHECK(thrown([&] { hosts.find(78); }).find("pid 78") != std::string::npos);

        // The tools on top: argument checking and the status text.
        mcp::HotppTools tools(hosts, {});
        mcp::CallContext ctx;
        Value bad = Value::object();
        bad.set("pid", "x");
        CHECK(tools.call("hotpp_reload", bad, ctx).is_error);
        Value extra = Value::object();
        extra.set("verbose", true);
        const mcp::ToolResult r = tools.call("hotpp_status", extra, ctx);
        CHECK(r.is_error && r.text.find("unknown argument") != std::string::npos);
        const mcp::ToolResult s = tools.call("hotpp_status", Value::object(), ctx);
        CHECK(!s.is_error && s.text.find("pid 77") != std::string::npos);
        Value cmd = Value::object();
        cmd.set("command", Value::array());
        CHECK(tools.call("hotpp_launch", cmd, ctx).is_error); // empty command
        const Value list = tools.list();
        CHECK(list.as_array().size() == 8);
        for (const Value& t : list.as_array())
            CHECK(t.get("inputSchema")->get("type")->as_string() == "object" &&
                  !t.get("inputSchema")->get("additionalProperties")->as_bool());
    }
    if (saved) setenv("XDG_RUNTIME_DIR", old_runtime.c_str(), 1);
    else unsetenv("XDG_RUNTIME_DIR");
    std::filesystem::remove_all(dir);
}

// Mutated messages: every one is answered or ignored, never crashes, and every
// reply is valid JSON-RPC. Run under -DHOTPP_SANITIZE=ON to catch memory errors.
HOTPP_TEST(mcp_survives_fuzzed_messages) {
    const char* seeds[] = {
        R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}})",
        R"({"jsonrpc":"2.0","id":"x","method":"tools/call","params":{"name":"echo","arguments":{"a":[1,2,{"b":"é"}]}}})",
        R"([{"jsonrpc":"2.0","id":2,"method":"ping"},{"jsonrpc":"2.0","method":"notifications/cancelled","params":{"requestId":2}}])",
        R"({"jsonrpc":"2.0","id":3,"method":"tools/list","params":{"cursor":"abc"}})",
    };
    const char* env = std::getenv("HOTPP_FUZZ_ITERATIONS");
    const int n = env ? std::atoi(env) : 2000;
    uint64_t state = 12345;
    auto rnd = [&] {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        return state;
    };
    Harness h;
    size_t replies = 0;
    for (int i = 0; i < n; ++i) {
        std::string m = seeds[rnd() % 4];
        const int edits = 1 + int(rnd() % 4);
        for (int e = 0; e < edits && !m.empty(); ++e) {
            const size_t at = rnd() % m.size();
            switch (rnd() % 4) {
            case 0: m[at] = char(rnd()); break;
            case 1: m.erase(at, 1 + rnd() % 8); break;
            case 2: m.insert(at, 1, "{}[]\",:\\0"[rnd() % 9]); break;
            default: m.resize(at); break;
            }
        }
        h.server.handle_line(m);
        if (i % 100 == 99) {
            for (const Value& v : h.take()) {
                ++replies;
                const Value& first = v.is_array() && v.size() ? v.as_array()[0] : v;
                CHECK(first.get("jsonrpc") && first.get("jsonrpc")->as_string() == "2.0");
            }
        }
    }
    h.take();
    CHECK(replies > 0);

    // The JSON reader on random bytes: an exception or a value that dumps and
    // parses back to the same text.
    for (int i = 0; i < n; ++i) {
        std::string m = seeds[rnd() % 4];
        for (int e = 0; e < 3; ++e) m[rnd() % m.size()] = char(rnd());
        try {
            const std::string once = Value::parse(m).dump();
            CHECK(Value::parse(once).dump() == once);
        } catch (const json::JsonError&) {
        }
    }
}
