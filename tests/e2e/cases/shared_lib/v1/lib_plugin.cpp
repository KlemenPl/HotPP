// Built into a shared library (lib_*.cpp), loaded by the program at startup.
#include "plugin.h"

int plugin_counter = 0;                // global, reached through the GOT (-fPIC)
static int plugin_ticks = 0;           // file-local
thread_local int plugin_tls = 0;       // TLS of the library: general-dynamic access

void plugin_update() {
    ++plugin_counter;
    ++plugin_ticks;
    ++plugin_tls;
}

int plugin_ticks_value() { return plugin_ticks; }
int plugin_tls_value() { return plugin_tls; }
int plugin_value() { return 1; }
int app_twice(int x);                  // defined in the executable
int plugin_calls_app() { return app_twice(1); }
