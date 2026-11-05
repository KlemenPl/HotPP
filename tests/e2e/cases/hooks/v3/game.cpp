#include <hotpp/hotpp.h>

int pre_count = 0;
int post_count = 0;

static void on_pre() { ++pre_count; }
static void on_post() { post_count += 10; }
HOTPP_PRE_PATCH(on_pre);
HOTPP_POST_PATCH(on_post);

int added = 0;
static void on_added() { ++added; }
HOTPP_POST_PATCH(on_added);
int added_count() { return added; }

int value() { return 3; }
