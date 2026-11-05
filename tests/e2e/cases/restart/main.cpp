// A change that can't be patched (a global changes size) is rebuilt and restarted by
// the host (--build ... --auto-restart). The pre-restart hook saves state, and the
// restarted process sees restarted() and reads it back.
#include "e2e.h"

#include <hotpp/hotpp.h>

#include <cstdio>
#include <cstdlib>

int value();
void update();
int counter_value();

static void save_state() {
    if (FILE* f = std::fopen(std::getenv("HOTPP_E2E_STATE"), "w")) {
        std::fprintf(f, "%d\n", counter_value());
        std::fclose(f);
    }
}
HOTPP_PRE_RESTART(save_state);

static int saved = -1;

void case_setup() {
    if (!hotpp::restarted()) return;
    if (FILE* f = std::fopen(std::getenv("HOTPP_E2E_STATE"), "r")) {
        if (std::fscanf(f, "%d", &saved) != 1) saved = -1;
        std::fclose(f);
    }
    std::printf("restarted, saved counter %d\n", saved);
}

void case_frame(int frame) {
    update();
    E2E_EXPECT(counter_value() == frame + 1); // a fresh process counts from the start
    E2E_EXPECT(value() == (hotpp::restarted() ? 2 : 1));
}

int case_finish() {
    E2E_EXPECT(hotpp::restarted());
    E2E_EXPECT(value() == 2);
    E2E_EXPECT(saved > 0);
    return 0;
}
