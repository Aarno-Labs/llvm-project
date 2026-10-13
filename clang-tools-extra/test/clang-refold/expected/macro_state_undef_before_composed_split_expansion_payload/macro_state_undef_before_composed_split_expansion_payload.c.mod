// RUN: %clang-refold-tester-verify-off macro_state_undef_before_composed_split_expansion_payload
// Regression: the token diff splits the second `M`'s expansion `((M)0)`
// between two hunks, keeping its inner `( M ) 0` as stable tokens.  Each
// hunk's direct edit replaces the whole `M` callsite, so the two overlap, and
// the applicator composed them into one edit spelling B's tokens between
// them, painted `M` included, under the live `#define M ((M)0)`.  That
// composition ran after the macro-state audit, so nothing read its text and
// `M` expanded again.  The fragments now compose before macro-state
// planning, and the partition leaves `M` unbound for the composed line.
typedef long M;
#define M ((M)0)
long f(long c, long s, long i) {
  c = (!c || i < 0) ? M
#undef M
      : (i < 0 ? (M)0 : c + i) + 1;
  return c;
}
