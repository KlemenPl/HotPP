// Changes that can't be applied are rejected without touching the program, and a
// valid change afterwards still applies.
#include "e2e.h"

void update();
int value();
int counter_value();

void case_setup() {}

void case_frame(int frame) {
    update();
    E2E_EXPECT(counter_value() == frame + 1);
    const int v = value();
    E2E_EXPECT(v == 1 || v == 3);
}

int case_finish() {
    E2E_EXPECT(value() == 3);
    return 0;
}
