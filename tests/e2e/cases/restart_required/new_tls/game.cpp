int counter = 0;
thread_local int fresh_tls = 0; // new thread_local
void update() { ++counter; }
int value() { return 2 + fresh_tls; }
int counter_value() { return counter; }
