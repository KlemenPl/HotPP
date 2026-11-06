#include <hotpp/hotpp.h>

#include <chrono>
#include <cstdio>
#include <thread>

int patches_applied();
int value() { return 2; }

void inner_loop() {
    std::printf("inner loop, v2\n");
    std::fflush(stdout);
    const int start = patches_applied();
    while (patches_applied() == start) {
        hotpp::sync_point();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
