// RUN: %clang-refold-tester generated_selector_boundary_insertion_before_call_refolds
// An insertion just before a call whose expansion forms a call from one of its
// actuals is made in source before the callsite.  G's name comes from CALL's
// actual, which offers the boundary of CALL's cover to a proof that rewrites
// that callee selector.  B prepends `long y;`, which no selector rewrite
// explains, and an owner that can only replace its callsite does not need
// the boundary: the TU edit at the gap keeps both calls.
#define G(v) g_##v
#define CALL(f, a) int a; int f(a
long y; CALL(G, x));
