long long counter = 0; // was int: can't be patched, needs a restart
void tick() { ++counter; }
int value() { return 4; }
int counter_value() { return int(counter); }
