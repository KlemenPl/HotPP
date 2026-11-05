int counter = 0;
void update() { ++counter; }
int value() {
    int bonus = counter * 2;
    return 1000000 + bonus; // line 5: gdb stops here
}
