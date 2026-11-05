// A program that uses only single_include/hotpp/hotpp.h: no hotpp_agent library and
// no compile_commands path built in. It still connects and gets patched.
#include "e2e.h"

#include "hotpp/hotpp.h"

extern int counter;
void update();
int value();

static int saw = 0;
static void after_patch() { ++saw; }
HOTPP_POST_PATCH(after_patch);

void case_setup() {}

void case_frame(int frame) {
    update();
    E2E_EXPECT(counter == frame + 1);
    const int v = value();
    E2E_EXPECT(v == 1 || v == 2);
}

int case_finish() {
    E2E_EXPECT(value() == 2);
    E2E_EXPECT(saw == 1);
    return 0;
}
