// RUN: %clang-refold-tester dag_selector_pasted_callee_edit_arg
#define PICK(sel,x) sel##_impl(x)
#define lo_impl(x) ((x)-1)
#define hi_impl(x) ((x)+1)
int v = PICK(hi, 40);
