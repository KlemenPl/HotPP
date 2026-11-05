// Async mode: the agent thread applies the patch while the main thread is busy in
// a loop that never reaches sync_point().
#include "e2e.h"

#include <chrono>

int value();

void case_setup() {}

void case_frame(int frame) {
    if (frame != 3) return;
    const auto start = std::chrono::steady_clock::now();
    while (value() == 1) // only an async patch can end this loop
        E2E_EXPECT(std::chrono::steady_clock::now() - start < std::chrono::seconds(30));
}

int case_finish() {
    E2E_EXPECT(value() == 2);
    return 0;
}
