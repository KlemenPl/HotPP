// Three patches in a row to the same function; a pointer taken at the start
// reaches the newest version, and state carries over every time.
#include "e2e.h"

extern int counter;
void update();
int value();
int (*value_pointer())();

static int (*early)() = nullptr;
static int last = 1;

void case_setup() { early = value_pointer(); }

void case_frame(int frame) {
    update();
    E2E_EXPECT(counter == frame + 1);
    const int v = value();
    E2E_EXPECT(v >= last && v <= 4); // versions only move forward
    E2E_EXPECT(early() == v);
    last = v;
}

int case_finish() {
    E2E_EXPECT(value() == 4);
    E2E_EXPECT(early() == 4);
    return 0;
}
