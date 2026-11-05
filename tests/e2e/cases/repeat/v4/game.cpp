int counter = 0;
void update() { ++counter; }
int value() { return 4; }
int (*value_pointer())() { return &value; }
