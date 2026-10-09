// RUN: %clang-refold-tester-verify-off generated_callee_actual_edit_inside_nested_call_comma
// An in-place edit of a root actual that lands inside a nested call's actuals
// must leave that call's actuals as they were.
//
// INC observes `((10)-1)`, the expansion of `APPLY(DEC, 10)`, and B's
// `((88, 99)-1)` differs from it only in `10` -> `88, 99`.  The generated-callee
// solver inverted that changed middle in place and emitted
// `APPLY(INC, APPLY(DEC, 88, 99))`, giving the nested APPLY three actuals.  Its
// only arity check looked at the root actual, where the nested parentheses
// hide the comma.  The invocation is expanded instead.  Verification is off
// so the planner alone must refuse the edit.
#define APPLY(f, x) f(x)
#define INC(x) ((x)+1)
#define DEC(x) ((x)-1)
int v = APPLY(INC, APPLY(DEC, 10));
