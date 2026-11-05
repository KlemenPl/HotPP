// New globals with dynamic initializers are initialized; existing ones (also
// dynamically initialized) keep their state.
#include "e2e.h"

int provide(int x) { return x; } // in another file, so the initializers stay dynamic

void update();
int value();
int existing_value();
int box_value();

static int last = -1;

void case_setup() {}

void case_frame(int frame) {
    update();
    E2E_EXPECT(existing_value() == 5 + frame + 1);
    E2E_EXPECT(box_value() == 7 + frame + 1);
    last = value();
    E2E_EXPECT(last == 0 || last == 140);
}

int case_finish() {
    E2E_EXPECT(last == 140);
    return 0;
}
