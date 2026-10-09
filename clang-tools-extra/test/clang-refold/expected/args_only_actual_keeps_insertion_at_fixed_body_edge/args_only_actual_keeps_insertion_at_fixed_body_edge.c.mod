// RUN: %clang-refold-tester-verify-off args_only_actual_keeps_insertion_at_fixed_body_edge
//
// B text inserted where an argument occurrence meets fixed replacement-list
// text belongs to the occurrence, because the `#define` cannot own it.
//
// Each `2 * p - q` becomes `2 * 2 * p + pz - q`.  The byte diff pairs A's `p`
// with the `p` of `pz` and reports `2 * p + ` as inserted at the argument's
// edge.  Trimming that edge and widening by the token hunks left the actual
// `+ pz - q`, and `M(+ pz - q)` expands to `2 * + pz - q`.  Widening the
// occurrence over the insertion at its fixed-text edge recovers
// `2 * p + pz - q`, and `M(2 * p + pz - q)` replays B; the remaining copies are
// expanded.  Six copies keep the alignment window too large to enumerate, so
// its core-forced hunks reach the planner.
#define M(x) 2 * x
void f(int p, int pz, int q) {
  (void)M(2 * p + pz - q);
  (void)2 * 2 * p + pz - q;
  (void)M(2 * p + pz - q);
  (void)M(2 * p + pz - q);
  (void)2 * 2 * p + pz - q;
  (void)M(2 * p + pz - q);
}
