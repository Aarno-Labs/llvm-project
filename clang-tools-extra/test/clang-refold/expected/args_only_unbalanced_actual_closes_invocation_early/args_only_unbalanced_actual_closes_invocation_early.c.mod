// RUN: %clang-refold-tester-relaxed-verify-off args_only_unbalanced_actual_closes_invocation_early
//
// Each `p - tbl` below becomes `(p + p_index_xj) - xjg->tbl`.  The byte diff
// pairs A's `p` with the leading `p` of `p_index_xj` and reports `(p + ` as
// inserted at the argument's edge, while the token diff keeps the new `(` in a
// hunk over the body's `((`.  Trimming that edge left the args-only actual
// `+ p_index_xj) - xjg->tbl == 62`, and the candidate
// `assert(+ p_index_xj) - xjg->tbl == 62)` was closed early by its unmatched
// `)`: the macro expanded from `+ p_index_xj` alone and the rest of the text
// followed the expansion, so the relaxed pipeline emitted source that does not
// replay B.
//
// The trimmed edge meets fixed replacement-list text, which cannot own an
// insertion, so the inserted text belongs to the occurrence.  The two copies
// whose byte diff splits this way fold to
// `assert((p + p_index_xj) - xjg->tbl == 62)`; the others are expanded.  Six
// copies keep the alignment window too large to enumerate, so the core-forced
// token hunks reach the planner instead of a resolved map that sidesteps them.
#define assert(expr) ((void) sizeof ((expr) ? 1 : 0), ({ if (expr) ; else __assert_fail(#expr); }))
#define N 62
void __assert_fail(const char *);
struct T { int len; };
struct G { struct T tbl[63]; };
struct T tbl[63];
void f(struct G *xjg, const struct T *p) {
  int p_index_xj = 0;
  ((void) sizeof (((p + p_index_xj) - xjg->tbl == 62) ? 1 : 0), ({ if ((p + p_index_xj) - xjg->tbl == 62) ; else __assert_fail("p - tbl == N"); }));
  assert((p + p_index_xj) - xjg->tbl == 62);
  ((void) sizeof (((p + p_index_xj) - xjg->tbl == 62) ? 1 : 0), ({ if ((p + p_index_xj) - xjg->tbl == 62) ; else __assert_fail("p - tbl == N"); }));
  ((void) sizeof (((p + p_index_xj) - xjg->tbl == 62) ? 1 : 0), ({ if ((p + p_index_xj) - xjg->tbl == 62) ; else __assert_fail("p - tbl == N"); }));
  assert((p + p_index_xj) - xjg->tbl == 62);
  ((void) sizeof (((p + p_index_xj) - xjg->tbl == 62) ? 1 : 0), ({ if ((p + p_index_xj) - xjg->tbl == 62) ; else __assert_fail("p - tbl == N"); }));
}
