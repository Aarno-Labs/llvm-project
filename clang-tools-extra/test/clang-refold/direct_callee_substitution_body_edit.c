// RUN: %clang-refold-tester direct_callee_substitution_body_edit SELECT
// RUN: %clang-refold-tester direct_callee_substitution_body_edit EXPAND
#define ADD(x,y) ((x) + (y))
#define SUB(x,y) ((x) - (y))

int x = ADD(5, 20);
