// RUN: %clang-refold-tester-verify-off generated_leaf_descendant_paste_observer_without_arg_provenance
// A generated-leaf rewrite is refused when an unchanged token comes from a
// generated callee whose argument provenance the producer did not record.
//
// `a` reaches both pasted names only through the generated calls `f(a)` and
// `g(a)`, and the producer records no `arg_deps` for a callee without its own
// spelling.  B edits `p_x` alone; rewriting `a` to `y` would also turn the
// unchanged `q_x` into `q_y`.  Nothing proves `q_x` independent of `a`, so the
// fold fails closed and the invocation is expanded.  This used to emit
// `CALL(P, Q, y)` at verify-off.
#define P(a) p_##a
#define Q(a) q_##a
#define CALL(f, g, a) int f(a); int g(a);
CALL(P, Q, x)
