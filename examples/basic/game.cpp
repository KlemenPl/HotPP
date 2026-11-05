// Edit this file while the example runs: change compute() and save.
#include "game.h"

int frame_counter = 0; // lives on across patches

int compute(int frame) { return frame * 2; }

void update() { ++frame_counter; }
