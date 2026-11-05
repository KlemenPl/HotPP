#include <hotpp/hotpp.h>

int pre_count = 0;
int post_count = 0;

static void on_pre() { ++pre_count; }
static void on_post() { ++post_count; }
HOTPP_PRE_PATCH(on_pre);
HOTPP_POST_PATCH(on_post);

int value() { return 1; }
