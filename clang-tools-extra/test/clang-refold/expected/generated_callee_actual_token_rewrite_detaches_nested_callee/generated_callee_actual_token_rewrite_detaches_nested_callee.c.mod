// RUN: %clang-refold-tester-verify-off generated_callee_actual_token_rewrite_detaches_nested_callee
// The token-for-token rewrite of a root actual is an in-place edit too, and
// must not separate a nested call's name from its argument list.
//
// INC observes `(10)`, the expansion of `PAREN(10)`, and B asks for `a+b`.
// The old and new values both have three tokens, so the generated-callee
// solver matched `(`, `10`, `)` inside the actual and replaced them one for
// one, emitting `APPLY(INC, PARENa+b)`.  That rewrite is refused like any
// other in-place edit, and the actual is replaced by B's `a+b`.  Verification
// is off so the planner alone must refuse the edit.
#define APPLY(f, x) f(x)
#define INC(x) ((x)+1)
#define PAREN(x) (x)
int a, b;
int v = APPLY(INC, a+b);
