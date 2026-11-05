int counter = 0;
static int added = 0; // new in v2: its state must survive host restarts
void update() { ++counter; ++added; }
int value() { return 2; }
int added_value() { return added; }
int (*value_pointer())() { return &value; }
