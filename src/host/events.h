// What a host has done, kept for controllers (hotpp mcp): a bounded log of events
// and the captured output of the program it started.
#pragma once

#include "host/report.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace hotpp::host {

struct Event {
    uint64_t seq = 0;     // 1, 2, 3, ... in the order events were added
    int64_t time_ms = 0;  // Unix time
    uint32_t pid = 0;     // the program it is about; 0 for the host itself
    std::string kind;     // log | patch | connected | disconnected | exited
    std::string text;
    std::shared_ptr<const PatchReport> patch; // kind == "patch"

    json::Value to_json() const;
};

class EventLog {
public:
    explicit EventLog(size_t capacity = 5000) : capacity_(capacity) {}

    // Adds an event and returns its sequence number. A patch event also stores the
    // number in the report (report->seq).
    uint64_t add(uint32_t pid, std::string kind, std::string text, std::shared_ptr<PatchReport> patch = nullptr);

    struct Page {
        std::vector<Event> events;
        uint64_t next = 0;      // pass as `since` to continue
        bool truncated = false; // events after `since` were dropped (the log is bounded)
    };
    // Events after `since` (0: from the start of what's kept), for one program if
    // `pid` isn't 0, at most `limit`. Waits up to `wait` for one to arrive, or until
    // `abort` is set (wake waiters with notify()).
    Page after(uint64_t since, uint32_t pid, size_t limit, std::chrono::milliseconds wait,
               const std::atomic<bool>* abort = nullptr) const;
    // The last `limit` events (for one program if `pid` isn't 0).
    Page tail(uint32_t pid, size_t limit) const;
    uint64_t last_seq() const;
    void notify() const { cv_.notify_all(); }

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    mutable std::condition_variable cv_;
    std::deque<Event> events_;
    uint64_t next_seq_ = 1;
};

// The last `capacity` bytes a program printed. Offsets count every byte since the
// start, so a reader can continue where it stopped and learn what it missed.
class OutputBuffer {
public:
    explicit OutputBuffer(size_t capacity = 1 << 20) : capacity_(capacity) {}

    void append(std::string_view data);
    void close(); // the program's output ended

    struct Chunk {
        std::string text;
        uint64_t start = 0;   // offset of text[0]
        uint64_t next = 0;    // offset to continue from
        uint64_t dropped = 0; // bytes between `since` and `start` no longer kept
        bool closed = false;
    };
    // Up to `max_bytes` from offset `since`. Never ends in the middle of a UTF-8
    // sequence unless that sequence is cut off for good.
    Chunk read(uint64_t since, size_t max_bytes) const;
    // The last `lines` lines (at most `max_bytes`), or the last `max_bytes` if lines is 0.
    Chunk tail(size_t lines, size_t max_bytes) const;
    uint64_t end() const;

private:
    size_t capacity_;
    mutable std::mutex mutex_;
    std::string data_;
    uint64_t base_ = 0; // offset of data_[0]
    bool closed_ = false;
};

int64_t unix_time_ms();

} // namespace hotpp::host
