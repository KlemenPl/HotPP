#include <functional>
template <class T> static T combine(T a, T b) { return a + b * 100; }
std::function<int(int)> make_op() {
    return [](int x) { return x + 100; };
}
int apply_template(int x) { return combine(x, 1); }
