// RUN: %clang-refold-tester-with-lines stress_new136_selector_pasted_callee_name_edit_selector_arg
#define ADD_OP ADD
#define SUB_OP SUB
#define PICK(prefix) prefix##_OP
#define APPLY(sel, arg, t) sel(arg) t
#define ADD(a, b) ((a) + (b))
#define SUB(a, b) ((a) - (b))

int x = APPLY(PICK, SUB, (7, 2));
