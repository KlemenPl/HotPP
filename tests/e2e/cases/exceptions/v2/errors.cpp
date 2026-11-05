#include "errors.h"
#include <stdexcept>
int risky(int x) {
    if (x < 1000) throw std::runtime_error("too small");
    return x;
}
int pass_through(int x) { return callback(x) + 2; }
int guarded(int x) {
    try {
        return callback(x);
    } catch (const Oops& e) {
        return e.code + 1000;
    }
}
