#include "host/events.h"

#include <algorithm>

namespace hotpp::host {

int64_t unix_time_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

json::Value Event::to_json() const {
    json::Value v = json::Value::object();
    v.set("seq", seq).set("time_ms", time_ms).set("pid", pid).set("kind", kind).set("text", text);
    if (patch) v.set("patch", patch->to_json());
    return v;
}

uint64_t EventLog::add(uint32_t pid, std::string kind, std::string text, std::shared_ptr<PatchReport> patch) {
    uint64_t seq;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        seq = next_seq_++;
        if (patch) patch->seq = seq;
        Event e;
        e.seq = seq;
        e.time_ms = unix_time_ms();
        e.pid = pid;
        e.kind = std::move(kind);
        e.text = std::move(text);
        e.patch = std::move(patch);
        events_.push_back(std::move(e));
        while (events_.size() > capacity_) events_.pop_front();
    }
    cv_.notify_all();
    return seq;
}

EventLog::Page EventLog::after(uint64_t since, uint32_t pid, size_t limit, std::chrono::milliseconds wait,
                               const std::atomic<bool>* abort) const {
    Page page;
    std::unique_lock<std::mutex> lock(mutex_);
    since = std::min(since, next_seq_ - 1); // a cursor from the future (another host): from now on
    auto matching = [&] {
        for (auto it = events_.rbegin(); it != events_.rend() && it->seq > since; ++it)
            if (pid == 0 || it->pid == pid) return true;
        return false;
    };
    if (wait.count() > 0)
        cv_.wait_for(lock, wait, [&] { return matching() || (abort && abort->load()); });
    page.truncated = !events_.empty() && events_.front().seq > since + 1;
    page.next = since;
    for (const Event& e : events_) {
        if (e.seq <= since) continue;
        if (pid != 0 && e.pid != pid) {
            page.next = e.seq; // other programs' events need not be read again
            continue;
        }
        if (page.events.size() >= limit) break;
        page.events.push_back(e);
        page.next = e.seq;
    }
    return page;
}

EventLog::Page EventLog::tail(uint32_t pid, size_t limit) const {
    Page page;
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto it = events_.rbegin(); it != events_.rend() && page.events.size() < limit; ++it)
        if (pid == 0 || it->pid == pid) page.events.push_back(*it);
    std::reverse(page.events.begin(), page.events.end());
    page.next = next_seq_ - 1;
    return page;
}

uint64_t EventLog::last_seq() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return next_seq_ - 1;
}

void OutputBuffer::append(std::string_view data) {
    std::lock_guard<std::mutex> lock(mutex_);
    data_.append(data);
    // Trim in batches so appending stays cheap.
    if (data_.size() > capacity_ + capacity_ / 4) {
        const size_t drop = data_.size() - capacity_;
        data_.erase(0, drop);
        base_ += drop;
    }
}

void OutputBuffer::close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
}

namespace {
// The largest n <= len such that s[0, n) doesn't end inside a UTF-8 sequence that
// continues after it.
size_t utf8_boundary(std::string_view s, size_t len) {
    if (len >= s.size()) return s.size();
    size_t i = len;
    // Back up over continuation bytes (at most 3) to the lead byte.
    size_t back = 0;
    while (i > 0 && back < 3 && (uint8_t(s[i]) & 0xC0) == 0x80) {
        --i;
        ++back;
    }
    return back > 0 || (uint8_t(s[len]) & 0xC0) == 0x80 ? i : len;
}
} // namespace

OutputBuffer::Chunk OutputBuffer::read(uint64_t since, size_t max_bytes) const {
    Chunk c;
    std::lock_guard<std::mutex> lock(mutex_);
    const uint64_t end = base_ + data_.size();
    c.closed = closed_;
    if (since > end) since = end;
    if (since < base_) {
        c.dropped = base_ - since;
        since = base_;
    }
    const std::string_view rest = std::string_view(data_).substr(size_t(since - base_));
    size_t n = std::min(rest.size(), max_bytes);
    if (n < rest.size()) {
        const size_t b = utf8_boundary(rest, n);
        if (b > 0) n = b;
    }
    c.text.assign(rest.substr(0, n));
    c.start = since;
    c.next = since + n;
    return c;
}

OutputBuffer::Chunk OutputBuffer::tail(size_t lines, size_t max_bytes) const {
    uint64_t from;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        const std::string_view all(data_);
        size_t start = all.size() > max_bytes ? all.size() - max_bytes : 0;
        if (lines > 0) {
            // Back to just after the lines-th newline from the end (a final newline
            // doesn't start another line).
            size_t pos = all.size();
            if (pos > start && all[pos - 1] == '\n') --pos;
            size_t found = 0;
            while (pos > start) {
                if (all[pos - 1] == '\n' && ++found == lines) break;
                --pos;
            }
            start = pos;
        }
        while (start < all.size() && (uint8_t(all[start]) & 0xC0) == 0x80) ++start;
        from = base_ + start;
    }
    Chunk c = read(from, max_bytes);
    c.dropped = 0; // a tail never asked for what came before
    return c;
}

uint64_t OutputBuffer::end() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return base_ + data_.size();
}

} // namespace hotpp::host
