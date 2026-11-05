// A struct that gains a member in a header: the patch applies, and the host warns
// that objects created before it still have the old layout.
#include "e2e.h"

int item_size();

void case_setup() {}

void case_frame(int) {
    const int s = item_size();
    E2E_EXPECT(s == 4 || s == 8);
}

int case_finish() {
    E2E_EXPECT(item_size() == 8);
    return 0;
}
