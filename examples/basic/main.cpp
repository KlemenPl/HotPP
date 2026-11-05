// Minimal HotPP sample: a loop calling a function you can edit while it runs.
//
//   hotpp -- ./hotpp_example_basic            # start it under a host
//   ./hotpp_example_basic --frames 10         # or connect to a running host
#include <hotpp/hotpp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

static int frame_counter = 0; // state that survives patches

int compute(int frame) { return frame * 2; }

int main(int argc, char** argv) {
    long frames = -1;
    long interval_ms = 500;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            frames = std::strtol(argv[++i], nullptr, 10);
        } else if (std::strcmp(argv[i], "--interval-ms") == 0 && i + 1 < argc) {
            interval_ms = std::strtol(argv[++i], nullptr, 10);
        } else {
            std::fprintf(stderr, "usage: %s [--frames N] [--interval-ms MS]\n", argv[0]);
            return 1;
        }
    }

    const bool hot = hotpp::init();
    std::printf("hot reload %s\n", hot ? "enabled" : "disabled");

    while (frames < 0 || frame_counter < frames) {
        hotpp::sync_point();
        std::printf("frame %d: compute() = %d\n", frame_counter, compute(frame_counter));
        std::fflush(stdout);
        ++frame_counter;
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
    }

    hotpp::shutdown();
    return 0;
}
