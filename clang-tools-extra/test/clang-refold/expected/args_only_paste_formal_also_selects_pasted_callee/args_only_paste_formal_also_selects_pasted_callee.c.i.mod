static int foo_body(int x) { return x; }
static int bar_body(int x) { return x + 1; }
int bar_tag = foo_body(1);
