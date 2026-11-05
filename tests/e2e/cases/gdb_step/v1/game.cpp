int counter = 0;
void update() { ++counter; }
int value() {
    int bonus = counter * 2;
    return bonus;
}
