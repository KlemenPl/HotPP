// Contract between an end-to-end case and the shared main() in e2e_main.cpp.
//
// The harness (run_case.py) starts the case under the host, swaps in newer
// versions of its sources step by step, and finally creates a stop file. The
// program checks its invariants every frame and its final state at the end.
#pragma once

#include <cstdio>
#include <cstdlib>

// Implemented by each case.
void case_setup();           // once, after hotpp::init()
void case_frame(int frame);  // every frame, right after hotpp::sync_point()
int case_finish();           // after the last step: 0 if the final state is right

// Fails the case at once with a message the harness shows.
#define E2E_EXPECT(cond)                                                                 \
    do {                                                                                 \
        if (!(cond)) {                                                                   \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                  \
            std::fflush(stdout);                                                         \
            std::_Exit(3);                                                               \
        }                                                                                \
    } while (0)
