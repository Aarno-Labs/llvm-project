// RUN: %clang-refold-tester nested_tuple_forwarding
#define EXPR(f,t) f t
#define ADD(g,x,t) ((x)+(g t))
#define SUB(x,y) ((x)-(y))

int x = 5;
int y = 2;
int z = 4;
int res = EXPR(ADD, (SUB, z, (x, y)));
