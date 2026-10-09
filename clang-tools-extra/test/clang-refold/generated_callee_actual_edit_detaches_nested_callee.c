// RUN: %clang-refold-tester-verify-off generated_callee_actual_edit_detaches_nested_callee
// An in-place edit of a root actual must not separate a nested call's name
// from its argument list.
//
// INC observes `(10)`, the expansion of `PAREN(10)`, and B asks for `-1`.  The
// generated-callee solver found `(10)` inside the actual and replaced it,
// emitting `APPLY(INC, PAREN-1)`: PAREN is no longer invoked, so the actual
// expands to `PAREN-1` instead of `-1`.  No token merges here; the edit is
// refused because its left edge splits `PAREN` from `(`, and the actual is
// replaced by B's `-1`.  Verification is off so the planner alone must refuse
// the edit.
#define APPLY(f, x) f(x)
#define INC(x) ((x)+1)
#define PAREN(x) (x)
int v = APPLY(INC, PAREN(10));
