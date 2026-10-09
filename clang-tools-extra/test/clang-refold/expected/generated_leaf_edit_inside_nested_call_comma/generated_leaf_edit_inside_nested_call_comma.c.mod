// RUN: %clang-refold-tester-verify-off generated_leaf_edit_inside_nested_call_comma
// A generated-leaf edit inside a nested call's actuals must leave that call's
// actuals as they were.
//
// STR stringifies the prescanned actual, so `ID(a + b)` reaches B as
// `"a + b"`, and B asks for `"a , b"`.  The generated-leaf solver rewrote the
// changed leaf `+` to `,` in place and emitted
// `APPLY(OUTER, INNER, STR, ID(a , b))`, giving ID two actuals; its only comma
// check looked at the root actual, where ID's parentheses hide the comma.  The
// invocation is expanded instead.  Verification is off so the planner alone
// must refuse the edit.
#define APPLY(F, G, H, X) F(G, H, X)
#define OUTER(G, H, X) G(H, X)
#define INNER(H, X) H(X)
#define STR(x) #x
#define ID(x) x
const char *s = "a , b";
