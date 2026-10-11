// RUN: %clang-refold-tester-verify-off args_only_formal_forwarded_to_generated_callee
// An args-only rewrite is refused when an unchanged token comes from a
// generated callee whose actual depends on the rewritten formal.
//
// `a` is used directly by `int a;` and is forwarded to the generated calls
// `f(a)` and `g(a)`, which paste it.  B renames only the direct use, so
// rewriting the actual to `y` would also turn the unchanged `p_x` and `q_x`
// into `p_y` and `q_y`.  A callee named by a formal carries no text
// `arg_deps`; the producer's token-location `arg_token_deps` show that both
// actuals are `CALL`'s `a`, so the fold is refused and the invocation is
// expanded.  This used to emit `CALL(P, Q, y)` at verify-off, and failed
// closed for want of provenance before `arg_token_deps`.
#define P(a) p_##a
#define Q(a) q_##a
#define CALL(f, g, a) int f(a); int g(a); int a;
CALL(P, Q, x)
