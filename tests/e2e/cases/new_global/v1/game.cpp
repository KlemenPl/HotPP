int provide(int x);

int existing = provide(5);
struct Box {
    int n;
    Box() : n(provide(7)) {}
};
Box box;

void update() {
    ++existing;
    ++box.n;
}

int value() { return 0; }
int existing_value() { return existing; }
int box_value() { return box.n; }
