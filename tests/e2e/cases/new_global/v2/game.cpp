int provide(int x);

int existing = provide(5);
struct Box {
    int n;
    Box() : n(provide(7)) {}
};
Box box;

int added = provide(100); // new, dynamically initialized
struct Fresh {
    int n;
    Fresh() : n(provide(40)) {}
};
Fresh fresh; // new, with a constructor

void update() {
    ++existing;
    ++box.n;
}

int value() { return added + fresh.n; }
int existing_value() { return existing; }
int box_value() { return box.n; }
