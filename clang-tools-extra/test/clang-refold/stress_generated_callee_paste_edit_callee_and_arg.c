// RUN: %clang-refold-tester-with-lines stress_generated_callee_paste_edit_callee_and_arg
#define GET(x) ((x) + 1)
#define SET(x) ((x) - 1)
#define DISPATCH(a, b, x) a##b(x)

int x = DISPATCH(G, ET, 7);
