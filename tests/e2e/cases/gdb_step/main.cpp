// Runs under gdb (see gdb_script.py): once the patch is live, gdb must stop at a
// line of the *new* code, read its locals and a variable kept from the old program,
// show a backtrace into old code and step back out.
#include "e2e.h"

extern int counter;
int value();
void update();

// gdb breaks here once the program sees the new value.
extern "C" __attribute__((noinline, used)) void e2e_patched(int v) { __asm__ volatile("" ::"r"(v)); }

static bool reported = false;
static int last = 0;

void case_setup() {}

void case_frame(int) {
    update();
    last = value();
    E2E_EXPECT(last == 2 * counter || last == 1000000 + 2 * counter);
    if (last >= 1000000 && !reported) { // only the new code reaches this
        reported = true;
        e2e_patched(last);
    }
}

int case_finish() {
    E2E_EXPECT(last >= 1000000);
    return 0;
}
