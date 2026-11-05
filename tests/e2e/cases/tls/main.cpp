// Existing thread_local variables keep their values, on every thread.
#include "e2e.h"

#include <atomic>
#include <chrono>
#include <thread>

void update();
int tls_value();
int tls_local_value();
int value();

static std::atomic<int> phase{0}; // 0: worker counting, 1: counted, 2: check, 3: checked
static std::atomic<int> worker_result{-1};
static std::thread worker;

void case_setup() {
    worker = std::thread([] {
        for (int i = 0; i < 3; ++i) update();
        phase = 1;
        while (phase != 2) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        worker_result = tls_value() * 1000 + tls_local_value(); // after the patch
        phase = 3;
    });
    while (phase != 1) std::this_thread::sleep_for(std::chrono::milliseconds(5));
}

void case_frame(int frame) {
    update();
    E2E_EXPECT(tls_value() == frame + 1);
    E2E_EXPECT(tls_local_value() == 100 + frame + 1);
}

int case_finish() {
    E2E_EXPECT(value() == 2);
    phase = 2;
    worker.join();
    E2E_EXPECT(worker_result == 3 * 1000 + 103);
    return 0;
}
