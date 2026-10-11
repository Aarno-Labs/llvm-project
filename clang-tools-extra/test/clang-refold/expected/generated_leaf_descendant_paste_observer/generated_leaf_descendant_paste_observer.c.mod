// RUN: %clang-refold-tester-verify-off generated_leaf_descendant_paste_observer
// A generated-leaf rewrite is refused when an unchanged token comes from a
// generated callee whose actual depends on the rewritten formal.
//
// `a` reaches both pasted names only through the generated calls `f(a)` and
// `g(a)`.  Their callee names come from formals, so their text cannot carry
// `arg_deps`; the producer's token-location `arg_token_deps` show that `Q`'s
// actual is `CALL`'s `a`.  B edits `p_x` alone; rewriting `a` to `y` would
// also turn the unchanged `q_x` into `q_y`, so the fold is refused and the
// invocation is expanded.  This used to emit `CALL(P, Q, y)` at verify-off,
// and failed closed for want of provenance before `arg_token_deps`.
#define P(a) p_##a
#define Q(a) q_##a
#define CALL(f, g, a) int f(a); int g(a);
int p_y; int q_x;
