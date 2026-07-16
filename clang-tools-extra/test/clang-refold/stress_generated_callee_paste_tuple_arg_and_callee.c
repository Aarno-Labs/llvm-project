// RUN: %clang-refold-tester-with-lines stress_generated_callee_paste_tuple_arg_and_callee
#define ADD(a, b) ((a) + (b))
#define SUB(a, b) ((a) - (b))
#define DISPATCH(a, b, t) a##b t

int x = DISPATCH(A, DD, (1, 2));
