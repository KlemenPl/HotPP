long long counter = 0; // was int: can't be patched, needs a restart
void update() { ++counter; }
int value() { return 2; }
int counter_value() { return int(counter); }
