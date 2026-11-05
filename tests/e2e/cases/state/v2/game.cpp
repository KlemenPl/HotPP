int counter = 0;
static int tick_count = 0;
namespace {
int call_count = 0;
}

static int bonus_base = 1;                // new variable
static int bonus() { return bonus_base; } // new function

void update() {
    ++counter;
    ++tick_count;
}

int value() {
    static int local_calls = 0;
    ++local_calls;
    ++call_count;
    return local_calls == call_count ? 1 + bonus() * 1000 + call_count : -1;
}

int ticks() { return tick_count; }
int calls() { return call_count; }
int (*value_pointer())() { return &value; }
