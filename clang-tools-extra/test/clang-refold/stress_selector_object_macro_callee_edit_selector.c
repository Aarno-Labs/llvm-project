// RUN: %clang-refold-tester-with-lines stress_selector_object_macro_callee_edit_selector
#define SELECT_ADD ADD
#define SELECT_SUB SUB
#define APPLY(f, t) f t
#define ADD(a, b) ((a) + (b))
#define SUB(a, b) ((a) - (b))

int x = APPLY(SELECT_ADD, (7, 2));
