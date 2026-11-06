// Driven through hotpp mcp by tests/mcp/run_mcp_test.py: every frame prints what
// the edited sources return, so the test can see each patch land through
// hotpp_output (or the host's output when it's attached to a user's host).
#include "e2e.h"

#include <hotpp/hotpp.h>

#include <cstdio>

int value();
const char* label();
void tick();
int counter_value();

void case_setup() {
    std::printf("started restarted=%d\n", hotpp::restarted() ? 1 : 0);
    std::fflush(stdout);
}

void case_frame(int) {
    tick();
    std::printf("value=%d label=%s counter=%d\n", value(), label(), counter_value());
}

int case_finish() { return 0; }
