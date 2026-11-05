// The host crashes (SIGKILL) and is restarted, then is stopped cleanly (SIGTERM)
// and restarted: each time the agent reconnects, the new host resumes the earlier
// patches, and patching carries on with all state intact.
#include "e2e.h"

extern int counter;
void update();
int value();
int added_value();
int (*value_pointer())();

static int (*early)() = nullptr;
static int last_value = 1, last_added = -1;

void case_setup() { early = value_pointer(); }

void case_frame(int frame) {
    update();
    E2E_EXPECT(counter == frame + 1);
    const int v = value(), a = added_value();
    E2E_EXPECT(v >= last_value && v <= 4);
    E2E_EXPECT(early() == v);
    if (v >= 2) E2E_EXPECT(a >= 1 && a >= last_added); // never reset by a later patch
    last_value = v;
    last_added = a;
}

int case_finish() {
    E2E_EXPECT(value() == 4);
    E2E_EXPECT(early() == 4);
    E2E_EXPECT(added_value() > 1);
    return 0;
}
