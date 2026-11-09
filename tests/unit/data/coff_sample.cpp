// Fixture for coff_test.cpp (Windows builds): an executable built the way
// hotpp_enable() builds programs, whose object, image and PDB the test reads.
static int counter = 1;
namespace {
int hidden = 2;
int helper(int x) { return x + hidden; }
} // namespace
static int over(int x) { return x * 2; }
static int over(double x) { return int(x) * 3; }

struct Point {
    int x;
    int y;
    double z;
};
static Point origin{1, 2, 3.0};
int global_value = 7;

struct Base {
    virtual ~Base() = default;
    virtual int f() { return 1; }
};
struct Derived : Base {
    int g = 3;
    int f() override { return g; }
};

int exported(int v) {
    static int calls = 0;
    ++calls;
    Derived d;
    Base* b = &d;
    return helper(v) + over(v) + over(1.5) + counter + calls + origin.x + global_value + b->f();
}

int main(int argc, char**) { return exported(argc) == 0; }
