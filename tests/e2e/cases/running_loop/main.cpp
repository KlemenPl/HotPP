// A patch that replaces a function while it runs (it loops, calling sync_point(),
// until a patch lands): the host warns that the running call keeps its old code.
#include "e2e.h"

#include <hotpp/hotpp.h>

int value();
void inner_loop();

static int patches = 0;
static void count_patch() { ++patches; }
HOTPP_POST_PATCH(count_patch);
int patches_applied() { return patches; }

void case_setup() {}

void case_frame(int frame) {
    if (frame == 3) inner_loop(); // returns once the patch is applied
    std::printf("value %d\n", value());
}

int case_finish() {
    E2E_EXPECT(value() == 2);
    return 0;
}
