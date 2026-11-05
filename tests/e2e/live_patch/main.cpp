// End-to-end patch test. The harness starts this under the host, swaps game.cpp for
// the v2 version while it runs and checks the exit code:
//   0  value() changed and all state survived      2  no patch arrived in time
//   3  state was lost or diverged                  10 the agent didn't connect
#include <hotpp/hotpp.h>

#include <chrono>
#include <cstdio>
#include <thread>

extern int counter;
int value();
void update();
int ticks();
int calls();
int (*value_pointer())();

int main() {
    if (!hotpp::init()) return 10;
    int (*const early_pointer)() = value_pointer(); // taken before the patch
    const int initial = value();
    int changed_at = -1;
    for (int frame = 0; frame < 600; ++frame) {
        hotpp::sync_point();
        update();
        const int v = value();
        std::printf("frame %d counter %d ticks %d calls %d value %d\n", frame, counter, ticks(), calls(), v);
        std::fflush(stdout);
        // update() and value() run once per frame (plus the initial value() call):
        // any reset or second copy of their state shows up here.
        if (counter != frame + 1 || ticks() != frame + 1 || calls() != frame + 2 || v < 0) {
            std::printf("STATE LOST\n");
            return 3;
        }
        if (v != initial && changed_at < 0) changed_at = frame;
        if (changed_at >= 0 && frame >= changed_at + 3) {
            const int via_pointer = early_pointer();
            std::printf("PATCHED value %d -> %d, old pointer -> %d\n", initial, v, via_pointer);
            if (via_pointer != v + 1) { // early_pointer() is one more call
                std::printf("STALE POINTER\n");
                return 3;
            }
            hotpp::shutdown();
            return 0;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::printf("TIMEOUT\n");
    return 2;
}
