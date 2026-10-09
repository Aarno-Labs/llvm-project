// RUN: %clang-refold-tester-verify-off args_only_rewritten_actual_replays_cover_image
//
// An args-only rewrite replaces its invocation's whole expansion, so the fixed
// replacement-list text together with the rewritten actuals must reproduce the
// cover's B tokens.  The literal-body audit checked the fixed text and that
// each argument surface sat between it, but never what a surface held.
//
// Each `-p - q` becomes `- -p + pz - q`.  The token diff keeps the new `-` in a
// hunk over the body's `-`, which no argument occurrence owns, so args-only
// derived the actual `p + pz - q` and emitted `M(p + pz - q)`, one `-` short.
// The audit refuses a rewrite whose expansion does not split the cover's
// token-diff image into the fixed text and its rewritten actuals, and these
// invocations are expanded.  Six copies keep the alignment window too large to
// enumerate, so its core-forced hunks reach the planner.
#define M(x) -x
void f(int p, int pz, int q) {
  (void)M(p - q);
  (void)M(p - q);
  (void)M(p - q);
  (void)M(p - q);
  (void)M(p - q);
  (void)M(p - q);
}
