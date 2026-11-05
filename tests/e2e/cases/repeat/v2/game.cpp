int counter = 0;
void update() { ++counter; }
int value() { return 2; }
int (*value_pointer())() { return &value; }
