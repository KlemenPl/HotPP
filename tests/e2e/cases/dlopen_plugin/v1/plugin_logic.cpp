// Built as a module the program loads with dlopen() after it connected.
static int calls = 0;
extern "C" int plugin_value() {
    ++calls;
    return 1 * 1000 + calls; // calls must keep counting across the patch
}
