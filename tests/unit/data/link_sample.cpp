// Compiled like a patch (see tests/CMakeLists.txt) and linked by linker_test.cpp.
#include <cstdio>

int counter = 0;
static int ticks = 0;

int value() { return counter + ticks; }

void update() {
    ++counter;
    ++ticks;
    std::printf("%d\n", counter);
}

int fresh() { return 42; }

int (*value_pointer())() { return &value; }

static int helper() { return 3; }
int (*helper_pointer())() { return &helper; } // GCC takes this address via the section symbol
