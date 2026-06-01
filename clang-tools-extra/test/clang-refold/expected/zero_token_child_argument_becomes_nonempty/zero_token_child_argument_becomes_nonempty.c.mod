// RUN: %clang-refold-tester-with-lines zero_token_child_argument_becomes_nonempty
#define EMPTY()
#define EXPR(a, b) int x = a + b;
EXPR(1, 3)
