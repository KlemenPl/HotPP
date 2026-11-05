// A thread calling the patched function in a tight loop keeps running and picks
// up the new code.
#include "e2e.h"

#include <atomic>
#include <thread>

extern std::atomic<long> work_calls;
int work();

static std::atomic<bool> stop{false};
static std::atomic<int> seen{0};
static std::thread worker;
static long last_calls = 0;

void case_setup() {
    worker = std::thread([] {
        while (!stop.load(std::memory_order_relaxed)) seen.store(work(), std::memory_order_relaxed);
    });
}

void case_frame(int frame) {
    // The worker is still running: it made progress within the last 25 frames
    // (a looser check than every frame, so a briefly starved thread isn't a failure).
    if (frame % 25 == 0) {
        const long calls = work_calls.load();
        if (frame > 0) E2E_EXPECT(calls > last_calls);
        last_calls = calls;
    }
    const int s = seen.load();
    E2E_EXPECT(s == 0 || s == 1 || s == 2);
}

int case_finish() {
    stop = true;
    worker.join();
    E2E_EXPECT(seen.load() == 2);
    E2E_EXPECT(work() == 2);
    return 0;
}
