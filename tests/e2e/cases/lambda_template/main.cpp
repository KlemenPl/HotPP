// Lambda bodies and template instantiations update, including a std::function
// that captured the old lambda before the patch.
#include "e2e.h"

#include <functional>

std::function<int(int)> make_op();
int apply_template(int x);

static std::function<int(int)> old_op;

void case_setup() { old_op = make_op(); }

void case_frame(int) {
    const int a = old_op(1), b = apply_template(1);
    E2E_EXPECT((a == 2 && b == 2) || (a == 101 && b == 101));
}

int case_finish() {
    E2E_EXPECT(old_op(1) == 101);
    E2E_EXPECT(make_op()(1) == 101);
    E2E_EXPECT(apply_template(1) == 101);
    return 0;
}
