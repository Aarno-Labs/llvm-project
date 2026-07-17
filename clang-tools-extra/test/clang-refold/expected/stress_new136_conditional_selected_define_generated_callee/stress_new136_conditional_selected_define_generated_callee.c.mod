// RUN: %clang-refold-tester-with-lines stress_new136_conditional_selected_define_generated_callee
#if 1
#define OP SUB
#else
#define OP SUB
#endif
#define APPLY(f, t) f t
#define ADD(a, b) ((a) + (b))
#define SUB(a, b) ((a) - (b))

int x = APPLY(OP, (9, 4));
