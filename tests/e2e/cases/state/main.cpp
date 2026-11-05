// Every kind of state survives: globals, file statics, anonymous namespaces,
// function-local statics. Also: a new function and variable, and a function
// pointer taken before the patch reaching the new code.
#include "e2e.h"

extern int counter;
int value();
void update();
int ticks();
int calls();
int (*value_pointer())();

static int (*early)() = nullptr;
static int initial = 0, last = 0;

void case_setup() {
    early = value_pointer();
    initial = value();
}

void case_frame(int frame) {
    update();
    last = value();
    E2E_EXPECT(counter == frame + 1);
    E2E_EXPECT(ticks() == frame + 1);
    E2E_EXPECT(calls() == frame + 2);
    E2E_EXPECT(last > 0);
}

int case_finish() {
    E2E_EXPECT(initial == 1);
    E2E_EXPECT(last > 1000);
    E2E_EXPECT(early() == last + 1); // one more call
    return 0;
}
