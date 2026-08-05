// RUN: %clang-refold-tester nested_tuple_forwarding_repeated_intermediate_element
// Regression: a root tuple element consumed by an *intermediate* replacement
// list is never a terminal generated-callee actual, so the terminal replay
// solution says nothing about it.  Here `x` is `ADD`'s own formal and occurs
// twice in `ADD`'s body, while `(y, z)` is forwarded on to `SUB`.  The refold
// must edit all three elements and every occurrence of `x` must agree.
#define EXPR(f,t) f t
#define ADD(g,x,t) ((x)+(x)+(g t))
#define SUB(x,y) ((x)-(y))

int x = 5;
int y = 2;
int z = 4;
int res = EXPR(ADD, (SUB, x, (y, z)));
