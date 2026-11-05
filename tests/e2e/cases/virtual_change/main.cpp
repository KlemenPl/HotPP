// A changed virtual function: objects created before the patch (old vtable) and
// after it both run the new body; dynamic_cast keeps working.
#include "e2e.h"
#include "shapes.h"

static Shape* old_square = nullptr;

void case_setup() { old_square = make_square(3); }

void case_frame(int) {
    const int a = old_square->area();
    Shape* fresh = make_square(4);
    const int b = fresh->area();
    delete fresh;
    E2E_EXPECT((a == 9 && b == 16) || (a == 10 && b == 17));
    E2E_EXPECT(is_square(old_square) == 1);
}

int case_finish() {
    E2E_EXPECT(old_square->area() == 10);
    Shape* s = make_square(2);
    E2E_EXPECT(s->area() == 5);
    E2E_EXPECT(is_square(s) == 1);
    delete s;
    return 0;
}
