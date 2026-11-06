int counter = 0;
void tick() { ++counter; }
int value() { return 3 } // missing semicolon: doesn't compile
int counter_value() { return counter; }
