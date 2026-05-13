#define EMPTY
#define FORWARD(...) __VA_ARGS__
int x =
1 + FORWARD(EMPTY)
#include "two.inc"
;
