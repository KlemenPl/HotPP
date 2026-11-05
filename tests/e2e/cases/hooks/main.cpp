// Pre/post-patch hooks run once per patch, a changed hook body takes effect, and a
// hook added by a patch is registered by it (and runs after it).
#include "e2e.h"

extern int pre_count, post_count;
int value();
int added_count(); // only exists from v3 on; weak so v1 links
__attribute__((weak)) int added_count() { return 0; }

void case_setup() {}

void case_frame(int) {
    const int v = value();
    std::printf("value %d pre %d post %d added %d\n", v, pre_count, post_count, added_count());
    // Hooks ran exactly once per applied patch so far.
    E2E_EXPECT(pre_count == v - 1);
    if (v == 1) E2E_EXPECT(post_count == 0);
    if (v == 2) E2E_EXPECT(post_count == 1);
    if (v == 3) E2E_EXPECT(post_count == 11 && added_count() == 1);
}

int case_finish() {
    E2E_EXPECT(value() == 3);
    E2E_EXPECT(pre_count == 2);
    E2E_EXPECT(post_count == 11);   // 1 from patch 1, then the new body's +10
    E2E_EXPECT(added_count() == 1); // registered and run by patch 2
    return 0;
}
