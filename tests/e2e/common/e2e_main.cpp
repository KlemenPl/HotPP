// Shared driver for end-to-end cases; see e2e.h.
#include "e2e.h"

#include <hotpp/hotpp.h>

#include <chrono>
#include <thread>

#include <unistd.h>

int main() {
    if (!hotpp::init()) {
        std::printf("FAIL: the agent did not connect\n");
        return 10;
    }
    const char* stop = std::getenv("HOTPP_E2E_STOP");
    case_setup();
    for (int frame = 0; frame < 4000; ++frame) { // ~80 s
        hotpp::sync_point();
        case_frame(frame);
        std::printf("frame %d\n", frame);
        std::fflush(stdout);
        if (stop && access(stop, F_OK) == 0) {
            const int rc = case_finish();
            std::printf(rc == 0 ? "PASS\n" : "FAIL: final state\n");
            std::fflush(stdout);
            hotpp::shutdown();
            return rc;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    std::printf("FAIL: timed out\n");
    return 2;
}
