// A library loaded with dlopen() after the program connected is patched too, and
// once it's dlclose()d the host stops patching it.
#include "e2e.h"

#include <dlfcn.h>

static void* handle = nullptr;
static int (*plugin_value)() = nullptr;
static int calls = 0, last = 0;

void case_setup() {}

void case_frame(int frame) {
    if (frame == 3) { // well after connecting
        handle = dlopen(HOTPP_E2E_PLUGIN, RTLD_NOW);
        E2E_EXPECT(handle != nullptr);
        plugin_value = reinterpret_cast<int (*)()>(dlsym(handle, "plugin_value"));
        E2E_EXPECT(plugin_value != nullptr);
    }
    if (!plugin_value) return;
    const int v = plugin_value();
    ++calls;
    E2E_EXPECT(v % 1000 == calls);                 // its static survived
    E2E_EXPECT(v / 1000 == 1 || v / 1000 == 2);
    last = v / 1000;
    if (last == 2 && calls > 20 && handle) {     // patched and used for a while: unload it
        dlclose(handle);
        handle = nullptr;
        plugin_value = nullptr;
    }
}

int case_finish() {
    E2E_EXPECT(last == 2);
    return 0;
}
