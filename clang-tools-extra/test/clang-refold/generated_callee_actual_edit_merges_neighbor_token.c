// RUN: %clang-refold-tester-verify-off generated_callee_actual_edit_merges_neighbor_token
// An in-place edit of a root actual must not merge with the token beside it.
//
// TAG expands to nothing, so INC observes `(10)` from the actual `(10)TAG`,
// and B asks for `20`.  The generated-callee solver replaced `(10)` in place
// and emitted `APPLY(INC, 20TAG)`, where `20TAG` lexes as one preprocessing
// number.  No identifier meets a `(` at either edge, so only the merge refuses
// the edit, and the actual is replaced by B's `20`.  Verification is off so
// the planner alone must refuse the edit.
#define APPLY(f, x) f(x)
#define INC(x) ((x)+1)
#define TAG
int v = APPLY(INC, (10)TAG);
