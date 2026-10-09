// RUN: %clang-refold-tester-verify-off tuple_generated_callee_element_edit_detaches_nested_callee
// An in-place edit of a forwarded tuple element is held to the same rule as an
// edit of a root actual: the calls around it must stay intact.
//
// ADD_ONE, called through the tuple, observes `(10)` from the element
// `PAREN(10)`, and B asks for `11`.  The tuple-generated solver replaced `(10)`
// inside the element and emitted `WRAP((ADD_ONE, PAREN11))`, which merges
// PAREN with the new value and leaves it uninvoked.  The invocation is
// expanded instead.  Verification is off so the planner alone must refuse the
// edit.
#define PAREN(x) (x)
#define CALL(F, X) F(X)
#define WRAP(PAIR) CALL PAIR
#define ADD_ONE(x) ((x) + 1)
int v = ((11) + 1);
