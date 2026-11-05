// Code in a shared library is patched like code in the executable: its globals,
// statics and thread_locals keep their values, and it can call back into the program.
#include "e2e.h"
#include "plugin.h"

int app_twice(int x) { return 2 * x; } // exported for the library

void case_setup() {}

void case_frame(int frame) {
    plugin_update();
    E2E_EXPECT(plugin_counter == frame + 1);
    E2E_EXPECT(plugin_ticks_value() == frame + 1);
    E2E_EXPECT(plugin_tls_value() == frame + 1);
    const int v = plugin_value();
    E2E_EXPECT(v == 1 || v == 2);
    E2E_EXPECT(plugin_calls_app() == 2 * v);
}

int case_finish() {
    E2E_EXPECT(plugin_value() == 2);
    return 0;
}
