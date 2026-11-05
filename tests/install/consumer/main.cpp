#include <hotpp/hotpp.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

int value();

int main(int argc, char** argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 3;
    if (!hotpp::init()) return 10;
    for (int i = 0; i < frames; ++i) {
        hotpp::sync_point();
        std::printf("frame %d value %d\n", i, value());
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    hotpp::shutdown();
    return 0;
}
