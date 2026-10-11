// RUN: %clang-refold-tester-verify-off args_only_generated_call_closed_after_callsite_keeps_pasted_observer
// An args-only rewrite is refused when an unchanged token comes from a call
// the root generated whose closing `)` is spelled in source after the root's
// callsite.
//
// `CALL` leaves `F(G(a` open, and the `))` after `CALL(F, x)` closes both
// calls.  The byte range the producer records for G therefore runs from its
// name in CALL's #define to that `)`, which contains CALL's own callsite.  The
// dependence proof used to read that containment as "CALL is spelled inside
// G's arguments", treat G as enclosing CALL, and never ask what G's pasted
// `g_x` observes.  B renames only `int x`; rewriting `x` to `y` would also turn
// the unchanged `g_x` into `g_y`.  The fold fails closed and the invocation is
// expanded.  This used to emit `CALL(F, y)));` at verify-off.
#define F(v) v
#define G(v) g_##v
#define CALL(f, a) int a; int f(G(a
int y; int g_x;
