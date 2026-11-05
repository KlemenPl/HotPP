#include "errors.h"
#include <stdexcept>
int risky(int x) {
    if (x < 0) throw std::runtime_error("negative");
    return x;
}
int pass_through(int x) { return callback(x) + 1; }
int guarded(int x) {
    try {
        return callback(x);
    } catch (const Oops& e) {
        return e.code;
    }
}
