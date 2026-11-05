// A broken edit applies nothing; fixing it applies the change.
#include "e2e.h"

int value();

void case_setup() {}

void case_frame(int) {
    const int v = value();
    E2E_EXPECT(v == 1 || v == 2);
}

int case_finish() {
    E2E_EXPECT(value() == 2);
    return 0;
}
