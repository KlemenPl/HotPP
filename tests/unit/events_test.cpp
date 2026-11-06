#include "host/events.h"
#include "host/registry.h"
#include "host/report.h"
#include "unit/test.h"

#include <thread>

using namespace hotpp::host;
using namespace std::chrono_literals;

HOTPP_TEST(event_log_pages_and_filters) {
    EventLog log(4);
    for (int i = 1; i <= 3; ++i) log.add(uint32_t(i % 2 ? 10 : 20), "log", "line " + std::to_string(i));
    EventLog::Page p = log.after(0, 0, 100, 0ms);
    CHECK(p.events.size() == 3 && p.next == 3 && !p.truncated);
    p = log.after(0, 10, 100, 0ms);
    CHECK(p.events.size() == 2 && p.events[1].text == "line 3" && p.next == 3);
    p = log.after(1, 0, 1, 0ms); // limit
    CHECK(p.events.size() == 1 && p.events[0].seq == 2 && p.next == 2);
    for (int i = 4; i <= 6; ++i) log.add(10, "log", "line " + std::to_string(i));
    p = log.after(0, 0, 100, 0ms); // capacity 4: 1 and 2 dropped
    CHECK(p.truncated && p.events.front().seq == 3 && p.events.size() == 4);
    p = log.tail(10, 2);
    CHECK(p.events.size() == 2 && p.events[1].text == "line 6" && p.next == 6);
    p = log.after(1000, 0, 10, 0ms); // a cursor from another host: from now on
    CHECK(p.events.empty() && p.next == 6);
}

HOTPP_TEST(event_log_waits_for_events) {
    EventLog log;
    const auto t0 = std::chrono::steady_clock::now();
    EventLog::Page p = log.after(0, 0, 10, 50ms);
    CHECK(p.events.empty() && std::chrono::steady_clock::now() - t0 >= 40ms);
    std::thread writer([&] {
        std::this_thread::sleep_for(20ms);
        log.add(1, "log", "hello");
    });
    p = log.after(0, 0, 10, 5s);
    writer.join();
    CHECK(p.events.size() == 1 && p.events[0].text == "hello");
    std::atomic<bool> abort{true};
    p = log.after(1, 0, 10, 5s, &abort); // aborted: no wait
    CHECK(p.events.empty());
    auto report = std::make_shared<PatchReport>();
    report->status = "applied";
    const uint64_t seq = log.add(1, "patch", "patch 1 applied", report);
    CHECK(report->seq == seq);
    CHECK(log.tail(0, 1).events[0].to_json().get("patch")->get("status")->as_string() == "applied");
}

HOTPP_TEST(output_buffer_keeps_the_end_with_offsets) {
    OutputBuffer out(16);
    out.append("hello\n");
    out.append("world\n");
    OutputBuffer::Chunk c = out.read(0, 100);
    CHECK(c.text == "hello\nworld\n" && c.start == 0 && c.next == 12 && c.dropped == 0);
    c = out.read(6, 3);
    CHECK(c.text == "wor" && c.next == 9);
    out.append("0123456789abcdef"); // trims to the last 16 bytes
    c = out.read(0, 100);
    CHECK(c.dropped == 12 && c.start == 12 && c.text == "0123456789abcdef");
    c = out.read(500, 10); // past the end
    CHECK(c.text.empty() && c.next == 28);
    OutputBuffer lines;
    lines.append("a\nb\nc\nd\n");
    CHECK(lines.tail(2, 100).text == "c\nd\n");
    CHECK(lines.tail(10, 100).text == "a\nb\nc\nd\n");
    CHECK(lines.tail(0, 3).text == "\nd\n");
    lines.close();
    CHECK(lines.read(0, 1).closed);
}

HOTPP_TEST(output_buffer_doesnt_split_utf8) {
    OutputBuffer out;
    out.append("ab\xE2\x82\xAC" "cd"); // "ab€cd"
    OutputBuffer::Chunk c = out.read(0, 3);
    CHECK(c.text == "ab" && c.next == 2);
    c = out.read(c.next, 3);
    CHECK(c.text == "\xE2\x82\xAC" && c.next == 5);
    CHECK(out.tail(0, 4).text == "cd"); // the cut would start inside the euro sign
}

HOTPP_TEST(reports_strip_color_and_cap_output) {
    CHECK(strip_ansi("\033[1m\033[31merror:\033[0m bad\r\n\033]8;;file://x\033\\link\033]8;;\033\\") == "error: bad\nlink");
    CHECK(strip_ansi("plain") == "plain");
    std::string big;
    for (int i = 0; i < 1000; ++i) big += "line " + std::to_string(i) + "\n";
    bool truncated = false;
    const std::string capped = cap_output(big, 100, 50, &truncated);
    CHECK(truncated && capped.size() < 200);
    CHECK(capped.rfind("line 0\n", 0) == 0 && capped.find("bytes omitted") != std::string::npos);
    CHECK(capped.size() >= 7 && capped.substr(capped.size() - 9) == "line 999\n");
    CHECK(cap_output("short", 100, 50, &truncated) == "short" && !truncated);
    PatchReport r;
    r.id = 3;
    r.status = "compile_failed";
    r.files = {"/a.cpp"};
    const auto j = r.to_json();
    CHECK(j.get("status")->as_string() == "compile_failed" && !j.get("counts"));
}

HOTPP_TEST(registry_lists_newest_session_and_owns_overrides) {
    Registry reg;
    auto a = std::make_shared<SessionHandle>(reg, 100, reg.next_session(), true);
    auto b = std::make_shared<SessionHandle>(reg, 200, reg.next_session(), true);
    reg.add(a);
    reg.add(b);
    CHECK(reg.list().size() == 2);
    reg.set_auto_reload(100, false, /*owner*/ 7);
    CHECK(!a->auto_reload() && !a->info().auto_reload && b->auto_reload());
    // A hot restart: a new session for pid 100 takes the override; list shows it only.
    auto a2 = std::make_shared<SessionHandle>(reg, 100, reg.next_session(), true);
    reg.add(a2);
    CHECK(!a2->auto_reload());
    CHECK(reg.list().size() == 2 && reg.find(100) == a2);
    reg.release_auto_reload(8); // someone else: no effect
    CHECK(!a2->auto_reload());
    reg.release_auto_reload(7);
    CHECK(a2->auto_reload());
    reg.remove(a.get());
    CHECK(reg.find(100) == a2);

    // Requests: queued, then failed when the session closes.
    auto req = std::make_shared<SessionRequest>(SessionRequest::Kind::Reload);
    CHECK(b->post(req));
    CHECK(b->take_requests().size() == 1);
    auto req2 = std::make_shared<SessionRequest>(SessionRequest::Kind::Reload);
    CHECK(b->post(req2));
    b->close("disconnected", "gone");
    CHECK(req2->done() && req2->result().outcome == "disconnected");
    CHECK(!b->post(req));
    req->finish("applied");
    req->finish("ignored");
    CHECK(req->result().outcome == "applied");

    // wait() wakes on changes.
    std::thread t([&] {
        std::this_thread::sleep_for(20ms);
        a2->update([](SessionInfo& i) { i.state = "watching"; });
    });
    CHECK(reg.wait([&] { return a2->info().state == "watching"; }, 5s));
    t.join();
    CHECK(!reg.wait([] { return false; }, 30ms));
}
