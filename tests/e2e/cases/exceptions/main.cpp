// Exceptions cross old and new frames in both directions, and catch clauses in
// new code match types thrown by old code.
#include "e2e.h"
#include "errors.h"

#include <stdexcept>

int callback(int x) {
    if (x < 0) throw Oops{x};
    return x;
}

static bool patched = false;

void case_setup() {}

void case_frame(int) {
    bool threw = false;
    try {
        risky(5); // new code throws for x < 1000
    } catch (const std::runtime_error&) {
        threw = true;
    }
    const int g = guarded(-3); // new code catches what old code throws
    int through = 0;
    try {
        pass_through(-4); // unwinds through a new frame
    } catch (const Oops& e) {
        through = e.code;
    }
    const int p = pass_through(1);
    E2E_EXPECT(through == -4);
    E2E_EXPECT((!threw && g == -3 && p == 2) || (threw && g == 997 && p == 3));
    patched = threw;
}

int case_finish() {
    E2E_EXPECT(patched);
    return 0;
}
