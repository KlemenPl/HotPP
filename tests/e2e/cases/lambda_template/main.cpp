// Lambda bodies and template instantiations update, including a std::function
// that captured the old lambda before the patch.
#include "e2e.h"

#include <functional>

std::function<int(int)> make_op();
int apply_template(int x);

static std::function<int(int)> old_op;

void case_setup() { old_op = make_op(); }

// MSVC names a lambda by a hash that changes when its file does, so a changed
// lambda is a new type there: a std::function made before the patch keeps calling
// the old body (README, "What needs a restart").
#if defined(_MSC_VER) && !defined(__clang__)
constexpr bool kOldLambdasFollow = false;
#else
constexpr bool kOldLambdasFollow = true;
#endif

void case_frame(int) {
    const int a = old_op(1), b = apply_template(1);
    if (kOldLambdasFollow) E2E_EXPECT((a == 2 && b == 2) || (a == 101 && b == 101));
    else E2E_EXPECT(a == 2 && (b == 2 || b == 101));
}

int case_finish() {
    E2E_EXPECT(old_op(1) == (kOldLambdasFollow ? 101 : 2));
    E2E_EXPECT(make_op()(1) == 101);
    E2E_EXPECT(apply_template(1) == 101);
    return 0;
}
