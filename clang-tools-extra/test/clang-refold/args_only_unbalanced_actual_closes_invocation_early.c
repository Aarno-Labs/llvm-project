// RUN: %clang-refold-tester-relaxed-verify-off args_only_unbalanced_actual_closes_invocation_early
//
// An args-only candidate must replace an invocation with an invocation: the
// actual list it writes has to close at the end of the replacement.
//
// Each `p - tbl` below becomes `(p + p_index_xj) - xjg->tbl`.  The byte diff
// pairs A's `p` with the leading `p` of `p_index_xj`, and the args-only
// builder derived the actual `+ p_index_xj) - xjg->tbl == 62` from it; the
// occurrence check reread the same envelope and agreed.  The candidate
// `assert(+ p_index_xj) - xjg->tbl == 62)` is closed by that actual's
// unmatched `)`: the macro expands from `+ p_index_xj` alone and the rest of
// the text follows the expansion, so the relaxed pipeline emitted source that
// does not replay B.
//
// Final admission refuses a replacement with tokens after the parenthesis
// that closes its actual list, and the invocation is realized by whole-cover
// expansion.  Six copies keep the alignment window too large to enumerate, so
// the core-forced token hunks that produce the candidate reach the planner
// instead of a resolved map that sidesteps them.
#define assert(expr) ((void) sizeof ((expr) ? 1 : 0), ({ if (expr) ; else __assert_fail(#expr); }))
#define N 62
void __assert_fail(const char *);
struct T { int len; };
struct G { struct T tbl[63]; };
struct T tbl[63];
void f(struct G *xjg, const struct T *p) {
  int p_index_xj = 0;
  assert(p - tbl == N);
  assert(p - tbl == N);
  assert(p - tbl == N);
  assert(p - tbl == N);
  assert(p - tbl == N);
  assert(p - tbl == N);
}
