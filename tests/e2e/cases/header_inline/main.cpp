// Editing an inline function and a template in a header updates every file that
// includes it, in one patch.
#include "e2e.h"

int a_value();
int a_twice();
int b_value();

void case_setup() {}

void case_frame(int) {
    const int a = a_value(), t = a_twice(), b = b_value();
    E2E_EXPECT((a == 2 && t == 10 && b == 20) || (a == 3 && t == 15 && b == 30));
}

int case_finish() {
    E2E_EXPECT(a_value() == 3);
    E2E_EXPECT(a_twice() == 15);
    E2E_EXPECT(b_value() == 30);
    return 0;
}
