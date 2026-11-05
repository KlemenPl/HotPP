// Version 1 of the patched file. Each kind of state the patch must keep:
int counter = 0;            // global
static int tick_count = 0;  // file-local
namespace {
int call_count = 0;         // anonymous namespace
}

void update() {
    ++counter;
    ++tick_count;
}

int value() {
    static int local_calls = 0; // function-local static
    ++local_calls;
    ++call_count;
    return local_calls == call_count ? 1 : -1;
}

int ticks() { return tick_count; }
int calls() { return call_count; }
int (*value_pointer())() { return &value; }
