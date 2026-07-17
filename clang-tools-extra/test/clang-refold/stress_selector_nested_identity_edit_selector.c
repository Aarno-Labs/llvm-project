// RUN: %clang-refold-tester-with-lines stress_selector_nested_identity_edit_selector
#define ID(x) x
#define SELECT_LEFT(a, b) ID(a)
#define SELECT_RIGHT(a, b) ID(b)
#define APPLY(sel, t) sel(ADD, SUB) t
#define ADD(a, b) ((a) + (b))
#define SUB(a, b) ((a) - (b))

int x = APPLY(SELECT_LEFT, (7, 2));
