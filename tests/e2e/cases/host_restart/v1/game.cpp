int counter = 0;
void update() { ++counter; }
int value() { return 1; }
int added_value() { return -1; }
int (*value_pointer())() { return &value; }
