#include "item.h"
Item* make_item() { return new Item{}; } // puts Item in the debug info
int item_size() { return int(sizeof(Item)); }
